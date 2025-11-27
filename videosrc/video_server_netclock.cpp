#include <gst/gst.h>
#include <gst/net/gstnet.h>
#include <iostream>
#include <csignal>

static GMainLoop* main_loop = nullptr;

static gboolean bus_callback(GstBus* bus, GstMessage* msg, gpointer user_data) {
    (void)bus;
    GstElement* pipeline = GST_ELEMENT(user_data);

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError* err = nullptr;
        gchar* dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        std::cerr << "[SERVER] ERRO: "
                  << (err ? err->message : "desconhecido") << std::endl;
        if (dbg) {
            std::cerr << "[SERVER] Debug: " << dbg << std::endl;
            g_free(dbg);
        }
        if (err) g_error_free(err);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        if (main_loop) g_main_loop_quit(main_loop);
        break;
    }
    case GST_MESSAGE_EOS:
        std::cout << "[SERVER] EOS (fim do arquivo)." << std::endl;

        // Se quiser loopar o vídeo:
        if (!gst_element_seek_simple(pipeline,
                                     GST_FORMAT_TIME,
                                     (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                                     0)) {
            std::cerr << "[SERVER] Falha ao fazer seek pra loop, encerrando." << std::endl;
            gst_element_set_state(pipeline, GST_STATE_NULL);
            if (main_loop) g_main_loop_quit(main_loop);
        } else {
            std::cout << "[SERVER] Loopando vídeo (seek 0)." << std::endl;
        }
        break;
    case GST_MESSAGE_STATE_CHANGED:
        if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
            GstState old_s, new_s, pending;
            gst_message_parse_state_changed(msg, &old_s, &new_s, &pending);
            std::cout << "[SERVER] Estado: "
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
        std::cout << "[SERVER] SIGINT recebido, saindo..." << std::endl;
        g_main_loop_quit(main_loop);
    }
}

// decodebin → escolhemos só o vídeo
static void on_decodebin_pad_added(GstElement* decodebin, GstPad* new_pad, gpointer user_data) {
    (void)decodebin;
    GstElement* queue = GST_ELEMENT(user_data);

    GstPad* sinkpad = gst_element_get_static_pad(queue, "sink");
    if (gst_pad_is_linked(sinkpad)) {
        gst_object_unref(sinkpad);
        return;
    }

    GstCaps* caps = gst_pad_get_current_caps(new_pad);
    if (!caps) {
        caps = gst_pad_query_caps(new_pad, nullptr);
    }
    if (!caps) {
        std::cerr << "[SERVER] Não foi possível obter caps do pad novo." << std::endl;
        gst_object_unref(sinkpad);
        return;
    }

    const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    std::cout << "[SERVER] Novo pad em decodebin: " << name << std::endl;

    if (g_str_has_prefix(name, "video/")) {
        if (gst_pad_link(new_pad, sinkpad) != GST_PAD_LINK_OK) {
            std::cerr << "[SERVER] Falha ao linkar decodebin -> queue." << std::endl;
        } else {
            std::cout << "[SERVER] decodebin (vídeo) linkado em queue." << std::endl;
        }
    } else {
        std::cout << "[SERVER] Ignorando pad não-vídeo (" << name << ")." << std::endl;
    }

    gst_caps_unref(caps);
    gst_object_unref(sinkpad);
}

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);

    if (argc < 2) {
        std::cerr << "Uso: " << argv[0]
                  << " <caminho_video> [rtsp_url] [clock_port]\n"
                  << "Exemplo: " << argv[0]
                  << " video.mp4 rtsp://0.0.0.0:8555/mystream 8557\n";
        return 1;
    }

    const char* video_path = argv[1];
    const char* rtsp_url   = (argc > 2) ? argv[2] : "rtsp://127.0.0.1:8555/mystream";
    int clock_port         = (argc > 3) ? std::stoi(argv[3]) : 8557;

    std::cout << "[SERVER] Arquivo de vídeo: " << video_path << std::endl;
    std::cout << "[SERVER] Publicando em: " << rtsp_url << std::endl;
    std::cout << "[SERVER] Servindo clock em UDP porta: " << clock_port << std::endl;

    // 1) Clock mestre
    GstClock* clock = gst_system_clock_obtain();

    // 2) NetTimeProvider
    GstNetTimeProvider* net_provider = gst_net_time_provider_new(
        clock,
        "0.0.0.0",
        clock_port
    );
    if (!net_provider) {
        std::cerr << "[SERVER] Falha ao criar GstNetTimeProvider." << std::endl;
        gst_object_unref(clock);
        return 1;
    }

    // 3) Pipeline
    GstElement* pipeline = gst_pipeline_new("video-file-service");

    GstElement* filesrc  = gst_element_factory_make("filesrc", "filesrc");
    GstElement* decode   = gst_element_factory_make("decodebin", "decode");
    // GstElement* queue    = gst_element_factory_make("queue", "queue");
    // GstElement* convert  = gst_element_factory_make("videoconvert", "convert");
    // GstElement* scale    = gst_element_factory_make("videoscale", "scale");
    // GstElement* enc      = gst_element_factory_make("x264enc", "enc");
    // GstElement* sink     = gst_element_factory_make("rtspclientsink", "sink");
    GstElement* queue    = gst_element_factory_make("queue", "queue");
    GstElement* convert  = gst_element_factory_make("videoconvert", "convert");
    GstElement* scale    = gst_element_factory_make("videoscale", "scale");
    GstElement* ident    = gst_element_factory_make("identity", "pacer");
    GstElement* enc      = gst_element_factory_make("x264enc", "enc");
    GstElement* sink     = gst_element_factory_make("rtspclientsink", "sink");

    if (!pipeline || !filesrc || !decode || !queue ||
        !convert || !scale || !ident || !enc || !sink) {
        std::cerr << "[SERVER] Falha ao criar elementos." << std::endl;
        if (pipeline) gst_object_unref(pipeline);
        g_object_unref(net_provider);
        gst_object_unref(clock);
        return 1;
    }

    g_object_set(filesrc,
                 "location", video_path,
                 NULL);

    // <<< AQUI FOI CORRIGIDO >>>
    g_object_set(enc,
                 "bitrate", 800,  // kbps
                 NULL);

    // identity que faz pacing em tempo real
    g_object_set(ident,
             "sync", TRUE,
             NULL);

    g_object_set(sink,
                 "location", rtsp_url,
                 NULL);

    gst_bin_add_many(GST_BIN(pipeline),
                 filesrc, decode, queue, convert, scale, ident, enc, sink,
                 NULL);

    if (!gst_element_link(filesrc, decode)) {
        std::cerr << "[SERVER] Falha ao linkar filesrc -> decodebin." << std::endl;
        gst_object_unref(pipeline);
        g_object_unref(net_provider);
        gst_object_unref(clock);
        return 1;
    }

    if (!gst_element_link_many(queue, convert, scale, ident, enc, sink, NULL)) {
        std::cerr << "[SERVER] Falha ao linkar cadeia de vídeo." << std::endl;
        gst_object_unref(pipeline);
        g_object_unref(net_provider);
        gst_object_unref(clock);
        return 1;
    }

    g_signal_connect(decode, "pad-added", G_CALLBACK(on_decodebin_pad_added), queue);

    // 4) Usa o mesmo clock que estamos servindo via rede
    gst_pipeline_use_clock(GST_PIPELINE(pipeline), clock);
    // gst_element_set_start_time(pipeline, GST_CLOCK_TIME_NONE);

    // 5) Bus + loop
    GstBus* bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_callback, pipeline);
    gst_object_unref(bus);

    main_loop = g_main_loop_new(nullptr, FALSE);
    std::signal(SIGINT, handle_sigint);

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[SERVER] Não entrou em PLAYING." << std::endl;
        gst_object_unref(pipeline);
        g_object_unref(net_provider);
        gst_object_unref(clock);
        g_main_loop_unref(main_loop);
        return 1;
    }

    std::cout << "[SERVER] Rodando. Ctrl+C para sair." << std::endl;
    g_main_loop_run(main_loop);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    g_object_unref(net_provider);
    gst_object_unref(clock);
    g_main_loop_unref(main_loop);
    main_loop = nullptr;

    std::cout << "[SERVER] Encerrado." << std::endl;
    return 0;
}
