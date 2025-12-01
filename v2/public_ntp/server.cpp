#include <gst/gst.h>
#include <gst/net/gstnet.h>
#include <iostream>
#include <csignal>

static GMainLoop* main_loop = nullptr;

// NTP Público - Referência externa compartilhada
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
                  << "Exemplo: " << argv[0] << " musica.mp3\n";
        return 1;
    }

    const char* audio_path = argv[1];

    std::cout << "[SERVER] =========================================" << std::endl;
    std::cout << "[SERVER] Versão: NTP PÚBLICO" << std::endl;
    std::cout << "[SERVER] Arquivo de audio: " << audio_path << std::endl;
    std::cout << "[SERVER] Publicando em: " << RTSP_URL << std::endl;
    std::cout << "[SERVER] Sincronizando com NTP: " << NTP_HOST << std::endl;
    std::cout << "[SERVER] =========================================" << std::endl;

    /* ============================================================================
     * SINCRONIZAÇÃO COM NTP PÚBLICO - CONCEITO CHAVE
     * ============================================================================
     * Esta versão usa um servidor NTP EXTERNO (a.st1.ntp.br) como referência de tempo.
     * 
     * COMO FUNCIONA:
     * - Tanto o servidor quanto o cliente se conectam ao MESMO servidor NTP público
     * - Ambos obtêm timestamps baseados nessa referência externa compartilhada
     * - Isso garante que estejam "na mesma página" em termos de tempo
     * 
     * VANTAGENS:
     * - Funciona mesmo que servidor e cliente estejam em redes diferentes
     * - Não precisa abrir portas no firewall do servidor
     * - Múltiplos servidores podem sincronizar entre si
     * 
     * DESVANTAGENS:
     * - Precisão limitada (~50-200ms dependendo da distância do servidor NTP)
     * - Depende de acesso à internet
     * - Latência variável
     * 
     * IMPORTANTE: O cliente DEVE usar o MESMO servidor NTP (a.st1.ntp.br)
     * ============================================================================ */
    GstClock* ntp_clock = gst_ntp_clock_new("ntp-server", NTP_HOST, NTP_PORT, 0);
    if (!ntp_clock) {
        std::cerr << "[SERVER] Falha ao criar GstNtpClock." << std::endl;
        return 1;
    }

    // Aguarda até que o clock sincronize com o servidor NTP (timeout de 5 segundos)
    if (!gst_clock_wait_for_sync(ntp_clock, 5 * GST_SECOND)) {
        std::cerr << "[SERVER] NtpClock não sincronizou em 5s." << std::endl;
        gst_object_unref(ntp_clock);
        return 1;
    }
    std::cout << "[SERVER] ✓ Sincronizado com " << NTP_HOST << std::endl;

    /* ============================================================================
     * PIPELINE DE ÁUDIO - ESTRUTURA MANUAL
     * ============================================================================
     * Criamos cada elemento individualmente para ter controle total sobre o fluxo e o clock.
     * 
     * FLUXO DO PIPELINE:
     * 
     * filesrc: Lê o arquivo de áudio do disco
     *    ↓
     * decodebin: Decodifica automaticamente qualquer formato (mp3, wav, flac, etc.)
     *    ↓
     * queue: Buffer para evitar bloqueios entre decodificação e processamento
     *    ↓
     * audioconvert: Converte o áudio para formato padrão (necessário para AAC)
     *    ↓
     * audioresample: Ajusta a taxa de amostragem para o que o encoder AAC aceita
     *    ↓
     * avenc_aac: Codifica o áudio em AAC (formato eficiente para streaming)
     *    ↓
     * rtspclientsink: Publica o áudio AAC em um servidor RTSP (mediamtx)
     * 
     * PONTO CRUCIAL:
     * rtspclientsink espera receber dados JÁ CODIFICADOS (AAC, não RTP!).
     * Ele mesmo faz o empacotamento RTP internamente. Por isso usamos avenc_aac
     * ANTES do sink, não um payloader RTP.
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
        gst_object_unref(ntp_clock);
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
        gst_object_unref(ntp_clock);
        return 1;
    }

    if (!gst_element_link_many(queue, convert, resample, enc, sink, nullptr)) {
        std::cerr << "[SERVER] Falha ao linkar cadeia de áudio." << std::endl;
        gst_object_unref(pipeline);
        gst_object_unref(ntp_clock);
        return 1;
    }

    /* ============================================================================
     * PAD-ADDED CALLBACK - LINKING DINÂMICO
     * ============================================================================
     * decodebin é um elemento DINÂMICO: ele só cria pads de saída DEPOIS que
     * detecta o tipo de mídia do arquivo.
     * 
     * FUNCIONAMENTO:
     * 1. decodebin analisa o arquivo e detecta streams (áudio, vídeo, legendas)
     * 2. Para cada stream, ele emite um sinal "pad-added" com um novo pad
     * 3. Este callback é chamado e verifica se o pad é de áudio
     * 4. Se for áudio, conecta ao queue para continuar o pipeline
     * 
     * VERIFICAÇÕES:
     * - Se o sink do queue já está linkado, ignora (evita links duplicados)
     * - Verifica os capabilities (caps) do pad para confirmar que é áudio
     * - Só conecta se os caps começam com "audio/" (audio/x-raw, audio/mpeg, etc.)
     * ============================================================================ */
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
     * CONFIGURAÇÃO DO CLOCK NO PIPELINE - SINCRONIZAÇÃO
     * ============================================================================
     * 
     * gst_pipeline_use_clock():
     * - Força o pipeline a usar o NTP clock que criamos
     * - TODOS os elementos do pipeline usarão este clock para timestamps
     * - Como o cliente também usa o MESMO NTP, os timestamps serão compatíveis
     * 
     * gst_pipeline_set_latency():
     * - Define a latência máxima aceitável do pipeline (2 segundos)
     * - Isso dá tempo para buffers e processamento sem descartar dados
     * - Importante para streaming ao vivo
     * ============================================================================ */
    gst_pipeline_use_clock(GST_PIPELINE(pipeline), ntp_clock);
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
        gst_object_unref(ntp_clock);
        g_main_loop_unref(main_loop);
        main_loop = nullptr;
        return 1;
    }

    std::cout << "[SERVER] Rodando. Ctrl+C para sair." << std::endl;
    g_main_loop_run(main_loop);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    gst_object_unref(ntp_clock);
    g_main_loop_unref(main_loop);
    main_loop = nullptr;

    std::cout << "[SERVER] Encerrado." << std::endl;
    return 0;
}
