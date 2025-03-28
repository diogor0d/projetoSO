#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#include "Controller.h"

#define SHM_TRANSACTIONS_POOL "/shm_transactions_pool"
#define SHM_LEDGER "/shm_ledger"

int NUM_MINERS;
int POOL_SIZE;
int TRANSACTIONS_PER_BLOCK;
int BLOCKCHAIN_BLOCKS;
int TRANSACTION_POOL_SIZE = 10000; // valor por omissão

int is_positive_integer(const char *str) {
    while (*str) {
        if (!isdigit(*str)) {
            return 0;
        }
        str++;
    }
    return 1;
}

void parse_config(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        perror("Erro na abertura do ficheiro de configurações.");
        exit(EXIT_FAILURE);
    }

    char buffer[128];
    if (fscanf(file, "NUM_MINERS=%s\n", buffer) != 1 || !is_positive_integer(buffer)) {
        fprintf(stderr, "Valor inválido para NUM_MINERS\n");
        fclose(file);
        exit(EXIT_FAILURE);
    }
    NUM_MINERS = atoi(buffer);

    if (fscanf(file, "POOL_SIZE=%s\n", buffer) != 1 || !is_positive_integer(buffer)) {
        fprintf(stderr, "Valor inválido para POOL_SIZE\n");
        fclose(file);
        exit(EXIT_FAILURE);
    }
    POOL_SIZE = atoi(buffer);

    if (fscanf(file, "TRANSACTIONS_PER_BLOCK=%s\n", buffer) != 1 || !is_positive_integer(buffer)) {
        fprintf(stderr, "Valor inválido para TRANSACTIONS_PER_BLOCK\n");
        fclose(file);
        exit(EXIT_FAILURE);
    }
    TRANSACTIONS_PER_BLOCK = atoi(buffer);

    if (fscanf(file, "BLOCKCHAIN_BLOCKS=%s\n", buffer) != 1 || !is_positive_integer(buffer)) {
        fprintf(stderr, "Valor inválido para BLOCKCHAIN_BLOCKS\n");
        fclose(file);
        exit(EXIT_FAILURE);
    }
    BLOCKCHAIN_BLOCKS = atoi(buffer);

    // efetuar a leitura transaction_pool_size apenas se existir
    if (fscanf(file, "TRANSACTION_POOL_SIZE=%s\n", buffer) == 1 && is_positive_integer(buffer)) {
        TRANSACTION_POOL_SIZE = atoi(buffer);
    }

    fclose(file);
}

int main() {
    parse_config("config.cfg");

    printf("NUM_MINERS: %d\n", NUM_MINERS);
    printf("POOL_SIZE: %d\n", POOL_SIZE);
    printf("TRANSACTIONS_PER_BLOCK: %d\n", TRANSACTIONS_PER_BLOCK);
    printf("BLOCKCHAIN_BLOCKS: %d\n", BLOCKCHAIN_BLOCKS);
    printf("TRANSACTION_POOL_SIZE: %d\n", TRANSACTION_POOL_SIZE);

    int shm_transactionspool_fd, shm_ledger_fd;
    int shm_size;

    // Create the shared memory segment for TRANSACTIONS_POOL
    shm_transactionspool_fd = shm_open(SHM_TRANSACTIONS_POOL, O_CREAT | O_RDWR, 0666);
    if (shm_transactionspool_fd  == -1) {
        perror("Erro na criação de SHM_TRANSACTIONSPOOL");
        exit(EXIT_FAILURE);
    }
    shm_size = sizeof(TransactionPool) * TRANSACTION_POOL_SIZE;
    if (ftruncate(shm_transactionspool_fd, shm_size) == -1) {
        perror("Erro ao definir o tamanho de SHM_TRANSACTIONSPOOL");
        exit(EXIT_FAILURE);
    }

    // Create the shared memory segment for LEDGER
    shm_ledger_fd = shm_open(SHM_LEDGER, O_CREAT | O_RDWR, 0666);
    if (shm_ledger_fd == -1) {
        perror("Erro na criação de SHM_LEDGER");
        exit(EXIT_FAILURE);
    }
    shm_size = 1024; // alterar no futuro para sizeof() com BLOCKCHAIN_BLOCKS * BLOCK_SIZE
    if (ftruncate(shm_ledger_fd, shm_size) == -1) {
        perror("Erro ao definir o tamanho de SHM_LEDGER");
        exit(EXIT_FAILURE);
    }




    // Unmap and close the shared memory segments
    if (munmap(NULL, shm_size) == -1) {
        perror("Erro ao desmapear SHM_TRANSACTIONSPOOL");
    }
    if (close(shm_transactionspool_fd) == -1) {
        perror("Erro ao fechar SHM_TRANSACTIONSPOOL");
    }
    if (shm_unlink(SHM_TRANSACTIONS_POOL) == -1) {
        perror("Erro ao desvincular SHM_TRANSACTIONSPOOL");
    }

    if (munmap(NULL, shm_size) == -1) {
        perror("Erro ao desmapear SHM_LEDGER");
    }
    if (close(shm_ledger_fd) == -1) {
        perror("Erro ao fechar SHM_LEDGER");
    }
    if (shm_unlink(SHM_LEDGER) == -1) {
        perror("Erro ao desvincular SHM_LEDGER");
    }

    return 0;
}