#include <gst/gst.h>
#include <pthread.h>

void gst_start()
{
    GstElement* pipeline = NULL;
    GstBus* bus = NULL;
    GstMessage* msg = NULL;
    gst_init(NULL, NULL);

    char gst_str[1024];
    sprintf(gst_str, 
        "rtspsrc location=%s protocols=tcp async-handling=TRUE short-header=TRUE name=rsrc ! rtph264depay ! "
        "video/x-h264,stream-format=avc,alignment=au ! h264parse ! "
        "kvssink storage-size=12 stream-name=testcamera name=sink "
        "rsrc. ! queue leaky=2 ! rtppcmudepay ! mulawdec ! audioconvert ! avenc_aac perfect-timestamp=TRUE ! sink. ",
        getenv("APP_RTSP_SRC"));

    printf(gst_str);
    pipeline = gst_parse_launch(gst_str, NULL);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    bus = gst_element_get_bus(pipeline);
    msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

    if (msg != NULL) {
        gst_message_unref(msg);
    }

    if (bus != NULL) {
        gst_object_unref(bus);
    }

    if (pipeline != NULL) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }
}

int main(int argc, char* argv[])
{
    pthread_t gst_thread;
    for (;;) {
        pthread_create(&gst_thread, NULL, gst_start, NULL);
        pthread_join(gst_thread, NULL);
    }
    return 0;
}
