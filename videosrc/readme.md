# PoC NTP Sync

## Netclock básico

Rodar servidor:
```bash
g++ server_netclock.cpp -o server_netclock $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)
./server_netclock /home/eliezer/Workspace/tpv/poc_ntp_sync/videosrc/elephants_dream_sample.mp4 rtsp://0.0.0.0:8555/live 8557
```

Rodar cliente:
```bash
g++ client_netclock.cpp -o client_netclock $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)
./client_netclock rtsp://127.0.0.1:8555/live 127.0.0.1 8557
```

## Netclock com overlay de timestamps

Rodar servidor:
```bash
g++ server_netclock_overlay.cpp -o server_netclock_overlay $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)
./server_netclock_overlay /home/eliezer/Workspace/tpv/poc_ntp_sync/videosrc/elephants_dream_sample.mp4 rtsp://0.0.0.0:8555/live
```

Rodar cliente:
```bash
g++ client_dualclock.cpp -o client_dualclock $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)

# Modo net clock
./client_dualclock net rtsp://127.0.0.1:8555/live 127.0.0.1 8557

# Modo system clock
./client_dualclock sys rtsp://127.0.0.1:8555/live 127.0.0.1 8557
```

## Netclock com drift

Rodar servidor:
```bash
g++ server_netclock_drift.cpp -o server_netclock_drift $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)
./server_netclock_drift /home/eliezer/Workspace/tpv/poc_ntp_sync/videosrc/elephants_dream_sample.mp4 rtsp://0.0.0.0:8555/live 1001 1000
```

# Rodar em dispositivos diferentes

Dispositivo A: SERVIDOR (mediaMTX e server_netclock*)
Dispositivo B: CLIENTE (client_netclock*)

## Preparar a rede

1. Coloque os dois dispositivos na mesma rede
2. Descubra o IP do dispositivo A:
   ```bash
    ip addr    
    hostname -I
    ```
3. Garanta que as portas 8555 e 8557 estejam livres e abertas no dispositivo A
   - 8555: RTSP
   - 8557: CLOCK

## Servidor no dispositivo A

1. Rode o mediaMTX
2. Servidor netclock:
   Compilar:
   ```bash
   g++ server_netclock_overlay.cpp -o server_netclock_overlay $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)
   ```

   Rodar:
   ```bash
   ./server_netclock_overlay /home/eliezer/Workspace/tpv/poc_ntp_sync/videosrc/elephants_dream_sample.mp4 rtsp://0.0.0.0:8555 8557
   ```
   RTSP: rtsp://HOST_IP:8555
   CLOCK: HOST_IP:8557 (o código bindou em 0.0.0.0:8557)
   > Importante:
   > HOST_IP é o IP do dispositivo A descoberto no passo anterior
   > O `0.0.0.0` é só pra bind.
   > Os clientes SEMPRE vão usar rtsp://HOST_IP:8555/stream e clock_host = HOST_IP

## Cliente no dispositivo B

1. Copie o código do client_final.cpp para o dispositivo B e compile:
   ```bash
   g++ client_final.cpp -o client_final $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)
   ```

2. Teste no modo NET
   ```bash
   ./client_final net rtsp://HOST_IP:8555 HOST_IP 8557
   ```

3. Teste no modo BAD
   ```bash
   ./client_final bad rtsp://HOST_IP:8555 HOST_IP 8557 1001 1000
   ```