//------------------------------------------------------------------------------
// Copyright (c) 2011 by European Organization for Nuclear Research (CERN)
// Author: Lukasz Janyst <ljanyst@cern.ch>
// See the LICENCE file for details.
//------------------------------------------------------------------------------

#ifndef __XRD_CL_XROOTD_TRANSPORT_HH__
#define __XRD_CL_XROOTD_TRANSPORT_HH__

#include "XrdCl/XrdClPostMaster.hh"
#include "XProtocol/XProtocol.hh"
#include "XrdSec/XrdSecInterface.hh"
#include "XrdOuc/XrdOucEnv.hh"

namespace XrdClient
{
  //----------------------------------------------------------------------------
  //! Information holder for XRootDStreams
  //----------------------------------------------------------------------------
  struct XRootDStreamInfo
  {
    //--------------------------------------------------------------------------
    // Define the stream status for the link negotiation purposes
    //--------------------------------------------------------------------------
    enum StreamStatus
    {
      Disconnected,
      Broken,
      HandShakeSent,
      HandShakeReceived,
      LoginSent,
      AuthSent,
      Connected
    };

    //--------------------------------------------------------------------------
    // Constructor
    //--------------------------------------------------------------------------
    XRootDStreamInfo(): status( Disconnected )
    {
    }

    StreamStatus status;
  };

  //----------------------------------------------------------------------------
  //! Information holder for xrootd channels
  //----------------------------------------------------------------------------
  struct XRootDChannelInfo
  {
    //--------------------------------------------------------------------------
    // Constructor
    //--------------------------------------------------------------------------
    XRootDChannelInfo():
      serverFlags(0),
      protocolVersion(0),
      authBuffer(0),
      authProtocol(0),
      authParams(0),
      authEnv(0)
    {
      memset( sessionId, 0, 16 );
    }

    //--------------------------------------------------------------------------
    // Destructor
    //--------------------------------------------------------------------------
    ~XRootDChannelInfo()
    {
      delete [] authBuffer;
    }

    typedef std::vector<XRootDStreamInfo> StreamInfoVector;

    //--------------------------------------------------------------------------
    // Data
    //--------------------------------------------------------------------------
    uint32_t          serverFlags;
    uint32_t          protocolVersion;
    uint8_t           sessionId[16];
    char             *authBuffer;
    XrdSecProtocol   *authProtocol;
    XrdSecParameters *authParams;
    XrdOucEnv        *authEnv;
    StreamInfoVector  stream;
  };

  //----------------------------------------------------------------------------
  // XRootD transport handler
  //----------------------------------------------------------------------------
  class XRootDTransport: public TransportHandler
  {
    public:
      //------------------------------------------------------------------------
      //! Constructor
      //------------------------------------------------------------------------
      XRootDTransport();

      //------------------------------------------------------------------------
      //! Destructor
      //------------------------------------------------------------------------
      ~XRootDTransport();

      //------------------------------------------------------------------------
      //! Read a message from the socket, the socket is non blocking, so if
      //! there is not enough data the function should retutn errRetry in which
      //! case it will be called again when more data arrives with the data
      //! previousely read stored in the message buffer
      //!
      //! @param message the message
      //! @param socket  the socket
      //! @return        stOK if the message has been processed properly,
      //!                stError & errRetry when the method needs to be called
      //!                again to finish reading the message
      //!                stError on faiure
      //------------------------------------------------------------------------
      virtual Status GetMessage( Message *message, Socket *socket );

      //------------------------------------------------------------------------
      //! Initialize channel
      //------------------------------------------------------------------------
      virtual void InitializeChannel( AnyObject &channelData );

      //------------------------------------------------------------------------
      //! Finalize channel
      //------------------------------------------------------------------------
      virtual void FinalizeChannel( AnyObject &channelData );

      //------------------------------------------------------------------------
      //! HandShake
      //------------------------------------------------------------------------
      virtual Status HandShake( HandShakeData *handShakeData,
                                AnyObject     &channelData );

      //------------------------------------------------------------------------
      //! Check if the stream should be disconnected
      //------------------------------------------------------------------------
      virtual bool IsStreamTTLElapsed( time_t     time,
                                       AnyObject &channelData );

      //------------------------------------------------------------------------
      //! Return a stream number by which the message should be sent and/or
      //! alter the message to include the info by which stream the response
      //! should be sent back
      //------------------------------------------------------------------------
      virtual uint16_t Multiplex( Message *msg, AnyObject &channelData );

      //------------------------------------------------------------------------
      //! Return the information whether a control connection needs to be
      //! valid before establishing other connections
      //------------------------------------------------------------------------
      virtual bool NeedControlConnection()
      {
        return true;
      }

      //------------------------------------------------------------------------
      //! Marshal the outgoing message
      //------------------------------------------------------------------------
      static Status Marshall( Message *msg );

      //------------------------------------------------------------------------
      //! Unmarshal the body of the incomming message
      //------------------------------------------------------------------------
      static Status UnMarshallBody( Message *msg, XRequestTypes reqType );

      //------------------------------------------------------------------------
      //! Unmarshal the header incomming message
      //------------------------------------------------------------------------
      static void UnMarshallHeader( Message *msg );

      //------------------------------------------------------------------------
      //! Log server error response
      //------------------------------------------------------------------------
      static void LogErrorResponse( const Message &msg );

      //------------------------------------------------------------------------
      //! The stream has been disconnected, do the cleanups
      //------------------------------------------------------------------------
      virtual void Disconnect( AnyObject &channelData, uint16_t streamId );

    private:

      //------------------------------------------------------------------------
      // Generate the message to be sent as an initial handshake
      //------------------------------------------------------------------------
      Message *GenerateInitialHS( HandShakeData     *hsData,
                                  XRootDChannelInfo *info );

      //------------------------------------------------------------------------
      // Process the server initial handshake response
      //------------------------------------------------------------------------
      Status ProcessServerHS( HandShakeData     *hsData,
                              XRootDChannelInfo *info );

      //------------------------------------------------------------------------
      // Process the protocol response
      //------------------------------------------------------------------------
      Status ProcessProtocolResp( HandShakeData     *hsData,
                                  XRootDChannelInfo *info );

      //------------------------------------------------------------------------
      // Generate the login  message
      //------------------------------------------------------------------------
      Message *GenerateLogIn( HandShakeData     *hsData,
                              XRootDChannelInfo *info );

      //------------------------------------------------------------------------
      // Process the login response
      //------------------------------------------------------------------------
      Status ProcessLogInResp( HandShakeData     *hsData,
                               XRootDChannelInfo *info );

      //------------------------------------------------------------------------
      // Do the authentication
      //------------------------------------------------------------------------
      Status DoAuthentication( HandShakeData     *hsData,
                               XRootDChannelInfo *info );

      //------------------------------------------------------------------------
      // Get the initial credentials using one of the protocols
      //------------------------------------------------------------------------
      Status GetCredentials( XrdSecCredentials *&credentials,
                             HandShakeData      *hsData,
                             XRootDChannelInfo  *info );

      //------------------------------------------------------------------------
      // Clean up the data structures created for the authentication process
      //------------------------------------------------------------------------
      Status CleanUpAuthentication( XRootDChannelInfo *info );

      //------------------------------------------------------------------------
      // Get the authentication function handle
      //------------------------------------------------------------------------
      typedef XrdSecProtocol *(*XrdSecGetProt_t)( const char             *,
                                                  const sockaddr         &,
                                                  const XrdSecParameters &,
                                                  XrdOucErrInfo          * );

      XrdSecGetProt_t GetAuthHandler();

      //------------------------------------------------------------------------
      // Get a string representation of the server flags
      //------------------------------------------------------------------------
      static std::string ServerFlagsToStr( uint32_t flags );

      void            *pSecLibHandle;
      XrdSecGetProt_t  pAuthHandler;
  };
}

#endif // __XRD_CL_XROOTD_TRANSPORT_HANDLER_HH__
