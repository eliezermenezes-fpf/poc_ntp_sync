#include <gst/gst.h>
#include <gst/net/gstnet.h>
#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>
#include <atomic>

static GMainLoop* main_loop = nullptr;

// Clock
static const char* SERVER_IP = "127.0.0.1";
static const int SERVER_PORT = 8557;

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
    std::cout << "[CLIENT] Versão: SERVIDOR NTP CUSTOMIZADO" << std::endl;
    std::cout << "[CLIENT] RTSP URI: " << RTSP_URL << std::endl;
    std::cout << "[CLIENT] Conectando ao servidor: " << SERVER_IP << ":" << SERVER_PORT << std::endl;
    std::cout << "[CLIENT] rtspsrc latency: " << rtspsrc_latency << " ms" << std::endl;
    std::cout << "[CLIENT] rtpjitterbuffer latency: " << jitter_latency << " ms" << std::endl;
    std::cout << "[CLIENT] =========================================" << std::endl;

    /* ============================================================================
     * NetClientClock - CONECTANDO AO SERVIDOR CUSTOMIZADO
     * ============================================================================
     * Em vez de GstNtpClock (que conecta a servidores NTP públicos), usamos
     * GstNetClientClock que conecta ao NetTimeProvider do nosso servidor.
     * 
     * DIFERENÇA CHAVE:
     * - GstNtpClock: usa protocolo NTP padrão, conecta a ntp.br, pool.ntp.org, etc.
     * - GstNetClientClock: usa protocolo GStreamer, conecta ao nosso servidor
     * 
     * PARÂMETROS:
     * - "net-client-clock": nome do clock (arbitrário)
     * - SERVER_IP: IP do servidor onde está rodando o NetTimeProvider
     * - SERVER_PORT: porta onde NetTimeProvider está escutando (8557)
     * - 0: base time (0 = usar tempo atual)
     * 
     * O QUE FAZ:
     * 1. Abre conexão UDP com o servidor na porta 8557
     * 2. Envia requisições periódicas para obter o tempo do servidor
     * 3. Compensa latência de rede automaticamente
     * 4. Mantém sincronização contínua
     * 
     * IMPORTANTE:
     * - SERVER_IP deve ser o IP real do servidor
     * - Porta 8557 UDP deve estar acessível
     * - Uso de VPN ou port forwarding caso esteja em rede diferente
     * ============================================================================ */
    GstClock* net_clock = gst_net_client_clock_new(
        "net-client-clock",
        SERVER_IP,
        SERVER_PORT,
        0
    );
    
    if (!net_clock) {
        std::cerr << "[CLIENT] Falha ao criar NetClientClock." << std::endl;
        return 1;
    }

    // Aguarda sincronização (pode demorar alguns segundos na primeira vez)
    if (!gst_clock_wait_for_sync(net_clock, 5 * GST_SECOND)) {
        std::cerr << "[CLIENT] NetClientClock não sincronizou em 5s." << std::endl;
        std::cerr << "[CLIENT] Verifique se o servidor está rodando em " 
                  << SERVER_IP << ":" << SERVER_PORT << std::endl;
        gst_object_unref(net_clock);
        return 1;
    }
    std::cout << "[CLIENT] ✓ Sincronizado com servidor!" << std::endl;

    GstClock* sys_clock = gst_system_clock_obtain();

    /* ============================================================================
     * PIPELINE DE RECEPÇÃO
     * ============================================================================
     * O pipeline é exatamente o mesmo da versão public_ntp.
     * A única diferença é qual CLOCK estamos usando:
     * 
     * - Versão pública: usa GstNtpClock (servidor NTP externo)
     * - Versão customizada: usa GstNetClientClock (conecta ao nosso servidor)
     * ============================================================================ */
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
        gst_object_unref(net_clock);
        gst_object_unref(sys_clock);
        return 1;
    }

    /* ============================================================================
     * APLICAR O CLOCK SINCRONIZADO AO PIPELINE
     * ============================================================================
     * Usamos o net_clock (que está sincronizado com o servidor via NetTimeProvider) no pipeline.
     * 
     * FLUXO DA SINCRONIZAÇÃO:
     * 
     * SERVIDOR:
     * 1. Usa system clock para timestamps do stream
     * 2. NetTimeProvider compartilha este clock na porta 8557
     * 
     * CLIENTE:
     * 3. NetClientClock conecta ao servidor e sincroniza
     * 4. Pipeline usa este clock sincronizado
     * 5. Timestamps do stream fazem sentido porque estão na mesma "escala de tempo"
     * 
     * VANTAGEM:
     * - Servidor e cliente têm praticamente o MESMO tempo (< 1ms de diferença em LAN)
     * - Muito mais preciso que NTP público (que pode ter 50-200ms de diferença)
     * 
     * MONITORAMENTO:
     * - A thread abaixo mostra o delta entre net_clock e sys_clock
     * - Em LAN, espera-se delta < 0.001s (1ms)
     * ============================================================================ */
    gst_pipeline_use_clock(GST_PIPELINE(pipeline), net_clock);
    gst_pipeline_set_latency(GST_PIPELINE(pipeline), 2 * GST_SECOND);

    GstBus* bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_callback, pipeline);
    gst_object_unref(bus);

    main_loop = g_main_loop_new(nullptr, FALSE);
    std::signal(SIGINT, handle_sigint);

    std::atomic<bool> running{true};

    /* ============================================================================
     * THREAD DE MONITORAMENTO
     * ============================================================================
     * Mostra a cada 1 segundo:
     * - net: tempo do clock sincronizado com o servidor
     * - sys: tempo do system clock local
     * - delta: diferença entre eles (deve ser muito pequeno em LAN!)
     * - pos: posição da reprodução
     * 
     * VALORES ESPERADOS (LAN):
     * - delta: entre -0.001 e 0.001 (< 1ms) - MUITO MELHOR que NTP público!
     * ============================================================================ */
    std::thread syncThread([&]() {
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            GstClockTime net_t = gst_clock_get_time(net_clock);
            GstClockTime sys_t = gst_clock_get_time(sys_clock);
            gdouble net_s = (gdouble)net_t / 1e9;
            gdouble sys_s = (gdouble)sys_t / 1e9;
            gdouble delta = (gdouble)(sys_t - net_t) / 1e9;

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

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[CLIENT] Não entrou em PLAYING." << std::endl;
        running = false;
        if (syncThread.joinable()) syncThread.join();
        gst_object_unref(pipeline);
        gst_object_unref(net_clock);
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
    gst_object_unref(net_clock);
    gst_object_unref(sys_clock);
    g_main_loop_unref(main_loop);
    main_loop = nullptr;

    std::cout << "[CLIENT] Encerrado." << std::endl;
    return 0;
}
