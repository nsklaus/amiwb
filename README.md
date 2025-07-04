# amiwb
window manager for linux using x11 and xlib

this is an experiment, playing with X11 WMs.

Project Structure 
src/
├── main.c         // Entry point: setup, scan, run event loop
├── wm.c           // WM logic: init, frame window, scan existing
├── events.c       // Event loop logic
├── icon_loader.c  // load amiga icon
├── Makefile       // plain makefile