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
EDITPAD_DIR = src/editpad

# Source files
AMIWB_SRCS = $(wildcard $(AMIWB_DIR)/*.c)
TOOLKIT_SRCS = $(wildcard $(TOOLKIT_DIR)/*.c)
REQASL_SRCS = $(wildcard $(REQASL_DIR)/*.c)
EDITPAD_SRCS = $(wildcard $(EDITPAD_DIR)/*.c)

# Object files
AMIWB_OBJS = $(AMIWB_SRCS:.c=.o)
TOOLKIT_OBJS = $(TOOLKIT_SRCS:.c=.o)
REQASL_OBJS = $(REQASL_SRCS:.c=.o)
EDITPAD_OBJS = $(EDITPAD_SRCS:.c=.o)

# Executables
AMIWB_EXEC = amiwb
REQASL_EXEC = reqasl
EDITPAD_EXEC = editpad
TOOLKIT_LIB = libamiwb-toolkit.a
REQASL_HOOK = reqasl_hook.so

# Default target
all: $(TOOLKIT_LIB) amiwb reqasl editpad $(REQASL_HOOK)

# Toolkit library (built for installation)
$(TOOLKIT_LIB): $(TOOLKIT_OBJS)
	$(AR) rcs $@ $(TOOLKIT_OBJS)
	@echo "Toolkit library built: $(TOOLKIT_LIB)"

# amiwb window manager
$(AMIWB_EXEC): $(AMIWB_OBJS) $(TOOLKIT_LIB)
	$(CC) $(AMIWB_OBJS) $(TOOLKIT_LIB) $(LIBS) -o $@
	@echo "amiwb executable built: $(AMIWB_EXEC)"

# ReqASL file requester  
$(REQASL_EXEC): $(REQASL_OBJS) $(TOOLKIT_LIB)
	$(CC) $(REQASL_OBJS) $(TOOLKIT_LIB) $(LIBS) -o $@
	@echo "ReqASL executable built: $(REQASL_EXEC)"

# EditPad text editor
$(EDITPAD_EXEC): $(EDITPAD_OBJS) $(TOOLKIT_LIB)
	$(CC) $(EDITPAD_OBJS) $(TOOLKIT_LIB) $(LIBS) -o $@
	@echo "EditPad executable built: $(EDITPAD_EXEC)"

# ReqASL hook library for intercepting file dialogs
$(REQASL_HOOK): $(REQASL_DIR)/reqasl_hook.c
	$(CC) -fPIC -shared -ldl -Wall -O2 -o $@ $<
	@echo "ReqASL hook library built: $(REQASL_HOOK)"

# Pattern rules for object files
$(AMIWB_DIR)/%.o: $(AMIWB_DIR)/%.c
	$(CC) $(COMMON_CFLAGS) $(COMMON_INCLUDES) -c $< -o $@

$(TOOLKIT_DIR)/%.o: $(TOOLKIT_DIR)/%.c
	$(CC) $(COMMON_CFLAGS) $(COMMON_INCLUDES) -c $< -o $@

$(REQASL_DIR)/%.o: $(REQASL_DIR)/%.c
	$(CC) $(COMMON_CFLAGS) $(COMMON_INCLUDES) -c $< -o $@

$(EDITPAD_DIR)/%.o: $(EDITPAD_DIR)/%.c
	$(CC) $(COMMON_CFLAGS) $(COMMON_INCLUDES) -DEDITPAD_BUILD -c $< -o $@

# Clean targets
clean:
	rm -f $(AMIWB_OBJS) $(TOOLKIT_OBJS) $(REQASL_OBJS) $(EDITPAD_OBJS)
	rm -f $(AMIWB_EXEC) $(REQASL_EXEC) $(EDITPAD_EXEC) $(TOOLKIT_LIB) $(REQASL_HOOK)
	rm -f src/*.o  # Clean any stray object files
	@echo "Clean complete"

clean-amiwb:
	rm -f $(AMIWB_OBJS) $(AMIWB_EXEC)

clean-toolkit:
	rm -f $(TOOLKIT_OBJS) $(TOOLKIT_LIB)

clean-reqasl:
	rm -f $(REQASL_OBJS) $(REQASL_EXEC) $(REQASL_HOOK)

clean-editpad:
	rm -f $(EDITPAD_OBJS) $(EDITPAD_EXEC)

# Install targets
# Order is important:
# 1st: toolkit needs to be installed first (amiwb and reqasl depend on it)
# 2nd: amiwb needs to be installed second (main window manager)
# 3rd: reqasl needs to be installed third (uses amiwb for window management)
# 4th: editpad needs to be installed fourth (text editor)
install: install-toolkit install-amiwb install-reqasl install-editpad

# 1st: Install toolkit library and headers (required by amiwb and reqasl)
install-toolkit: $(TOOLKIT_LIB)
	mkdir -p /usr/local/lib
	cp $(TOOLKIT_LIB) /usr/local/lib/
	mkdir -p /usr/local/include/amiwb/toolkit
	cp $(TOOLKIT_DIR)/*.h /usr/local/include/amiwb/toolkit/
	@echo "Toolkit library installed"

# 2nd: Install amiwb window manager (requires toolkit to be installed)
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
		if [ ! -d "$$CONFIG_DIR" ]; then \
			mkdir -p "$$CONFIG_DIR"; \
			cp dotfiles/home_dot_config_amiwb/amiwbrc "$$CONFIG_DIR/amiwbrc"; \
			cp dotfiles/home_dot_config_amiwb/toolsdaemonrc "$$CONFIG_DIR/toolsdaemonrc"; \
			if [ -n "$$SUDO_USER" ]; then \
				chown -R $$SUDO_USER:$$SUDO_USER "$$CONFIG_DIR"; \
			elif [ -n "$$USER" ] && [ "$$USER" != "root" ]; then \
				chown -R $$USER:$$USER "$$CONFIG_DIR" 2>/dev/null || true; \
			fi; \
			echo "Config files installed to $$CONFIG_DIR/"; \
		else \
			echo "Config directory $$CONFIG_DIR/ already exists (not overwritten)"; \
		fi; \
	fi
	@echo "AmiWB installed"

# 3rd: Install reqasl file requester (requires toolkit and works with amiwb)
install-reqasl: reqasl $(REQASL_HOOK)
	mkdir -p /usr/local/bin
	cp $(REQASL_EXEC) /usr/local/bin/
	mkdir -p /usr/local/lib
	cp $(REQASL_HOOK) /usr/local/lib/
	@echo "ReqASL installed"

# 4th: Install editpad text editor
install-editpad: editpad
	mkdir -p /usr/local/bin
	cp $(EDITPAD_EXEC) /usr/local/bin/
	@echo "EditPad installed"

uninstall:
	rm -f /usr/local/bin/amiwb
	rm -f /usr/local/bin/reqasl
	rm -f /usr/local/bin/editpad
	rm -f /usr/local/lib/libamiwb-toolkit.a
	rm -f /usr/local/lib/reqasl_hook.so
	rm -rf /usr/local/include/amiwb
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