#ifndef __WEBRTC_RTSP_CLIENT_H__
#define __WEBRTC_RTSP_CLIENT_H__

#include "StreamClientState.h"
#include "liveMedia.hh"

class WebRTCRTSPClient : public RTSPClient {
public:
    static WebRTCRTSPClient* createNew(UsageEnvironment& env, char const* rtspURL,
        int verbosityLevel = 0,
        char const* applicationName = NULL,
        portNumBits tunnelOverHTTPPortNum = 0);

protected:
    WebRTCRTSPClient(UsageEnvironment& env, char const* rtspURL,
        int verbosityLevel, char const* applicationName, portNumBits tunnelOverHTTPPortNum);
    // called only by createNew();
    virtual ~WebRTCRTSPClient();

public:
    StreamClientState scs;
};

#endif
