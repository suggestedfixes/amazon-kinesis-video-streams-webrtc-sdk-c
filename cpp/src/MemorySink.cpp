#include "MemorySink.h"
#include "liveMedia.hh"

#define MEMORY_SINK_RECEIVE_BUFFER_SIZE (10 * 1024 * 1024)

extern PSampleConfiguration gSampleConfiguration;

MemorySink* MemorySink::createNew(UsageEnvironment& env, MediaSubsession& subsession, char const* streamId)
{
    return new MemorySink(env, subsession, streamId);
}

MemorySink::MemorySink(UsageEnvironment& env, MediaSubsession& subsession, char const* streamId)
    : MediaSink(env)
    , fSubsession(subsession)
{
    fStreamId = strDup(streamId);
    fReceiveBuffer = new u_int8_t[MEMORY_SINK_RECEIVE_BUFFER_SIZE];
    h264Buffer = new u_int8_t[MEMORY_SINK_RECEIVE_BUFFER_SIZE];
    h264Buffer[0] = 0;
    h264Buffer[1] = 0;
    h264Buffer[2] = 0;
    h264Buffer[3] = 1;

    frame.duration = 0;
    frame.version = FRAME_CURRENT_VERSION;
}

MemorySink::~MemorySink()
{
    delete[] fReceiveBuffer;
    delete[] h264Buffer;
    delete[] fStreamId;
}

void MemorySink::afterGettingFrame(void* clientData, unsigned frameSize, unsigned numTruncatedBytes,
    struct timeval presentationTime, unsigned durationInMicroseconds)
{
    MemorySink* sink = (MemorySink*)clientData;
    sink->afterGettingFrame(frameSize, numTruncatedBytes, presentationTime, durationInMicroseconds);
}

// If you don't want to see debugging output for each received frame, then comment out the following line:
//#define DEBUG_PRINT_EACH_RECEIVED_FRAME
//#define DEBUG_PRINT_NPT

void MemorySink::afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
    struct timeval presentationTime, unsigned /*durationInMicroseconds*/)
{

    STATUS status;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration)gSampleConfiguration;

    PSampleStreamingSession pSampleStreamingSession = NULL;
    PRtcRtpTransceiver pRtcRtpTransceiver = NULL;
    UINT64 time = GETTIME();
    int nal_type = 1;


    char channel;
    if (fStreamId && strlen(fStreamId) > 10) {
        channel = fStreamId[strlen(fStreamId) - 2];
    }

    if (strncmp(fSubsession.codecName(), "H264", 4) == 0 && pSampleConfiguration) {
        nal_type = fReceiveBuffer[0] & 0x1f;
        frame.flags = FRAME_FLAG_NONE; 
        MEMCPY((PBYTE)h264Buffer + 4, (PBYTE)fReceiveBuffer, frameSize);

        frame.trackId = DEFAULT_VIDEO_TRACK_ID;
        frame.frameData = h264Buffer; 
        frame.size = frameSize + 4;

        if (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->updatingSampleStreamingSessionList)) {
            ATOMIC_INCREMENT(&pSampleConfiguration->streamingSessionListReadingThreadCount);
            for (int i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
                pSampleStreamingSession = pSampleConfiguration->sampleStreamingSessionList[i];

                frame.index = (UINT32)ATOMIC_INCREMENT(&pSampleStreamingSession->frameIndex);
                BOOL isMainstream = ATOMIC_LOAD_BOOL(&pSampleStreamingSession->mainstream);

                if ((channel == '1' && isMainstream) || (channel == '2' && !isMainstream)) {
                    pRtcRtpTransceiver = pSampleStreamingSession->pVideoRtcRtpTransceiver;
                    frame.presentationTs = pSampleStreamingSession->videoTimestamp;
                    frame.decodingTs = frame.presentationTs;
                    pSampleStreamingSession->videoTimestamp = time;
                    status = writeFrame(pRtcRtpTransceiver, &frame);
                    if (STATUS_FAILED(status)) {
                        DLOGD("writeFrame failed with 0x%08x", status);
                    }
                }
            }
            ATOMIC_DECREMENT(&pSampleConfiguration->streamingSessionListReadingThreadCount);
        }
    }

// We've just received a frame of data.  (Optionally) print out information about it:
#ifdef DEBUG_PRINT_EACH_RECEIVED_FRAME
    if (fStreamId != NULL)
        envir() << "Stream \"" << fStreamId << "\"; ";
    envir() << fSubsession.mediumName() << "/" << fSubsession.codecName() << ":\tReceived " << frameSize << " bytes";
    if (numTruncatedBytes > 0)
        envir() << " (with " << numTruncatedBytes << " bytes truncated)";
    char uSecsStr[6 + 1]; // used to output the 'microseconds' part of the presentation time
    sprintf(uSecsStr, "%06u", (unsigned)presentationTime.tv_usec);
    envir() << ".\tPresentation time: " << (int)presentationTime.tv_sec << "." << uSecsStr;
    if (fSubsession.rtpSource() != NULL && !fSubsession.rtpSource()->hasBeenSynchronizedUsingRTCP()) {
        envir() << "!"; // mark the debugging output to indicate that this presentation time is not RTCP-synchronized
    }

    int column_width = 16;
    char data_buffer[500];
    printf("\n");
    for (int j = 0; j < frame.size; j++) {
        int i = j % column_width;
        data_buffer[i] = frame.frameData[j]; 

        if (i == column_width - 1) {
            for (int k = 0; k < column_width; k++) {
                printf("%2x ", data_buffer[k]); 
            }
            printf(" ");
            for (int k = 0; k < column_width; k++) {
                if (data_buffer[k] >= 0x20) {
                    printf("%c", data_buffer[k]); 
                } else {
                    printf(".");
                }
            }
            printf("\n");
        }
        int u = j / column_width;
        if (u > 2) {
            break;
        }
    }
#ifdef DEBUG_PRINT_NPT
    envir() << "\tNPT: " << fSubsession.getNormalPlayTime(presentationTime);
#endif
    envir() << "\n";
#endif

    // Then continue, to request the next frame of data:
    continuePlaying();
}

Boolean MemorySink::continuePlaying()
{
    if (fSource == NULL)
        return False; // sanity check (should not happen)

    // Request the next frame of data from our input source.  "afterGettingFrame()" will get called later, when it arrives:
    fSource->getNextFrame(fReceiveBuffer, MEMORY_SINK_RECEIVE_BUFFER_SIZE,
        afterGettingFrame, this,
        onSourceClosure, this);
    return True;
}
