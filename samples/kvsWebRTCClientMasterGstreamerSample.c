#include "Samples.h"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

extern PSampleConfiguration gSampleConfiguration;

GstFlowReturn on_new_sample(GstElement *sink, gpointer data, uint64_t trackid)
{
    GstBuffer *buffer;
    BOOL isDroppable, delta;
    GstFlowReturn ret = GST_FLOW_OK;
    GstSample *sample = NULL;
    GstMapInfo info;
    GstSegment *segment;
    GstClockTime buf_pts;
    Frame frame;
    STATUS retStatus = STATUS_SUCCESS, status;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) data;
    PSampleStreamingSession pSampleStreamingSession = NULL;
    PRtcRtpTransceiver pRtcRtpTransceiver = NULL;
    uint32_t i;

    if(pSampleConfiguration == NULL) {
        retStatus = STATUS_NULL_ARG;
        goto CleanUp;
    }

    info.data = NULL;
    sample = gst_app_sink_pull_sample(GST_APP_SINK (sink));

    buffer = gst_sample_get_buffer(sample);
    isDroppable = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED) ||
                  GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DECODE_ONLY) ||
                  (GST_BUFFER_FLAGS(buffer) == GST_BUFFER_FLAG_DISCONT) ||
                  (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT) && GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT)) ||
                  // drop if buffer contains header only and has invalid timestamp
                  !GST_BUFFER_PTS_IS_VALID(buffer);

    if (!isDroppable) {
        delta = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

        frame.flags = delta ? FRAME_FLAG_NONE : FRAME_FLAG_KEY_FRAME;

        // convert from segment timestamp to running time in live mode.
        segment = gst_sample_get_segment(sample);
        buf_pts = gst_segment_to_running_time(segment, GST_FORMAT_TIME, buffer->pts);
        if (!GST_CLOCK_TIME_IS_VALID(buf_pts)) {
            DLOGD("Frame contains invalid PTS dropping the frame.");
            if(retStatus == FALSE) {
                goto CleanUp;
            }
        }

        retStatus = gst_buffer_map(buffer, &info, GST_MAP_READ);
        if(retStatus != STATUS_SUCCESS) {
            goto CleanUp;
        }

        frame.trackId = trackid;
        frame.duration = 0;
        frame.version = FRAME_CURRENT_VERSION;
        frame.size = (uint32_t) info.size;
        frame.frameData = (PBYTE) info.data;

        if (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->updatingSampleStreamingSessionList)) {
            ATOMIC_INCREMENT(&pSampleConfiguration->streamingSessionListReadingThreadCount);
            for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {

                pSampleStreamingSession = pSampleConfiguration->sampleStreamingSessionList[i];
                frame.index = (uint32_t) ATOMIC_INCREMENT(&pSampleStreamingSession->frameIndex);

                if (trackid == DEFAULT_AUDIO_TRACK_ID) {
                    pRtcRtpTransceiver = pSampleStreamingSession->pAudioRtcRtpTransceiver;
                    frame.presentationTs = pSampleStreamingSession->audioTimestamp;
                    frame.decodingTs = frame.presentationTs;
                    pSampleStreamingSession->audioTimestamp += SAMPLE_AUDIO_FRAME_DURATION; // assume audio frame size is 20ms, which is default in opusenc

                } else {
                    pRtcRtpTransceiver = pSampleStreamingSession->pVideoRtcRtpTransceiver;
                    frame.presentationTs = pSampleStreamingSession->videoTimestamp;
                    frame.decodingTs = frame.presentationTs;
                    pSampleStreamingSession->videoTimestamp += SAMPLE_VIDEO_FRAME_DURATION; // assume video fps is 30
                }

                status = writeFrame(pRtcRtpTransceiver, &frame);
                if (status != STATUS_SUCCESS) {
                    DLOGD("writeFrame failed with 0x%08x", status);
                }
            }
            ATOMIC_DECREMENT(&pSampleConfiguration->streamingSessionListReadingThreadCount);
        }
    }

CleanUp:

    if (info.data != NULL) {
        gst_buffer_unmap(buffer, &info);
    }

    if (sample != NULL) {
        gst_sample_unref(sample);
    }

    if (ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        ret = GST_FLOW_EOS;
    }

    return ret;
}

GstFlowReturn on_new_sample_video(GstElement *sink, gpointer data) {
    return on_new_sample(sink, data, DEFAULT_VIDEO_TRACK_ID);
}

GstFlowReturn on_new_sample_audio(GstElement *sink, gpointer data) {
    return on_new_sample(sink, data, DEFAULT_AUDIO_TRACK_ID);
}

void* sendGstreamerAudioVideo(void* args)
{
    STATUS retStatus = STATUS_SUCCESS;
    GstElement *appsinkVideo = NULL, *appsinkAudio = NULL, *pipeline = NULL;
    GstBus *bus;
    GstMessage *msg;
    GError *error = NULL;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;

    if(pSampleConfiguration == NULL) {
        retStatus = STATUS_NULL_ARG;
        goto CleanUp;
    }

    // Use x264enc as its available on mac, pi, ubuntu and windows
    // mac pipeline fails if resolution is not 720p
    //
    // For alaw
    // audiotestsrc ! audio/x-raw, rate=8000, channels=1, format=S16LE, layout=interleaved ! alawenc ! appsink sync=TRUE emit-signals=TRUE name=appsink-audio
    //
    // For VP8
    // videotestsrc ! video/x-raw,width=1280,height=720,framerate=30/1 ! vp8enc error-resilient=partitions keyframe-max-dist=10 auto-alt-ref=true cpu-used=5 deadline=1 ! appsink sync=TRUE emit-signals=TRUE name=appsink-video

    switch (pSampleConfiguration->mediaType) {
        case SAMPLE_STREAMING_VIDEO_ONLY:
            pipeline = gst_parse_launch(
                    "videotestsrc ! queue ! videoconvert ! video/x-raw,width=1280,height=720,framerate=30/1 ! x264enc bframes=0 speed-preset=veryfast key-int-max=30 bitrate=512 ! "
                    "video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline ! appsink sync=TRUE emit-signals=TRUE name=appsink-video",
                    &error);
            break;

        case SAMPLE_STREAMING_AUDIO_VIDEO:
            pipeline = gst_parse_launch(
                    "videotestsrc ! queue ! videoconvert ! video/x-raw,width=1280,height=720,framerate=30/1 ! x264enc bframes=0 speed-preset=veryfast key-int-max=30 bitrate=512 ! "
                            "video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline ! appsink sync=TRUE emit-signals=TRUE name=appsink-video audiotestsrc ! "
                            "queue leaky=2 max-size-buffers=400 ! audioconvert ! audioresample ! opusenc ! audio/x-opus,rate=48000,channels=2 ! appsink sync=TRUE emit-signals=TRUE name=appsink-audio",
                    &error);
            break;
    }
    if(pipeline == NULL) {
        retStatus = STATUS_INTERNAL_ERROR;
        DLOGE("Failed to launch gstreamer");
        goto CleanUp;
    }

    appsinkVideo = gst_bin_get_by_name(GST_BIN(pipeline), "appsink-video");
    appsinkAudio = gst_bin_get_by_name(GST_BIN(pipeline), "appsink-audio");

    if(appsinkVideo == NULL || appsinkAudio == NULL)
    {
        retStatus = STATUS_INTERNAL_ERROR;
        DLOGE("cant find appsink");
        goto CleanUp;
    }

    if (appsinkVideo != NULL) {
        g_signal_connect(appsinkVideo, "new-sample", G_CALLBACK(on_new_sample_video), (gpointer) pSampleConfiguration);
    }

    if (appsinkAudio != NULL) {
        g_signal_connect(appsinkAudio, "new-sample", G_CALLBACK(on_new_sample_audio), (gpointer) pSampleConfiguration);
    }

    gst_element_set_state (pipeline, GST_STATE_PLAYING);

    /* block until error or EOS */
    bus = gst_element_get_bus(pipeline);
    msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

    /* Free resources */
    if (msg != NULL) {
        gst_message_unref(msg);
    }
    gst_object_unref (bus);
    gst_element_set_state (pipeline, GST_STATE_NULL);
    gst_object_unref (pipeline);

CleanUp:

    if (error != NULL) {
        DLOGE("%s", error->message);
        g_clear_error (&error);
    }

    if(retStatus != STATUS_SUCCESS) {
        DLOGE("operation returned status code: 0x%08x", retStatus);
    }

    return (void*) (unsigned long long) retStatus;
}

VOID onGstAudioFrameReady(uint64_t customData, PFrame pFrame)
{
    GstFlowReturn ret;
    GstBuffer *buffer;
    GstElement *appsrcAudio = (GstElement *) customData;

    /* Create a new empty buffer */
    buffer = gst_buffer_new_and_alloc(pFrame->size);
    gst_buffer_fill(buffer, 0, pFrame->frameData, pFrame->size);

    /* Push the buffer into the appsrc */
    g_signal_emit_by_name(appsrcAudio, "push-buffer", buffer, &ret);

    /* Free the buffer now that we are done with it */
    gst_buffer_unref(buffer);
}

VOID onSampleStreamingSessionShutdown(uint64_t customData, PSampleStreamingSession pSampleStreamingSession)
{
    UNUSED_PARAM(pSampleStreamingSession);
    GstElement *appsrc = (GstElement *) customData;
    GstFlowReturn ret;

    g_signal_emit_by_name (appsrc, "end-of-stream", &ret);
}

void* receiveGstreamerAudioVideo(void* args)
{
    STATUS retStatus = STATUS_SUCCESS;
    GstElement *pipeline = NULL, *appsrcAudio = NULL;
    GstBus *bus;
    GstMessage *msg;
    GError *error = NULL;
    PSampleStreamingSession pSampleStreamingSession = (PSampleStreamingSession) args;
    gchar *videoDescription = "", *audioDescription = "", *audioVideoDescription;

    if(pSampleStreamingSession == NULL)
    {
        retStatus = STATUS_NULL_ARG;
        goto CleanUp;
    }

    //TODO: Wire video up with gstreamer pipeline

    switch (pSampleStreamingSession->pAudioRtcRtpTransceiver->receiver.track.codec) {
        case RTC_CODEC_OPUS:
            audioDescription = "appsrc name=appsrc-audio ! opusparse ! decodebin ! autoaudiosink";
            break;

        case RTC_CODEC_MULAW:
        case RTC_CODEC_ALAW:
            audioDescription = "appsrc name=appsrc-audio ! rawaudioparse ! decodebin ! autoaudiosink";
            break;
        default:
            break;
    }

    audioVideoDescription = g_strjoin(" ", audioDescription, videoDescription, NULL);

    pipeline = gst_parse_launch(audioVideoDescription, &error);

    appsrcAudio = gst_bin_get_by_name(GST_BIN(pipeline), "appsrc-audio");
    if(appsrcAudio == NULL)
    {
         retStatus = STATUS_INTERNAL_ERROR;
         DLOGE("cant find appsrc");
         goto CleanUp;
    }

    transceiverOnFrame(pSampleStreamingSession->pAudioRtcRtpTransceiver,
                       (uint64_t) appsrcAudio,
                       onGstAudioFrameReady);

    retStatus = streamingSessionOnShutdown(pSampleStreamingSession, (uint64_t) appsrcAudio, onSampleStreamingSessionShutdown);
    if(retStatus != STATUS_SUCCESS) {
        printf("Streaming session shutdown failed\n");
        goto CleanUp;
    }

    g_free(audioVideoDescription);

    if(pipeline == NULL) {
        retStatus = STATUS_INTERNAL_ERROR;
        DLOGE("Failed to launch gstreamer");
    }

    gst_element_set_state (pipeline, GST_STATE_PLAYING);

    /* block until error or EOS */
    bus = gst_element_get_bus(pipeline);
    msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

    /* Free resources */
    if (msg != NULL) {
        gst_message_unref(msg);
    }
    gst_object_unref (bus);
    gst_element_set_state (pipeline, GST_STATE_NULL);
    gst_object_unref (pipeline);

CleanUp:
    if (error != NULL) {
        DLOGE("%s", error->message);
        g_clear_error (&error);
    }

    if(retStatus != STATUS_SUCCESS) {
        DLOGE("operation returned status code: 0x%08x", retStatus);
    }
    return (void*) (unsigned long long) retStatus;
}

int32_t main(int32_t argc, char *argv[])
{
    STATUS retStatus = STATUS_SUCCESS;
    SignalingClientCallbacks signalingClientCallbacks;
    SignalingClientInfo clientInfo;
    PSampleConfiguration pSampleConfiguration = NULL;

    signal(SIGINT, sigintHandler);

    // do trickle-ice by default
    printf("[KVS GStreamer Master] Using trickleICE by default\n");

    retStatus = createSampleConfiguration(argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME,
                                          SIGNALING_CHANNEL_ROLE_TYPE_MASTER,
                                          TRUE,
                                          TRUE,
                                          &pSampleConfiguration);
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS GStreamer Master] Signaling channel could not be created\n");
        goto CleanUp;
    }

    printf("[KVS GStreamer Master] Created signaling channel %s\n", (argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME));

    pSampleConfiguration->videoSource = sendGstreamerAudioVideo;
    pSampleConfiguration->mediaType = SAMPLE_STREAMING_VIDEO_ONLY;
    pSampleConfiguration->receiveAudioVideoSource = receiveGstreamerAudioVideo;
    pSampleConfiguration->onDataChannel = onDataChannel;
    pSampleConfiguration->customData = (uint64_t) pSampleConfiguration;

    /* Initialize GStreamer */
    gst_init(&argc, &argv);
    printf("[KVS Gstreamer Master] Finished initializing GStreamer\n");

    if (argc > 2) {
        if (STRCMP(argv[2], "video-only") == 0) {
            pSampleConfiguration->mediaType = SAMPLE_STREAMING_VIDEO_ONLY;
            printf("[KVS Gstreamer Master] Streaming video only\n");
        } else if (STRCMP(argv[2], "audio-video") == 0) {
            pSampleConfiguration->mediaType = SAMPLE_STREAMING_AUDIO_VIDEO;
            printf("[KVS Gstreamer Master] Streaming audio and video\n");
        } else {
            DLOGD("Unrecognized streaming type. Default to video-only");
            printf("[KVS Gstreamer Master] Streaming video only\n");
        }
    }
    else {
        printf("[KVS Gstreamer Master] Streaming video only\n");
    }

    switch (pSampleConfiguration->mediaType) {
        case SAMPLE_STREAMING_VIDEO_ONLY:
            DLOGD("streaming type video-only");
            break;
        case SAMPLE_STREAMING_AUDIO_VIDEO:
            DLOGD("streaming type audio-video");
            break;
    }

    // Initalize KVS WebRTC. This must be done before anything else, and must only be done once.
    retStatus = initKvsWebRtc();
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS GStreamer Master] KVS WebRTC initialization failed\n");
        goto CleanUp;
    }
    printf("[KVS GStreamer Master] KVS WebRTC initialization completed successfully\n");

    signalingClientCallbacks.version = SIGNALING_CLIENT_CALLBACKS_CURRENT_VERSION;
    signalingClientCallbacks.messageReceivedFn = masterMessageReceived;
    signalingClientCallbacks.errorReportFn = NULL;
    signalingClientCallbacks.stateChangeFn = signalingClientStateChanged;
    signalingClientCallbacks.customData = (uint64_t) pSampleConfiguration;

    clientInfo.version = SIGNALING_CLIENT_INFO_CURRENT_VERSION;
    strcpy(clientInfo.clientId, SAMPLE_MASTER_CLIENT_ID);

    retStatus = createSignalingClientSync(&clientInfo, &pSampleConfiguration->channelInfo,
                                          &signalingClientCallbacks, pSampleConfiguration->pCredentialProvider,
                                          &pSampleConfiguration->signalingClientHandle);
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS GStreamer Master] Signaling client sync failed\n");
    }
    printf("[KVS GStreamer Master] Signaling client created successfully\n");

    // Enable the processing of the messages
    retStatus = signalingClientConnectSync(pSampleConfiguration->signalingClientHandle);
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS Master] Signaling client connection to socket failed\n");
        goto CleanUp;
    }
    printf("[KVS GStreamer Master] Signaling client connection to socket established\n");

    printf("[KVS Gstreamer Master] Beginning streaming...check the stream over channel %s\n",
            (argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME));

    gSampleConfiguration = pSampleConfiguration;

    // Checking for termination
    retStatus = sessionCleanupWait(pSampleConfiguration);
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS GStreamer Master] Streaming session termination failed\n");
        goto CleanUp;
    }

    printf("[KVS GStreamer Master] Streaming session terminated\n");

CleanUp:

    printf("[KVS GStreamer Master] Cleaning up....\n");
    if(retStatus != STATUS_SUCCESS) {
        DLOGE("operation returned status code: 0x%08x", retStatus);
    }

    if (pSampleConfiguration != NULL) {

        // Kick of the termination sequence
        ATOMIC_STORE_BOOL(&pSampleConfiguration->appTerminateFlag, TRUE);

        if (pSampleConfiguration->videoSenderTid != (uint64_t) NULL) {
            // Join the threads
            if(pthread_join((pthread_t) pSampleConfiguration->videoSenderTid, NULL) != 0) {
                printf("Video sender thread failed to join...%s (code: %d)\n", strerror(errno), errno);
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
    printf("[KVS Gstreamer Master] Cleanup done\n");
    return (int32_t) retStatus;
}
