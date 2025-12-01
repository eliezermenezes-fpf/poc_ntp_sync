# POC - Servidor NTP Customizado (NetTimeProvider)

## Descrição

Esta versão usa o próprio servidor (Emulador TV) como mestre de tempo. O servidor compartilha seu clock via NetTimeProvider e os clientes se conectam diretamente a ele.

## Quando usar

- Servidor e cliente na mesma rede (LAN/WiFi)
- Máxima precisão necessária (< 1ms)
- Ambiente controlado (casa, escritório)
- VPN entre servidor e clientes
- Você controla a infraestrutura de rede

## Como funciona

```
      [Servidor]
      Emulador TV
   (Mestre de tempo)
            ↓
   NetTimeProvider (porta 8557 UDP)
            ↓
     ┌──────┴──────┐
     ↓             ↓
[Cliente 1]   [Cliente 2]
 Celular       Desktop
```

O servidor é a referência absoluta de tempo. Clientes conectam diretamente ao servidor via UDP.

## Compilar

```bash
# Servidor
g++ -o server server.cpp $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)

# Cliente
g++ -o client client.cpp $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)
```

## Executar

### 1. Servidor (Emulador TV):
```bash
./server ../media/audio.wav
```

Você verá:
```
[SERVER] ✓ NetTimeProvider ativo na porta 8557 UDP
[SERVER] ✓ Clientes devem conectar em: <IP_SERVIDOR>:8557
```

### 2. Cliente (Celular/Desktop):

**Primeiro, edite `client.cpp` com o IP do servidor:**
```cpp
// Linha 10
static const char* SERVER_IP = "192.168.1.100";
```

**Depois compile e execute:**
```bash
g++ -o client client.cpp $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)

# Sintaxe: ./client [rtspsrc_latency_ms] [jitter_latency_ms]
./client 100 100
```

## Configuração

### Descobrir IP do servidor:
```bash
# No servidor (Emulador TV)
hostname -I
# ou
ip addr show
```

### Configurar IP no cliente:

**Em `client.cpp`:**
```cpp
// Linha 10
static const char* SERVER_IP = "192.168.1.100";
```

### Mudar porta do clock:

**Em `server.cpp`:**
```cpp
// Linha 9
static const int CLOCK_PORT = 8557;
```

**Em `client.cpp`:**
```cpp
// Linha 11
static const int SERVER_PORT = 8557;
```

### Mudar URL RTSP:

**Em `server.cpp` e `client.cpp`:**
```cpp
// Linha 14 (server) e 14 (client)
static const char* RTSP_URL = "rtsp://IP_SERVIDOR:8555/mystream";
```

## Acesso via Internet

### Opção 1: Port Forwarding
```bash
# No roteador, encaminhar:
- Porta 8557 UDP → IP do Servidor (NetTimeProvider)
- Porta 8555 TCP → IP do Servidor (RTSP)

# No cliente, usar IP público:
static const char* SERVER_IP = "200.100.50.25";
static const char* RTSP_URL = "rtsp://200.100.50.25:8555/mystream";
```

### Opção 2: VPN (Recomendado)
```bash
# Criar rede privada: 10.0.0.0/24
# Servidor: 10.0.0.1
# Clientes: 10.0.0.2, 10.0.0.3, etc.

# No cliente:
static const char* SERVER_IP = "10.0.0.1";
```

## Precisão esperada

| Cenário      | Latência Clock | Qualidade |
|--------------|----------------|-----------|
| LAN/WiFi     | < 1ms          | Perfeita  |
| VPN          | 2-10ms         | Excelente |
| Internet     | 10-50ms        | Boa       |
| +            |
| Port Forward |

## Vantagens

- **Precisão máxima** (< 1ms em LAN)
- Não depende de servidores externos
- Controle total sobre o timing
- Grátis
- Simples de implementar

## Desvantagens

- Requer acesso à porta 8557 UDP do servidor
- Não funciona bem através de NAT complexo
- Clientes precisam saber o IP do servidor
- Limitado a rede local (sem VPN)

## Firewall

### No servidor:
```bash
# Permitir porta do clock
sudo ufw allow 8557/udp

# Permitir porta RTSP
sudo ufw allow 8555/tcp

# Verificar
sudo ufw status
```

## Notas

- **Precisão**: Essa versão oferece a melhor sincronização possível (< 1ms em LAN)
- **Escalabilidade**: Suporta múltiplos clientes conectando ao mesmo servidor
