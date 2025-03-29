#
#   Projeto de Sistemas Operativos 2024/2025 - DEIChain
#   Diogo Nuno Fonseca Rodrigues 2022257625
#   Guilherme Teixeira Gonçalves Rosmaninho 2022257636
#

# Compiler
CC = gcc

# Compiler flags
CFLAGS = -Wall -Wextra

# Source files
SRCS = Controller.c Miner.c

# Output executable
TARGET = DeiChain

# Build target
$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) -lrt -pthread
	$(CC) $(CFLAGS) -o txgen TxGen.c -lrt -pthread -g

# Clean target
clean:
	rm -f $(TARGET) txgen
