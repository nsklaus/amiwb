CC = gcc

CFLAGS = -g \
  -Iinclude \
  -I/usr/include/X11/Xft \
  -I/usr/include/freetype2 
  
  # attempt at getting menus from clients
  #-I/usr/include/libdbusmenu-glib-0.4 \
  #-I/usr/include/libdbusmenu-gtk3-0.4 \
  #-I/usr/include/gtk-3.0 \
  #-I/usr/include/glib-2.0 \
  #-I/usr/lib64/glib-2.0/include \
  #-I/usr/include/dbus-1.0 \
  #-I/usr/lib64/dbus-1.0/include

LIBS = \
  -lSM \
  -lICE \
  -lXext \
  -lXmu \
  -lX11 \
  -lXrender \
  -lXft \
  -lXrandr \
  -lm \
  -lImlib2 \
  -lfontconfig 

  # attempt at getting menus from clients
  # -ldbusmenu-gtk3 \
  # -ldbus-1 \
  # -ldbusmenu-glib \
  # -lgobject-2.0 -lglib-2.0

SRC_DIR = src
SRC = $(wildcard $(SRC_DIR)/*.c)

OBJ = $(SRC:.c=.o)
EXEC = amiwb

all: $(EXEC)

$(EXEC): $(OBJ)
	$(CC) $(OBJ) $(LIBS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(EXEC)

install:
	mkdir -p /usr/local/bin
	cp $(EXEC) /usr/local/bin/amiwb
	mkdir -p /usr/local/share/amiwb/icons
	cp -r icons/* /usr/local/share/amiwb/icons/
	mkdir -p /usr/local/share/amiwb/patterns
	cp -r patterns/* /usr/local/share/amiwb/patterns/
	mkdir -p /usr/local/share/amiwb/fonts
	cp -r fonts/* /usr/local/share/amiwb/fonts/

uninstall:
	rm -f /usr/local/bin/amiwb
	rm -rf /usr/local/share/amiwb