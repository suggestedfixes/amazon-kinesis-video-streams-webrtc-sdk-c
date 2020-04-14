#include "webrtc.h"

#define APP_WEBRTC_CHECK_PERIOD   60 
#define APP_WEBRTC_ANSWER_TIMEOUT 40 

BOOL checkWebrtcStatus(int argc, char** argv)
{
    STATUS retStatus = STATUS_SUCCESS;
    RtcSessionDescriptionInit offerSessionDescriptionInit;
    UINT32 buffLen = 0;
    SignalingMessage message;
    PSampleConfiguration pSampleConfiguration = NULL;
    PSampleStreamingSession pSampleStreamingSession = NULL;
    BOOL locked = FALSE;
    BOOL alive = FALSE;

    signal(SIGINT, sigintHandler);

    // do trickle-ice by default
    printf("[KVS Viewer] Using trickleICE by default\n");

    retStatus = createSampleConfiguration(argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME,
        SIGNALING_CHANNEL_ROLE_TYPE_VIEWER,
        APP_TRICKLE_ICE,
        FALSE,
        &pSampleConfiguration);
    if (retStatus != STATUS_SUCCESS) {
        printf("[KVS Viewer] createSampleConfiguration(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }
    printf("[KVS Viewer] Created signaling channel %s\n", (argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME));

    // Initialize KVS WebRTC. This must be done before anything else, and must only be done once.
    retStatus = initKvsWebRtc();
    if (retStatus != STATUS_SUCCESS) {
        printf("[KVS Viewer] initKvsWebRtc(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }

    printf("[KVS Viewer] KVS WebRTC initialization completed successfully\n");

    pSampleConfiguration->signalingClientCallbacks.messageReceivedFn = viewerMessageReceived;

    strcpy(pSampleConfiguration->clientInfo.clientId, SAMPLE_VIEWER_CLIENT_ID);

    retStatus = createSignalingClientSync(&pSampleConfiguration->clientInfo, &pSampleConfiguration->channelInfo,
        &pSampleConfiguration->signalingClientCallbacks, pSampleConfiguration->pCredentialProvider,
        &pSampleConfiguration->signalingClientHandle);
    if (retStatus != STATUS_SUCCESS) {
        printf("[KVS Viewer] createSignalingClientSync(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }

    printf("[KVS Viewer] Signaling client created successfully\n");

    // Initialize streaming session
    MUTEX_LOCK(pSampleConfiguration->sampleConfigurationObjLock);
    locked = TRUE;

    retStatus = createSampleStreamingSession(pSampleConfiguration, NULL, FALSE, &pSampleStreamingSession);
    if (retStatus != STATUS_SUCCESS) {
        printf("[KVS Viewer] createSampleStreamingSession(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }

    printf("[KVS Viewer] Creating streaming session...completed\n");
    pSampleConfiguration->sampleStreamingSessionList[pSampleConfiguration->streamingSessionCount++] = pSampleStreamingSession;

    MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    locked = FALSE;

    // Enable the processing of the messages
    retStatus = signalingClientConnectSync(pSampleConfiguration->signalingClientHandle);
    if (retStatus != STATUS_SUCCESS) {
        printf("[KVS Viewer] signalingClientConnectSync(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }

    printf("[KVS Viewer] Signaling client connection to socket established\n");

    memset(&offerSessionDescriptionInit, 0x00, SIZEOF(RtcSessionDescriptionInit));

    retStatus = createOffer(pSampleStreamingSession->pPeerConnection, &offerSessionDescriptionInit);
    if (retStatus != STATUS_SUCCESS) {
        printf("[KVS Viewer] createOffer(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }
    printf("[KVS Viewer] Offer creation successful\n");

    retStatus = setLocalDescription(pSampleStreamingSession->pPeerConnection, &offerSessionDescriptionInit);
    if (retStatus != STATUS_SUCCESS) {
        printf("[KVS Viewer] setLocalDescription(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }
    printf("[KVS Viewer] Completed setting local description\n");

    retStatus = transceiverOnFrame(pSampleStreamingSession->pVideoRtcRtpTransceiver,
                                   (UINT64) pSampleStreamingSession,
                                    sampleFrameHandler);
    if (retStatus != STATUS_SUCCESS) {
        printf("[KVS Viewer] transceiverOnFrame(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }

    printf("[KVS Viewer] Generating JSON of session description....");
    retStatus = serializeSessionDescriptionInit(&offerSessionDescriptionInit, NULL, &buffLen);
    if (retStatus != STATUS_SUCCESS) {
        printf("[KVS Viewer] serializeSessionDescriptionInit(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }
    if (buffLen >= SIZEOF(message.payload)) {
        printf("[KVS Viewer] serializeSessionDescriptionInit(): operation returned status code: 0x%08x \n", STATUS_INVALID_OPERATION);
        retStatus = STATUS_INVALID_OPERATION;
        goto CleanUp;
    }

    retStatus = serializeSessionDescriptionInit(&offerSessionDescriptionInit, message.payload, &buffLen);
    if (retStatus != STATUS_SUCCESS) {
        printf("[KVS Viewer] serializeSessionDescriptionInit(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }

    message.version = SIGNALING_MESSAGE_CURRENT_VERSION;
    message.messageType = SIGNALING_MESSAGE_TYPE_OFFER;
    STRCPY(message.peerClientId, SAMPLE_MASTER_CLIENT_ID);
    message.payloadLen = (buffLen / SIZEOF(CHAR)) - 1;
    message.correlationId[0] = '\0';

    retStatus = signalingClientSendMessageSync(pSampleConfiguration->signalingClientHandle, &message);
    if (retStatus != STATUS_SUCCESS) {
        printf("[KVS Viewer] signalingClientSendMessageSync(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }

    MUTEX_LOCK(pSampleConfiguration->videoFrameLock);
    CVAR_WAIT(pSampleConfiguration->signalVideoFrame, pSampleConfiguration->videoFrameLock, APP_WEBRTC_ANSWER_TIMEOUT * HUNDREDS_OF_NANOS_IN_A_SECOND);

    if (ATOMIC_LOAD_BOOL(&pSampleConfiguration->videoFrameReceived)) {
        DLOGD("Monitor received a video frame in time.\n");
        retStatus = STATUS_SUCCESS;
        alive = TRUE;
    } else {
        DLOGD("Monitor did not receive a video frame in time.\n");
        retStatus = STATUS_OPERATION_TIMED_OUT;
        alive = FALSE;
    }
    MUTEX_UNLOCK(pSampleConfiguration->videoFrameLock);
CleanUp:

    if (retStatus != STATUS_SUCCESS) {
        printf("[KVS Viewer] Terminated with status code 0x%08x", retStatus);
    }

    printf("[KVS Viewer] Cleaning up....\n");

    if (locked) {
        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    }

    if (pSampleConfiguration != NULL) {
        retStatus = freeSignalingClient(&pSampleConfiguration->signalingClientHandle);
        if (retStatus != STATUS_SUCCESS) {
            printf("[KVS Viewer] freeSignalingClient(): operation returned status code: 0x%08x \n", retStatus);
        }

        retStatus = freeSampleConfiguration(&pSampleConfiguration);
        if (retStatus != STATUS_SUCCESS) {
            printf("[KVS Viewer] freeSampleConfiguration(): operation returned status code: 0x%08x \n", retStatus);
        }
    }
    return alive;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        printf("Usage: program channel cmd\n");
        return 0;
    }
    system(argv[2]);
    for (;;) {
        THREAD_SLEEP(APP_WEBRTC_CHECK_PERIOD * HUNDREDS_OF_NANOS_IN_A_SECOND);
        if (!checkWebrtcStatus(argc, argv)) {
            system(argv[2]);
        }
    }
    return 0;
}
