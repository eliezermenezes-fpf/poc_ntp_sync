#include <gst/gst.h>
#include <gst/net/gstnet.h>
#include <iostream>
#include <csignal>

static GMainLoop* main_loop = nullptr;

// Clock
static const int CLOCK_PORT = 8557;

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

    if (argc < 2) {
        std::cerr << "Uso: " << argv[0] << " <caminho_audio>\n"
                  << "Exemplo: " << argv[0] << " audio.wav\n";
        return 1;
    }

    const char* audio_path = argv[1];

    std::cout << "[SERVER] =========================================" << std::endl;
    std::cout << "[SERVER] Versão: SERVIDOR NTP CUSTOMIZADO" << std::endl;
    std::cout << "[SERVER] Arquivo de audio: " << audio_path << std::endl;
    std::cout << "[SERVER] Publicando em: " << RTSP_URL << std::endl;
    std::cout << "[SERVER] Servidor é a referência de tempo (system clock)" << std::endl;
    std::cout << "[SERVER] =========================================" << std::endl;

    /* ============================================================================
     * SERVIDOR NTP CUSTOMIZADO
     * ============================================================================
     * AQUI o servidor não sincroniza com NTP externo.
     * Em vez disso, o SERVIDOR se torna a FONTE DE VERDADE do tempo!
     * 
     * COMO FUNCIONA:
     * 1. Usamos o system clock do servidor como referência (gst_system_clock_obtain)
     * 2. Criamos um NetTimeProvider que compartilha este clock via rede (porta 8557 UDP)
     * 3. Clientes se conectam a esta porta e sincronizam COM O SERVIDOR
     * 
     * VANTAGENS:
     * - Precisão máxima (< 1ms em LAN, não há intermediário)
     * - Não depende de internet ou servidores externos
     * - Controle total sobre a referência de tempo
     * 
     * DESVANTAGENS:
     * - Requer que clientes acessem a porta 8557 UDP do servidor
     * - Não funciona bem através de NAT/firewall complexo (use VPN nesses casos)
     * - Limitado a rede local sem configuração adicional
     * 
     * CENÁRIO IDEAL:
     * - Servidor e clientes na mesma LAN/WiFi
     * - OU conectados via VPN
     * ============================================================================ */
    GstClock* master_clock = gst_system_clock_obtain();
    
    /* ============================================================================
     * NetTimeProvider - COMPARTILHANDO O CLOCK VIA REDE
     * ============================================================================
     * 
     * PARÂMETROS:
     * - master_clock: O clock que será compartilhado (system clock do servidor)
     * - "0.0.0.0": Escuta em TODAS as interfaces de rede (WiFi, Ethernet, etc.)
     * - CLOCK_PORT (8557): Porta UDP onde os clientes vão se conectar
     * 
     * O QUE FAZ:
     * - Abre um socket UDP na porta 8557
     * - Quando um cliente envia uma requisição, responde com o tempo atual
     * - Usa protocolo similar ao NTP para compensar latência de rede
     * - Múltiplos clientes podem conectar simultaneamente
     * 
     * IMPORTANTE:
     * - Esta porta precisa estar aberta no firewall
     * - Clientes precisam saber o IP do servidor
     *
     * ============================================================================ */
    GstNetTimeProvider* net_provider = gst_net_time_provider_new(
        master_clock,
        "0.0.0.0",  // Escuta em todas as interfaces
        CLOCK_PORT
    );
    
    if (!net_provider) {
        std::cerr << "[SERVER] Falha ao criar NetTimeProvider." << std::endl;
        gst_object_unref(master_clock);
        return 1;
    }

    std::cout << "[SERVER] ✓ NetTimeProvider ativo na porta " << CLOCK_PORT << " UDP" << std::endl;
    std::cout << "[SERVER] ✓ Clientes devem conectar em: <IP_SERVIDOR>:" << CLOCK_PORT << std::endl;

    /* ============================================================================
     * PIPELINE
     * ============================================================================
     * 
     * filesrc → decodebin → queue → audioconvert → audioresample → avenc_aac → rtspclientsink
     * 
     * A ÚNICA diferença entre servidor público e customizável está no CLOCK usado:
     * - Versão pública: usa NTP clock externo
     * - Versão customizada: usa system clock local (que é compartilhado via NetTimeProvider)
     *
     * ============================================================================ */
    GstElement* pipeline = gst_pipeline_new("audio-server");
    GstElement* filesrc = gst_element_factory_make("filesrc", "filesrc");
    GstElement* decode = gst_element_factory_make("decodebin", "decode");
    GstElement* queue = gst_element_factory_make("queue", "queue");
    GstElement* convert = gst_element_factory_make("audioconvert", "convert");
    GstElement* resample = gst_element_factory_make("audioresample", "resample");
    GstElement* enc = gst_element_factory_make("avenc_aac", "enc");
    GstElement* sink = gst_element_factory_make("rtspclientsink", "sink");

    if (!pipeline || !filesrc || !decode || !queue ||
        !convert || !resample || !enc || !sink) {
        std::cerr << "[SERVER] Falha ao criar elementos." << std::endl;
        if (pipeline) gst_object_unref(pipeline);
        g_object_unref(net_provider);
        gst_object_unref(master_clock);
        return 1;
    }

    g_object_set(filesrc, "location", audio_path, nullptr);
    g_object_set(sink, "location", RTSP_URL, nullptr);

    gst_bin_add_many(GST_BIN(pipeline),
                     filesrc, decode, queue, convert, resample, enc, sink,
                     nullptr);

    if (!gst_element_link(filesrc, decode)) {
        std::cerr << "[SERVER] Falha ao linkar filesrc -> decodebin." << std::endl;
        gst_object_unref(pipeline);
        g_object_unref(net_provider);
        gst_object_unref(master_clock);
        return 1;
    }

    if (!gst_element_link_many(queue, convert, resample, enc, sink, nullptr)) {
        std::cerr << "[SERVER] Falha ao linkar cadeia de áudio." << std::endl;
        gst_object_unref(pipeline);
        g_object_unref(net_provider);
        gst_object_unref(master_clock);
        return 1;
    }

    g_signal_connect(decode, "pad-added", G_CALLBACK(+[](GstElement*, GstPad* new_pad, gpointer data) {
        GstElement* queue = GST_ELEMENT(data);
        GstPad* sinkpad = gst_element_get_static_pad(queue, "sink");

        if (gst_pad_is_linked(sinkpad)) {
            gst_object_unref(sinkpad);
            return;
        }

        GstCaps* caps = gst_pad_get_current_caps(new_pad);
        if (!caps) caps = gst_pad_query_caps(new_pad, nullptr);
        
        if (caps) {
            const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
            if (g_str_has_prefix(name, "audio/")) {
                gst_pad_link(new_pad, sinkpad);
            }
            gst_caps_unref(caps);
        }
        gst_object_unref(sinkpad);
    }), queue);

    /* ============================================================================
     * USANDO O CLOCK DO SERVIDOR (MASTER CLOCK)
     * ============================================================================
     * Aqui configuramos o pipeline para usar o master_clock (system clock).
     * 
     * FLUXO COMPLETO:
     * 1. Pipeline usa master_clock para gerar timestamps
     * 2. NetTimeProvider compartilha este clock na porta 8557
     * 3. Clientes se conectam e obtêm uma cópia sincronizada do clock
     * 4. Clientes interpretam timestamps do stream usando este clock sincronizado
     * 
     * RESULTADO:
     * - Servidor e clientes têm exatamente a mesma noção de tempo
     * - Precisão limitada apenas pela latência da rede local (geralmente < 1ms)
     * ============================================================================ */
    gst_pipeline_use_clock(GST_PIPELINE(pipeline), master_clock);
    gst_pipeline_set_latency(GST_PIPELINE(pipeline), 2 * GST_SECOND);

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
        gst_object_unref(master_clock);
        g_main_loop_unref(main_loop);
        main_loop = nullptr;
        return 1;
    }

    std::cout << "[SERVER] Rodando. Ctrl+C para sair." << std::endl;
    g_main_loop_run(main_loop);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    g_object_unref(net_provider);
    gst_object_unref(master_clock);
    g_main_loop_unref(main_loop);
    main_loop = nullptr;

    std::cout << "[SERVER] Encerrado." << std::endl;
    return 0;
}
