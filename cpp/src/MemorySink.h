#ifndef __WEBRTC_MEMORY_SINK_H__
#define __WEBRTC_MEMORY_SINK_H__

#include "MediaSink.hh"
#include "MediaSession.hh"
#include "webrtc.h"

class MemorySink : public MediaSink {
public:
    static MemorySink* createNew(UsageEnvironment& env,
        MediaSubsession& subsession, // identifies the kind of data that's being received
        char const* streamId = NULL); // identifies the stream itself (optional)

private:
    MemorySink(UsageEnvironment& env, MediaSubsession& subsession, char const* streamId);
    // called only by "createNew()"
    virtual ~MemorySink();

    static void afterGettingFrame(void* clientData, unsigned frameSize,
        unsigned numTruncatedBytes,
        struct timeval presentationTime,
        unsigned durationInMicroseconds);
    void afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
        struct timeval presentationTime, unsigned durationInMicroseconds);

private:
    // redefined virtual functions:
    virtual Boolean continuePlaying();

private:
    u_int8_t* fReceiveBuffer;
    u_int8_t* h264Buffer;
    MediaSubsession& fSubsession;
    char* fStreamId;
    Frame frame;
};

#endif
