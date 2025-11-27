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
        std::cout << "[SERVER] EOS." << std::endl;
        gst_element_set_state(pipeline, GST_STATE_NULL);
        if (main_loop) g_main_loop_quit(main_loop);
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

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);

    // RTSP URL (mediaMTX)
    const char* rtsp_url = (argc > 1)
        ? argv[1]
        : "rtsp://127.0.0.1:8555/mystream";

    // Porta do serviço de clock
    int clock_port = 8557;
    if (argc > 2) {
        clock_port = std::stoi(argv[2]);
    }

    std::cout << "[SERVER] Publicando áudio em: " << rtsp_url << std::endl;
    std::cout << "[SERVER] Servindo clock em UDP porta: " << clock_port << std::endl;

    // 1) Clock mestre (pode ser system clock)
    GstClock* clock = gst_system_clock_obtain();

    // 2) Serviço de clock de rede
    //    endereço "0.0.0.0" = escuta em todas as interfaces
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

    // 3) Pipeline de áudio -> RTSP
    GstElement* pipeline = gst_pipeline_new("service-player");

    GstElement* src      = gst_element_factory_make("audiotestsrc", "src");
    GstElement* convert  = gst_element_factory_make("audioconvert", "convert");
    GstElement* resample = gst_element_factory_make("audioresample", "resample");
    GstElement* enc      = gst_element_factory_make("opusenc", "enc");
    GstElement* sink     = gst_element_factory_make("rtspclientsink", "sink");

    if (!pipeline || !src || !convert || !resample || !enc || !sink) {
        std::cerr << "[SERVER] Falha ao criar elementos do pipeline." << std::endl;
        if (pipeline) gst_object_unref(pipeline);
        gst_object_unref(clock);
        g_object_unref(net_provider);
        return 1;
    }

    g_object_set(src,
                 "is-live", TRUE,
                 "wave", 0,  // seno, só pra ter som
                 NULL);

    g_object_set(sink,
                 "location", rtsp_url,
                 NULL);

    gst_bin_add_many(GST_BIN(pipeline),
                     src, convert, resample, enc, sink,
                     NULL);

    if (!gst_element_link_many(src, convert, resample, enc, sink, NULL)) {
        std::cerr << "[SERVER] Falha ao linkar elementos." << std::endl;
        gst_object_unref(pipeline);
        gst_object_unref(clock);
        g_object_unref(net_provider);
        return 1;
    }

    // 4) Usa o MESMO clock do NetTimeProvider no pipeline
    gst_pipeline_use_clock(GST_PIPELINE(pipeline), clock);

    // (Opcional, mas comum p/ live) deixa o start_time em NONE
    gst_element_set_start_time(pipeline, GST_CLOCK_TIME_NONE);

    // 5) Bus + main loop
    GstBus* bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_callback, pipeline);
    gst_object_unref(bus);

    main_loop = g_main_loop_new(nullptr, FALSE);
    std::signal(SIGINT, handle_sigint);

    // 6) PLAYING
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[SERVER] Não entrou em PLAYING." << std::endl;
        gst_object_unref(pipeline);
        gst_object_unref(clock);
        g_object_unref(net_provider);
        g_main_loop_unref(main_loop);
        return 1;
    }

    std::cout << "[SERVER] Rodando. Ctrl+C para sair." << std::endl;
    g_main_loop_run(main_loop);

    // cleanup
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    g_object_unref(net_provider); // para o serviço de clock
    gst_object_unref(clock);

    g_main_loop_unref(main_loop);
    main_loop = nullptr;

    std::cout << "[SERVER] Encerrado." << std::endl;
    return 0;
}
