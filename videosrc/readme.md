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

Rodar servidor com overlay:
```bash
g++ server_netclock_overlay.cpp -o server_netclock_overlay $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)
./server_netclock_overlay /home/eliezer/Workspace/tpv/poc_ntp_sync/videosrc/elephants_dream_sample.mp4 rtsp://0.0.0.0:8555/live
```

Rodar cliente com overlay:
```bash
g++ client_dualclock.cpp -o client_dualclock $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)

# Modo net clock
./client_dualclock net rtsp://127.0.0.1:8555/live 127.0.0.1 8557

# Modo system clock
./client_dualclock sys rtsp://127.0.0.1:8555/live 127.0.0.1 8557
```
