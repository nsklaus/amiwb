# EditPad Text Editor Makefile

CC = gcc
CFLAGS = -g -Wall -DEDITPAD_BUILD
INCLUDES = -I../.. -I../../src -I/usr/include/freetype2 -I/usr/include/X11/Xft
LIBS = -lSM -lICE -lXext -lXmu -lX11 -lXrender -lXfixes -lXdamage \
       -lXft -lXrandr -lXcomposite -lm -lImlib2 -lfontconfig

# Toolkit library
TOOLKIT_LIB = -L../.. -lamiwb

# Source files
SRCS = $(wildcard *.c)
OBJS = $(SRCS:.c=.o)

# Executable
EXEC = editpad

# Default target
all: $(EXEC)

# Build editpad
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
	cp $(EXEC) /usr/local/bin/editpad.new
	mv /usr/local/bin/editpad.new /usr/local/bin/editpad
	# Install config and desktop files for user
	@if [ -n "$$SUDO_USER" ]; then \
		USER_HOME=$$(getent passwd $$SUDO_USER | cut -d: -f6); \
	elif [ -n "$$USER" ] && [ "$$USER" != "root" ]; then \
		USER_HOME=$$(getent passwd $$USER | cut -d: -f6); \
	else \
		USER_HOME="$$HOME"; \
	fi; \
	if [ -n "$$USER_HOME" ] && [ -d "$$USER_HOME" ]; then \
		CONFIG_DIR="$$USER_HOME/.config/amiwb/editpad"; \
		mkdir -p "$$CONFIG_DIR"; \
		if [ ! -f "$$CONFIG_DIR/editpadrc" ] && [ -f ../../dotfiles/home_dot_config_amiwb/editpad/editpadrc ]; then \
			cp ../../dotfiles/home_dot_config_amiwb/editpad/editpadrc "$$CONFIG_DIR/editpadrc"; \
			echo "Installed editpadrc to $$CONFIG_DIR/"; \
		fi; \
		mkdir -p "$$USER_HOME/.local/share/applications"; \
		if [ -f ../../dotfiles/editpad.desktop ]; then \
			cp ../../dotfiles/editpad.desktop "$$USER_HOME/.local/share/applications/"; \
			echo "EditPad desktop file installed to $$USER_HOME/.local/share/applications/"; \
		fi; \
		if [ -n "$$SUDO_USER" ]; then \
			chown -R $$SUDO_USER:$$SUDO_USER "$$CONFIG_DIR" 2>/dev/null || true; \
			chown $$SUDO_USER:$$SUDO_USER "$$USER_HOME/.local/share/applications/editpad.desktop" 2>/dev/null || true; \
		elif [ -n "$$USER" ] && [ "$$USER" != "root" ]; then \
			chown -R $$USER:$$USER "$$CONFIG_DIR" 2>/dev/null || true; \
			chown $$USER:$$USER "$$USER_HOME/.local/share/applications/editpad.desktop" 2>/dev/null || true; \
		fi; \
	fi
	@echo "EditPad installed"

# Uninstall target
uninstall:
	rm -f /usr/local/bin/editpad
	@echo "EditPad uninstalled"

.PHONY: all clean install uninstall