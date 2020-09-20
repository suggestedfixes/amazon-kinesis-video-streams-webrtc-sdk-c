#define LOG_CLASS "WebRTC"
#include "webrtc.h"

PSampleConfiguration gSampleConfiguration = NULL;

VOID sigintHandler(INT32 sigNum)
{
    UNUSED_PARAM(sigNum);
    if (gSampleConfiguration != NULL) {
        ATOMIC_STORE_BOOL(&gSampleConfiguration->interrupted, TRUE);
        CVAR_BROADCAST(gSampleConfiguration->cvar);
    }
    fflush(stdout);
    fflush(stderr);
    fclose(stdout);
    fclose(stderr);
    exit(0);
}

void setMainstream(PSampleStreamingSession pSession, BOOL mainstream)
{
    MUTEX_LOCK(pSession->sessionLock);
    ATOMIC_STORE_BOOL(&pSession->pendingStream, mainstream);
    MUTEX_UNLOCK(pSession->sessionLock);
}

VOID onDataChannelMessage(UINT64 customData, PRtcDataChannel pDataChannel, BOOL isBinary, PBYTE pMessage, UINT32 pMessageLen)
{
    PSampleStreamingSession pSession = (PSampleStreamingSession) customData;
    if (isBinary) {
        DLOGD("DataChannel Binary Message");
    } else {
        DLOGD(">>>>>>>>>>>>>>>>>DataChannel String Message: %.*s\n", pMessageLen, pMessage);
    }
    if (strncmp(pMessage, "mainstream", 10) == 0) {
        setMainstream(pSession, TRUE);
    }
    if (strncmp(pMessage, "substream", 9) == 0) {
        setMainstream(pSession, FALSE);
    }
    if (strncmp(pMessage, "close", 5) == 0) {
        ATOMIC_STORE_BOOL(&pSession->terminateFlag, TRUE);
        CVAR_BROADCAST(pSession->pSampleConfiguration->cvar);
    }
}

VOID onDataChannel(UINT64 customData, PRtcDataChannel pRtcDataChannel)
{
    DLOGD("New DataChannel has been opened %s \n", pRtcDataChannel->name);
    dataChannelOnMessage(pRtcDataChannel, customData, onDataChannelMessage);
    PSampleStreamingSession pSession = (PSampleStreamingSession) customData;
    pSession->pDataChannel = pRtcDataChannel;
}

VOID onConnectionStateChange(UINT64 customData, RTC_PEER_CONNECTION_STATE newState)
{
    PSampleStreamingSession pSampleStreamingSession = (PSampleStreamingSession) customData;
    STATUS retStatus = STATUS_SUCCESS;
    DLOGI("New connection state %u", newState);

    if (newState == RTC_PEER_CONNECTION_STATE_FAILED || newState == RTC_PEER_CONNECTION_STATE_CLOSED ||
        newState == RTC_PEER_CONNECTION_STATE_DISCONNECTED) {
        ATOMIC_STORE_BOOL(&pSampleStreamingSession->terminateFlag, TRUE);
        CVAR_BROADCAST(pSampleStreamingSession->pSampleConfiguration->cvar);
    } else if (newState == RTC_PEER_CONNECTION_STATE_CONNECTED) {
        if (STATUS_FAILED(retStatus = logSelectedIceCandidatesInformation(pSampleStreamingSession))) {
            DLOGW("Failed to get information about selected Ice candidates: 0x%08x", retStatus);
        }
    }
}

STATUS viewerMessageReceived(UINT64 customData, PReceivedSignalingMessage pReceivedSignalingMessage)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) customData;
    PSampleStreamingSession pSampleStreamingSession = NULL;
    BOOL locked = FALSE;

    CHK(pSampleConfiguration != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pSampleConfiguration->sampleConfigurationObjLock);
    locked = TRUE;

    // viewer should only be viewing a single master. So there should only be one streaming session if running viewer sample
    CHK_ERR(pSampleConfiguration->streamingSessionCount > 0, STATUS_INTERNAL_ERROR, "Should've created streaming session for viewer");
    pSampleStreamingSession = pSampleConfiguration->sampleStreamingSessionList[0];

    MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    locked = FALSE;

    switch (pReceivedSignalingMessage->signalingMessage.messageType) {
        case SIGNALING_MESSAGE_TYPE_OFFER:
            DLOGE("Unexpected message SIGNALING_MESSAGE_TYPE_OFFER \n");
            break;
        case SIGNALING_MESSAGE_TYPE_ICE_CANDIDATE:
            CHK_STATUS(handleRemoteCandidate(pSampleStreamingSession, &pReceivedSignalingMessage->signalingMessage));
            break;
        case SIGNALING_MESSAGE_TYPE_ANSWER:
            CHK_STATUS(handleAnswer(pSampleConfiguration, pSampleStreamingSession, &pReceivedSignalingMessage->signalingMessage));
            break;
        default:
            DLOGW("Unknown message type %u", pReceivedSignalingMessage->signalingMessage.messageType);
    }

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    }

    // Return success to continue
    return retStatus;
}

STATUS signalingClientStateChanged(UINT64 customData, SIGNALING_CLIENT_STATE state)
{
    UNUSED_PARAM(customData);
    STATUS retStatus = STATUS_SUCCESS;
    PCHAR pStateStr;

    signalingClientGetStateString(state, &pStateStr);

    DLOGV("Signaling client state changed to %d - '%s'", state, pStateStr);

    // Return success to continue
    return retStatus;
}

STATUS signalingClientError(UINT64 customData, STATUS status, PCHAR msg, UINT32 msgLen)
{
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) customData;

    DLOGW("Signaling client generated an error 0x%08x - '%.*s'", status, msgLen, msg);

    // We will force re-create the signaling client on the following errors
    if (status == STATUS_SIGNALING_ICE_CONFIG_REFRESH_FAILED || status == STATUS_SIGNALING_RECONNECT_FAILED) {
        ATOMIC_STORE_BOOL(&pSampleConfiguration->recreateSignalingClient, TRUE);
        CVAR_BROADCAST(pSampleConfiguration->cvar);
    }

    return STATUS_SUCCESS;
}

STATUS masterMessageReceived(UINT64 customData, PReceivedSignalingMessage pReceivedSignalingMessage)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) customData;
    PSampleStreamingSession pSampleStreamingSession = NULL;
    UINT32 i;
    BOOL locked = FALSE;
    ATOMIC_BOOL expected = FALSE;

    CHK(pSampleConfiguration != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pSampleConfiguration->sampleConfigurationObjLock);
    locked = TRUE;
    // ice candidate message and offer message can come at any order. Therefore, if we see a new peerId, then create
    // a new SampleStreamingSession, which in turn creates a new peerConnection
    for (i = 0; i < pSampleConfiguration->streamingSessionCount && pSampleStreamingSession == NULL; ++i) {
        if (0 == STRCMP(pReceivedSignalingMessage->signalingMessage.peerClientId, pSampleConfiguration->sampleStreamingSessionList[i]->peerId)) {
            pSampleStreamingSession = pSampleConfiguration->sampleStreamingSessionList[i];
        }
    }

    if (pSampleStreamingSession == NULL) {
        CHK_WARN(pSampleConfiguration->streamingSessionCount < ARRAY_SIZE(pSampleConfiguration->sampleStreamingSessionList), retStatus,
                 "Dropping signalling message from peer %s since maximum simultaneous streaming session of %u is reached",
                 pReceivedSignalingMessage->signalingMessage.peerClientId, ARRAY_SIZE(pSampleConfiguration->sampleStreamingSessionList));

        DLOGD("Creating new streaming session for peer %s", pReceivedSignalingMessage->signalingMessage.peerClientId);
        CHK_STATUS(createSampleStreamingSession(pSampleConfiguration, pReceivedSignalingMessage->signalingMessage.peerClientId, TRUE,
                                                &pSampleStreamingSession));
        pSampleStreamingSession->firstSdpMsgReceiveTime = GETTIME();
        pSampleConfiguration->sampleStreamingSessionList[pSampleConfiguration->streamingSessionCount++] = pSampleStreamingSession;
    }

    switch (pReceivedSignalingMessage->signalingMessage.messageType) {
        case SIGNALING_MESSAGE_TYPE_OFFER:
            if (ATOMIC_COMPARE_EXCHANGE_BOOL(&pSampleStreamingSession->sdpOfferAnswerExchanged, &expected, TRUE)) {
                CHK_STATUS(handleOffer(pSampleConfiguration, pSampleStreamingSession, &pReceivedSignalingMessage->signalingMessage));
            } else {
                DLOGD("Offer already received, ignore new offer from client id %s", pReceivedSignalingMessage->signalingMessage.peerClientId);
            }
            break;
        case SIGNALING_MESSAGE_TYPE_ICE_CANDIDATE:
            CHK_STATUS(handleRemoteCandidate(pSampleStreamingSession, &pReceivedSignalingMessage->signalingMessage));
            break;
        case SIGNALING_MESSAGE_TYPE_ANSWER:
            DLOGE("Unexpected message SIGNALING_MESSAGE_TYPE_ANSWER \n");
            break;
        default:
            DLOGW("Unknown message type %u", pReceivedSignalingMessage->signalingMessage.messageType);
    }

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    }

    // Return success to continue
    return retStatus;
}

STATUS logSelectedIceCandidatesInformation(PSampleStreamingSession pSampleStreamingSession)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    RtcStats rtcMetrics;

    CHK(pSampleStreamingSession != NULL, STATUS_NULL_ARG);
    rtcMetrics.requestedTypeOfStats = RTC_STATS_TYPE_LOCAL_CANDIDATE;
    CHK_STATUS(rtcPeerConnectionGetMetrics(pSampleStreamingSession->pPeerConnection, NULL, &rtcMetrics));
    DLOGD("Local Candidate IP Address: %s", rtcMetrics.rtcStatsObject.localIceCandidateStats.address);
    DLOGD("Local Candidate type: %s", rtcMetrics.rtcStatsObject.localIceCandidateStats.candidateType);
    DLOGD("Local Candidate port: %d", rtcMetrics.rtcStatsObject.localIceCandidateStats.port);
    DLOGD("Local Candidate priority: %d", rtcMetrics.rtcStatsObject.localIceCandidateStats.priority);
    DLOGD("Local Candidate transport protocol: %s", rtcMetrics.rtcStatsObject.localIceCandidateStats.protocol);
    DLOGD("Local Candidate relay protocol: %s", rtcMetrics.rtcStatsObject.localIceCandidateStats.relayProtocol);
    DLOGD("Local Candidate Ice server source: %s", rtcMetrics.rtcStatsObject.localIceCandidateStats.url);

    rtcMetrics.requestedTypeOfStats = RTC_STATS_TYPE_REMOTE_CANDIDATE;
    CHK_STATUS(rtcPeerConnectionGetMetrics(pSampleStreamingSession->pPeerConnection, NULL, &rtcMetrics));
    DLOGD("Remote Candidate IP Address: %s", rtcMetrics.rtcStatsObject.remoteIceCandidateStats.address);
    DLOGD("Remote Candidate type: %s", rtcMetrics.rtcStatsObject.localIceCandidateStats.candidateType);
    DLOGD("Remote Candidate port: %d", rtcMetrics.rtcStatsObject.remoteIceCandidateStats.port);
    DLOGD("Remote Candidate priority: %d", rtcMetrics.rtcStatsObject.remoteIceCandidateStats.priority);
    DLOGD("Remote Candidate transport protocol: %s", rtcMetrics.rtcStatsObject.remoteIceCandidateStats.protocol);
CleanUp:
    LEAVES();
    return retStatus;
}

STATUS handleAnswer(PSampleConfiguration pSampleConfiguration, PSampleStreamingSession pSampleStreamingSession, PSignalingMessage pSignalingMessage)
{

    STATUS retStatus = STATUS_SUCCESS;
    RtcSessionDescriptionInit answerSessionDescriptionInit;

    MEMSET(&answerSessionDescriptionInit, 0x00, SIZEOF(RtcSessionDescriptionInit));

    ATOMIC_STORE_BOOL(&pSampleConfiguration->answerReceived, TRUE);
    CHK_STATUS(deserializeSessionDescriptionInit(pSignalingMessage->payload, pSignalingMessage->payloadLen, &answerSessionDescriptionInit));
    CHK_STATUS(setRemoteDescription(pSampleStreamingSession->pPeerConnection, &answerSessionDescriptionInit));
    CHK_LOG_ERR(retStatus);
CleanUp:


    return retStatus;
}

STATUS handleOffer(PSampleConfiguration pSampleConfiguration, PSampleStreamingSession pSampleStreamingSession, PSignalingMessage pSignalingMessage)
{
    STATUS retStatus = STATUS_SUCCESS;
    RtcSessionDescriptionInit offerSessionDescriptionInit;
    BOOL locked = FALSE;
    NullableBool canTrickle;

    CHK(pSampleConfiguration != NULL && pSignalingMessage != NULL, STATUS_NULL_ARG);

    MEMSET(&offerSessionDescriptionInit, 0x00, SIZEOF(RtcSessionDescriptionInit));
    MEMSET(&pSampleStreamingSession->answerSessionDescriptionInit, 0x00, SIZEOF(RtcSessionDescriptionInit));

    CHK_STATUS(deserializeSessionDescriptionInit(pSignalingMessage->payload, pSignalingMessage->payloadLen, &offerSessionDescriptionInit));
    CHK_STATUS(setRemoteDescription(pSampleStreamingSession->pPeerConnection, &offerSessionDescriptionInit));
    canTrickle = canTrickleIceCandidates(pSampleStreamingSession->pPeerConnection);
    /* cannot be null after setRemoteDescription */
    CHECK(!NULLABLE_CHECK_EMPTY(canTrickle));
    pSampleConfiguration->trickleIce = canTrickle.value;
    CHK_STATUS(createAnswer(pSampleStreamingSession->pPeerConnection, &pSampleStreamingSession->answerSessionDescriptionInit));
    CHK_STATUS(setLocalDescription(pSampleStreamingSession->pPeerConnection, &pSampleStreamingSession->answerSessionDescriptionInit));


    if (!pSampleConfiguration->trickleIce) {
        MUTEX_LOCK(pSampleConfiguration->sampleConfigurationObjLock);
        locked = TRUE;

        while (!ATOMIC_LOAD_BOOL(&pSampleStreamingSession->candidateGatheringDone)) {
            CHK_WARN(!ATOMIC_LOAD_BOOL(&pSampleStreamingSession->terminateFlag), STATUS_OPERATION_TIMED_OUT,
                     "application terminated and candidate gathering still not done");
            CVAR_WAIT(pSampleConfiguration->cvar, pSampleConfiguration->sampleConfigurationObjLock, 500 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
        }

        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
        locked = FALSE;

        DLOGD("Candidate collection done for non trickle ice");
        // get the latest local description once candidate gathering is done
        CHK_STATUS(peerConnectionGetCurrentLocalDescription(pSampleStreamingSession->pPeerConnection,
                                                            &pSampleStreamingSession->answerSessionDescriptionInit));
    }

    CHK_STATUS(respondWithAnswer(pSampleStreamingSession));
    DLOGD("time taken to send answer %" PRIu64 " ms",
          (GETTIME() - pSampleStreamingSession->firstSdpMsgReceiveTime) / HUNDREDS_OF_NANOS_IN_A_MILLISECOND);

    if (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->mediaThreadStarted) || !IS_VALID_TID_VALUE(pSampleConfiguration->videoSenderTid)) {
        ATOMIC_STORE_BOOL(&pSampleConfiguration->mediaThreadStarted, TRUE);
        if (pSampleConfiguration->videoSource != NULL) {
            THREAD_CREATE(&pSampleConfiguration->videoSenderTid, pSampleConfiguration->videoSource, (PVOID) pSampleConfiguration);
        }

        if (pSampleConfiguration->audioSource != NULL) {
            THREAD_CREATE(&pSampleConfiguration->audioSenderTid, pSampleConfiguration->audioSource, (PVOID) pSampleConfiguration);
        }

        if ((retStatus = timerQueueAddTimer(pSampleConfiguration->timerQueueHandle, SAMPLE_STATS_DURATION, SAMPLE_STATS_DURATION,
                                            getIceCandidatePairStatsCallback, (UINT64) pSampleConfiguration,
                                            &pSampleConfiguration->iceCandidatePairStatsTimerId)) != STATUS_SUCCESS) {
            DLOGW("Failed to add getIceCandidatePairStatsCallback to add to timer queue (code 0x%08x). Cannot pull ice candidate pair metrics "
                  "periodically",
                  retStatus);
        }
    }

    // The audio video receive routine should be per streaming session
    if (pSampleConfiguration->receiveAudioVideoSource != NULL) {
        THREAD_CREATE(&pSampleStreamingSession->receiveAudioVideoSenderTid, pSampleConfiguration->receiveAudioVideoSource,
                      (PVOID) pSampleStreamingSession);
    }
CleanUp:

    CHK_LOG_ERR(retStatus);

    if (locked) {
        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    }

    return retStatus;
}

STATUS respondWithAnswer(PSampleStreamingSession pSampleStreamingSession)
{
    STATUS retStatus = STATUS_SUCCESS;
    SignalingMessage message;
    UINT32 buffLen = 0;

    CHK_STATUS(serializeSessionDescriptionInit(&pSampleStreamingSession->answerSessionDescriptionInit, NULL, &buffLen));
    CHK_STATUS(serializeSessionDescriptionInit(&pSampleStreamingSession->answerSessionDescriptionInit, message.payload, &buffLen));

    message.version = SIGNALING_MESSAGE_CURRENT_VERSION;
    message.messageType = SIGNALING_MESSAGE_TYPE_ANSWER;
    STRCPY(message.peerClientId, pSampleStreamingSession->peerId);
    message.payloadLen = (UINT32) STRLEN(message.payload);
    message.correlationId[0] = '\0';

    retStatus = signalingClientSendMessageSync(pSampleStreamingSession->pSampleConfiguration->signalingClientHandle, &message);

CleanUp:

    CHK_LOG_ERR(retStatus);
    return retStatus;
}

VOID onIceCandidateHandler(UINT64 customData, PCHAR candidateJson)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleStreamingSession pSampleStreamingSession = (PSampleStreamingSession) customData;
    SignalingMessage message;

    CHK(pSampleStreamingSession != NULL, STATUS_NULL_ARG);

    if (candidateJson == NULL) {
        DLOGD("ice candidate gathering finished");
        ATOMIC_STORE_BOOL(&pSampleStreamingSession->candidateGatheringDone, TRUE);
        CVAR_BROADCAST(pSampleStreamingSession->pSampleConfiguration->cvar);
    } else if (pSampleStreamingSession->pSampleConfiguration->trickleIce && ATOMIC_LOAD_BOOL(&pSampleStreamingSession->peerIdReceived)) {
        message.version = SIGNALING_MESSAGE_CURRENT_VERSION;
        message.messageType = SIGNALING_MESSAGE_TYPE_ICE_CANDIDATE;
        STRCPY(message.peerClientId, pSampleStreamingSession->peerId);
        message.payloadLen = (UINT32) STRLEN(candidateJson);
        STRCPY(message.payload, candidateJson);
        message.correlationId[0] = '\0';
        CHK_STATUS(signalingClientSendMessageSync(pSampleStreamingSession->pSampleConfiguration->signalingClientHandle, &message));
    }

CleanUp:

    CHK_LOG_ERR(retStatus);
}

STATUS initializePeerConnection(PSampleConfiguration pSampleConfiguration, PRtcPeerConnection* ppRtcPeerConnection)
{
    STATUS retStatus = STATUS_SUCCESS;
    RtcConfiguration configuration;
    UINT32 i, j, iceConfigCount, uriCount;
    PIceConfigInfo pIceConfigInfo;
    const UINT32 maxTurnServer = 1;
    uriCount = 0;

    CHK(pSampleConfiguration != NULL && ppRtcPeerConnection != NULL, STATUS_NULL_ARG);

    MEMSET(&configuration, 0x00, SIZEOF(RtcConfiguration));

    if (APP_PREGEN_CERTS) {
        configuration.certificates[0].pCertificate = pSampleConfiguration->rtcConfig.certificates[0].pCertificate;
        configuration.certificates[0].pPrivateKey = pSampleConfiguration->rtcConfig.certificates[0].pPrivateKey;
    }

    // Set this to custom callback to enable filtering of interfaces
    configuration.kvsRtcConfiguration.iceSetInterfaceFilterFunc = NULL;
    configuration.kvsRtcConfiguration.generatedCertificateBits = APP_GENCERTBITS_OVERRIDE;

    // Set the  STUN server
    SNPRINTF(configuration.iceServers[0].urls, MAX_ICE_CONFIG_URI_LEN, KINESIS_VIDEO_STUN_URL, pSampleConfiguration->channelInfo.pRegion);

    if (pSampleConfiguration->useTurn) {
        // Set the URIs from the configuration
        CHK_STATUS(awaitGetIceConfigInfoCount(pSampleConfiguration->signalingClientHandle, &iceConfigCount));

        /* signalingClientGetIceConfigInfoCount can return more than one turn server. Use only one to optimize
         * candidate gathering latency. But user can also choose to use more than 1 turn server. */
        for (uriCount = 0, i = 0; i < maxTurnServer; i++) {
            CHK_STATUS(signalingClientGetIceConfigInfo(pSampleConfiguration->signalingClientHandle, i, &pIceConfigInfo));
            for (j = 0; j < pIceConfigInfo->uriCount; j++) {
                CHECK(uriCount < MAX_ICE_SERVERS_COUNT);
                STRNCPY(configuration.iceServers[uriCount + 1].urls, pIceConfigInfo->uris[j], MAX_ICE_CONFIG_URI_LEN);
                STRNCPY(configuration.iceServers[uriCount + 1].credential, pIceConfigInfo->password, MAX_ICE_CONFIG_CREDENTIAL_LEN);
                STRNCPY(configuration.iceServers[uriCount + 1].username, pIceConfigInfo->userName, MAX_ICE_CONFIG_USER_NAME_LEN);

                uriCount++;
            }
        }
    }
    pSampleConfiguration->iceUriCount = uriCount + 1;
    CHK_STATUS(createPeerConnection(&configuration, ppRtcPeerConnection));

CleanUp:

    return retStatus;
}

// Return ICE server stats for a specific streaming session
STATUS gatherIceServerStats(PSampleStreamingSession pSampleStreamingSession)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    RtcStats rtcmetrics;
    UINT32 j = 0;
    rtcmetrics.requestedTypeOfStats = RTC_STATS_TYPE_ICE_SERVER;
    for (; j < pSampleStreamingSession->pSampleConfiguration->iceUriCount; j++) {
        rtcmetrics.rtcStatsObject.iceServerStats.iceServerIndex = j;
        CHK_STATUS(rtcPeerConnectionGetMetrics(pSampleStreamingSession->pPeerConnection, NULL, &rtcmetrics));
        DLOGD("ICE Server URL: %s", rtcmetrics.rtcStatsObject.iceServerStats.url);
        DLOGD("ICE Server port: %d", rtcmetrics.rtcStatsObject.iceServerStats.port);
        DLOGD("ICE Server protocol: %s", rtcmetrics.rtcStatsObject.iceServerStats.protocol);
        DLOGD("Total requests sent:%" PRIu64, rtcmetrics.rtcStatsObject.iceServerStats.totalRequestsSent);
        DLOGD("Total responses received: %" PRIu64, rtcmetrics.rtcStatsObject.iceServerStats.totalResponsesReceived);
        DLOGD("Total round trip time: %" PRIu64 "ms",
              rtcmetrics.rtcStatsObject.iceServerStats.totalRoundTripTime / HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
    }
CleanUp:
    LEAVES();
    return retStatus;
}

STATUS awaitGetIceConfigInfoCount(SIGNALING_CLIENT_HANDLE signalingClientHandle, PUINT32 pIceConfigInfoCount)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT64 elapsed = 0;

    CHK(IS_VALID_SIGNALING_CLIENT_HANDLE(signalingClientHandle) && pIceConfigInfoCount != NULL, STATUS_NULL_ARG);

    while (TRUE) {
        // Get the configuration count
        CHK_STATUS(signalingClientGetIceConfigInfoCount(signalingClientHandle, pIceConfigInfoCount));

        // Return OK if we have some ice configs
        CHK(*pIceConfigInfoCount == 0, retStatus);

        // Check for timeout
        CHK_ERR(elapsed <= ASYNC_ICE_CONFIG_INFO_WAIT_TIMEOUT, STATUS_OPERATION_TIMED_OUT, "Couldn't retrieve ICE configurations in alotted time.");

        THREAD_SLEEP(ICE_CONFIG_INFO_POLL_PERIOD);
        elapsed += ICE_CONFIG_INFO_POLL_PERIOD;
    }

CleanUp:

    return retStatus;
}

STATUS createSampleStreamingSession(PSampleConfiguration pSampleConfiguration, PCHAR peerId, BOOL isMaster,
                                    PSampleStreamingSession* ppSampleStreamingSession)
{
    STATUS retStatus = STATUS_SUCCESS;
    RtcMediaStreamTrack videoTrack, audioTrack;
    PSampleStreamingSession pSampleStreamingSession = NULL;

    MEMSET(&videoTrack, 0x00, SIZEOF(RtcMediaStreamTrack));
    MEMSET(&audioTrack, 0x00, SIZEOF(RtcMediaStreamTrack));

    CHK(pSampleConfiguration != NULL && ppSampleStreamingSession != NULL, STATUS_NULL_ARG);
    CHK((isMaster && peerId != NULL) || !isMaster, STATUS_INVALID_ARG);

    pSampleStreamingSession = (PSampleStreamingSession) MEMCALLOC(1, SIZEOF(SampleStreamingSession));
    CHK(pSampleStreamingSession != NULL, STATUS_NOT_ENOUGH_MEMORY);

    if (isMaster) {
        STRCPY(pSampleStreamingSession->peerId, peerId);
    } else {
        STRCPY(pSampleStreamingSession->peerId, "monitor");
    }
    ATOMIC_STORE_BOOL(&pSampleStreamingSession->peerIdReceived, TRUE);

    pSampleStreamingSession->pSampleConfiguration = pSampleConfiguration;
    pSampleStreamingSession->rtcMetricsHistory.prevTs = GETTIME();

    ATOMIC_STORE_BOOL(&pSampleStreamingSession->terminateFlag, FALSE);
    ATOMIC_STORE_BOOL(&pSampleStreamingSession->candidateGatheringDone, FALSE);
    ATOMIC_STORE_BOOL(&pSampleStreamingSession->mainstream, FALSE);
    ATOMIC_STORE_BOOL(&pSampleStreamingSession->pendingStream, FALSE);
    ATOMIC_STORE_BOOL(&pSampleStreamingSession->shouldDropDeltaTillKey, FALSE);

    CHK_STATUS(initializePeerConnection(pSampleConfiguration, &pSampleStreamingSession->pPeerConnection));
    CHK_STATUS(peerConnectionOnIceCandidate(pSampleStreamingSession->pPeerConnection, (UINT64) pSampleStreamingSession, onIceCandidateHandler));
    CHK_STATUS(
        peerConnectionOnConnectionStateChange(pSampleStreamingSession->pPeerConnection, (UINT64) pSampleStreamingSession, onConnectionStateChange));
    if (pSampleConfiguration->onDataChannel != NULL) {
        CHK_STATUS(peerConnectionOnDataChannel(pSampleStreamingSession->pPeerConnection, (UINT64) pSampleStreamingSession,
                                               pSampleConfiguration->onDataChannel));
    }

    // Declare that we support H264,Profile=42E01F,level-asymmetry-allowed=1,packetization-mode=1 and Opus
    CHK_STATUS(addSupportedCodec(pSampleStreamingSession->pPeerConnection, RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE));
    CHK_STATUS(addSupportedCodec(pSampleStreamingSession->pPeerConnection, RTC_CODEC_OPUS));

    // Add a SendRecv Transceiver of type video
    videoTrack.kind = MEDIA_STREAM_TRACK_KIND_VIDEO;
    videoTrack.codec = RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE;
    STRCPY(videoTrack.streamId, "myKvsVideoStream");
    STRCPY(videoTrack.trackId, "myVideoTrack");
    CHK_STATUS(addTransceiver(pSampleStreamingSession->pPeerConnection, &videoTrack, NULL, &pSampleStreamingSession->pVideoRtcRtpTransceiver));
    // Add a SendRecv Transceiver of type audio
    audioTrack.kind = MEDIA_STREAM_TRACK_KIND_AUDIO;
    audioTrack.codec = RTC_CODEC_OPUS;
    STRCPY(audioTrack.streamId, "myKvsVideoStream");
    STRCPY(audioTrack.trackId, "myAudioTrack");
    CHK_STATUS(addTransceiver(pSampleStreamingSession->pPeerConnection, &audioTrack, NULL, &pSampleStreamingSession->pAudioRtcRtpTransceiver));

    pSampleStreamingSession->sessionLock = MUTEX_CREATE(FALSE);
    pSampleStreamingSession->firstFrame = TRUE;
    pSampleStreamingSession->startUpLatency = 0;
    pSampleStreamingSession->pDataChannel = NULL;
CleanUp:

    if (STATUS_FAILED(retStatus) && pSampleStreamingSession != NULL) {
        freeSampleStreamingSession(&pSampleStreamingSession);
        pSampleStreamingSession = NULL;
    }

    if (ppSampleStreamingSession != NULL) {
        *ppSampleStreamingSession = pSampleStreamingSession;
    }

    return retStatus;
}

STATUS freeSampleStreamingSession(PSampleStreamingSession* ppSampleStreamingSession)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleStreamingSession pSampleStreamingSession = NULL;

    CHK(ppSampleStreamingSession != NULL, STATUS_NULL_ARG);
    pSampleStreamingSession = *ppSampleStreamingSession;
    CHK(pSampleStreamingSession != NULL, retStatus);

    DLOGD("Freeing streaming session with peer id: %s ", pSampleStreamingSession->peerId);

    if (IS_VALID_MUTEX_VALUE(pSampleStreamingSession->sessionLock)) {
        MUTEX_LOCK(pSampleStreamingSession->sessionLock);
        MUTEX_UNLOCK(pSampleStreamingSession->sessionLock);
        MUTEX_FREE(pSampleStreamingSession->sessionLock);
    }

    ATOMIC_STORE_BOOL(&pSampleStreamingSession->terminateFlag, TRUE);

    if (pSampleStreamingSession->shutdownCallback != NULL) {
        pSampleStreamingSession->shutdownCallback(pSampleStreamingSession->shutdownCallbackCustomData, pSampleStreamingSession);
    }

    if (IS_VALID_TID_VALUE(pSampleStreamingSession->receiveAudioVideoSenderTid)) {
        THREAD_JOIN(pSampleStreamingSession->receiveAudioVideoSenderTid, NULL);
    }

    CHK_LOG_ERR(closePeerConnection(pSampleStreamingSession->pPeerConnection));
    CHK_LOG_ERR(freePeerConnection(&pSampleStreamingSession->pPeerConnection));
    SAFE_MEMFREE(pSampleStreamingSession);

CleanUp:

    CHK_LOG_ERR(retStatus);

    return retStatus;
}

STATUS streamingSessionOnShutdown(PSampleStreamingSession pSampleStreamingSession, UINT64 customData,
                                  StreamSessionShutdownCallback streamSessionShutdownCallback)
{
    STATUS retStatus = STATUS_SUCCESS;

    CHK(pSampleStreamingSession != NULL && streamSessionShutdownCallback != NULL, STATUS_NULL_ARG);

    pSampleStreamingSession->shutdownCallbackCustomData = customData;
    pSampleStreamingSession->shutdownCallback = streamSessionShutdownCallback;

CleanUp:

    return retStatus;
}

VOID sampleFrameHandler(UINT64 customData, PFrame pFrame)
{
    PSampleStreamingSession pSampleStreamingSession = (PSampleStreamingSession)customData;
    DLOGV("Frame received. TrackId: %" PRIu64 ", Size: %u, Flags %u", pFrame->trackId, pFrame->size, pFrame->flags);
    ATOMIC_STORE_BOOL(&pSampleStreamingSession->pSampleConfiguration->frameReceived, TRUE);
}

VOID sampleBandwidthEstimationHandler(UINT64 customData, DOUBLE maxiumBitrate)
{
    UNUSED_PARAM(customData);
    DLOGV("received bitrate suggestion: %f", maxiumBitrate);
}

STATUS handleRemoteCandidate(PSampleStreamingSession pSampleStreamingSession, PSignalingMessage pSignalingMessage)
{
    STATUS retStatus = STATUS_SUCCESS;
    RtcIceCandidateInit iceCandidate;

    CHK_STATUS(deserializeRtcIceCandidateInit(pSignalingMessage->payload, pSignalingMessage->payloadLen, &iceCandidate));
    CHK_STATUS(addIceCandidate(pSampleStreamingSession->pPeerConnection, iceCandidate.candidate));

CleanUp:

    CHK_LOG_ERR(retStatus);
    return retStatus;
}

STATUS traverseDirectoryPEMFileScan(UINT64 customData, DIR_ENTRY_TYPES entryType, PCHAR fullPath, PCHAR fileName)
{
    UNUSED_PARAM(entryType);
    UNUSED_PARAM(fullPath);

    PCHAR certName = (PCHAR) customData;
    UINT32 fileNameLen = STRLEN(fileName);

    if (fileNameLen > ARRAY_SIZE(CA_CERT_PEM_FILE_EXTENSION) + 1 &&
        (STRCMPI(CA_CERT_PEM_FILE_EXTENSION, &fileName[fileNameLen - ARRAY_SIZE(CA_CERT_PEM_FILE_EXTENSION) + 1]) == 0)) {
        certName[0] = FPATHSEPARATOR;
        certName++;
        STRCPY(certName, fileName);
    }

    return STATUS_SUCCESS;
}

STATUS lookForSslCert(PSampleConfiguration* ppSampleConfiguration)
{
    STATUS retStatus = STATUS_SUCCESS;
    struct stat pathStat;
    CHAR certName[MAX_PATH_LEN];
    PSampleConfiguration pSampleConfiguration = *ppSampleConfiguration;

    MEMSET(certName, 0x0, ARRAY_SIZE(certName));
    pSampleConfiguration->pCaCertPath = getenv(CACERT_PATH_ENV_VAR);

    // if ca cert path is not set from the environment, try to use the one that cmake detected
    if (pSampleConfiguration->pCaCertPath == NULL) {
        CHK_ERR(STRNLEN(DEFAULT_KVS_CACERT_PATH, MAX_PATH_LEN) > 0, STATUS_INVALID_OPERATION, "No ca cert path given (error:%s)", strerror(errno));
        pSampleConfiguration->pCaCertPath = DEFAULT_KVS_CACERT_PATH;
    } else {
        // Check if the environment variable is a path
        CHK(0 == FSTAT(pSampleConfiguration->pCaCertPath, &pathStat), STATUS_DIRECTORY_ENTRY_STAT_ERROR);

        if (S_ISDIR(pathStat.st_mode)) {
            CHK_STATUS(traverseDirectory(pSampleConfiguration->pCaCertPath, (UINT64) &certName, /* iterate */ FALSE, traverseDirectoryPEMFileScan));

            if (certName[0] != 0x0) {
                STRCAT(pSampleConfiguration->pCaCertPath, certName);
            } else {
                DLOGW("Cert not found in path set...checking if CMake detected a path\n");
                CHK_ERR(STRNLEN(DEFAULT_KVS_CACERT_PATH, MAX_PATH_LEN) > 0, STATUS_INVALID_OPERATION, "No ca cert path given (error:%s)",
                        strerror(errno));
                DLOGI("CMake detected cert path\n");
                pSampleConfiguration->pCaCertPath = DEFAULT_KVS_CACERT_PATH;
            }
        }
    }

CleanUp:

    CHK_LOG_ERR(retStatus);
    return retStatus;
}

STATUS createSampleConfiguration(PCHAR channelName, SIGNALING_CHANNEL_ROLE_TYPE roleType, BOOL trickleIce, BOOL useTurn,
                                 PSampleConfiguration* ppSampleConfiguration)
{
    STATUS retStatus = STATUS_SUCCESS;
    PCHAR pAccessKey, pSecretKey, pSessionToken, pLogLevel;
    PSampleConfiguration pSampleConfiguration = NULL;
    UINT32 logLevel;

    CHK(ppSampleConfiguration != NULL, STATUS_NULL_ARG);

    CHK(NULL != (pSampleConfiguration = (PSampleConfiguration) MEMCALLOC(1, SIZEOF(SampleConfiguration))), STATUS_NOT_ENOUGH_MEMORY);

    pSampleConfiguration->enableFileLogging = FALSE;
    if (NULL != getenv(ENABLE_FILE_LOGGING)) {
        pSampleConfiguration->enableFileLogging = TRUE;
    }
    if ((pSampleConfiguration->channelInfo.pRegion = getenv(DEFAULT_REGION_ENV_VAR)) == NULL) {
        pSampleConfiguration->channelInfo.pRegion = DEFAULT_AWS_REGION;
    }

    CHK_STATUS(lookForSslCert(&pSampleConfiguration));


    if (NULL == (pLogLevel = getenv(DEBUG_LOG_LEVEL_ENV_VAR)) || (STATUS_SUCCESS != STRTOUI32(pLogLevel, NULL, 10, &logLevel))) {
        logLevel = LOG_LEVEL_WARN;
    }

    SET_LOGGER_LOG_LEVEL(logLevel);

    if ((pAccessKey = getenv(ACCESS_KEY_ENV_VAR)) != NULL && (pSecretKey = getenv(SECRET_KEY_ENV_VAR)) != NULL) {
        pSessionToken = getenv(SESSION_TOKEN_ENV_VAR);
        CHK_STATUS(createStaticCredentialProvider(pAccessKey, 0, pSecretKey, 0, pSessionToken, 0, MAX_UINT64, &pSampleConfiguration->pCredentialProvider));
    }

    pSampleConfiguration->audioSenderTid = INVALID_TID_VALUE;
    pSampleConfiguration->videoSenderTid = INVALID_TID_VALUE;
    pSampleConfiguration->signalingClientHandle = INVALID_SIGNALING_CLIENT_HANDLE_VALUE;
    pSampleConfiguration->sampleConfigurationObjLock = MUTEX_CREATE(TRUE);
    pSampleConfiguration->cvar = CVAR_CREATE();


    pSampleConfiguration->trickleIce = trickleIce;
    pSampleConfiguration->useTurn = useTurn;

    pSampleConfiguration->channelInfo.version = CHANNEL_INFO_CURRENT_VERSION;
    pSampleConfiguration->channelInfo.pChannelName = channelName;
    pSampleConfiguration->channelInfo.pKmsKeyId = NULL;
    pSampleConfiguration->channelInfo.tagCount = 0;
    pSampleConfiguration->channelInfo.pTags = NULL;
    pSampleConfiguration->channelInfo.channelType = SIGNALING_CHANNEL_TYPE_SINGLE_MASTER;
    pSampleConfiguration->channelInfo.channelRoleType = roleType;
    pSampleConfiguration->channelInfo.cachingPolicy = SIGNALING_API_CALL_CACHE_TYPE_NONE;
    pSampleConfiguration->channelInfo.cachingPeriod = SIGNALING_API_CALL_CACHE_TTL_SENTINEL_VALUE;
    pSampleConfiguration->channelInfo.asyncIceServerConfig = TRUE;
    pSampleConfiguration->channelInfo.retry = TRUE;
    pSampleConfiguration->channelInfo.reconnect = TRUE;
    pSampleConfiguration->channelInfo.pCertPath = pSampleConfiguration->pCaCertPath;
    pSampleConfiguration->channelInfo.messageTtl = 0; // Default is 60 seconds

    pSampleConfiguration->signalingClientCallbacks.version = SIGNALING_CLIENT_CALLBACKS_CURRENT_VERSION;
    pSampleConfiguration->signalingClientCallbacks.errorReportFn = signalingClientError;
    pSampleConfiguration->signalingClientCallbacks.stateChangeFn = signalingClientStateChanged;
    pSampleConfiguration->signalingClientCallbacks.customData = (UINT64) pSampleConfiguration;
    pSampleConfiguration->signalingClientCallbacks.messageReceivedFn = masterMessageReceived;

    pSampleConfiguration->clientInfo.version = SIGNALING_CLIENT_INFO_CURRENT_VERSION;
    pSampleConfiguration->clientInfo.loggingLevel = logLevel;
    pSampleConfiguration->iceCandidatePairStatsTimerId = MAX_UINT32;

    ATOMIC_STORE_BOOL(&pSampleConfiguration->interrupted, FALSE);
    ATOMIC_STORE_BOOL(&pSampleConfiguration->mediaThreadStarted, FALSE);
    ATOMIC_STORE_BOOL(&pSampleConfiguration->appTerminateFlag, FALSE);
    ATOMIC_STORE_BOOL(&pSampleConfiguration->recreateSignalingClient, FALSE);

    CHK_STATUS(timerQueueCreate(&pSampleConfiguration->timerQueueHandle));

    pSampleConfiguration->iceUriCount = 0;

CleanUp:

    if (STATUS_FAILED(retStatus)) {
        freeSampleConfiguration(&pSampleConfiguration);
    }

    if (ppSampleConfiguration != NULL) {
        *ppSampleConfiguration = pSampleConfiguration;
    }

    return retStatus;
}

STATUS logSignalingClientStats(PSignalingClientMetrics pSignalingClientMetrics)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    CHK(pSignalingClientMetrics != NULL, STATUS_NULL_ARG);
    DLOGD("Signaling client connection duration: %" PRIu64 " ms",
          (pSignalingClientMetrics->signalingClientStats.connectionDuration / HUNDREDS_OF_NANOS_IN_A_MILLISECOND));
    DLOGD("Number of signaling client API errors: %d", pSignalingClientMetrics->signalingClientStats.numberOfErrors);
    DLOGD("Number of runtime errors in the session: %d", pSignalingClientMetrics->signalingClientStats.numberOfRuntimeErrors);
    DLOGD("Signaling client uptime: %" PRIu64 " ms",
          (pSignalingClientMetrics->signalingClientStats.connectionDuration / HUNDREDS_OF_NANOS_IN_A_MILLISECOND));
    // This gives the EMA of the createChannel, describeChannel, getChannelEndpoint and deleteChannel calls
    DLOGD("Control Plane API call latency: %" PRIu64 " ms",
          (pSignalingClientMetrics->signalingClientStats.cpApiCallLatency / HUNDREDS_OF_NANOS_IN_A_MILLISECOND));
    // This gives the EMA of the getIceConfig() call.
    DLOGD("Data Plane API call latency: %" PRIu64 " ms",
          (pSignalingClientMetrics->signalingClientStats.dpApiCallLatency / HUNDREDS_OF_NANOS_IN_A_MILLISECOND));
CleanUp:
    LEAVES();
    return retStatus;
}

STATUS getIceCandidatePairStatsCallback(UINT32 timerId, UINT64 currentTime, UINT64 customData)
{
    UNUSED_PARAM(timerId);
    UNUSED_PARAM(currentTime);
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) customData;
    UINT32 i;
    pSampleConfiguration->rtcIceCandidatePairMetrics.requestedTypeOfStats = RTC_STATS_TYPE_CANDIDATE_PAIR;
    UINT64 currentMeasureDuration = 0;
    DOUBLE averagePacketsDiscardedOnSend = 0.0;
    DOUBLE averageNumberOfPacketsSentPerSecond = 0.0;
    DOUBLE averageNumberOfPacketsReceivedPerSecond = 0.0;
    DOUBLE outgoingBitrate = 0.0;
    DOUBLE incomingBitrate = 0.0;
    if (pSampleConfiguration == NULL) {
        DLOGW("[KVS Master] getPeriodicStats(): operation returned status code: 0x%08x \n", STATUS_NULL_ARG);
        goto CleanUp;
    }
    for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
        if (STATUS_SUCCEEDED(rtcPeerConnectionGetMetrics(pSampleConfiguration->sampleStreamingSessionList[i]->pPeerConnection, NULL,
                                                         &pSampleConfiguration->rtcIceCandidatePairMetrics))) {
            currentMeasureDuration = (pSampleConfiguration->rtcIceCandidatePairMetrics.timestamp -
                                      pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevTs) /
                HUNDREDS_OF_NANOS_IN_A_SECOND;
            DLOGD("Current duration: %" PRIu64 " seconds", currentMeasureDuration);
            if (currentMeasureDuration > 0) {
                DLOGD("Selected local candidate ID: %s",
                      pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.localCandidateId);
                DLOGD("Selected remote candidate ID: %s",
                      pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.remoteCandidateId);
                // TODO: Display state as a string for readability
                DLOGD("Ice Candidate Pair state: %d", pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.state);
                DLOGD("Nomination state: %s",
                      pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.nominated ? "nominated"
                                                                                                                      : "not nominated");
                averageNumberOfPacketsSentPerSecond =
                    (DOUBLE)(pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.packetsSent -
                             pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevNumberOfPacketsSent) /
                    (DOUBLE) currentMeasureDuration;
                DLOGD("Packet send rate: %lf pkts/sec", averageNumberOfPacketsSentPerSecond);

                averageNumberOfPacketsReceivedPerSecond =
                    (DOUBLE)(pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.packetsReceived -
                             pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevNumberOfPacketsReceived) /
                    (DOUBLE) currentMeasureDuration;
                DLOGD("Packet receive rate: %lf pkts/sec", averageNumberOfPacketsReceivedPerSecond);

                outgoingBitrate = (DOUBLE)((pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.bytesSent -
                                            pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevNumberOfBytesSent) *
                                           8.0) /
                    currentMeasureDuration;
                DLOGD("Outgoing bit rate: %lf bps", outgoingBitrate);

                incomingBitrate = (DOUBLE)((pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.bytesReceived -
                                            pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevNumberOfBytesReceived) *
                                           8.0) /
                    currentMeasureDuration;
                DLOGD("Incoming bit rate: %lf bps", incomingBitrate);

                averagePacketsDiscardedOnSend =
                    (DOUBLE)(pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.packetsDiscardedOnSend -
                             pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevPacketsDiscardedOnSend) /
                    (DOUBLE) currentMeasureDuration;
                DLOGD("Packet discard rate: %lf pkts/sec", averagePacketsDiscardedOnSend);

                DLOGD("Current STUN request round trip time: %lf sec",
                      pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.currentRoundTripTime);
                DLOGD("Number of STUN responses received: %llu",
                      pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.responsesReceived);

                pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevTs =
                    pSampleConfiguration->rtcIceCandidatePairMetrics.timestamp;
                pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevNumberOfPacketsSent =
                    pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.packetsSent;
                pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevNumberOfPacketsReceived =
                    pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.packetsReceived;
                pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevNumberOfBytesSent =
                    pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.bytesSent;
                pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevNumberOfBytesReceived =
                    pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.bytesReceived;
                pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevPacketsDiscardedOnSend =
                    pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.packetsDiscardedOnSend;
            }
        }
    }
CleanUp:
    return retStatus;
}

STATUS freeSampleConfiguration(PSampleConfiguration* ppSampleConfiguration)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration;
    UINT32 i;

    CHK(ppSampleConfiguration != NULL, STATUS_NULL_ARG);
    pSampleConfiguration = *ppSampleConfiguration;

    CHK(pSampleConfiguration != NULL, retStatus);
    for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
        retStatus = gatherIceServerStats(pSampleConfiguration->sampleStreamingSessionList[i]);
        if (STATUS_FAILED(retStatus)) {
            DLOGW("Failed to ICE Server Stats for streaming session %d: %08x", i, retStatus);
        }
        freeSampleStreamingSession(&pSampleConfiguration->sampleStreamingSessionList[i]);
    }

    deinitKvsWebRtc();

    SAFE_MEMFREE(pSampleConfiguration->pVideoFrameBuffer);
    SAFE_MEMFREE(pSampleConfiguration->pAudioFrameBuffer);

    if (IS_VALID_CVAR_VALUE(pSampleConfiguration->cvar) && IS_VALID_MUTEX_VALUE(pSampleConfiguration->sampleConfigurationObjLock)) {
        CVAR_BROADCAST(pSampleConfiguration->cvar);
        // lock to wait until awoken thread finish.
        MUTEX_LOCK(pSampleConfiguration->sampleConfigurationObjLock);
        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    }

    if (IS_VALID_MUTEX_VALUE(pSampleConfiguration->sampleConfigurationObjLock)) {
        MUTEX_FREE(pSampleConfiguration->sampleConfigurationObjLock);
    }

    if (IS_VALID_CVAR_VALUE(pSampleConfiguration->cvar)) {
        CVAR_FREE(pSampleConfiguration->cvar);
    }


    if (getenv(ACCESS_KEY_ENV_VAR) != NULL && getenv(SECRET_KEY_ENV_VAR) != NULL) {
        freeStaticCredentialProvider(&pSampleConfiguration->pCredentialProvider);
    } else {
        freeIotCredentialProvider(&pSampleConfiguration->pCredentialProvider);
    }

    if (pSampleConfiguration->iceCandidatePairStatsTimerId != MAX_UINT32) {
        CHK_STATUS(timerQueueCancelTimer(pSampleConfiguration->timerQueueHandle, pSampleConfiguration->iceCandidatePairStatsTimerId,
                                         (UINT64) pSampleConfiguration));
        pSampleConfiguration->iceCandidatePairStatsTimerId = MAX_UINT32;
    }
    if (IS_VALID_TIMER_QUEUE_HANDLE(pSampleConfiguration->timerQueueHandle)) {
        timerQueueFree(&pSampleConfiguration->timerQueueHandle);
    }

    MEMFREE(*ppSampleConfiguration);
    *ppSampleConfiguration = NULL;

CleanUp:

    LEAVES();
    return retStatus;
}

STATUS sessionCleanupWait(PSampleConfiguration pSampleConfiguration)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PSampleStreamingSession pSampleStreamingSession = NULL;
    UINT32 i;
    BOOL locked = FALSE;
    SIGNALING_CLIENT_STATE signalingClientState;

    CHK(pSampleConfiguration != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pSampleConfiguration->sampleConfigurationObjLock);
    locked = TRUE;

    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->interrupted)) {
        // scan and cleanup terminated streaming session
        DLOGD(">>>Number of sessions %d", pSampleConfiguration->streamingSessionCount);
        for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
            if (ATOMIC_LOAD_BOOL(&pSampleConfiguration->sampleStreamingSessionList[i]->terminateFlag)) {
                pSampleStreamingSession = pSampleConfiguration->sampleStreamingSessionList[i];

                pSampleConfiguration->streamingSessionCount--;
                pSampleConfiguration->sampleStreamingSessionList[i] =
                    pSampleConfiguration->sampleStreamingSessionList[pSampleConfiguration->streamingSessionCount];

                CHK_STATUS(freeSampleStreamingSession(&pSampleStreamingSession));
                CHK_STATUS(genCerts(pSampleConfiguration));
            }
        }

        // periodically wake up and clean up terminated streaming session
        CVAR_WAIT(pSampleConfiguration->cvar, pSampleConfiguration->sampleConfigurationObjLock, 5 * HUNDREDS_OF_NANOS_IN_A_SECOND);

        // Check if we need to re-create the signaling client on-the-fly
        if (ATOMIC_LOAD_BOOL(&pSampleConfiguration->recreateSignalingClient) &&
            STATUS_SUCCEEDED(freeSignalingClient(&pSampleConfiguration->signalingClientHandle)) &&
            STATUS_SUCCEEDED(createSignalingClientSync(&pSampleConfiguration->clientInfo, &pSampleConfiguration->channelInfo,
                                                       &pSampleConfiguration->signalingClientCallbacks, pSampleConfiguration->pCredentialProvider,
                                                       &pSampleConfiguration->signalingClientHandle))) {
            // Re-set the variable again
            ATOMIC_STORE_BOOL(&pSampleConfiguration->recreateSignalingClient, FALSE);
        }

        // Check the signaling client state and connect if needed
        if (IS_VALID_SIGNALING_CLIENT_HANDLE(pSampleConfiguration->signalingClientHandle)) {
            CHK_STATUS(signalingClientGetCurrentState(pSampleConfiguration->signalingClientHandle, &signalingClientState));
            if (signalingClientState == SIGNALING_CLIENT_STATE_READY) {
                UNUSED_PARAM(signalingClientConnectSync(pSampleConfiguration->signalingClientHandle));
            }
        }

        if (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->mediaThreadStarted) || !IS_VALID_TID_VALUE(pSampleConfiguration->videoSenderTid)) {
            ATOMIC_STORE_BOOL(&pSampleConfiguration->mediaThreadStarted, TRUE);
            if (pSampleConfiguration->videoSource != NULL) {
                THREAD_CREATE(&pSampleConfiguration->videoSenderTid, pSampleConfiguration->videoSource,
                    (PVOID)pSampleConfiguration);
            }
        }
    }

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (locked) {
        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    }

    LEAVES();
    return retStatus;
}

STATUS genCerts(PSampleConfiguration pConfig)
{
    STATUS retStatus = STATUS_SUCCESS;
    X509* pCert = NULL;
    EVP_PKEY* pKey = NULL;

    CHK_STATUS(createCertificateAndKey(APP_GENCERTBITS_OVERRIDE, FALSE, &pCert, &pKey));

    MEMSET(&(pConfig->rtcConfig), 0x00, SIZEOF(RtcConfiguration));
    pConfig->rtcConfig.certificates[0].pCertificate = (PBYTE)pCert;
    pConfig->rtcConfig.certificates[0].pPrivateKey = (PBYTE)pKey;

CleanUp:
    CHK_LOG_ERR(retStatus);
    return retStatus;
}

VOID genRandomId()
{
    /*
    for (int i = 0; i < APP_PEER_ID_LENGTH; i++) {
        name_buffer[i] = app_peer_id_charset[(random() % 29) & 0x1f];
    }
    name_buffer[APP_PEER_ID_LENGTH] = 0;
    */
}

VOID std2fileLogger(PVOID args)
{
    PCHAR filePath = (PCHAR)args;
    char CMD_BUFFER[256];
    // 5000 lines is about 5MB
    int LINE_COUNT_LIMIT = 5000;
    freopen(filePath, "a+", stdout);
    freopen(filePath, "a+", stderr);
    for (;;) {
        THREAD_SLEEP(10 * HUNDREDS_OF_NANOS_IN_A_SECOND);
        fflush(stdout);
        fflush(stderr);
        SPRINTF(CMD_BUFFER, "cat %s | tail -n%d > %s.bak\0", filePath, LINE_COUNT_LIMIT, filePath);
        system(CMD_BUFFER);
        SPRINTF(CMD_BUFFER, "cat %s.bak > %s\0", filePath, filePath);
        system(CMD_BUFFER);
        SPRINTF(CMD_BUFFER, "rm %s.bak\0", filePath);
        system(CMD_BUFFER);
    }
    fclose(stdout);
    fclose(stderr);
}
