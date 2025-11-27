Rodar servidor:
```bash
g++ video_server_netclock.cpp -o video_server_netclock $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)
./video_server_netclock /home/eliezer/Workspace/tpv/poc_ntp_sync/videosrc/elephants_dream_sample.mp4 rtsp://0.0.0.0:8555/live 8557
```

Rodar cliente:
```bash
g++ video_client_netclock.cpp -o video_client_netclock $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)
./video_client_netclock rtsp://127.0.0.1:8555/live 127.0.0.1 8557
```