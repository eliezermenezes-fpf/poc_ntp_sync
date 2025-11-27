#include <gst/gst.h>
#include <gst/net/gstnet.h>
#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>

static GMainLoop* main_loop = nullptr;

static gboolean bus_callback(GstBus* bus, GstMessage* msg, gpointer user_data) {
    (void)bus;
    GstElement* pipeline = GST_ELEMENT(user_data);

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError* err = nullptr;
        gchar* dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        std::cerr << "[CLIENT] ERRO: "
                  << (err ? err->message : "desconhecido") << std::endl;
        if (dbg) {
            std::cerr << "[CLIENT] Debug: " << dbg << std::endl;
            g_free(dbg);
        }
        if (err) g_error_free(err);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        if (main_loop) g_main_loop_quit(main_loop);
        break;
    }
    case GST_MESSAGE_EOS:
        std::cout << "[CLIENT] EOS." << std::endl;
        gst_element_set_state(pipeline, GST_STATE_NULL);
        if (main_loop) g_main_loop_quit(main_loop);
        break;
    case GST_MESSAGE_STATE_CHANGED:
        if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
            GstState old_s, new_s, pending;
            gst_message_parse_state_changed(msg, &old_s, &new_s, &pending);
            std::cout << "[CLIENT] Estado: "
                      << gst_element_state_get_name(old_s) << " -> "
                      << gst_element_state_get_name(new_s) << std::endl;
        }
        break;
    default:
        break;
    }
    return TRUE;
}

static void handle_sigint(int) {
    if (main_loop) {
        std::cout << "[CLIENT] SIGINT recebido, saindo..." << std::endl;
        g_main_loop_quit(main_loop);
    }
}

// callback pra rtspsrc pad-added (vídeo H.264)
static void on_pad_added(GstElement* src, GstPad* new_pad, gpointer user_data) {
    (void)src;
    GstElement* depay = GST_ELEMENT(user_data);
    GstPad* sinkpad = gst_element_get_static_pad(depay, "sink");

    if (gst_pad_is_linked(sinkpad)) {
        gst_object_unref(sinkpad);
        return;
    }

    GstCaps* caps = gst_pad_get_current_caps(new_pad);
    if (!caps) {
        caps = gst_pad_query_caps(new_pad, nullptr);
    }
    if (!caps) {
        std::cerr << "[CLIENT] Não conseguiu caps do pad." << std::endl;
        gst_object_unref(sinkpad);
        return;
    }

    const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    std::cout << "[CLIENT] Novo pad em rtspsrc: " << name << std::endl;

    if (g_str_has_prefix(name, "application/x-rtp")) {
        // assumindo que é o vídeo H.264 do nosso stream
        if (gst_pad_link(new_pad, sinkpad) != GST_PAD_LINK_OK) {
            std::cerr << "[CLIENT] Falha ao linkar rtspsrc -> depay." << std::endl;
        } else {
            std::cout << "[CLIENT] rtspsrc ligado em rtph264depay." << std::endl;
        }
    } else {
        std::cout << "[CLIENT] Ignorando pad não-RTP (" << name << ")." << std::endl;
    }

    gst_caps_unref(caps);
    gst_object_unref(sinkpad);
}

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);

    const char* rtsp_url   = (argc > 1) ? argv[1] : "rtsp://127.0.0.1:8555/mystream";
    const char* clock_host = (argc > 2) ? argv[2] : "127.0.0.1";
    int clock_port         = (argc > 3) ? std::stoi(argv[3]) : 8557;

    std::cout << "[CLIENT] RTSP URL: " << rtsp_url << std::endl;
    std::cout << "[CLIENT] Net clock: " << clock_host << ":" << clock_port << std::endl;

    // 1) NetClientClock
    GstClock* net_clock = gst_net_client_clock_new(
        "net-clock",
        clock_host,
        clock_port,
        0    // base-time local
    );
    if (!net_clock) {
        std::cerr << "[CLIENT] Falha ao criar GstNetClientClock." << std::endl;
        return 1;
    }

    // Espera sincronizar (5s)
    GstClockTime timeout = 5 * GST_SECOND;
    gboolean synced = gst_clock_wait_for_sync(net_clock, timeout);
    if (!synced) {
        std::cerr << "[CLIENT] NetClientClock não sincronizou (timeout)." << std::endl;
        gst_object_unref(net_clock);
        return 1;
    }
    std::cout << "[CLIENT] NetClientClock sincronizado." << std::endl;

    // 2) Pipeline: rtspsrc -> rtph264depay -> avdec_h264 -> videoconvert -> autovideosink
    GstElement* pipeline = gst_pipeline_new("video-client");
    if (!pipeline) {
        std::cerr << "[CLIENT] Falha ao criar pipeline." << std::endl;
        gst_object_unref(net_clock);
        return 1;
    }

    GstElement* src   = gst_element_factory_make("rtspsrc", "src");
    GstElement* depay = gst_element_factory_make("rtph264depay", "depay");
    GstElement* dec   = gst_element_factory_make("avdec_h264", "dec");
    GstElement* conv  = gst_element_factory_make("videoconvert", "conv");
    GstElement* sink  = gst_element_factory_make("autovideosink", "sink");

    if (!src || !depay || !dec || !conv || !sink) {
        std::cerr << "[CLIENT] Falha ao criar elementos." << std::endl;
        if (pipeline) gst_object_unref(pipeline);
        gst_object_unref(net_clock);
        return 1;
    }

    g_object_set(src,
                 "location", rtsp_url,
                 NULL);

    gst_bin_add_many(GST_BIN(pipeline),
                     src, depay, dec, conv, sink,
                     NULL);

    if (!gst_element_link_many(depay, dec, conv, sink, NULL)) {
        std::cerr << "[CLIENT] Falha ao linkar cadeia de vídeo." << std::endl;
        gst_object_unref(pipeline);
        gst_object_unref(net_clock);
        return 1;
    }

    g_signal_connect(src, "pad-added", G_CALLBACK(on_pad_added), depay);

    // 3) Usa o clock de rede no pipeline
    gst_pipeline_use_clock(GST_PIPELINE(pipeline), net_clock);
    gst_element_set_start_time(pipeline, GST_CLOCK_TIME_NONE);

    // 4) Bus + loop
    GstBus* bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_callback, pipeline);
    gst_object_unref(bus);

    main_loop = g_main_loop_new(nullptr, FALSE);
    std::signal(SIGINT, handle_sigint);

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[CLIENT] Não entrou em PLAYING." << std::endl;
        gst_object_unref(pipeline);
        gst_object_unref(net_clock);
        g_main_loop_unref(main_loop);
        return 1;
    }

    // Thread opcional pra logar posição
    std::thread posThread([pipeline]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            GstState state;
            gst_element_get_state(pipeline, &state, nullptr, 0);
            if (state != GST_STATE_PLAYING) break;
            gint64 pos = 0;
            if (gst_element_query_position(pipeline, GST_FORMAT_TIME, &pos)) {
                std::cout << "[CLIENT] Posição ~ " << (pos / 1e9) << " s" << std::endl;
            }
        }
    });

    std::cout << "[CLIENT] Rodando. Ctrl+C para sair." << std::endl;
    g_main_loop_run(main_loop);

    if (posThread.joinable()) posThread.join();

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    gst_object_unref(net_clock);
    g_main_loop_unref(main_loop);
    main_loop = nullptr;

    std::cout << "[CLIENT] Encerrado." << std::endl;
    return 0;
}
