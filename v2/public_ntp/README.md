# POC - NTP Público (ntp.br)

## Descrição

Esta versão usa um servidor NTP público como referência de tempo compartilhada entre servidor e cliente.

## Quando usar

- Servidor e cliente em redes/locais diferentes
- Não é possível abrir portas no servidor
- Acesso via internet (4G/5G)
- Múltiplos servidores precisam estar sincronizados

## Como funciona

```
Servidor NTP Público (a.st1.ntp.br)
            ↓                    ↓
      [Servidor]            [Cliente]
      Emulador TV           Celular
            ↓                    ↓
       RTSP Stream ──────────────→
```

Ambos sincronizam com o mesmo servidor NTP público, garantindo referência de tempo comum.

## Compilar

```bash
# Servidor
g++ -o server server.cpp $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)

# Cliente
g++ -o client client.cpp $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)
```

## Executar

### Servidor (Emulador TV):
```bash
./server ../media/audio.wav
```

### Cliente (Celular/Desktop):
```bash
# Sintaxe: ./client [rtspsrc_latency_ms] [jitter_latency_ms]

# Desenvolvimento/LAN
./client 100 100

# Produção/Internet
./client 300 300

# 4G/5G
./client 500 500
```

## Configuração

### Mudar servidor NTP:

**Em `server.cpp` e `client.cpp`:**
```cpp
// Linha 9-10
static const char* NTP_HOST = "a.st1.ntp.br";  // ou "pool.ntp.org"
static const int NTP_PORT = 123;
```

**IMPORTANTE**: Servidor e cliente DEVEM usar o MESMO servidor NTP!

### Mudar URL RTSP:

**Em `server.cpp` e `client.cpp`:**
```cpp
// Linha 13
static const char* RTSP_URL = "rtsp://IP_SERVIDOR:8555/mystream";
```

## Precisão esperada

| Cenário  | Latência Clock | Qualidade |
|----------|----------------|-----------|
| LAN/WiFi | 10-50ms        | Excelente |
| Internet | 50-200ms       | Boa       |
| 4G/5G    | 100-500ms      | Aceitável |

## Vantagens

- Funciona em qualquer rede
- Não precisa configurar firewall/portas
- Servidor pode estar em qualquer lugar
- Simples de configurar

## Desvantagens

- Precisão limitada pela distância do NTP
- Latência variável
- Depende de internet
- ~20-200ms de sincronização