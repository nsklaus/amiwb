CC = gcc
CFLAGS = -O2 -Wall -g
PKG = `pkg-config --cflags --libs x11`
# x11-xcb xext xmu`

TARGET = amiwb
SRCS = main.c wm.c events.c icon_loader.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(PKG)

clean:
	rm -f $(TARGET)

