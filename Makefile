# AMIWB Desktop Environment Master Makefile

CC = gcc
AR = ar

# Common flags
COMMON_CFLAGS = -g -Wall
COMMON_INCLUDES = -I. -Isrc -I/usr/include/freetype2 -I/usr/include/X11/Xft

# Libraries
LIBS = -lSM -lICE -lXext -lXmu -lX11 -lXrender -lXfixes -lXdamage \
       -lXft -lXrandr -lXcomposite -lm -lImlib2 -lfontconfig

# Directories
AMIWB_DIR = src/amiwb
TOOLKIT_DIR = src/toolkit
REQASL_DIR = src/reqasl

# Source files
AMIWB_SRCS = $(wildcard $(AMIWB_DIR)/*.c)
TOOLKIT_SRCS = $(wildcard $(TOOLKIT_DIR)/*.c)
REQASL_SRCS = $(wildcard $(REQASL_DIR)/*.c)

# Object files
AMIWB_OBJS = $(AMIWB_SRCS:.c=.o)
TOOLKIT_OBJS = $(TOOLKIT_SRCS:.c=.o)
REQASL_OBJS = $(REQASL_SRCS:.c=.o)

# Executables
AMIWB_EXEC = amiwb
REQASL_EXEC = reqasl
TOOLKIT_LIB = libamiwb-toolkit.a

# Default target
all: toolkit amiwb reqasl

# Toolkit library (built first as others depend on it)
toolkit: $(TOOLKIT_LIB)

$(TOOLKIT_LIB): $(TOOLKIT_OBJS)
	$(AR) rcs $@ $(TOOLKIT_OBJS)
	@echo "Toolkit library built: $(TOOLKIT_LIB)"

# amiwb window manager
amiwb: toolkit $(AMIWB_EXEC)

$(AMIWB_EXEC): $(AMIWB_OBJS) $(TOOLKIT_LIB)
	$(CC) $(AMIWB_OBJS) $(TOOLKIT_LIB) $(LIBS) -o $@
	@echo "amiwb executable built: $(AMIWB_EXEC)"

# ReqASL file requester
reqasl: toolkit $(REQASL_EXEC)

$(REQASL_EXEC): $(REQASL_OBJS) $(TOOLKIT_LIB)
	$(CC) $(REQASL_OBJS) $(TOOLKIT_LIB) $(LIBS) -o $@
	@echo "ReqASL executable built: $(REQASL_EXEC)"

# Pattern rules for object files
$(AMIWB_DIR)/%.o: $(AMIWB_DIR)/%.c
	$(CC) $(COMMON_CFLAGS) $(COMMON_INCLUDES) -c $< -o $@

$(TOOLKIT_DIR)/%.o: $(TOOLKIT_DIR)/%.c
	$(CC) $(COMMON_CFLAGS) $(COMMON_INCLUDES) -c $< -o $@

$(REQASL_DIR)/%.o: $(REQASL_DIR)/%.c
	$(CC) $(COMMON_CFLAGS) $(COMMON_INCLUDES) -c $< -o $@

# Clean targets
clean:
	rm -f $(AMIWB_OBJS) $(TOOLKIT_OBJS) $(REQASL_OBJS)
	rm -f $(AMIWB_EXEC) $(REQASL_EXEC) $(TOOLKIT_LIB)
	rm -f src/*.o  # Clean any stray object files
	@echo "Clean complete"

clean-amiwb:
	rm -f $(AMIWB_OBJS) $(AMIWB_EXEC)

clean-toolkit:
	rm -f $(TOOLKIT_OBJS) $(TOOLKIT_LIB)

clean-reqasl:
	rm -f $(REQASL_OBJS) $(REQASL_EXEC)

# Install targets
install: install-amiwb install-reqasl

install-amiwb: amiwb
	mkdir -p /usr/local/bin
	cp $(AMIWB_EXEC) /usr/local/bin/amiwb.new
	mv /usr/local/bin/amiwb.new /usr/local/bin/amiwb
	mkdir -p /usr/local/share/amiwb/icons
	cp -r icons/* /usr/local/share/amiwb/icons/ 2>/dev/null || true
	mkdir -p /usr/local/share/amiwb/patterns
	cp -r patterns/* /usr/local/share/amiwb/patterns/ 2>/dev/null || true
	mkdir -p /usr/local/share/amiwb/fonts
	cp -r fonts/* /usr/local/share/amiwb/fonts/ 2>/dev/null || true
	mkdir -p /usr/local/share/amiwb/dotfiles
	cp -r dotfiles/* /usr/local/share/amiwb/dotfiles/ 2>/dev/null || true
	@echo "AmiWB installed"

install-reqasl: reqasl
	mkdir -p /usr/local/bin
	cp $(REQASL_EXEC) /usr/local/bin/
	@echo "ReqASL installed"

uninstall:
	rm -f /usr/local/bin/amiwb
	rm -f /usr/local/bin/reqasl
	rm -rf /usr/local/share/amiwb
	@echo "AMIWB uninstalled"

# Test targets
test: test-amiwb test-reqasl

test-amiwb: amiwb
	./$(AMIWB_EXEC)

test-reqasl: reqasl
	./$(REQASL_EXEC)

# Help target
help:
	@echo "AMIWB Build System"
	@echo "================="
	@echo "Targets:"
	@echo "  make all           - Build everything"
	@echo "  make toolkit 	- Build toolkit library"
	@echo "  make amiwb    	- Build amiwb window manager"
	@echo "  make reqasl        - Build ReqASL file requester"
	@echo "  make clean         - Clean all build artifacts"
	@echo "  make install       - Install amiwb and reqasl"
	@echo "  make uninstall     - Uninstall AmiWB"
	@echo "  make help          - Show this help"

.PHONY: all toolkit amiwb reqasl clean clean-amiwb clean-toolkit clean-reqasl \
        install install-amiwb install-reqasl uninstall test test-amiwb test-reqasl help