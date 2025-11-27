Rodar servidor:
```bash
g++ server_netclock.cpp -o server_netclock $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)
./server_netclock rtsp://0.0.0.0:8555/live 8557
```

Rodar cliente:
```bash
g++ client_netclock.cpp -o client_netclock $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-net-1.0)
./client_netclock rtsp://127.0.0.1:8555/live 127.0.0.1 8557
```