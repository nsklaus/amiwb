# Skeleton App Template Makefile
# This is a template for creating new AmiWB applications

CC = gcc
CFLAGS = -g -Wall
INCLUDES = -I../.. -I../../src -I/usr/include/freetype2 -I/usr/include/X11/Xft
LIBS = -lSM -lICE -lXext -lXmu -lX11 -lXrender -lXfixes -lXdamage \
       -lXft -lXrandr -lXcomposite -lm -lImlib2 -lfontconfig

# Toolkit library location
TOOLKIT_LIB = ../../libamiwb-toolkit.a

# Source files
SRCS = $(wildcard *.c)
OBJS = $(SRCS:.c=.o)

# Executable (change this for your application)
EXEC = skeleton_app

# Default target
all: $(EXEC)

# Build skeleton_app
$(EXEC): $(OBJS) $(TOOLKIT_LIB)
	$(CC) $(OBJS) $(TOOLKIT_LIB) $(LIBS) -o $(EXEC)

# Pattern rule for object files
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Clean target
clean:
	rm -f $(OBJS) $(EXEC)

# Install target (customize for your application)
install: $(EXEC)
	@echo "Skeleton app is a template - customize the install target for your application"
	# Example:
	# mkdir -p /usr/local/bin
	# cp $(EXEC) /usr/local/bin/$(EXEC).new
	# mv /usr/local/bin/$(EXEC).new /usr/local/bin/$(EXEC)
	# @echo "$(EXEC) installed"

# Uninstall target
uninstall:
	@echo "Skeleton app is a template - customize the uninstall target for your application"
	# Example:
	# rm -f /usr/local/bin/$(EXEC)
	# @echo "$(EXEC) uninstalled"

# Test target - useful for development
test: $(EXEC)
	./$(EXEC)

.PHONY: all clean install uninstall test