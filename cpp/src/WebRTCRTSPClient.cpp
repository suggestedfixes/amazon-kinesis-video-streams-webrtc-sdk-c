#include "WebRTCRTSPClient.h"

WebRTCRTSPClient* WebRTCRTSPClient::createNew(UsageEnvironment& env, char const* rtspURL,
    int verbosityLevel, char const* applicationName, portNumBits tunnelOverHTTPPortNum)
{
    return new WebRTCRTSPClient(env, rtspURL, verbosityLevel, applicationName, tunnelOverHTTPPortNum);
}

WebRTCRTSPClient::WebRTCRTSPClient(UsageEnvironment& env, char const* rtspURL,
    int verbosityLevel, char const* applicationName, portNumBits tunnelOverHTTPPortNum)
    : RTSPClient(env, rtspURL, verbosityLevel, applicationName, tunnelOverHTTPPortNum, -1)
{
}

WebRTCRTSPClient::~WebRTCRTSPClient() {
}
