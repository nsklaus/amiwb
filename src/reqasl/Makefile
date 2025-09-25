# ReqASL File Requester Makefile

CC = gcc
CFLAGS = -g -Wall
INCLUDES = -I../.. -I../../src -I/usr/include/freetype2 -I/usr/include/X11/Xft
LIBS = -lSM -lICE -lXext -lXmu -lX11 -lXrender -lXfixes -lXdamage \
       -lXft -lXrandr -lXcomposite -lm -lImlib2 -lfontconfig

# Toolkit library location
TOOLKIT_LIB = ../../libamiwb-toolkit.a

# Source files (excluding reqasl_hook.c which is built separately)
SRCS = $(filter-out reqasl_hook.c, $(wildcard *.c))
OBJS = $(SRCS:.c=.o)

# Executables
EXEC = reqasl
HOOK = reqasl_hook.so

# Default target
all: $(EXEC) $(HOOK)

# Build reqasl
$(EXEC): $(OBJS) $(TOOLKIT_LIB)
	$(CC) $(OBJS) $(TOOLKIT_LIB) $(LIBS) -o $(EXEC)

# Build reqasl hook library
$(HOOK): reqasl_hook.c
	$(CC) -fPIC -shared -ldl -Wall -O2 -o $(HOOK) reqasl_hook.c

# Pattern rule for object files
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Clean target
clean:
	rm -f $(OBJS) $(EXEC) $(HOOK)

# Install target
install: $(EXEC) $(HOOK)
	mkdir -p /usr/local/bin
	cp $(EXEC) /usr/local/bin/reqasl.new
	mv /usr/local/bin/reqasl.new /usr/local/bin/reqasl
	mkdir -p /usr/local/lib
	cp $(HOOK) /usr/local/lib/reqasl_hook.so.new
	mv /usr/local/lib/reqasl_hook.so.new /usr/local/lib/reqasl_hook.so
	@echo "ReqASL installed"

# Uninstall target
uninstall:
	rm -f /usr/local/bin/reqasl
	rm -f /usr/local/lib/reqasl_hook.so
	@echo "ReqASL uninstalled"

.PHONY: all clean install uninstall