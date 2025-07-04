# amiwb
window manager for linux using x11 and xlib

```
Project Structure 
src/
├── main.c         // Entry point: setup, scan, run event loop
├── wm.c           // WM logic: init, frame window, scan existing
├── events.c       // Event loop logic
├── icon_loader.c  // load amiga icon
├── Makefile       // plain makefile
```

start the environment:
```
$ Xephyr -br -ac -noreset -screen 800x600 :1
$ DISPLAY=:1 xterm
$ DISPLAY=:1 ./amiwb
```
