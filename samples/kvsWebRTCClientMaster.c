#include "Samples.h"

extern PSampleConfiguration gSampleConfiguration;

int32_t main(int32_t argc, char *argv[])
{
    STATUS retStatus = STATUS_SUCCESS;
    SignalingClientCallbacks signalingClientCallbacks;
    SignalingClientInfo clientInfo;
    uint32_t frameSize;
    PSampleConfiguration pSampleConfiguration = NULL;

    signal(SIGINT, sigintHandler);

    // do tricketIce by default
    printf("[KVS Master] Using trickleICE by default\n");

    retStatus = createSampleConfiguration(argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME,
                                          SIGNALING_CHANNEL_ROLE_TYPE_MASTER,
                                          TRUE,
                                          TRUE,
                                          &pSampleConfiguration);
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS Master] Sample configuration creation failed\n");
        goto CleanUp;
    }

    printf("[KVS Master] Created signaling channel %s\n", (argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME));

    // Set the audio and video handlers
    pSampleConfiguration->audioSource = sendAudioPackets;
    pSampleConfiguration->videoSource = sendVideoPackets;
    pSampleConfiguration->receiveAudioVideoSource = sampleReceiveAudioFrame;
    pSampleConfiguration->onDataChannel = onDataChannel;
    pSampleConfiguration->mediaType = SAMPLE_STREAMING_AUDIO_VIDEO;
    printf("[KVS Master] Finished setting audio and video handlers\n");

    // Check if the samples are present

    retStatus = readFrameFromDisk(NULL, &frameSize, "./h264SampleFrames/frame-001.h264");
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS Master] Video sample read failed\n");
        goto CleanUp;
    }
    printf("[KVS Master] Checked sample video frame availability....available\n");

    retStatus = readFrameFromDisk(NULL, &frameSize, "./opusSampleFrames/sample-001.opus");
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS Master] Audio sample read failed\n");
        goto CleanUp;
    }
    printf("[KVS Master] Checked sample audio frame availability....available\n");

    retStatus = initKvsWebRtc();
    // Initialize KVS WebRTC. This must be done before anything else, and must only be done once.
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS Master] WebRTC init failed\n");
        goto CleanUp;
    }
    printf("[KVS Master] KVS WebRTC initialization completed successfully\n");

    signalingClientCallbacks.version = SIGNALING_CLIENT_CALLBACKS_CURRENT_VERSION;
    signalingClientCallbacks.messageReceivedFn = masterMessageReceived;
    signalingClientCallbacks.errorReportFn = NULL;
    signalingClientCallbacks.stateChangeFn = signalingClientStateChanged;
    signalingClientCallbacks.customData = (unsigned long long) pSampleConfiguration;

    clientInfo.version = SIGNALING_CLIENT_INFO_CURRENT_VERSION;
    strcpy(clientInfo.clientId, SAMPLE_MASTER_CLIENT_ID);

    retStatus = createSignalingClientSync(&clientInfo, &pSampleConfiguration->channelInfo,
                                          &signalingClientCallbacks, pSampleConfiguration->pCredentialProvider,
                                          &pSampleConfiguration->signalingClientHandle);
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS Master] Signaling client Sync creation failed\n");
        goto CleanUp;

    }
    printf("[KVS Master] Signaling client created successfully\n");

    // Enable the processing of the messages
    retStatus = signalingClientConnectSync(pSampleConfiguration->signalingClientHandle);
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS Master] Signaling client connection to socket failed\n");
        goto CleanUp;
    }
    printf("[KVS Master] Signaling client connection to socket established\n");

    gSampleConfiguration = pSampleConfiguration;

    printf("[KVS Master] Beginning audio-video streaming...check the stream over channel %s\n",
            (argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME));

    // Checking for termination
    retStatus = sessionCleanupWait(pSampleConfiguration);
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS Master] Streaming session termination failed\n");
        goto CleanUp;
    }

    printf("[KVS Master] Streaming session terminated\n");

CleanUp:
    printf("[KVS Master] Cleaning up....\n");
    if(retStatus != STATUS_SUCCESS) {
        DLOGE("operation returned status code: 0x%08x", retStatus);
    }

    if (pSampleConfiguration != NULL) {
        // Kick of the termination sequence
        ATOMIC_STORE_BOOL(&pSampleConfiguration->appTerminateFlag, TRUE);

        // Join the threads
        if (pSampleConfiguration->videoSenderTid != (uint64_t) NULL) {
            if(pthread_join((pthread_t) pSampleConfiguration->videoSenderTid, NULL) != 0) {
                printf("Video Sender thread failed to join...%s (code %d)\n", strerror(errno), errno);
            }
        }

        if (pSampleConfiguration->audioSenderTid != (uint64_t) NULL) {
            if(pthread_join((pthread_t) pSampleConfiguration->audioSenderTid, NULL) != 0) {
                printf("Audio Sender thread failed to join...%s (code %d)\n", strerror(errno), errno);
            }
        }

        retStatus = freeSignalingClient(&pSampleConfiguration->signalingClientHandle);
        if(retStatus != STATUS_SUCCESS) {
            DLOGE("operation returned status code: 0x%08x", retStatus);
        }

        retStatus = freeSampleConfiguration(&pSampleConfiguration);
        if(retStatus != STATUS_SUCCESS) {
            DLOGE("operation returned status code: 0x%08x", retStatus);
        }
    }
    printf("[KVS Master] Cleanup done\n");
    return (int32_t) retStatus;
}

STATUS readFrameFromDisk(uint8_t* pFrame, uint32_t* pSize, char* frameFilePath)
{
    STATUS retStatus = STATUS_SUCCESS;
    unsigned long long size = 0;

    if(pSize == NULL) {
       retStatus = STATUS_NULL_ARG;
       goto CleanUp;
    }

    size = *pSize;

    // Get the size and read into frame
    retStatus = readFile(frameFilePath, TRUE, pFrame, &size);
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS Master] Frame read failed\n");
        goto CleanUp;
    }

CleanUp:

    if (pSize != NULL) {
        *pSize = (uint32_t) size;
    }

    return retStatus;
}

void* sendVideoPackets(void* args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;
    Frame frame;
    uint32_t fileIndex = 0, frameSize;
    char filePath[MAX_PATH_LEN + 1];
    STATUS status;
    uint32_t i;

    if(pSampleConfiguration == NULL) {
        retStatus = STATUS_NULL_ARG;
        goto CleanUp;
    }

    frame.presentationTs = 0;

    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        fileIndex = fileIndex % NUMBER_OF_H264_FRAME_FILES + 1;
        snprintf(filePath, MAX_PATH_LEN, "./h264SampleFrames/frame-%03d.h264", fileIndex);

        retStatus = readFrameFromDisk(NULL, &frameSize, filePath);
        if(retStatus != STATUS_SUCCESS) {
            printf("[KVS Master] Video frame read failed\n");
            goto CleanUp;
        }

        // Re-alloc if needed
        if (frameSize > pSampleConfiguration->videoBufferSize) {
            pSampleConfiguration->pVideoFrameBuffer = (uint8_t*) MEMREALLOC(pSampleConfiguration->pVideoFrameBuffer, frameSize);
            if(pSampleConfiguration->pVideoFrameBuffer == NULL)
            {
                printf("[KVS Master] Video frame Buffer reallocation failed\n");
                retStatus = STATUS_NOT_ENOUGH_MEMORY;
                goto CleanUp;
            }

            pSampleConfiguration->videoBufferSize = frameSize;
        }

        frame.frameData = pSampleConfiguration->pVideoFrameBuffer;
        frame.size = frameSize;

        retStatus = readFrameFromDisk(frame.frameData, &frameSize, filePath);
        if(retStatus != STATUS_SUCCESS) {
            printf("[KVS Master] Video frame read failed\n");
            goto CleanUp;
        }

        frame.presentationTs += SAMPLE_VIDEO_FRAME_DURATION;

        if (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->updatingSampleStreamingSessionList)) {
            ATOMIC_INCREMENT(&pSampleConfiguration->streamingSessionListReadingThreadCount);
            for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
                status = writeFrame(pSampleConfiguration->sampleStreamingSessionList[i]->pVideoRtcRtpTransceiver, &frame);
                if (status != STATUS_SUCCESS) {
                    DLOGD("writeFrame failed with 0x%08x", status);
                }
            }
            ATOMIC_DECREMENT(&pSampleConfiguration->streamingSessionListReadingThreadCount);
        }
        usleep(SAMPLE_VIDEO_FRAME_DURATION / HUNDREDS_OF_NANOS_IN_A_MICROSECOND);
    }

CleanUp:

    if(retStatus != STATUS_SUCCESS) {
        DLOGE("operation returned status code: 0x%08x", retStatus);
    }
    return (void*) (unsigned long long) retStatus;
}

void* sendAudioPackets(void* args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;
    Frame frame;
    uint32_t fileIndex = 0, frameSize;
    char filePath[MAX_PATH_LEN + 1];
    uint32_t i;
    STATUS status;

    if(pSampleConfiguration == NULL)
    {
    	retStatus = STATUS_NULL_ARG;
    	goto CleanUp;
    }

    frame.presentationTs = 0;

    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        fileIndex = fileIndex % NUMBER_OF_OPUS_FRAME_FILES + 1;
        snprintf(filePath, MAX_PATH_LEN, "./opusSampleFrames/sample-%03d.opus", fileIndex);

        retStatus = readFrameFromDisk(NULL, &frameSize, filePath);
        if(retStatus != STATUS_SUCCESS) {
            printf("[KVS Master] Video frame read failed\n");
            goto CleanUp;
        }

        // Re-alloc if needed
        if (frameSize > pSampleConfiguration->audioBufferSize) {
            pSampleConfiguration->pAudioFrameBuffer = (uint8_t*) realloc(pSampleConfiguration->pAudioFrameBuffer, frameSize);
            if(pSampleConfiguration->pAudioFrameBuffer == NULL) {
                printf("[KVS Master] Audio frame Buffer reallocation failed\n");
                retStatus = STATUS_NOT_ENOUGH_MEMORY;
                goto CleanUp;
            }
        }

        frame.frameData = pSampleConfiguration->pAudioFrameBuffer;
        frame.size = frameSize;

        retStatus = readFrameFromDisk(frame.frameData, &frameSize, filePath);
        if(retStatus != STATUS_SUCCESS) {
            printf("[KVS Master] Video frame read failed\n");
            goto CleanUp;
        }

        frame.presentationTs += SAMPLE_AUDIO_FRAME_DURATION;

        if (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->updatingSampleStreamingSessionList)) {
            ATOMIC_INCREMENT(&pSampleConfiguration->streamingSessionListReadingThreadCount);
            for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
                status = writeFrame(pSampleConfiguration->sampleStreamingSessionList[i]->pAudioRtcRtpTransceiver, &frame);
                if (status != STATUS_SUCCESS) {
                    DLOGD("writeFrame failed with 0x%08x", status);
                }
            }
            ATOMIC_DECREMENT(&pSampleConfiguration->streamingSessionListReadingThreadCount);
        }
        usleep(SAMPLE_AUDIO_FRAME_DURATION / HUNDREDS_OF_NANOS_IN_A_MICROSECOND);
    }

CleanUp:

    if(retStatus != STATUS_SUCCESS) {
        DLOGE("operation returned status code: 0x%08x", retStatus);
    }
    return (void*) (unsigned long long) retStatus;
}

void* sampleReceiveAudioFrame(void* args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleStreamingSession pSampleStreamingSession = (PSampleStreamingSession) args;
    if(pSampleStreamingSession == NULL) {
           retStatus = STATUS_NULL_ARG;
           goto CleanUp;
    }

    retStatus = transceiverOnFrame(pSampleStreamingSession->pAudioRtcRtpTransceiver,
            (unsigned long long) pSampleStreamingSession,
            sampleFrameHandler);
    if(retStatus != STATUS_SUCCESS)
    {
        goto CleanUp;
    }

CleanUp:

    if(retStatus != STATUS_SUCCESS) {
        DLOGE("operation returned status code: 0x%08x", retStatus);
    }
    return (void*) (unsigned long long) retStatus;
}
