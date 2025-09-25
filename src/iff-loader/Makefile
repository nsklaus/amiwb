# IFF ILBM Loader for gdk-pixbuf Makefile

CC = gcc
CFLAGS = -shared -fPIC -Wall -O2
INCLUDES = $(shell pkg-config --cflags gdk-pixbuf-2.0)
LIBS = $(shell pkg-config --libs gdk-pixbuf-2.0)

# gdk-pixbuf loader directory
GDK_PIXBUF_LOADER_DIR = $(shell pkg-config --variable=gdk_pixbuf_moduledir gdk-pixbuf-2.0)

# Source files
SRCS = iff-ilbm-loader.c
OBJS = $(SRCS:.c=.o)

# Shared library
LOADER = libpixbufloader-iff.so

# Default target
all: $(LOADER)

# Build IFF ILBM loader
$(LOADER): $(SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) $(SRCS) -o $(LOADER) $(LIBS)

# Clean target
clean:
	rm -f $(OBJS) $(LOADER)

# Install target
install: $(LOADER)
	@echo "Installing IFF ILBM loader to $(GDK_PIXBUF_LOADER_DIR)"
	cp $(LOADER) $(GDK_PIXBUF_LOADER_DIR)/iff-loader.so.new
	mv $(GDK_PIXBUF_LOADER_DIR)/iff-loader.so.new $(GDK_PIXBUF_LOADER_DIR)/iff-loader.so
	gdk-pixbuf-query-loaders-64 --update-cache
	@echo "IFF ILBM support installed - imv and GTK apps can now display IFF files!"

# Uninstall target
uninstall:
	rm -f $(GDK_PIXBUF_LOADER_DIR)/iff-loader.so
	gdk-pixbuf-query-loaders-64 --update-cache
	@echo "IFF ILBM loader uninstalled"

# Test target
test: $(LOADER)
	@echo "Testing IFF loader..."
	@if [ -f test.iff ]; then \
		gdk-pixbuf-thumbnailer test.iff test.png; \
		echo "If test.png was created, the loader works!"; \
	else \
		echo "No test.iff file found"; \
	fi

.PHONY: all clean install uninstall test