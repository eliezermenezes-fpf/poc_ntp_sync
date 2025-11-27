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

// callback p/ rtspsrc pad-added
static void on_pad_added(GstElement* src, GstPad* new_pad, gpointer user_data) {
    (void)src;
    GstElement* depay = GST_ELEMENT(user_data);
    GstPad* sinkpad = gst_element_get_static_pad(depay, "sink");

    if (gst_pad_is_linked(sinkpad)) {
        gst_object_unref(sinkpad);
        return;
    }

    GstCaps* caps = gst_pad_get_current_caps(new_pad);
    gchar* desc = gst_caps_to_string(caps);
    std::cout << "[CLIENT] Novo pad do rtspsrc: " << desc << std::endl;
    g_free(desc);
    gst_caps_unref(caps);

    if (gst_pad_link(new_pad, sinkpad) != GST_PAD_LINK_OK) {
        std::cerr << "[CLIENT] Falha ao linkar pad do rtspsrc." << std::endl;
    }
    gst_object_unref(sinkpad);
}

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);

    // RTSP + clock host
    const char* rtsp_url   = (argc > 1) ? argv[1] : "rtsp://127.0.0.1:8555/mystream";
    const char* clock_host = (argc > 2) ? argv[2] : "127.0.0.1";
    int clock_port         = (argc > 3) ? std::stoi(argv[3]) : 8557;

    std::cout << "[CLIENT] RTSP URL: " << rtsp_url << std::endl;
    std::cout << "[CLIENT] Net clock: " << clock_host << ":" << clock_port << std::endl;

    // 1) Cria o NetClientClock
    GstClock* net_clock = gst_net_client_clock_new(
        "net-clock",
        clock_host,
        clock_port,
        0    // base-time local (normalmente 0)
    );
    if (!net_clock) {
        std::cerr << "[CLIENT] Falha ao criar GstNetClientClock." << std::endl;
        return 1;
    }

    // Espera sincronizar (timeout 5 segundos)
    GstClockTime timeout = 5 * GST_SECOND;
    gboolean synced = gst_clock_wait_for_sync(net_clock, timeout);
    if (!synced) {
        std::cerr << "[CLIENT] NetClientClock não sincronizou (timeout)." << std::endl;
        gst_object_unref(net_clock);
        return 1;
    }
    std::cout << "[CLIENT] NetClientClock sincronizado." << std::endl;

    // 2) Cria pipeline de áudio (rtspsrc -> rtpopusdepay -> opusdec -> audioconvert -> audioresample -> autoaudiosink)
    GstElement* pipeline = gst_pipeline_new("mobile-player");
    if (!pipeline) {
        std::cerr << "[CLIENT] Falha ao criar pipeline." << std::endl;
        gst_object_unref(net_clock);
        return 1;
    }

    GstElement* src   = gst_element_factory_make("rtspsrc", "src");
    GstElement* depay = gst_element_factory_make("rtpopusdepay", "depay");
    GstElement* dec   = gst_element_factory_make("opusdec", "dec");
    GstElement* conv  = gst_element_factory_make("audioconvert", "conv");
    GstElement* res   = gst_element_factory_make("audioresample", "res");
    GstElement* sink  = gst_element_factory_make("autoaudiosink", "sink");

    if (!src || !depay || !dec || !conv || !res || !sink) {
        std::cerr << "[CLIENT] Falha ao criar elementos do pipeline." << std::endl;
        if (pipeline) gst_object_unref(pipeline);
        gst_object_unref(net_clock);
        return 1;
    }

    g_object_set(src,
                 "location", rtsp_url,
                 // aqui, como estamos usando um clock externo, podemos
                 // manter ntp-sync/rtcp-sync false (default).
                 NULL);

    gst_bin_add_many(GST_BIN(pipeline),
                     src, depay, dec, conv, res, sink,
                     NULL);

    if (!gst_element_link_many(depay, dec, conv, res, sink, NULL)) {
        std::cerr << "[CLIENT] Falha ao linkar cadeia de áudio." << std::endl;
        gst_object_unref(pipeline);
        gst_object_unref(net_clock);
        return 1;
    }

    g_signal_connect(src, "pad-added", G_CALLBACK(on_pad_added), depay);

    // 3) Força o pipeline a usar o NetClientClock
    gst_pipeline_use_clock(GST_PIPELINE(pipeline), net_clock);
    // live pipeline: start_time NONE
    gst_element_set_start_time(pipeline, GST_CLOCK_TIME_NONE);

    // 4) Bus e main loop
    GstBus* bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_callback, pipeline);
    gst_object_unref(bus);

    main_loop = g_main_loop_new(nullptr, FALSE);
    std::signal(SIGINT, handle_sigint);

    // 5) Estado PLAYING
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

    // cleanup
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    gst_object_unref(net_clock);
    g_main_loop_unref(main_loop);
    main_loop = nullptr;

    std::cout << "[CLIENT] Encerrado." << std::endl;
    return 0;
}
