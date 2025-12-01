# NTP público vs NTP customizado

## Resumo

| Aspecto                 | NTP Público                     | NTP Customizado                    |
|-------------------------|---------------------------------|------------------------------------|
| **Referência de tempo** | Servidor externo (a.st1.ntp.br) | Próprio servidor (system clock)    |
| **Precisão**            | 50-200ms                        | < 1ms (em LAN)                     |
| **Dependência**         | Internet + servidor NTP         | Apenas rede local                  |
| **Configuração**        | Simples (mesmo código em ambos) | Requer configurar IP do servidor   |
| **Firewall**            | Porta 123 UDP saída             | Porta 8557 UDP entrada no servidor |
| **Uso ideal**           | Redes diferentes, internet      | Mesma LAN/WiFi, VPN                |

---

## Análise

### **NTP público** (`public_ntp/`)

#### Como funciona:
```
Servidor NTP Externo (a.st1.ntp.br)
        ↓                    ↓
   [Servidor]            [Cliente]
   Emulador TV           Celular
        ↓                    ↓
    RTSP Stream ─────────────→
```

#### Código chave (Servidor):
```cpp
// Ambos sincronizam com o MESMO servidor NTP externo
GstClock* ntp_clock = gst_ntp_clock_new("ntp-server", "a.st1.ntp.br", 123, 0);
```

#### Código chave (Cliente):
```cpp
// DEVE usar o MESMO servidor NTP!
GstClock* ntp_clock = gst_ntp_clock_new("ntp-client", "a.st1.ntp.br", 123, 0);
```

#### Quando usar:
- Servidor e cliente em **redes/locais diferentes**
- Acesso via **internet** (4G/5G)
- **Não pode abrir portas** no servidor
- Múltiplos servidores precisam sincronizar entre si

#### Limitações:
- Precisão limitada pela distância do servidor NTP
- Latência variável (50-200ms típico)
- Depende de acesso à internet
- Servidor e cliente DEVEM usar o mesmo NTP

---

### **NTP customizado** (`custom_ntp/`)

#### Como funciona:
```
      [Servidor]
      Emulador TV
   (Mestre de tempo)
         ↓
  NetTimeProvider
   (porta 8557 UDP)
         ↓
    ┌────┴────┐
    ↓         ↓
[Cliente 1] [Cliente 2]
```

#### Código chave (Servidor):
```cpp
// Servidor usa seu próprio relógio como referência
GstClock* master_clock = gst_system_clock_obtain();

// Compartilha este relógio via rede
GstNetTimeProvider* net_provider = gst_net_time_provider_new(
    master_clock,
    "0.0.0.0",  // Escuta em todas as interfaces
    8557        // Porta UDP
);
```

#### Código chave (Cliente):
```cpp
// Cliente conecta DIRETAMENTE ao servidor
GstClock* net_clock = gst_net_client_clock_new(
    "net-client-clock",
    "192.168.1.100",  // IP DO SERVIDOR
    8557,             // Mesma porta do NetTimeProvider
    0
);
```

#### Quando usar:
- Servidor e cliente na **mesma rede (LAN/WiFi)**
- Precisão **máxima** necessária (< 1ms)
- Ambiente **controlado** (casa, escritório)
- Conexão via **VPN**

#### Limitações:
- Requer acesso à **porta 8557 UDP** do servidor
- Não funciona bem através de **NAT complexo**
- Cliente precisa saber o **IP do servidor**
- Precisa configurar firewall

---

## Diferenças técnicas no código

### Inicialização do Clock

| Componente    | NTP Público            | NTP Customizado                    |
|---------------|------------------------|------------------------------------|
| **Servidor**  | `GstNtpClock`          | `GstSystemClock + NetTimeProvider` |
| **Cliente**   | `GstNtpClock`          | `GstNetClientClock`                |
| **Porta**     | 123 (NTP padrão)       | 8557 (customizável)                |
| **Host**      | a.st1.ntp.br (externo) | IP do servidor (interno)           |

### Pipeline

**Ambas as versões usam EXATAMENTE o mesmo pipeline!**

**Servidor:**
```
filesrc → decodebin → queue → audioconvert → audioresample → avenc_aac → rtspclientsink
```

**Cliente:**
```
rtspsrc → rtpjitterbuffer → rtpmp4adepay → aacparse → avdec_aac → audioconvert → audioresample → autoaudiosink
```

A **única diferença** é qual clock é aplicado ao pipeline com `gst_pipeline_use_clock()`.

---

## Métricas esperadas

### Precisão de sincronização

| Cenário       | NTP Público | NTP Customizado   |
|---------------|-------------|-------------------|
| **LAN/WiFi**  | 10-50ms     | **< 1ms**         |
| **Internet**  | 50-200ms    | 10-50ms (via VPN) |
| **4G/5G**     | 100-500ms   | Não recomendado   |

### Latência total (Áudio)

Depende dos parâmetros `rtspsrc_latency` e `jitter_latency`:

```bash
# Desenvolvimento (LAN)
./client 100 100    # 200ms total

# Produção (Internet)
./client 300 300    # 600ms total

# 4G/5G
./client 500 500    # 1000ms total
```

---

## Configuração necessária

### NTP público

**Servidor:**
```cpp
// Linha 9-10 em server.cpp
static const char* NTP_HOST = "a.st1.ntp.br";  // Pode mudar
static const int NTP_PORT = 123;
```

**Cliente:**
```cpp
// Linha 12-13 em client.cpp
static const char* NTP_HOST = "a.st1.ntp.br";  // DEVE SER IGUAL AO SERVIDOR
static const int NTP_PORT = 123;
```

**Firewall:**
```bash
# Permitir saída para NTP
sudo ufw allow out 123/udp
```

### NTP customizado

**Servidor:**
```cpp
// Linha 9 em server.cpp
static const int CLOCK_PORT = 8557;  // Porta do NetTimeProvider
```

**Cliente:**
```cpp
// Linha 12-13 em client.cpp
static const char* SERVER_IP = "192.168.1.100";  // IP DO SERVIDOR
static const int SERVER_PORT = 8557;
```

**Firewall (Servidor):**
```bash
# Permitir entrada na porta do clock
sudo ufw allow 8557/udp

# Permitir entrada RTSP
sudo ufw allow 8555/tcp
```

---

## Pontos chave

### 1. Por que precisamos de sincronização?

Sem sincronização, servidor e cliente têm "relógios" diferentes:
- Servidor marca frame com timestamp `100.000`
- Cliente recebe e pensa que é o tempo `100.500`
- Resultado: áudio fica fora de sincronia, cortes, delays

### 2. Como NTP público resolve?

Ambos "acertam seus relógios" com a mesma referência externa:
- Servidor: "Que horas são?" → NTP: "15:30:00.000"
- Cliente: "Que horas são?" → NTP: "15:30:00.050"
- Agora ambos sabem que estão 50ms fora de sincronia e podem compensar

### 3. Como NTP customizado resolve?

O servidor se torna a "fonte da verdade":
- Servidor: "São 15:30:00.000 (no meu relógio)"
- Cliente: "Ok, vou usar seu relógio como referência"
- Resultado: sincronização perfeita (limitada apenas pela latência de rede)

### 4. Por que NTP customizado é mais preciso?

- **NTP público:** Cliente → NTP Brasil (latência) → Servidor → NTP Brasil (latência)
  - Duas viagens através da internet
  - Latência variável e cumulativa
  
- **NTP customizado:** Cliente ←→ Servidor (mesma LAN)
  - Uma conexão direta
  - Latência mínima e constante

---

## Conceitos GStreamer

### 1. `gst_pipeline_use_clock()`
Força todos os elementos do pipeline a usar um clock específico.
Sem isso, elementos usariam clocks diferentes → dessincronização

### 2. `gst_clock_wait_for_sync()`
Aguarda o clock sincronizar antes de continuar.
Evita começar com timestamps errados.

### 3. `ntp-sync=true` no rtspsrc
Ativa sincronização baseada em NTP no recebimento RTSP.
Sem isso, o cliente ignora os timestamps NTP do servidor.

### 4. NetTimeProvider vs NetClientClock
- **NetTimeProvider:** Servidor que compartilha um clock
- **NetClientClock:** Cliente que recebe clock compartilhado
- Similar a servidor/cliente HTTP, mas para sincronização de tempo

---

## Próximos passos

### Para testar NTP público:
```bash
cd public_ntp
g++ -o server server.cpp $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)
g++ -o client client.cpp $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)

# Terminal 1
./server ../media/audio.wav

# Terminal 2
./client 100 100
```

### Para testar NTP customizado:
```bash
cd custom_ntp

# 1. IMPORTANTE: Editar client.cpp e mudar SERVER_IP
# Descobrir IP do servidor:
hostname -I

# 2. Compilar
g++ -o server server.cpp $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)
g++ -o client client.cpp $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)

# 3. Terminal 1 (servidor)
./server ../media/audio.wav

# 4. Terminal 2 (cliente)
./client 100 100
```

### Monitorar sincronização:
Ambas as versões imprimem a cada 1 segundo:
```
[SYNC] ntp/net=1733097600.123 s | sys=1733097600.124 s | delta=0.001 s | pos=5.234 s
```

- **delta pequeno (< 0.1s):** Boa sincronização
- **delta grande (> 1s):** Problema de sincronização
