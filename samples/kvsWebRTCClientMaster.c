#include "Samples.h"

extern PSampleConfiguration gSampleConfiguration;

// #define VERBOSE
PCHAR data_path;
CHAR filePath[MAX_PATH_LEN + 1];
Frame frames[100];

INT32 main(INT32 argc, CHAR *argv[])
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 frameSize;
    PSampleConfiguration pSampleConfiguration = NULL;

    signal(SIGINT, sigintHandler);

    // do tricketIce by default
    printf("[KVS Master] Using trickleICE by default\n");

    data_path = GETENV("APP_DATA_PATH");
    int i = 0;

    for (i = 0; i < 100; i++) {
        snprintf(filePath, MAX_PATH_LEN, "%s/%d.h264", data_path, i);
        readFrameFromDisk(NULL, &(frames[i].size), filePath);
        frames[i].frameData = malloc(frames[i].size);
        readFrameFromDisk(frames[i].frameData, &frames[i].size, filePath);
    }

    retStatus = createSampleConfiguration(argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME,
                                          SIGNALING_CHANNEL_ROLE_TYPE_MASTER,
                                          TRUE,
                                          TRUE,
                                          &pSampleConfiguration);
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS Master] createSampleConfiguration(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }

    printf("[KVS Master] Created signaling channel %s\n", (argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME));

    // Set the audio and video handlers
    //pSampleConfiguration->audioSource = sendAudioPackets;
    pSampleConfiguration->videoSource = sendVideoPackets;
//    pSampleConfiguration->receiveAudioVideoSource = sampleReceiveAudioFrame;
    pSampleConfiguration->onDataChannel = onDataChannel;
    pSampleConfiguration->mediaType = SAMPLE_STREAMING_AUDIO_VIDEO;
    printf("[KVS Master] Finished setting audio and video handlers\n");

    // Check if the samples are present

    /*
    retStatus = readFrameFromDisk(NULL, &frameSize, "./data/0.h264");
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS Master] readFrameFromDisk(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }
    */
    printf("[KVS Master] Checked sample video frame availability....available\n");

    /*
    retStatus = readFrameFromDisk(NULL, &frameSize, "./opusSampleFrames/sample-001.opus");
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS Master] readFrameFromDisk(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }
    printf("[KVS Master] Checked sample audio frame availability....available\n");
    */

    // Initialize KVS WebRTC. This must be done before anything else, and must only be done once.
    retStatus = initKvsWebRtc();
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS Master] initKvsWebRtc(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }
    printf("[KVS Master] KVS WebRTC initialization completed successfully\n");

    pSampleConfiguration->signalingClientCallbacks.messageReceivedFn = masterMessageReceived;

    strcpy(pSampleConfiguration->clientInfo.clientId, SAMPLE_MASTER_CLIENT_ID);

    retStatus = createSignalingClientSync(&pSampleConfiguration->clientInfo, &pSampleConfiguration->channelInfo,
                                          &pSampleConfiguration->signalingClientCallbacks, pSampleConfiguration->pCredentialProvider,
                                          &pSampleConfiguration->signalingClientHandle);
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS Master] createSignalingClientSync(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;

    }
    printf("[KVS Master] Signaling client created successfully\n");

    // Enable the processing of the messages
    retStatus = signalingClientConnectSync(pSampleConfiguration->signalingClientHandle);
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS Master] signalingClientConnectSync(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }
    printf("[KVS Master] Signaling client connection to socket established\n");

    gSampleConfiguration = pSampleConfiguration;

    printf("[KVS Master] Beginning audio-video streaming...check the stream over channel %s\n",
            (argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME));

    // Checking for termination
    retStatus = sessionCleanupWait(pSampleConfiguration);
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS Master] sessionCleanupWait(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }

    printf("[KVS Master] Streaming session terminated\n");

CleanUp:

    if (retStatus != STATUS_SUCCESS) {
        printf("[KVS Master] Terminated with status code 0x%08x", retStatus);
    }

    printf("[KVS Master] Cleaning up....\n");

    if (pSampleConfiguration != NULL) {
        // Kick of the termination sequence
        ATOMIC_STORE_BOOL(&pSampleConfiguration->appTerminateFlag, TRUE);

        // Join the threads
        if (pSampleConfiguration->videoSenderTid != (UINT64) NULL) {
           // Join the threads
           THREAD_JOIN(pSampleConfiguration->videoSenderTid, NULL);
        }

        if (pSampleConfiguration->audioSenderTid != (UINT64) NULL) {
            // Join the threads
            THREAD_JOIN(pSampleConfiguration->audioSenderTid, NULL);
        }

        retStatus = freeSignalingClient(&pSampleConfiguration->signalingClientHandle);
        if(retStatus != STATUS_SUCCESS) {
            printf("[KVS Master] freeSignalingClient(): operation returned status code: 0x%08x", retStatus);
        }

        retStatus = freeSampleConfiguration(&pSampleConfiguration);
        if(retStatus != STATUS_SUCCESS) {
            printf("[KVS Master] freeSampleConfiguration(): operation returned status code: 0x%08x", retStatus);
        }
    }
    printf("[KVS Master] Cleanup done\n");
    return (INT32) retStatus;
}

STATUS readFrameFromDisk(PBYTE pFrame, PUINT32 pSize, PCHAR frameFilePath)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT64 size = 0;

    if(pSize == NULL) {
       printf("[KVS Master] readFrameFromDisk(): operation returned status code: 0x%08x \n", STATUS_NULL_ARG);
       goto CleanUp;
    }

    size = *pSize;

    // Get the size and read into frame
    retStatus = readFile(frameFilePath, TRUE, pFrame, &size);
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS Master] readFile(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }

CleanUp:

    if (pSize != NULL) {
        *pSize = (UINT32) size;
    }

    return retStatus;
}

PVOID sendVideoPackets(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;
    Frame frame;
    UINT32 fileIndex = 0, frameSize;
    STATUS status;
    UINT32 i;

    if(pSampleConfiguration == NULL) {
        printf("[KVS Master] sendVideoPackets(): operation returned status code: 0x%08x \n", STATUS_NULL_ARG);
        goto CleanUp;
    }

    frame.presentationTs = 0; 

    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        frame.frameData = frames[fileIndex % 100].frameData;
        frame.size = frames[fileIndex % 100].size;
        frame.presentationTs = GETTIME(); 
        fileIndex++;
        
        /*
        // Re-alloc if needed
        if (frameSize > pSampleConfiguration->videoBufferSize) {
            pSampleConfiguration->pVideoFrameBuffer = (PBYTE) realloc(pSampleConfiguration->pVideoFrameBuffer, frameSize);
            if(pSampleConfiguration->pVideoFrameBuffer == NULL)
            {
                printf("[KVS Master] Video frame Buffer reallocation failed...%s (code %d)\n", strerror(errno), errno);
                printf("[KVS Master] realloc(): operation returned status code: 0x%08x \n", STATUS_NOT_ENOUGH_MEMORY);
                goto CleanUp;
            }

            pSampleConfiguration->videoBufferSize = frameSize;
        }

        memcpy(pSampleConfiguration->pVideoFrameBuffer, &(frames[i].frameData), frameSize);
        */

        if (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->updatingSampleStreamingSessionList)) {
            ATOMIC_INCREMENT(&pSampleConfiguration->streamingSessionListReadingThreadCount);
            for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
                status = writeFrame(pSampleConfiguration->sampleStreamingSessionList[i]->pVideoRtcRtpTransceiver, &frame);
                if (status != STATUS_SUCCESS) {
                    #ifdef VERBOSE
                        printf("writeFrame() failed with 0x%08x", status);
                    #endif
                }
            }
            ATOMIC_DECREMENT(&pSampleConfiguration->streamingSessionListReadingThreadCount);
        }
        THREAD_SLEEP(SAMPLE_VIDEO_FRAME_DURATION);
    }

CleanUp:

    return (PVOID) (ULONG_PTR) retStatus;
}

PVOID sendAudioPackets(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;
    Frame frame;
    UINT32 fileIndex = 0, frameSize;
    CHAR filePath[MAX_PATH_LEN + 1];
    UINT32 i;
    STATUS status;

    if(pSampleConfiguration == NULL) {
        printf("[KVS Master] sendAudioPackets(): operation returned status code: 0x%08x \n", STATUS_NULL_ARG);
        goto CleanUp;
    }

    frame.presentationTs = 0;

    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        fileIndex = fileIndex % NUMBER_OF_OPUS_FRAME_FILES + 1;
//        snprintf(filePath, MAX_PATH_LEN, "./opusSampleFrames/sample-%03d.opus", fileIndex);

        retStatus = readFrameFromDisk(NULL, &frameSize, filePath);
        if(retStatus != STATUS_SUCCESS) {
            printf("[KVS Master] readFrameFromDisk(): operation returned status code: 0x%08x \n", retStatus);
            goto CleanUp;
        }

        // Re-alloc if needed
        if (frameSize > pSampleConfiguration->audioBufferSize) {
            pSampleConfiguration->pAudioFrameBuffer = (uint8_t*) realloc(pSampleConfiguration->pAudioFrameBuffer, frameSize);
            if(pSampleConfiguration->pAudioFrameBuffer == NULL) {
                printf("[KVS Master] Audio frame Buffer reallocation failed...%s (code %d)\n", strerror(errno), errno);
                printf("[KVS Master] realloc(): operation returned status code: 0x%08x \n", STATUS_NOT_ENOUGH_MEMORY);
                goto CleanUp;
            }
        }

        frame.frameData = pSampleConfiguration->pAudioFrameBuffer;
        frame.size = frameSize;

        retStatus = readFrameFromDisk(frame.frameData, &frameSize, filePath);
        if(retStatus != STATUS_SUCCESS) {
            printf("[KVS Master] readFrameFromDisk(): operation returned status code: 0x%08x \n", retStatus);
            goto CleanUp;
        }

        frame.presentationTs += SAMPLE_AUDIO_FRAME_DURATION;

        if (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->updatingSampleStreamingSessionList)) {
            ATOMIC_INCREMENT(&pSampleConfiguration->streamingSessionListReadingThreadCount);
            for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
                status = writeFrame(pSampleConfiguration->sampleStreamingSessionList[i]->pAudioRtcRtpTransceiver, &frame);
                if (status != STATUS_SUCCESS) {
                    printf("writeFrame failed with 0x%08x", status);
                }
            }
            ATOMIC_DECREMENT(&pSampleConfiguration->streamingSessionListReadingThreadCount);
        }
        THREAD_SLEEP(SAMPLE_AUDIO_FRAME_DURATION);
    }

CleanUp:

    return (PVOID) (ULONG_PTR) retStatus;
}

PVOID sampleReceiveAudioFrame(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleStreamingSession pSampleStreamingSession = (PSampleStreamingSession) args;
    if(pSampleStreamingSession == NULL) {
        printf("[KVS Master] sampleReceiveAudioFrame(): operation returned status code: 0x%08x \n", STATUS_NULL_ARG);
        goto CleanUp;
    }

    retStatus = transceiverOnFrame(pSampleStreamingSession->pAudioRtcRtpTransceiver,
                                   (UINT64) pSampleStreamingSession,
                                   sampleFrameHandler);
    if(retStatus != STATUS_SUCCESS)
    {
        printf("[KVS Master] transceiverOnFrame(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }

CleanUp:

    return (PVOID) (ULONG_PTR) retStatus;
}
