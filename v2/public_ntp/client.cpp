#include <gst/gst.h>
#include <gst/net/gstnet.h>
#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>
#include <atomic>

static GMainLoop* main_loop = nullptr;

// NTP Público - MESMO do servidor!
static const char* NTP_HOST = "a.st1.ntp.br";
static const int NTP_PORT = 123;

// RTSP
static const char* RTSP_URL = "rtsp://127.0.0.1:8555/mystream";

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

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);

    long rtspsrc_latency = (argc > 1) ? std::stol(argv[1]) : 200;
    long jitter_latency  = (argc > 2) ? std::stol(argv[2]) : 200;

    std::cout << "[CLIENT] =========================================" << std::endl;
    std::cout << "[CLIENT] Versão: NTP PÚBLICO" << std::endl;
    std::cout << "[CLIENT] RTSP URI: " << RTSP_URL << std::endl;
    std::cout << "[CLIENT] Sincronizando com NTP: " << NTP_HOST << std::endl;
    std::cout << "[CLIENT] rtspsrc latency: " << rtspsrc_latency << " ms" << std::endl;
    std::cout << "[CLIENT] rtpjitterbuffer latency: " << jitter_latency << " ms" << std::endl;
    std::cout << "[CLIENT] =========================================" << std::endl;

    // Sincroniza com o MESMO NTP público do servidor
    GstClock* ntp_clock = gst_ntp_clock_new("ntp-client", NTP_HOST, NTP_PORT, 0);
    if (!ntp_clock) {
        std::cerr << "[CLIENT] Falha ao criar GstNtpClock." << std::endl;
        return 1;
    }

    if (!gst_clock_wait_for_sync(ntp_clock, 5 * GST_SECOND)) {
        std::cerr << "[CLIENT] NtpClock não sincronizou em 5s." << std::endl;
        gst_object_unref(ntp_clock);
        return 1;
    }
    std::cout << "[CLIENT] ✓ Sincronizado com " << NTP_HOST << std::endl;

    GstClock* sys_clock = gst_system_clock_obtain();

    // Pipeline: rtspsrc -> rtpjitterbuffer -> rtpmp4adepay -> aacparse -> avdec_aac -> audioconvert -> audioresample -> autoaudiosink
    gchar* pipeline_desc = g_strdup_printf(
        "rtspsrc location=\"%s\" name=src latency=%ld ntp-sync=true ntp-time-source=3 buffer-mode=1 "
        "! rtpjitterbuffer latency=%ld "
        "! rtpmp4adepay "
        "! aacparse ! avdec_aac "
        "! audioconvert ! audioresample ! volume volume=1.0 ! autoaudiosink",
        RTSP_URL,
        rtspsrc_latency,
        jitter_latency
    );

    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(pipeline_desc, &error);
    g_free(pipeline_desc);

    if (!pipeline) {
        std::cerr << "[CLIENT] Falha ao criar pipeline." << std::endl;
        if (error) {
            std::cerr << "[CLIENT] Erro: " << error->message << std::endl;
            g_error_free(error);
        }
        gst_object_unref(ntp_clock);
        gst_object_unref(sys_clock);
        return 1;
    }

    // Usar NTP clock no pipeline
    gst_pipeline_use_clock(GST_PIPELINE(pipeline), ntp_clock);
    gst_pipeline_set_latency(GST_PIPELINE(pipeline), 2 * GST_SECOND);

    GstBus* bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_callback, pipeline);
    gst_object_unref(bus);

    main_loop = g_main_loop_new(nullptr, FALSE);
    std::signal(SIGINT, handle_sigint);

    std::atomic<bool> running{true};

    std::thread syncThread([&]() {
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            GstClockTime ntp_t = gst_clock_get_time(ntp_clock);
            GstClockTime sys_t = gst_clock_get_time(sys_clock);
            gdouble ntp_s = (gdouble)ntp_t / 1e9;
            gdouble sys_s = (gdouble)sys_t / 1e9;
            gdouble delta = (gdouble)(sys_t - ntp_t) / 1e9;

            gint64 pos = 0;
            gdouble pos_s = 0.0;
            if (gst_element_query_position(pipeline, GST_FORMAT_TIME, &pos)) {
                pos_s = (gdouble)pos / 1e9;
            }

            std::cout << "[SYNC] ntp=" << ntp_s
                      << " s | sys=" << sys_s
                      << " s | delta=" << delta
                      << " s | pos=" << pos_s << " s" << std::endl;
        }
    });

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[CLIENT] Não entrou em PLAYING." << std::endl;
        running = false;
        if (syncThread.joinable()) syncThread.join();
        gst_object_unref(pipeline);
        gst_object_unref(ntp_clock);
        gst_object_unref(sys_clock);
        g_main_loop_unref(main_loop);
        main_loop = nullptr;
        return 1;
    }

    std::cout << "[CLIENT] Rodando. Ctrl+C para sair." << std::endl;
    g_main_loop_run(main_loop);

    running = false;
    if (syncThread.joinable()) syncThread.join();

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    gst_object_unref(ntp_clock);
    gst_object_unref(sys_clock);
    g_main_loop_unref(main_loop);
    main_loop = nullptr;

    std::cout << "[CLIENT] Encerrado." << std::endl;
    return 0;
}
