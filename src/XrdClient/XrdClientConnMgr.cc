//////////////////////////////////////////////////////////////////////////
//                                                                      //
// XrdClientConnMgr                                                     //
// Author: Fabrizio Furano (INFN Padova, 2004)                          //
// Adapted from TXNetFile (root.cern.ch) originally done by             //
//  Alvise Dorigo, Fabrizio Furano                                      //
//          INFN Padova, 2003                                           //
//                                                                      //
// The connection manager maps multiple logical connections on a single //
// physical connection.                                                 //
// There is one and only one logical connection per client              //
// and one and only one physical connection per server:port.            //
// Thus multiple objects withing a given application share              //
// the same physical TCP channel to communicate with a server.          //
// This reduces the time overhead for socket creation and reduces also  //
// the server load due to handling many sockets.                        //
//                                                                      //
//////////////////////////////////////////////////////////////////////////

//       $Id$

#include "XrdClient/XrdClientConnMgr.hh"
#include "XrdClient/XrdClientDebug.hh"
#include "XrdClient/XrdClientMessage.hh"
#include "XrdOuc/XrdOucPthread.hh"
#include "XrdClient/XrdClientEnv.hh"
#include "XrdClient/XrdClientSid.hh"
#ifdef WIN32
#include "XrdSys/XrdWin32.hh"
#endif

#ifdef AIX
#include <sys/sem.h>
#else
#include <semaphore.h>
#endif

// For user info
#ifndef WIN32
#include <sys/types.h>
#include <pwd.h>
#endif

//_____________________________________________________________________________
void * GarbageCollectorThread(void *arg, XrdClientThread *thr)
{
   // Function executed in the garbage collector thread

   int i;
   XrdClientConnectionMgr *thisObj = (XrdClientConnectionMgr *)arg;

   thr->SetCancelDeferred();
   thr->SetCancelOn();

   while (1) {
      thr->CancelPoint();

      thisObj->GarbageCollect();

      for (i = 0; i < 10; i++) {
	 thr->CancelPoint();

         usleep(200000);
      }
   }

   return 0;
}

//_____________________________________________________________________________
XrdClientConnectionMgr::XrdClientConnectionMgr() : fGarbageColl(0)
{
   // XrdClientConnectionMgr constructor.
   // Creates a Connection Manager object.
   // Starts the garbage collector thread.

   // Garbage collector thread creation
   if (EnvGetLong(NAME_STARTGARBAGECOLLECTORTHREAD)) {
      fGarbageColl = new XrdClientThread(GarbageCollectorThread);

      if (!fGarbageColl)
	 Error("ConnectionMgr",
	       "Can't create garbage collector thread: out of system resources");

      fGarbageColl->Run(this);
   }
   else
      if(DebugLevel() >= XrdClientDebug::kHIDEBUG)
         Info(XrdClientDebug::kHIDEBUG,
	      "ConnectionMgr",
              "Explicitly requested not to start the garbage collector"
              " thread. Are you sure?");
}

//_____________________________________________________________________________
XrdClientConnectionMgr::~XrdClientConnectionMgr()
{
   // Deletes mutex locks, stops garbage collector thread.

   int i=0;

   {
      XrdOucMutexHelper mtx(fMutex);

      for (i = 0; i < fLogVec.GetSize(); i++)
	 if (fLogVec[i]) Disconnect(i, FALSE);

   }

   if (fGarbageColl) {
      fGarbageColl->Cancel();
      fGarbageColl->Join(0);
      delete fGarbageColl;
   }

   GarbageCollect();
}

//_____________________________________________________________________________
void XrdClientConnectionMgr::GarbageCollect()
{
   // Get rid of unused physical connections. 'Unused' means not used for a
   // TTL time from any logical one. The TTL depends on the kind of remote
   // server. For a load balancer the TTL is very high, while for a data server
   // is quite small.

   // Mutual exclusion on the vectors and other vars
   {
      XrdOucMutexHelper mtx(fMutex);

      // We cycle all the physical connections to disconnect the elapsed ones
      for (int i = 0; i < fPhyVec.GetSize(); i++) { 
   
	 // If a single physical connection has no linked logical connections,
	 // then we kill it if its TTL has expired
	 if ( fPhyVec[i] && (GetPhyConnectionRefCount(fPhyVec[i]) <= 0) && 
	      fPhyVec[i]->ExpiredTTL() && fPhyVec[i]->IsValid() ) {
      
	    Info(XrdClientDebug::kUSERDEBUG,
		 "GarbageCollect", "Disconnecting physical connection " << i);

	    fPhyVec[i]->Touch();
	    fPhyVec[i]->Disconnect();
	          
	    Info(XrdClientDebug::kUSERDEBUG,
		 "GarbageCollect", "Disconnected physical connection " << i);

	 }
      }




      // We cycle all the physical connections to destroy the
      // elapsed once more after a disconnection
      // Doing this way, a phyconn in async mode has all the time it needs
      //  to terminate its reader thread
      for (int i = 0; i < fPhyVec.GetSize(); i++) { 
   
	 // If a single physical connection has no linked logical connections,
	 // then we kill it if its TTL has expired after disconnection
	 if ( fPhyVec[i] && (GetPhyConnectionRefCount(fPhyVec[i]) <= 0) && 
	      fPhyVec[i]->ExpiredTTL() && !fPhyVec[i]->IsValid() ) {
      
	    Info(XrdClientDebug::kUSERDEBUG,
		 "GarbageCollect", "Purging physical connection " << i);

	    delete fPhyVec[i];
	    fPhyVec[i] = 0;
      
	    Info(XrdClientDebug::kUSERDEBUG,
		 "GarbageCollect", "Purged physical connection " << i);

	 }
      }

   }

}

//_____________________________________________________________________________
int XrdClientConnectionMgr::Connect(XrdClientUrlInfo RemoteServ)
{
   // Connects to the remote server:
   //  - Looks first for an existing physical connection already bound to
   //    User@RemoteAddress:TcpPort;
   //  - If needed, creates a TCP channel to User@RemoteAddress:TcpPort
   //   (this is a physical connection);
   //  - Creates a logical connection and binds it to the previous 
   //    created physical connection;
   //  - Returns the logical connection ID. Every client will use this
   //    ID to deal with the server.

   XrdClientLogConnection *logconn = 0;
   XrdClientPhyConnection *phyconn = 0;
   int newid;
   bool phyfound = FALSE;

   // First we get a new logical connection object
   Info(XrdClientDebug::kHIDEBUG,
	"Connect", "Creating a logical connection...");

   logconn = new XrdClientLogConnection();
   if (!logconn) {
      Error("Connect", "Object creation failed. Aborting.");
      abort();
   }

   // If empty, fill the user name with the default to avoid fake mismatches
   if (RemoteServ.User.length() <= 0) {
#ifndef WIN32
      struct passwd *pw = getpwuid(getuid());
      RemoteServ.User = (pw) ? pw->pw_name : "";
#else
      char  name[256];
      DWORD length = sizeof (name);
      ::GetUserName(name, &length);
      RemoteServ.User = name;
#endif
   }

   { XrdOucMutexHelper mtx(fMutex);

      // If we already have a physical connection to that host:port, 
      // then we use that
      for (int i=0; i < fPhyVec.GetSize(); i++) {
	 if ( fPhyVec[i] && fPhyVec[i]->IsValid() &&
	      fPhyVec[i]->IsPort(RemoteServ.Port) &&
	      fPhyVec[i]->IsUser(RemoteServ.User) &&
	      (fPhyVec[i]->IsAddress(RemoteServ.Host) ||
	       fPhyVec[i]->IsAddress(RemoteServ.HostAddr)) ) {

	    // We link that physical connection to the new logical connection
	    fPhyVec[i]->Touch();
	    logconn->SetPhyConnection( fPhyVec[i] );
	    phyfound = TRUE;
	    break;
	 }
      }

   }

   if (!phyfound) {
      
      Info(XrdClientDebug::kHIDEBUG,
	   "Connect",
	   "Physical connection not found. Creating a new one...");

      // If not already present, then we must build a new physical connection, 
      // and try to connect it
      // While we are trying to connect, the mutex must be unlocked
      // Note that at this point logconn is a pure local instance, so it 
      // does not need to be protected by mutex
      phyconn = new XrdClientPhyConnection(this);

      if (!phyconn) {
	 Error("Connect", "Object creation failed. Aborting.");
         abort();
      }
      if ( phyconn && phyconn->Connect(RemoteServ) ) {

         logconn->SetPhyConnection(phyconn);

         if (DebugLevel() >= XrdClientDebug::kHIDEBUG)
            Info(XrdClientDebug::kHIDEBUG,
		 "Connect",
		 "New physical connection to server " <<
		 RemoteServ.Host << ":" << RemoteServ.Port <<
		 " succesfully created.");

      } else {
	 delete logconn;
	 delete phyconn;
	 
         return -1;
      }

   }


   // Now, we are connected to the host desired.
   // The physical connection can be old or newly created
   {
      XrdOucMutexHelper mtx(fMutex);

      // Then, if needed, we push the physical connection into its vector
      if (!phyfound)
	 fPhyVec.Push_back(phyconn);

      // Then we push the logical connection into its vector
      fLogVec.Push_back(logconn);
 
      // Its ID is its position inside the vector, we must return it later
      newid = fLogVec.GetSize()-1;

      // Now some debug log
      if (DebugLevel() >= XrdClientDebug::kHIDEBUG) {
	 int logCnt = 0, phyCnt = 0;

	 for (int i=0; i < fPhyVec.GetSize(); i++)
	    if (fPhyVec[i])
	       phyCnt++;
	 for (int i=0; i < fLogVec.GetSize(); i++)
	    if (fLogVec[i])
	       logCnt++;

	 Info(XrdClientDebug::kHIDEBUG,
	      "Connect",
	      "LogConn: size:" << fLogVec.GetSize() << " count: " << logCnt <<
	      "PhyConn: size:" << fPhyVec.GetSize() << " count: " << phyCnt );

      }

   }
  

   return newid;
}

//_____________________________________________________________________________
void XrdClientConnectionMgr::Disconnect(short int LogConnectionID, 
                                 bool ForcePhysicalDisc)
{
   // Deletes a logical connection.
   // Also deletes the related physical one if ForcePhysicalDisc=TRUE.
   if (LogConnectionID < 0) return;

   {
      XrdOucMutexHelper mtx(fMutex);

      if ((LogConnectionID < 0) ||
	  (LogConnectionID >= fLogVec.GetSize()) || (!fLogVec[LogConnectionID])) {
	 Error("Disconnect", "Destroying nonexistent logconn " << LogConnectionID);
	 return;
      }


      if (ForcePhysicalDisc) {
	 // We disconnect the phyconn
	 // But it will be removed by the garbagecollector as soon as possible
	 // Note that here we cannot destroy the phyconn, since there can be other 
	 // logconns pointing to it the phyconn will be removed when there are no 
	 // more logconns pointing to it
	 //fLogVec[LogConnectionID]->GetPhyConnection()->SetTTL(0);
	 fLogVec[LogConnectionID]->GetPhyConnection()->Disconnect();
      }
    
      fLogVec[LogConnectionID]->GetPhyConnection()->Touch();
      delete fLogVec[LogConnectionID];
      fLogVec[LogConnectionID] = 0;


   }

}

//_____________________________________________________________________________
int XrdClientConnectionMgr::ReadRaw(short int LogConnectionID, void *buffer, 
                               int BufferLength)
{
   // Read BufferLength bytes from the logical connection LogConnectionID

   XrdClientLogConnection *logconn;

   logconn = GetConnection(LogConnectionID);

   if (logconn) {
      return logconn->ReadRaw(buffer, BufferLength);
   }
   else {
      Error("ReadRaw", "There's not a logical connection with id " <<
           LogConnectionID);

      return(TXSOCK_ERR);
   }
}

//_____________________________________________________________________________
XrdClientMessage *XrdClientConnectionMgr::ReadMsg(short int LogConnectionID)
{
   XrdClientLogConnection *logconn;
   XrdClientMessage *mex;

   logconn = GetConnection(LogConnectionID);

   // Now we get the message from the queue, with the timeouts needed
   // Note that the physical connection know about streamids, NOT logconnids !!
   mex = logconn->GetPhyConnection()->ReadMessage(logconn->Streamid());

   // Return the message unmarshalled to ClientServerCmd
   return mex;
}

//_____________________________________________________________________________
int XrdClientConnectionMgr::WriteRaw(short int LogConnectionID, const void *buffer, 
				     int BufferLength, int substreamid) {
   // Write BufferLength bytes into the logical connection LogConnectionID

   XrdClientLogConnection *logconn;

   logconn = GetConnection(LogConnectionID);

   if (logconn) {
       return logconn->WriteRaw(buffer, BufferLength, substreamid);
   }
   else {
      Error("WriteRaw", "There's not a logical connection with id " <<
	    LogConnectionID);

      return(TXSOCK_ERR);
   }
}

//_____________________________________________________________________________
XrdClientLogConnection *XrdClientConnectionMgr::GetConnection(short int LogConnectionID)
{
   // Return a logical connection object that has LogConnectionID as its ID.
   XrdOucMutexHelper mtx(fMutex);
 
   return fLogVec[LogConnectionID];

}

//_____________________________________________________________________________
short int XrdClientConnectionMgr::GetPhyConnectionRefCount(XrdClientPhyConnection *PhyConn)
{
   // Return the number of logical connections bound to the physical one 'PhyConn'
   int cnt = 0;

   {
      XrdOucMutexHelper mtx(fMutex);

      for (int i = 0; i < fLogVec.GetSize(); i++)
	 if ( fLogVec[i] && (fLogVec[i]->GetPhyConnection() == PhyConn) ) cnt++;

   }
  
   return cnt;
}

//_____________________________________________________________________________
UnsolRespProcResult XrdClientConnectionMgr::ProcessUnsolicitedMsg(XrdClientUnsolMsgSender *sender,
                                             XrdClientMessage *unsolmsg)
{
   // We are here if an unsolicited response comes from a physical connection
   // The response comes in the form of an TXMessage *, that must NOT be
   // destroyed after processing. It is destroyed by the first sender.
   // Remember that we are in a separate thread, since unsolicited responses
   // are asynchronous by nature.

   Info(XrdClientDebug::kDUMPDEBUG, "ConnectionMgr",
       "Processing unsolicited response (ID:"<<unsolmsg->HeaderSID()<<")");
   UnsolRespProcResult res = kUNSOL_CONTINUE;
   // Local processing ....

   // Now we propagate the message to the interested objects.
   // In our architecture, the interested objects are the objects which
   // self-registered in the logical connections belonging to the Phyconn
   // which threw the event
   // So we throw the evt towards each logical connection
   {
      XrdOucMutexHelper mtx(fMutex);

      for (int i = 0; i < fLogVec.GetSize(); i++)
	 if ( fLogVec[i] && (fLogVec[i]->GetPhyConnection() == sender) ) {
	    res = fLogVec[i]->ProcessUnsolicitedMsg(sender, unsolmsg);

	    if (res != kUNSOL_CONTINUE) break;
	 }
   }


   return res;
}
