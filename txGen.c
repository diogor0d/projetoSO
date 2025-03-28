#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>

#include "Controller.h"

#define SHM_TRANSACTIONS_POOL "/shm_transactions_pool"
#define TRANSACTION_POOL_SIZE 10000

typedef struct {
    int id;
    double reward;
    int sender;
    int receiver;
    double value;
    time_t created_at;
} Transaction;

typedef struct {
    int current_block_id;
    Transaction transactions[TRANSACTION_POOL_SIZE];
} TransactionPool;

void generate_transaction(Transaction *tx, int id) {
    tx->id = id;
    tx->reward = (double)(rand() % 1000) / 100.0;
    tx->sender = rand() % 1000;
    tx->receiver = rand() % 1000;
    tx->value = (double)(rand() % 10000) / 100.0;
    tx->created_at = time(NULL);
}

int main() {
    int shm_fd;
    TransactionPool *tx_pool;

    // Open the shared memory segment
    shm_fd = shm_open(SHM_TRANSACTIONS_POOL, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("Erro ao abrir SHM_TRANSACTIONS_POOL");
        exit(EXIT_FAILURE);
    }

    // Map the shared memory segment
    tx_pool = mmap(NULL, sizeof(TransactionPool), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (tx_pool == MAP_FAILED) {
        perror("Erro ao mapear SHM_TRANSACTIONS_POOL");
        close(shm_fd);
        exit(EXIT_FAILURE);
    }

    srand(time(NULL));
    int tx_id = 0;

    while (1) {
        Transaction new_tx;
        generate_transaction(&new_tx, tx_id);

        // Add the new transaction to the pool
        tx_pool->transactions[tx_id % TRANSACTION_POOL_SIZE] = new_tx;
        tx_id++;

        printf("Generated transaction ID: %d\n", new_tx.id);

        // Sleep for a specified time period (e.g., 1 second)
        sleep(1);
    }

    // Unmap and close the shared memory segment
    if (munmap(tx_pool, sizeof(TransactionPool)) == -1) {
        perror("Erro ao desmapear SHM_TRANSACTIONS_POOL");
    }
    if (close(shm_fd) == -1) {
        perror("Erro ao fechar SHM_TRANSACTIONS_POOL");
    }

    return 0;
}
