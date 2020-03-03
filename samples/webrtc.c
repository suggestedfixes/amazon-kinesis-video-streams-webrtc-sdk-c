#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include "webrtc.h"

extern PSampleConfiguration gSampleConfiguration;
CHAR appGstStr[APP_GST_STRLEN];

typedef struct _GstCustomData {
    gboolean is_live;
    GstElement* pipeline;
    GMainLoop* loop;
} GstCustomData;

static void gstMessageCallback(GstBus* bus, GstMessage* msg,
    GstCustomData* data)
{
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError* err;
        gchar* debug;

        gst_message_parse_error(msg, &err, &debug);
        g_print("GST Error: %s\n", err->message);
        g_error_free(err);
        g_free(debug);

        gst_element_set_state(data->pipeline, GST_STATE_READY);
        g_main_loop_quit(data->loop);
        break;
    }
    case GST_MESSAGE_EOS: {
        gst_element_set_state(data->pipeline, GST_STATE_READY);
        g_main_loop_quit(data->loop);
        break;
    }
    case GST_MESSAGE_BUFFERING: {
        gint percent = 0;
        /* If the stream is live, we do not care about buffering. */
        if (data->is_live)
            break;

        gst_message_parse_buffering(msg, &percent);
        g_print("Buffering (%3d%%)\r", percent);
        /* Wait until buffering is complete before start/resume playing */
        if (percent < 100)
            gst_element_set_state(data->pipeline, GST_STATE_PAUSED);
        else
            gst_element_set_state(data->pipeline, GST_STATE_PLAYING);
        break;
    }
    case GST_MESSAGE_CLOCK_LOST:
        /* Get a new clock */
        gst_element_set_state(data->pipeline, GST_STATE_PAUSED);
        gst_element_set_state(data->pipeline, GST_STATE_PLAYING);
        break;
    default:
        /* Unhandled message */
        break;
    }
}

VOID cleanGst(GMainLoop* loop, GstElement* pipeline, GstBus* bus,
    GstMessage* msg, BOOL gstCleaned)
{
    if (!gstCleaned) {
        if (loop != NULL) {
            g_main_loop_unref(loop);
        }
        if (bus != NULL) {
            gst_object_unref(bus);
        }
        if (pipeline != NULL) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
        }
        if (msg != NULL) {
            gst_message_unref(msg);
        }
    }
}

GstFlowReturn on_new_sample(GstElement* sink, gpointer data, UINT64 trackid)
{
    GstBuffer* buffer;
    BOOL isDroppable, delta;
    GstFlowReturn ret = GST_FLOW_OK;
    GstSample* sample = NULL;
    GstMapInfo info;
    GstSegment* segment;
    GstClockTime buf_pts;
    Frame frame;
    STATUS retStatus = STATUS_SUCCESS, status;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration)data;
    PSampleStreamingSession pSampleStreamingSession = NULL;
    PRtcRtpTransceiver pRtcRtpTransceiver = NULL;
    UINT32 i;

    CHK(pSampleConfiguration != NULL, STATUS_NULL_ARG);

    info.data = NULL;
    sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));

    buffer = gst_sample_get_buffer(sample);
    isDroppable = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED) || GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DECODE_ONLY) || (GST_BUFFER_FLAGS(buffer) == GST_BUFFER_FLAG_DISCONT) || (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT) && GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT)) ||
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
            CHK(FALSE, retStatus);
        }

        CHK(gst_buffer_map(buffer, &info, GST_MAP_READ), retStatus);

        frame.trackId = trackid;
        frame.duration = 0;
        frame.version = FRAME_CURRENT_VERSION;
        frame.size = (UINT32)info.size;
        frame.frameData = (PBYTE)info.data;

        if (!ATOMIC_LOAD_BOOL(
                &pSampleConfiguration->updatingSampleStreamingSessionList)) {
            ATOMIC_INCREMENT(
                &pSampleConfiguration->streamingSessionListReadingThreadCount);
            for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
                pSampleStreamingSession = pSampleConfiguration->sampleStreamingSessionList[i];
                frame.index = (UINT32)ATOMIC_INCREMENT(&pSampleStreamingSession->frameIndex);

                if (trackid == DEFAULT_AUDIO_TRACK_ID) {
                    pRtcRtpTransceiver = pSampleStreamingSession->pAudioRtcRtpTransceiver;
                    frame.presentationTs = pSampleStreamingSession->audioTimestamp;
                    frame.decodingTs = frame.presentationTs;
                    pSampleStreamingSession->audioTimestamp += SAMPLE_AUDIO_FRAME_DURATION; // assume audio frame size
                        // is 20ms, which is
                        // default in opusenc

                } else {
                    pRtcRtpTransceiver = pSampleStreamingSession->pVideoRtcRtpTransceiver;
                    frame.presentationTs = pSampleStreamingSession->videoTimestamp;
                    frame.decodingTs = frame.presentationTs;
                    pSampleStreamingSession->videoTimestamp += SAMPLE_VIDEO_FRAME_DURATION; // assume video fps is 30
                }

                status = writeFrame(pRtcRtpTransceiver, &frame);
                if (STATUS_FAILED(status)) {
                    DLOGD("writeFrame failed with 0x%08x", status);
                }
            }
            ATOMIC_DECREMENT(
                &pSampleConfiguration->streamingSessionListReadingThreadCount);
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

GstFlowReturn on_new_sample_video(GstElement* sink, gpointer data)
{
    return on_new_sample(sink, data, DEFAULT_VIDEO_TRACK_ID);
}

GstFlowReturn on_new_sample_audio(GstElement* sink, gpointer data)
{
    return on_new_sample(sink, data, DEFAULT_AUDIO_TRACK_ID);
}

PVOID sendGstreamerAudioVideo(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    GstElement *appsinkVideo = NULL, *appsinkAudio = NULL, *pipeline = NULL;
    GstBus* bus = NULL;
    GstMessage* msg = NULL;
    GError* error = NULL;
    GMainLoop* main_loop = NULL;
    GstStateChangeReturn ret;
    GstCustomData data;
    MEMSET(&data, 0, sizeof(data));

    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration)args;
    PCHAR gstStr = NULL;
    PCHAR rtspSrc = GETENV("APP_RTSP_SRC");
    BOOL gstCleaned = FALSE;

    CHK(pSampleConfiguration != NULL, STATUS_NULL_ARG);
    CHK(rtspSrc != NULL, STATUS_NULL_ARG);

    // Use x264enc as its available on mac, pi, ubuntu and windows
    // mac pipeline fails if resolution is not 720p
    //
    // For alaw
    // audiotestsrc ! audio/x-raw, rate=8000, channels=1, format=S16LE,
    // layout=interleaved ! alawenc ! appsink sync=TRUE emit-signals=TRUE
    // name=appsink-audio
    //
    // For VP8
    // videotestsrc ! video/x-raw,width=1280,height=720,framerate=30/1 ! vp8enc
    // error-resilient=partitions keyframe-max-dist=10 auto-alt-ref=true
    // cpu-used=5 deadline=1 ! appsink sync=TRUE emit-signals=TRUE
    // name=appsink-video
    //
    // For rtsp
    // rtspsrc location=rtspsrc short-header=TRUE ! rtph264depay !
    // video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline !
    // appsink sync=TRUE emit-signals=TRUE name=appsink-video

    switch (pSampleConfiguration->mediaType) {
    case SAMPLE_STREAMING_VIDEO_ONLY:
        gstStr = "rtspsrc %s location=%s short-header=TRUE %s ! %s rtph264depay ! "
                 "video/"
                 "x-h264,stream-format=byte-stream,alignment=au,profile=baseline ! "
                 "appsink sync=TRUE emit-signals=TRUE name=appsink-video";
        break;

    case SAMPLE_STREAMING_AUDIO_VIDEO:
        gstStr = "rtspsrc %s location=%s short-header=TRUE %s ! %s ! rtph264depay ! "
                 "video/"
                 "x-h264,stream-format=byte-stream,alignment=au,profile=baseline ! "
                 "h264parse ! appsink sync=TRUE emit-signals=TRUE "
                 "name=appsink-video autoaudiosrc ! "
                 "queue leaky=2 max-size-buffers=400 ! audioconvert ! audioresample "
                 "! opusenc ! audio/x-opus,rate=48000,channels=2 ! appsink "
                 "sync=TRUE emit-signals=TRUE name=appsink-audio";
        break;
    }

    SPRINTF(appGstStr, gstStr, APP_GST_RTSPSRC_EXT ? "num-buffers=180" : "",
        rtspSrc, APP_GST_ENFORCE_TCP ? "protocols=tcp" : "",
        APP_GST_RTSPSRC_AFT ? "queue leaky=2 ! " : "");

    printf("%s\n", appGstStr);

    // recreate gstreamer pipeline on error or eof if enabled
    // blocks on g_main_loop_run
    do {
        gstCleaned = FALSE;
        pipeline = gst_parse_launch(appGstStr, &error);

        CHK_ERR(pipeline != NULL, STATUS_INTERNAL_ERROR,
            "Failed to launch gstreamer");

        appsinkVideo = gst_bin_get_by_name(GST_BIN(pipeline), "appsink-video");
        appsinkAudio = gst_bin_get_by_name(GST_BIN(pipeline), "appsink-audio");
        CHK_ERR(appsinkVideo != NULL || appsinkAudio != NULL, STATUS_INTERNAL_ERROR,
            "cant find appsink");

        if (appsinkVideo != NULL) {
            g_signal_connect(appsinkVideo, "new-sample",
                G_CALLBACK(on_new_sample_video),
                (gpointer)pSampleConfiguration);
        }

        if (appsinkAudio != NULL) {
            g_signal_connect(appsinkAudio, "new-sample",
                G_CALLBACK(on_new_sample_audio),
                (gpointer)pSampleConfiguration);
        }

        bus = gst_element_get_bus(pipeline);

        /* Start playing */
        ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            g_printerr("Unable to set the pipeline to the playing state.\n");
            CHK(FALSE, retStatus);
        } else if (ret == GST_STATE_CHANGE_NO_PREROLL) {
            data.is_live = TRUE;
        }

        main_loop = g_main_loop_new(NULL, FALSE);
        data.loop = main_loop;
        data.pipeline = pipeline;

        gst_bus_add_signal_watch(bus);
        g_signal_connect(bus, "message", G_CALLBACK(gstMessageCallback), &data);
        g_main_loop_run(main_loop);

        cleanGst(main_loop, pipeline, bus, msg, gstCleaned);
        gstCleaned = TRUE;
    } while (APP_GST_ERR_RECOVERY);

CleanUp:

    if (error != NULL) {
        DLOGE("%s", error->message);
        g_clear_error(&error);
    }
    cleanGst(main_loop, pipeline, bus, msg, gstCleaned);
    CHK_LOG_ERR_NV(retStatus);
    return (PVOID)(ULONG_PTR)retStatus;
}

VOID onGstAudioFrameReady(UINT64 customData, PFrame pFrame)
{
    GstFlowReturn ret;
    GstBuffer* buffer;
    GstElement* appsrcAudio = (GstElement*)customData;

    /* Create a new empty buffer */
    buffer = gst_buffer_new_and_alloc(pFrame->size);
    gst_buffer_fill(buffer, 0, pFrame->frameData, pFrame->size);

    /* Push the buffer into the appsrc */
    g_signal_emit_by_name(appsrcAudio, "push-buffer", buffer, &ret);

    /* Free the buffer now that we are done with it */
    gst_buffer_unref(buffer);
}

VOID onSampleStreamingSessionShutdown(
    UINT64 customData, PSampleStreamingSession pSampleStreamingSession)
{
    UNUSED_PARAM(pSampleStreamingSession);
    GstElement* appsrc = (GstElement*)customData;
    GstFlowReturn ret;

    g_signal_emit_by_name(appsrc, "end-of-stream", &ret);
}

PVOID receiveGstreamerAudioVideo(PVOID args)
{
    return (PVOID)(ULONG_PTR)STATUS_SUCCESS;
    STATUS retStatus = STATUS_SUCCESS;
    GstElement *pipeline = NULL, *appsrcAudio = NULL;
    GstBus* bus = NULL;
    GstMessage* msg = NULL;
    GError* error = NULL;
    PSampleStreamingSession pSampleStreamingSession = (PSampleStreamingSession)args;
    gchar *videoDescription = "", *audioDescription = "", *audioVideoDescription;
    BOOL gstCleaned = FALSE;

    CHK(pSampleStreamingSession != NULL, STATUS_NULL_ARG);

    // TODO: Wire video up with gstreamer pipeline

    switch (
        pSampleStreamingSession->pAudioRtcRtpTransceiver->receiver.track.codec) {
    case RTC_CODEC_OPUS:
        audioDescription = "appsrc name=appsrc-audio ! opusparse ! decodebin ! autoaudiosink";
        break;

    case RTC_CODEC_MULAW:
    case RTC_CODEC_ALAW:
        audioDescription = "appsrc name=appsrc-audio ! rawaudioparse ! "
                           "decodebin ! autoaudiosink";
        break;
    default:
        break;
    }

    // disable audio for now
    audioDescription = "";

    // recreate gstreamer pipeline on error, exit the loop on EOS
    // as long as there is no error nor eos, block on gst_bus_timed_pop_filtered
    // TODO: convert to gstreamer main loop
    do {
        gstCleaned = TRUE;
        audioVideoDescription = g_strjoin(" ", audioDescription, videoDescription, NULL);

        pipeline = gst_parse_launch(audioVideoDescription, &error);

        appsrcAudio = gst_bin_get_by_name(GST_BIN(pipeline), "appsrc-audio");
        CHK_ERR(appsrcAudio != NULL, STATUS_INTERNAL_ERROR, "cant find appsrc");

        transceiverOnFrame(pSampleStreamingSession->pAudioRtcRtpTransceiver,
            (UINT64)appsrcAudio, onGstAudioFrameReady);

        CHK_STATUS(streamingSessionOnShutdown(pSampleStreamingSession,
            (UINT64)appsrcAudio,
            onSampleStreamingSessionShutdown));

        g_free(audioVideoDescription);

        CHK_ERR(pipeline != NULL, STATUS_INTERNAL_ERROR,
            "Failed to launch gstreamer");

        gst_element_set_state(pipeline, GST_STATE_PLAYING);

        /* block until error or EOS */
        bus = gst_element_get_bus(pipeline);
        msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,
            GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            if (APP_GST_EOS_EXIT) {
                goto CleanUp;
            }
            break;
        default:
            break;
        }

        cleanGst(NULL, pipeline, bus, msg, gstCleaned);
        gstCleaned = TRUE;
    } while (APP_GST_ERR_RECOVERY);

CleanUp:
    if (error != NULL) {
        DLOGE("%s", error->message);
        g_clear_error(&error);
    }

    cleanGst(NULL, pipeline, bus, msg, gstCleaned);
    CHK_LOG_ERR_NV(retStatus);
    return (PVOID)(ULONG_PTR)retStatus;
}

INT32 main(INT32 argc, CHAR* argv[])
{
    STATUS retStatus = STATUS_SUCCESS;
    SignalingClientCallbacks signalingClientCallbacks;
    SignalingClientInfo clientInfo;
    PSampleConfiguration pSampleConfiguration = NULL;

    signal(SIGINT, sigintHandler);

    // do trickle-ice by default
    printf("[KVS GStreamer Master] Using trickleICE by default\n");

    CHK_STATUS(createSampleConfiguration(argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME,
        SIGNALING_CHANNEL_ROLE_TYPE_MASTER,
        APP_TRICKLE_ICE, APP_TURN,
        &pSampleConfiguration));
    printf("[KVS GStreamer Master] Created signaling channel %s\n",
        (argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME));

    pSampleConfiguration->videoSource = sendGstreamerAudioVideo;
    pSampleConfiguration->mediaType = SAMPLE_STREAMING_VIDEO_ONLY;

    if (APP_RECEIVE_VIDEO_AUDIO) {
        pSampleConfiguration->receiveAudioVideoSource = receiveGstreamerAudioVideo;
    }

    if (APP_DATA_TRANSFER) {
        pSampleConfiguration->onDataChannel = onDataChannel;
    }

    pSampleConfiguration->customData = (UINT64)pSampleConfiguration;

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
    } else {
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

    // Initalize KVS WebRTC. This must be done before anything else, and must
    // only be done once.
    CHK_STATUS(initKvsWebRtc());
    printf("[KVS GStreamer Master] KVS WebRTC initialization completed "
           "successfully\n");

    if (APP_PREGEN_CERTS) {
        CHK_STATUS(genCerts(pSampleConfiguration));
    }

    signalingClientCallbacks.version = SIGNALING_CLIENT_CALLBACKS_CURRENT_VERSION;
    signalingClientCallbacks.messageReceivedFn = masterMessageReceived;
    signalingClientCallbacks.errorReportFn = NULL;
    signalingClientCallbacks.stateChangeFn = signalingClientStateChanged;
    signalingClientCallbacks.customData = (UINT64)pSampleConfiguration;

    clientInfo.version = SIGNALING_CLIENT_INFO_CURRENT_VERSION;
    STRCPY(clientInfo.clientId, SAMPLE_MASTER_CLIENT_ID);

    CHK_STATUS(createSignalingClientSync(
        &clientInfo, &pSampleConfiguration->channelInfo,
        &signalingClientCallbacks, pSampleConfiguration->pCredentialProvider,
        &pSampleConfiguration->signalingClientHandle));
    printf("[KVS GStreamer Master] Signaling client created successfully\n");

    // Enable the processing of the messages
    CHK_STATUS(
        signalingClientConnectSync(pSampleConfiguration->signalingClientHandle));
    printf("[KVS GStreamer Master] Signaling client connection to socket "
           "established\n");

    printf("[KVS Gstreamer Master] Beginning streaming...check the stream over "
           "channel %s\n",
        (argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME));

    gSampleConfiguration = pSampleConfiguration;

    // Checking for termination
    CHK_STATUS(sessionCleanupWait(pSampleConfiguration));

    printf("[KVS GStreamer Master] Streaming session terminated\n");

CleanUp:

    printf("[KVS GStreamer Master] Cleaning up....\n");
    CHK_LOG_ERR_NV(retStatus);

    if (pSampleConfiguration != NULL) {
        // Kick of the termination sequence
        ATOMIC_STORE_BOOL(&pSampleConfiguration->appTerminateFlag, TRUE);

        if (IS_VALID_TID_VALUE(pSampleConfiguration->videoSenderTid)) {
            // Join the threads
            THREAD_JOIN(pSampleConfiguration->videoSenderTid, NULL);
        }

        CHK_LOG_ERR_NV(
            freeSignalingClient(&pSampleConfiguration->signalingClientHandle));
        CHK_LOG_ERR_NV(freeSampleConfiguration(&pSampleConfiguration));
    }
    printf("[KVS Gstreamer Master] Cleanup done\n");
    return (INT32)retStatus;
}
