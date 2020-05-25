#ifndef __WEBRTC_STREAM_CLIENT_STATE_H__
#define __WEBRTC_STREAM_CLIENT_STATE_H__

#include "MediaSession.hh"

class StreamClientState {
public:
    StreamClientState();
    virtual ~StreamClientState();

public:
    MediaSubsessionIterator* iter;
    MediaSession* session;
    MediaSubsession* subsession;
    TaskToken streamTimerTask;
    double duration;
};

#endif
