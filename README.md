# amiwb
window manager for linux using x11 and xlib

dependencies:
```
-lSM -lICE -lXext -lXmu -lX11 -lXrender -lXft
```

install:
```
$ make
$ make install
```

start the environment:
```
$ Xephyr -br -ac -noreset -screen 800x600 :1
$ DISPLAY=:1 ./amiwb
```
