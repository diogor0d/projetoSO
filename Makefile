# Compiler
CC = gcc

# Compiler flags
CFLAGS = -Wall -Wextra -Werror

# Source files
SRCS = Controller.c

# Output executable
TARGET = DeiChain

# Build target
$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) -lrt

# Clean target
clean:
	rm -f $(TARGET)