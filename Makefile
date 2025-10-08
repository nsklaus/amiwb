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
AMIWB_SRCS = $(wildcard $(AMIWB_DIR)/*.c) \
             $(wildcard $(AMIWB_DIR)/intuition/*.c) \
             $(wildcard $(AMIWB_DIR)/workbench/*.c) \
             $(wildcard $(AMIWB_DIR)/menus/*.c) \
             $(wildcard $(AMIWB_DIR)/menus/addons/*.c) \
             $(wildcard $(AMIWB_DIR)/dialogs/*.c)
TOOLKIT_SRCS = $(wildcard $(TOOLKIT_DIR)/*.c) \
               $(wildcard $(TOOLKIT_DIR)/button/*.c) \
               $(wildcard $(TOOLKIT_DIR)/inputfield/*.c) \
               $(wildcard $(TOOLKIT_DIR)/listview/*.c) \
               $(wildcard $(TOOLKIT_DIR)/progressbar/*.c) \
               $(wildcard $(TOOLKIT_DIR)/textview/*.c)

# Object files
AMIWB_OBJS = $(AMIWB_SRCS:.c=.o)
TOOLKIT_OBJS = $(TOOLKIT_SRCS:.c=.o)

# Outputs
AMIWB_EXEC = amiwb
TOOLKIT_LIB = libamiwb.so

# Default target
all: $(TOOLKIT_LIB) $(AMIWB_EXEC)

# Toolkit shared library
$(TOOLKIT_LIB): $(TOOLKIT_OBJS)
	$(CC) -shared -o $@ $(TOOLKIT_OBJS) $(LIBS)

# amiwb window manager
$(AMIWB_EXEC): $(AMIWB_OBJS) $(TOOLKIT_LIB)
	$(CC) $(AMIWB_OBJS) -L. -lamiwb $(LIBS) -Wl,-rpath,'$$ORIGIN' -Wl,-rpath,/usr/local/lib -o $@

# Pattern rules for object files
$(AMIWB_DIR)/%.o: $(AMIWB_DIR)/%.c
	$(CC) $(COMMON_CFLAGS) $(COMMON_INCLUDES) -c $< -o $@

$(AMIWB_DIR)/intuition/%.o: $(AMIWB_DIR)/intuition/%.c
	$(CC) $(COMMON_CFLAGS) $(COMMON_INCLUDES) -c $< -o $@

$(AMIWB_DIR)/workbench/%.o: $(AMIWB_DIR)/workbench/%.c
	$(CC) $(COMMON_CFLAGS) $(COMMON_INCLUDES) -c $< -o $@

$(AMIWB_DIR)/menus/%.o: $(AMIWB_DIR)/menus/%.c
	$(CC) $(COMMON_CFLAGS) $(COMMON_INCLUDES) -c $< -o $@

$(AMIWB_DIR)/menus/addons/%.o: $(AMIWB_DIR)/menus/addons/%.c
	$(CC) $(COMMON_CFLAGS) $(COMMON_INCLUDES) -c $< -o $@

$(AMIWB_DIR)/dialogs/%.o: $(AMIWB_DIR)/dialogs/%.c
	$(CC) $(COMMON_CFLAGS) $(COMMON_INCLUDES) -c $< -o $@

$(TOOLKIT_DIR)/%.o: $(TOOLKIT_DIR)/%.c
	$(CC) $(COMMON_CFLAGS) $(COMMON_INCLUDES) -fPIC -c $< -o $@

$(TOOLKIT_DIR)/button/%.o: $(TOOLKIT_DIR)/button/%.c
	$(CC) $(COMMON_CFLAGS) $(COMMON_INCLUDES) -fPIC -c $< -o $@

$(TOOLKIT_DIR)/inputfield/%.o: $(TOOLKIT_DIR)/inputfield/%.c
	$(CC) $(COMMON_CFLAGS) $(COMMON_INCLUDES) -fPIC -c $< -o $@

$(TOOLKIT_DIR)/listview/%.o: $(TOOLKIT_DIR)/listview/%.c
	$(CC) $(COMMON_CFLAGS) $(COMMON_INCLUDES) -fPIC -c $< -o $@

$(TOOLKIT_DIR)/progressbar/%.o: $(TOOLKIT_DIR)/progressbar/%.c
	$(CC) $(COMMON_CFLAGS) $(COMMON_INCLUDES) -fPIC -c $< -o $@

$(TOOLKIT_DIR)/textview/%.o: $(TOOLKIT_DIR)/textview/%.c
	$(CC) $(COMMON_CFLAGS) $(COMMON_INCLUDES) -fPIC -c $< -o $@

# Clean
clean:
	rm -f $(AMIWB_OBJS) $(TOOLKIT_OBJS) $(AMIWB_EXEC) $(TOOLKIT_LIB)

# Install
install: $(TOOLKIT_LIB) $(AMIWB_EXEC)
	# Install toolkit shared library and headers
	mkdir -p /usr/local/lib
	cp $(TOOLKIT_LIB) /usr/local/lib/libamiwb.so.new
	mv /usr/local/lib/libamiwb.so.new /usr/local/lib/libamiwb.so
	ldconfig
	mkdir -p /usr/local/include/amiwb/toolkit
	cp $(TOOLKIT_DIR)/*.h /usr/local/include/amiwb/toolkit/
	cp $(TOOLKIT_DIR)/button/*.h /usr/local/include/amiwb/toolkit/
	cp $(TOOLKIT_DIR)/inputfield/*.h /usr/local/include/amiwb/toolkit/
	cp $(TOOLKIT_DIR)/listview/*.h /usr/local/include/amiwb/toolkit/
	cp $(TOOLKIT_DIR)/progressbar/*.h /usr/local/include/amiwb/toolkit/
	cp $(TOOLKIT_DIR)/textview/*.h /usr/local/include/amiwb/toolkit/
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
	rm -f /usr/local/lib/libamiwb.so
	ldconfig
	rm -rf /usr/local/include/amiwb
	rm -rf /usr/local/share/amiwb
	@echo "AmiWB uninstalled"

.PHONY: all clean install uninstall