# AMIWB Desktop Environment Makefile
# Builds only amiwb window manager and toolkit library

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

# Source files
AMIWB_SRCS = $(wildcard $(AMIWB_DIR)/*.c)
TOOLKIT_SRCS = $(wildcard $(TOOLKIT_DIR)/*.c)

# Object files
AMIWB_OBJS = $(AMIWB_SRCS:.c=.o)
TOOLKIT_OBJS = $(TOOLKIT_SRCS:.c=.o)

# Outputs
AMIWB_EXEC = amiwb
TOOLKIT_LIB = libamiwb-toolkit.a

# Default target
all: $(TOOLKIT_LIB) $(AMIWB_EXEC)

# Toolkit library
$(TOOLKIT_LIB): $(TOOLKIT_OBJS)
	$(AR) rcs $@ $(TOOLKIT_OBJS)

# amiwb window manager
$(AMIWB_EXEC): $(AMIWB_OBJS) $(TOOLKIT_LIB)
	$(CC) $(AMIWB_OBJS) $(TOOLKIT_LIB) $(LIBS) -o $@

# Pattern rules for object files
$(AMIWB_DIR)/%.o: $(AMIWB_DIR)/%.c
	$(CC) $(COMMON_CFLAGS) $(COMMON_INCLUDES) -c $< -o $@

$(TOOLKIT_DIR)/%.o: $(TOOLKIT_DIR)/%.c
	$(CC) $(COMMON_CFLAGS) $(COMMON_INCLUDES) -c $< -o $@

# Clean
clean:
	rm -f $(AMIWB_OBJS) $(TOOLKIT_OBJS) $(AMIWB_EXEC) $(TOOLKIT_LIB)

# Install
install: $(TOOLKIT_LIB) $(AMIWB_EXEC)
	# Install toolkit library and headers
	mkdir -p /usr/local/lib
	cp $(TOOLKIT_LIB) /usr/local/lib/libamiwb-toolkit.a.new
	mv /usr/local/lib/libamiwb-toolkit.a.new /usr/local/lib/libamiwb-toolkit.a
	mkdir -p /usr/local/include/amiwb/toolkit
	cp $(TOOLKIT_DIR)/*.h /usr/local/include/amiwb/toolkit/
	# Install amiwb binary
	mkdir -p /usr/local/bin
	cp $(AMIWB_EXEC) /usr/local/bin/amiwb.new
	mv /usr/local/bin/amiwb.new /usr/local/bin/amiwb
	# Install resources
	mkdir -p /usr/local/share/amiwb/icons
	cp -r icons/* /usr/local/share/amiwb/icons/ 2>/dev/null || true
	mkdir -p /usr/local/share/amiwb/patterns
	cp -r patterns/* /usr/local/share/amiwb/patterns/ 2>/dev/null || true
	mkdir -p /usr/local/share/amiwb/fonts
	cp -r fonts/* /usr/local/share/amiwb/fonts/ 2>/dev/null || true
	mkdir -p /usr/local/share/amiwb/dotfiles
	cp -r dotfiles/* /usr/local/share/amiwb/dotfiles/ 2>/dev/null || true
	# Install default config files to user's home directory if they don't exist
	@if [ -n "$$SUDO_USER" ]; then \
		USER_HOME=$$(getent passwd $$SUDO_USER | cut -d: -f6); \
	elif [ -n "$$USER" ] && [ "$$USER" != "root" ]; then \
		USER_HOME=$$(getent passwd $$USER | cut -d: -f6); \
	else \
		USER_HOME="$$HOME"; \
	fi; \
	if [ -n "$$USER_HOME" ] && [ -d "$$USER_HOME" ]; then \
		CONFIG_DIR="$$USER_HOME/.config/amiwb"; \
		mkdir -p "$$CONFIG_DIR"; \
		if [ ! -f "$$CONFIG_DIR/amiwbrc" ]; then \
			cp dotfiles/home_dot_config_amiwb/amiwbrc "$$CONFIG_DIR/amiwbrc"; \
			echo "Installed amiwbrc to $$CONFIG_DIR/"; \
		fi; \
		if [ ! -f "$$CONFIG_DIR/toolsdaemonrc" ]; then \
			cp dotfiles/home_dot_config_amiwb/toolsdaemonrc "$$CONFIG_DIR/toolsdaemonrc"; \
			echo "Installed toolsdaemonrc to $$CONFIG_DIR/"; \
		fi; \
		if [ -n "$$SUDO_USER" ]; then \
			chown -R $$SUDO_USER:$$SUDO_USER "$$CONFIG_DIR"; \
		elif [ -n "$$USER" ] && [ "$$USER" != "root" ]; then \
			chown -R $$USER:$$USER "$$CONFIG_DIR" 2>/dev/null || true; \
		fi; \
	fi
	@echo "AmiWB and toolkit installed"

# Uninstall
uninstall:
	rm -f /usr/local/bin/amiwb
	rm -f /usr/local/lib/libamiwb-toolkit.a
	rm -rf /usr/local/include/amiwb
	rm -rf /usr/local/share/amiwb
	@echo "AmiWB uninstalled"

.PHONY: all clean install uninstall