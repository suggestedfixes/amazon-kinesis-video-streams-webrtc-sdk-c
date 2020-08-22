#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/rtsp/gstrtsptransport.h>

#include "webrtc.h"

extern PSampleConfiguration gSampleConfiguration;
CHAR appGstStr[APP_GST_STRLEN];

typedef struct _GstCustomData {
    gboolean is_live;
    gboolean eos;
    GstElement* pipeline;
    GMainLoop* loop;
} GstCustomData;

static void gstMessageCallback(GstBus* bus, GstMessage* msg,
    GstCustomData* data)
{
    data->eos = FALSE;
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
        data->eos = TRUE;
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
    isDroppable = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED) || GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DECODE_ONLY) ||
        (GST_BUFFER_FLAGS(buffer) == GST_BUFFER_FLAG_DISCONT) ||
        (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT) && GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT)) ||
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
        frame.size = (UINT32) info.size;
        frame.frameData = (PBYTE) info.data;

        MUTEX_LOCK(pSampleConfiguration->sampleConfigurationObjLock);
            for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
                pSampleStreamingSession = pSampleConfiguration->sampleStreamingSessionList[i];
                BOOL isMainstream = ATOMIC_LOAD_BOOL(&pSampleStreamingSession->mainstream);
                if ((trackid == DEFAULT_VIDEO_TRACK_ID && isMainstream) || (trackid == APP_DEFAULT_SUBSTREAM_ID && !isMainstream)) {
                    pRtcRtpTransceiver = pSampleStreamingSession->pVideoRtcRtpTransceiver;
                    frame.index = (UINT32)ATOMIC_INCREMENT(&pSampleStreamingSession->frameIndex);
                    frame.presentationTs = pSampleStreamingSession->videoTimestamp;
                    frame.decodingTs = frame.presentationTs;
                    pSampleStreamingSession->videoTimestamp = buf_pts;
                    status = writeFrame(pRtcRtpTransceiver, &frame);
                    if (STATUS_FAILED(status)) {
                        DLOGD("writeFrame failed with 0x%08x", status);
                    }
                }
            }
        }
        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);

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

GstFlowReturn on_new_sample_video_substream(GstElement* sink, gpointer data)
{
    return on_new_sample(sink, data, APP_DEFAULT_SUBSTREAM_ID);
}

GstFlowReturn on_new_sample_audio(GstElement* sink, gpointer data)
{
    return on_new_sample(sink, data, DEFAULT_AUDIO_TRACK_ID);
}

static void cb_rtsp_pad_created(GstElement* element, GstPad* pad, gpointer data)
{
    gchar* pad_name = gst_pad_get_name(pad);
    GstElement* other = (GstElement*)(data);
    if (gst_element_link(element, other)) {
        g_print("Source linked.\n");
    } else {
        g_print("Source link FAILED\n");
    }
    g_free(pad_name);
}

typedef struct _GstCollection {
    GstElement *appsink, *pipeline, *source, *depay, *filter, *caps;
} GstCollection;

void makeGstPipline(PSampleConfiguration pSampleConfiguration, PCHAR rtspsrc, PCHAR srcName, PCHAR sinkName, GCallback callback, GstCollection* gc)
{
    GstElement *appsink = NULL, *pipeline = NULL, *source = NULL, *depay = NULL, *filter = NULL, *caps = NULL;
    GstStateChangeReturn ret;

    source = gst_element_factory_make("rtspsrc", srcName);
    g_object_set(G_OBJECT(source),
        "location", rtspsrc,
        "short-header", TRUE,
        "protocols", GST_RTSP_LOWER_TRANS_TCP,
        "drop-on-latency", TRUE,
        NULL);

    depay = gst_element_factory_make("rtph264depay", "depay");

    filter = gst_element_factory_make("capsfilter", "encoder_filter");
    GstCaps* h264_caps = gst_caps_new_simple("video/x-h264",
        "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment", G_TYPE_STRING, "au",
        "profile", G_TYPE_STRING, "baseline",
        NULL);

    g_object_set(G_OBJECT(filter), "caps", h264_caps, NULL);
    gst_caps_unref(h264_caps);

    appsink = gst_element_factory_make("appsink", sinkName);
    g_object_set(G_OBJECT(appsink),
        "emit-signals", TRUE,
        "sync", FALSE,
        NULL);

    pipeline = gst_pipeline_new("rtsp-pipeline");

    if (!pipeline || !source || !depay || !filter || !appsink) {
        g_printerr("Not all elements could be created:\n");
        if (!pipeline)
            g_printerr("\tCore pipeline\n");
        if (!source)
            g_printerr("\trtspsrc (gst-plugins-good)\n");
        if (!depay)
            g_printerr("\trtph264depay (gst-plugins-good)\n");
        if (!filter)
            g_printerr("\tcaps filter missing\n");
        if (!appsink)
            g_printerr("\tappsink (gst-plugins-base)\n");
    }

    if (appsink != NULL) {
        g_signal_connect(appsink, "new-sample",
            callback,
            (gpointer)pSampleConfiguration);
    }

    g_signal_connect(source, "pad-added", G_CALLBACK(cb_rtsp_pad_created), depay);

    gst_bin_add_many(GST_BIN(pipeline), source, depay, filter, appsink, NULL);

    if (!gst_element_link_many(depay, filter, appsink, NULL)) {
        g_printerr("Cannot link gstreamer elements");
    }

    ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("Unable to set the pipeline to the playing state.\n");
    }
    gc->pipeline = pipeline;
    gc->source = source;
    gc->depay = depay;
    gc->filter = filter;
    gc->appsink = appsink;
}

PVOID sendGstreamerAudioVideo(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
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
    PCHAR rtspSrcSubstream = GETENV("APP_RTSP_SRC_SUBSTREAM");
    BOOL gstCleaned = FALSE;

    CHK(pSampleConfiguration != NULL, STATUS_NULL_ARG);
    CHK(rtspSrc != NULL, STATUS_NULL_ARG);

    do {
        GstCollection gc0;
        GstCollection gc1;

        memset(&gc0, 0, sizeof(GstCollection));
        memset(&gc1, 0, sizeof(GstCollection));

        makeGstPipline(pSampleConfiguration, rtspSrc, "src0", "mainstream", G_CALLBACK(on_new_sample_video), &gc0);
        makeGstPipline(pSampleConfiguration, rtspSrcSubstream, "src1", "substream", G_CALLBACK(on_new_sample_video_substream), &gc1);

        data.loop = g_main_loop_new(NULL, FALSE);
        g_main_loop_run(data.loop);

        cleanGst(NULL, gc0.pipeline, NULL, NULL, FALSE);
        cleanGst(NULL, gc1.pipeline, NULL, NULL, FALSE);
    } while (APP_GST_ERR_RECOVERY);

CleanUp:

    if (error != NULL) {
        DLOGE("%s", error->message);
        g_clear_error(&error);
    }

    CHK_LOG_ERR(retStatus);
    ATOMIC_STORE_BOOL(&pSampleConfiguration->mediaThreadStarted, FALSE);
    pSampleConfiguration->videoSenderTid = INVALID_TID_VALUE;
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
}

void trampoline(int argc, char** argv)
{
    STATUS retStatus = STATUS_SUCCESS;
    SignalingClientCallbacks signalingClientCallbacks;
    SignalingClientInfo clientInfo;
    PSampleConfiguration pSampleConfiguration = NULL;

    if (!gst_is_initialized()) {
        gst_init(&argc, &argv);
    }

    CHK_STATUS(createSampleConfiguration(argv[1],
        SIGNALING_CHANNEL_ROLE_TYPE_MASTER,
        APP_TRICKLE_ICE, APP_TURN,
        &pSampleConfiguration));

    strcpy(pSampleConfiguration->clientInfo.clientId, SAMPLE_MASTER_CLIENT_ID);

    pSampleConfiguration->videoSource = sendGstreamerAudioVideo;
    pSampleConfiguration->mediaType = SAMPLE_STREAMING_VIDEO_ONLY;

    if (APP_RECEIVE_VIDEO_AUDIO) {
        pSampleConfiguration->receiveAudioVideoSource = receiveGstreamerAudioVideo;
    }

    if (APP_DATA_TRANSFER) {
        pSampleConfiguration->onDataChannel = onDataChannel;
    }

    pSampleConfiguration->customData = (UINT64)pSampleConfiguration;

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

    CHK_STATUS(createSignalingClientSync(
        &pSampleConfiguration->clientInfo,
        &pSampleConfiguration->channelInfo,
        &pSampleConfiguration->signalingClientCallbacks,
        pSampleConfiguration->pCredentialProvider,
        &pSampleConfiguration->signalingClientHandle));
    printf("[KVS GStreamer Master] Signaling client created successfully\n");

    // Enable the processing of the messages
    CHK_STATUS(signalingClientConnectSync(pSampleConfiguration->signalingClientHandle));

    printf("[KVS GStreamer Master] Signaling client connection to socket "
           "established\n");

    printf("[KVS Gstreamer Master] Beginning streaming...check the stream over "
           "channel %s\n",
        argv[1]);

    gSampleConfiguration = pSampleConfiguration;

    // Checking for termination
    CHK_STATUS(sessionCleanupWait(pSampleConfiguration));

    printf("[KVS GStreamer Master] Streaming session terminated\n");

CleanUp:

    printf("[KVS GStreamer Master] Cleaning up....\n");
    CHK_LOG_ERR(retStatus);

    if (pSampleConfiguration != NULL) {
        // Kick of the termination sequence
        ATOMIC_STORE_BOOL(&pSampleConfiguration->appTerminateFlag, TRUE);

        if (IS_VALID_TID_VALUE(pSampleConfiguration->videoSenderTid)) {
            // Join the threads
            THREAD_JOIN(pSampleConfiguration->videoSenderTid, NULL);
        }

        CHK_LOG_ERR(
            freeSignalingClient(&pSampleConfiguration->signalingClientHandle));
        CHK_LOG_ERR(freeSampleConfiguration(&pSampleConfiguration));
    }
    printf("[KVS Gstreamer Master] Cleanup done\n");
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: program channel\n");
        return 0;
    }

    TID logId;
    signal(SIGINT, sigintHandler);

    for (;;) {
        int pid = fork();
        // if child process, do webrtc tasks
        if (pid == 0) {
            prctl(PR_SET_PDEATHSIG, SIGKILL);
            prctl(PR_SET_NAME, "webrtc_worker");

            if (argc >= 3) {
                if (strcmp(argv[2], "nolog") != 0) {
                    THREAD_CREATE(&logId, std2fileLogger, APP_LOG_PATH);
                }
            } else {
               THREAD_CREATE(&logId, std2fileLogger, APP_LOG_PATH);
            }

            trampoline(argc, argv);
            exit(0);
        } else {
            while (wait(NULL) > 0)
                ;
        }
    }

    return 0;
}
