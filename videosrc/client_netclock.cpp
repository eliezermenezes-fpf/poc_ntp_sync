#include <gst/gst.h>
#include <gst/net/gstnet.h>
#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>
#include <atomic>

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

    // NetClientClock (clock do servidor visto pelo cliente)
    GstClock* net_clock = gst_net_client_clock_new(
        "net-clock",
        clock_host,
        clock_port,
        0
    );
    if (!net_clock) {
        std::cerr << "[CLIENT] Falha ao criar GstNetClientClock." << std::endl;
        return 1;
    }

    GstClockTime timeout = 5 * GST_SECOND;
    gboolean synced = gst_clock_wait_for_sync(net_clock, timeout);
    if (!synced) {
        std::cerr << "[CLIENT] NetClientClock não sincronizou (timeout)." << std::endl;
        gst_object_unref(net_clock);
        return 1;
    }
    std::cout << "[CLIENT] NetClientClock sincronizado." << std::endl;

    // Pipeline: rtspsrc -> rtph264depay -> avdec_h264 -> videoconvert -> autovideosink
    GstElement* pipeline = gst_pipeline_new("video-client");
    if (!pipeline) {
        std::cerr << "[CLIENT] Falha ao criar pipeline." << std::endl;
        gst_object_unref(net_clock);
        return 1;
    }

    GstElement* src   = gst_element_factory_make("rtspsrc",       "src");
    GstElement* depay = gst_element_factory_make("rtph264depay",  "depay");
    GstElement* dec   = gst_element_factory_make("avdec_h264",    "dec");
    GstElement* conv  = gst_element_factory_make("videoconvert",  "conv");
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

    // Usa o relógio de rede no pipeline
    gst_pipeline_use_clock(GST_PIPELINE(pipeline), net_clock);

    // Bus + loop
    GstBus* bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_callback, pipeline);
    gst_object_unref(bus);

    main_loop = g_main_loop_new(nullptr, FALSE);
    std::signal(SIGINT, handle_sigint);

    std::atomic<bool> running{true};

    // Clock local do sistema (pra comparar com o net_clock)
    GstClock* sys_clock = gst_system_clock_obtain();

    // Thread de sync – UM PRINT por segundo
    std::thread syncThread([&]() {
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            GstClockTime net_t = gst_clock_get_time(net_clock);   // tempo do servidor
            GstClockTime sys_t = gst_clock_get_time(sys_clock);   // tempo local
            gdouble net_s = (gdouble)net_t / 1e9;
            gdouble sys_s = (gdouble)sys_t / 1e9;
            gdouble delta = (gdouble)(sys_t - net_t) / 1e9;       // client - server

            gint64 pos = 0;
            gdouble pos_s = 0.0;
            if (gst_element_query_position(pipeline, GST_FORMAT_TIME, &pos)) {
                pos_s = (gdouble)pos / 1e9;
            }

            std::cout << "[SYNC] net=" << net_s
                      << " s | sys=" << sys_s
                      << " s | delta=" << delta
                      << " s | pos=" << pos_s << " s" << std::endl;
        }
    });

    // Vai para PLAYING
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[CLIENT] Não entrou em PLAYING." << std::endl;
        running = false;
        if (syncThread.joinable()) syncThread.join();

        gst_object_unref(sys_clock);
        gst_object_unref(pipeline);
        gst_object_unref(net_clock);
        g_main_loop_unref(main_loop);
        main_loop = nullptr;
        return 1;
    }

    // Só pra confirmar que o pipeline está usando o net_clock
    {
        GstClock* pipeline_clock = gst_pipeline_get_clock(GST_PIPELINE(pipeline));
        if (pipeline_clock) {
            std::cout << "[CLIENT] Tipo do clock do pipeline: "
                      << G_OBJECT_TYPE_NAME(pipeline_clock) << std::endl;
            std::cout << "[CLIENT] Tipo do net_clock:         "
                      << G_OBJECT_TYPE_NAME(net_clock) << std::endl;
            std::cout << "[CLIENT] Mesmo ponteiro? "
                      << (pipeline_clock == net_clock ? "SIM" : "NAO") << std::endl;
            gst_object_unref(pipeline_clock);
        } else {
            std::cout << "[CLIENT] Pipeline não tem clock (NULL)." << std::endl;
        }
    }

    std::cout << "[CLIENT] Rodando. Ctrl+C para sair." << std::endl;
    g_main_loop_run(main_loop);

    // Encerrando
    running = false;
    if (syncThread.joinable()) syncThread.join();

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(sys_clock);
    gst_object_unref(pipeline);
    gst_object_unref(net_clock);
    g_main_loop_unref(main_loop);
    main_loop = nullptr;

    std::cout << "[CLIENT] Encerrado." << std::endl;
    return 0;
}
