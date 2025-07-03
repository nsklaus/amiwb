# Compiler and flags
CC = gcc
CFLAGS = -O2 -Wall
PKG = `pkg-config --cflags --libs x11`

# Target
TARGET = amiwb
SRC = amiwb.c

# Default build
all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(PKG)

# Clean build artifacts
clean:
	rm -f $(TARGET)
