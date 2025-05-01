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
SRCS = src/Controller.c src/Miner.c src/SHMManagement.c src/Validator.c src/Statistics.c src/PoW/pow.c

# Output executable
TARGET = DeiChain

# Phony target to force recompilation
.PHONY: all clean

# Build target
all:
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) -lrt -pthread -lssl -lcrypto
	$(CC) $(CFLAGS) -o txgen src/TxGen.c -lrt -pthread

# Clean target
clean:
	rm -f $(TARGET) txgen