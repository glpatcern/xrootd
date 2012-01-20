//------------------------------------------------------------------------------
// Copyright (c) 2011 by European Organization for Nuclear Research (CERN)
// Author: Lukasz Janyst <ljanyst@cern.ch>
// See the LICENCE file for details.
//------------------------------------------------------------------------------

#ifndef __XRD_CL_STREAM_HH__
#define __XRD_CL_STREAM_HH__

#include "XrdCl/XrdClPoller.hh"
#include "XrdCl/XrdClStatus.hh"
#include "XrdCl/XrdClURL.hh"
#include "XrdCl/XrdClPostMasterInterfaces.hh"

#include "XrdSys/XrdSysPthread.hh"
#include <list>

namespace XrdClient
{
  class  Message;
  class  MessageStatusHandler;
  class  Channel;
  class  TransportHandler;
  class  InQueue;
  class  TaskManager;
  struct OutMessageHelper;

  //----------------------------------------------------------------------------
  //! Stream
  //----------------------------------------------------------------------------
  class Stream: public SocketHandler
  {
    public:
      enum StreamStatus
      {
        Disconnected    = 0,
        Connected       = 1,
        Connecting      = 2,
        Error           = 3
      };

      //------------------------------------------------------------------------
      //! Constructor
      //------------------------------------------------------------------------
      Stream( const URL *url, uint16_t streamNum );

      //------------------------------------------------------------------------
      //! Destructor
      //------------------------------------------------------------------------
      ~Stream();

      //------------------------------------------------------------------------
      //! Handle a socket event
      //------------------------------------------------------------------------
      virtual void Event( uint8_t  type, Socket *socket );

      //------------------------------------------------------------------------
      //! Queue the message for sending
      //------------------------------------------------------------------------
      Status QueueOut( Message              *msg,
                       MessageStatusHandler *handler,
                       uint32_t              timeout );

      //------------------------------------------------------------------------
      //! Set the transport
      //------------------------------------------------------------------------
      void SetTransport( TransportHandler *transport )
      {
        pTransport = transport;
      }

      //------------------------------------------------------------------------
      //! Set the poller
      //------------------------------------------------------------------------
      void SetPoller( Poller *poller )
      {
        pPoller = poller;
      }

      //------------------------------------------------------------------------
      //! Set the incoming queue
      //------------------------------------------------------------------------
      void SetIncomingQueue( InQueue *incomingQueue )
      {
        pIncomingQueue = incomingQueue;
      }

      //------------------------------------------------------------------------
      //! Set the channel data
      //------------------------------------------------------------------------
      void SetChannelData( AnyObject *channelData )
      {
        pChannelData = channelData;
      }

      //------------------------------------------------------------------------
      //! Set task manager
      //------------------------------------------------------------------------
      void SetTaskManager( TaskManager *taskManager )
      {
        pTaskManager = taskManager;
      }

      //------------------------------------------------------------------------
      //! Establish the connection if needed
      //------------------------------------------------------------------------
      Status CheckConnection();

      //------------------------------------------------------------------------
      //! Run the async connection process
      //------------------------------------------------------------------------
      Status Connect();

      //------------------------------------------------------------------------
      //! Disconnect the stream
      //------------------------------------------------------------------------
      void Disconnect( bool force = false );

      //------------------------------------------------------------------------
      //! Handle a clock event generated either by socket timeout, or by
      //! the task manager event
      //------------------------------------------------------------------------
      void Tick( time_t now );

    private:

      //------------------------------------------------------------------------
      // Handle the socket readiness to write in the connection stage
      //------------------------------------------------------------------------
      void ConnectingReadyToWrite();

      //------------------------------------------------------------------------
      // Handle the ReadyToWrite message in the connected stage
      //------------------------------------------------------------------------
      void ConnectedReadyToWrite();

      //------------------------------------------------------------------------
      // Write a message from an outgoing queue
      //------------------------------------------------------------------------
      Status WriteMessage( std::list<OutMessageHelper *> &queue );

      //------------------------------------------------------------------------
      // Handle the socket readiness to read in the connection stage
      //------------------------------------------------------------------------
      void ConnectingReadyToRead();

      //------------------------------------------------------------------------
      // Handle the socket readiness to read in the connected stage
      //------------------------------------------------------------------------
      void ConnectedReadyToRead();

      //------------------------------------------------------------------------
      // Get a message from a socket
      //------------------------------------------------------------------------
      Status ReadMessage();

      //------------------------------------------------------------------------
      // Handle timeouts
      //------------------------------------------------------------------------
      void HandleConnectingTimeout();
      void HandleReadTimeout();
      void HandleWriteTimeout();

      //------------------------------------------------------------------------
      // Handle stream fault
      //------------------------------------------------------------------------
      void HandleStreamFault( Status status = Status() );

      //------------------------------------------------------------------------
      // Fail outgoing handlers
      //------------------------------------------------------------------------
      void FailOutgoingHandlers( Status status );

      const URL                     *pUrl;
      uint16_t                       pStreamNum;
      TransportHandler              *pTransport;
      Socket                        *pSocket;
      Poller                        *pPoller;
      TaskManager                   *pTaskManager;
      XrdSysRecMutex                 pMutex;
      std::list<OutMessageHelper *>  pOutQueue;
      OutMessageHelper              *pCurrentOut;
      InQueue                       *pIncomingQueue;
      Message                       *pIncoming;
      StreamStatus                   pStreamStatus;
      AnyObject                     *pChannelData;
      uint16_t                       pTimeoutResolution;
      uint16_t                       pLastStreamError;
      time_t                         pErrorTime;
      uint16_t                       pStreamErrorWindow;
      time_t                         pLastActivity;

      //------------------------------------------------------------------------
      // Connect stage stuff
      //------------------------------------------------------------------------
      HandShakeData                 *pHandShakeData;
      std::list<OutMessageHelper *>  pOutQueueConnect;
      uint16_t                       pConnectionCount;
      time_t                         pConnectionInitTime;
      uint16_t                       pConnectionWindow;
      uint16_t                       pConnectionRetry;
  };
}

#endif // __XRD_CL_STREAM_HH__
