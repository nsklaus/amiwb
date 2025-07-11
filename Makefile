# Makefile for amiwb window manager
# Builds binaries in /home/klaus/Sources/amiwb/
# Sources are in /home/klaus/Sources/amiwb/src/

# Compiler and flags
CC = gcc
CFLAGS = -Wall -O2 -fPIC -I/usr/include/freetype2 -I/usr/include/pixman-1 -I.
LDFLAGS = -lX11 -lXext -lpixman-1 -lfreetype

# Directories
SRC_DIR = /home/klaus/Sources/amiwb/src
BIN_DIR = /home/klaus/Sources/amiwb

# Source files for amiwb
SOURCES = $(SRC_DIR)/main.c $(SRC_DIR)/icons.c $(SRC_DIR)/intuition.c $(SRC_DIR)/menus.c \
          $(SRC_DIR)/events.c $(SRC_DIR)/decoration.c $(SRC_DIR)/render.c
OBJECTS = $(SOURCES:.c=.o)
BINARY = $(BIN_DIR)/amiwb

# Source files for testwb
TEST_SOURCES = $(SRC_DIR)/testwb.c
TEST_OBJECTS = $(TEST_SOURCES:.c=.o)
TEST_BINARY = $(BIN_DIR)/testwb

# Default target
all: $(BINARY) $(TEST_BINARY)

# Link object files into binary
$(BINARY): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

# Link testwb
$(TEST_BINARY): $(TEST_OBJECTS)
	$(CC) $(TEST_OBJECTS) -o $@ $(LDFLAGS)

# Compile source files to object files
$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up
clean:
	rm -f $(OBJECTS) $(TEST_OBJECTS) $(BINARY) $(TEST_BINARY)

# Phony targets
.PHONY: all clean
