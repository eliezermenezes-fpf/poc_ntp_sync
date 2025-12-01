#include <gst/gst.h>
#include <gst/net/gstnet.h>
#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>
#include <atomic>
#include <string>

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

// callback do rtspsrc pad-added (vídeo H.264 RTP)
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
            std::cerr << "[CLIENT] Falha ao linkar rtspsrc -> rtph264depay." << std::endl;
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

    if (argc < 2) {
        std::cerr
            << "Uso: " << argv[0]
            << " <modo> [rtsp_url] [clock_host] [clock_port] [bad_rate_num] [bad_rate_den]\n"
            << "  modo: 'net' (clock de rede), 'sys' (clock local), 'bad' (clock local driftado)\n"
            << "\n"
            << "Parâmetros:\n"
            << "  rtsp_url     = URL RTSP (ex.: rtsp://HOST:8000/stream)\n"
            << "  clock_host   = host/IP do GstNetTimeProvider (normalmente o HOST do RTSP)\n"
            << "  clock_port   = porta UDP do clock (ex.: 3000)\n"
            << "  bad_rate_num/den = (apenas modo 'bad') drift do clock local (default 1001/1000)\n"
            << "\n"
            << "Exemplos:\n"
            << "  " << argv[0] << " net rtsp://127.0.0.1:8000/stream 127.0.0.1 3000\n"
            << "  " << argv[0] << " sys rtsp://127.0.0.1:8000/stream 127.0.0.1 3000\n"
            << "  " << argv[0] << " bad rtsp://127.0.0.1:8000/stream 127.0.0.1 3000 1001 1000\n";
        return 1;
    }

    std::string mode = argv[1];
    if (mode != "net" && mode != "sys" && mode != "bad") {
        std::cerr << "Modo inválido: " << mode << " (use 'net', 'sys' ou 'bad').\n";
        return 1;
    }

    const char* rtsp_url   = (argc > 2) ? argv[2] : "rtsp://127.0.0.1:8000/stream";
    const char* clock_host = (argc > 3) ? argv[3] : "127.0.0.1";
    int clock_port         = (argc > 4) ? std::stoi(argv[4]) : 3000;
    int bad_rate_num       = (argc > 5) ? std::stoi(argv[5]) : 1001;
    int bad_rate_den       = (argc > 6) ? std::stoi(argv[6]) : 1000;

    std::cout << "[CLIENT] Modo: " << mode << std::endl;
    std::cout << "[CLIENT] RTSP URL: " << rtsp_url << std::endl;
    std::cout << "[CLIENT] Clock host: " << clock_host << " porta " << clock_port << std::endl;
    if (mode == "bad") {
        std::cout << "[CLIENT] Drift local (bad_rate_num/den): "
                  << bad_rate_num << "/" << bad_rate_den << std::endl;
    }

    // Clock remoto (server) – sempre para medir net_time
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

    // Clock local base (monotonic)
    GstClock* sys_clock = gst_system_clock_obtain();

    // Clock que o pipeline vai usar
    GstClock* pipeline_clock = nullptr;

    if (mode == "net") {
        pipeline_clock = net_clock;
        std::cout << "[CLIENT] Pipeline usando NET clock (servidor)." << std::endl;
    } else if (mode == "sys") {
        pipeline_clock = sys_clock;
        std::cout << "[CLIENT] Pipeline usando SYS clock (local, sem drift)." << std::endl;
    } else { // "bad"
        // Criamos um clock local "driftado" baseado no system_clock
        // Na prática, vamos calibrar o sys_clock, mas só para o pipeline.
        GstClockTime internal = gst_clock_get_internal_time(sys_clock);
        GstClockTime external = internal; // sem offset, só drift

        gst_clock_set_calibration(
            sys_clock,
            internal,
            external,
            bad_rate_num,
            bad_rate_den
        );

        pipeline_clock = sys_clock;
        std::cout << "[CLIENT] Pipeline usando SYS clock calibrado com drift (modo BAD)." << std::endl;
    }

    gst_pipeline_use_clock(GST_PIPELINE(pipeline), pipeline_clock);

    // Bus + loop
    GstBus* bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_callback, pipeline);
    gst_object_unref(bus);

    main_loop = g_main_loop_new(nullptr, FALSE);
    std::signal(SIGINT, handle_sigint);

    std::atomic<bool> running{true};

    // Thread de sync – UM PRINT por segundo
    std::thread syncThread([&]() {
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            GstClockTime net_t = gst_clock_get_time(net_clock);   // tempo do servidor
            GstClockTime sys_t = gst_clock_get_time(sys_clock);   // tempo local (possivelmente calibrado)
            gdouble net_s = (gdouble)net_t / 1e9;
            gdouble sys_s = (gdouble)sys_t / 1e9;
            gdouble delta = (gdouble)(sys_t - net_t) / 1e9;       // client - server

            gint64 pos = 0;
            gdouble pos_s = 0.0;
            if (gst_element_query_position(pipeline, GST_FORMAT_TIME, &pos)) {
                pos_s = (gdouble)pos / 1e9;
            }

            std::cout << "[SYNC-" << mode << "] net=" << net_s
                      << " s | sys=" << sys_s
                      << " s | delta=" << delta
                      << " s | pos=" << pos_s << " s" << std::endl;
        }
    });

    // PLAYING
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

    // Log de qual clock foi realmente ligado
    {
        GstClock* used_clock = gst_pipeline_get_clock(GST_PIPELINE(pipeline));
        if (used_clock) {
            std::cout << "[CLIENT] Clock real usado no pipeline: "
                      << G_OBJECT_TYPE_NAME(used_clock) << std::endl;
            std::cout << "[CLIENT] É o net_clock? "
                      << (used_clock == net_clock ? "SIM" : "NAO") << std::endl;
            std::cout << "[CLIENT] É o sys_clock? "
                      << (used_clock == sys_clock ? "SIM" : "NAO") << std::endl;
            gst_object_unref(used_clock);
        } else {
            std::cout << "[CLIENT] Pipeline não retornou clock (NULL)." << std::endl;
        }
    }

    std::cout << "[CLIENT] Rodando. Ctrl+C para sair." << std::endl;
    g_main_loop_run(main_loop);

    // Encerrar
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
