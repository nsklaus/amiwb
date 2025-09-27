# Hobbler Web Browser Makefile

CC = gcc
CFLAGS = -g -Wall
INCLUDES = -I../.. -I../../src -I/usr/include/freetype2 -I/usr/include/X11/Xft \
           $(shell pkg-config --cflags webkit2gtk-4.0)
LIBS = -lSM -lICE -lXext -lXmu -lX11 -lXrender -lXfixes -lXdamage \
       -lXft -lXrandr -lXcomposite -lm -lImlib2 -lfontconfig \
       $(shell pkg-config --libs webkit2gtk-4.0)

# Toolkit library
TOOLKIT_LIB = -L../.. -lamiwb

# Source files
SRCS = $(wildcard *.c)
OBJS = $(SRCS:.c=.o)

# Executable
EXEC = hobbler

# Default target
all: $(EXEC)

# Build hobbler
$(EXEC): $(OBJS)
	$(CC) $(OBJS) $(TOOLKIT_LIB) $(LIBS) -Wl,-rpath,'$$ORIGIN/../..' -Wl,-rpath,/usr/local/lib -o $(EXEC)

# Pattern rule for object files
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Clean target
clean:
	rm -f $(OBJS) $(EXEC)

# Install target
install: $(EXEC)
	mkdir -p /usr/local/bin
	cp $(EXEC) /usr/local/bin/hobbler.new
	mv /usr/local/bin/hobbler.new /usr/local/bin/hobbler
	@echo "Hobbler web browser installed"

# Uninstall target
uninstall:
	rm -f /usr/local/bin/hobbler
	@echo "Hobbler uninstalled"

.PHONY: all clean install uninstall