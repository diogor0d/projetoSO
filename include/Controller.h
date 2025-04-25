/*
    Projeto de Sistemas Operativos 2024/2025 - DEIChain
    Diogo Nuno Fonseca Rodrigues 2022257625
    Guilherme Teixeira Gonçalves Rosmaninho 2022257636
*/

#include <semaphore.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef CONTROLLER_H
#define CONTROLLER_H

#define CONFIG_FILE "config.cfg"
#define LOG_FILE "DEIChain_log.txt"

#define SEM_TRANSACTIONS_POOL "/sem_transactions_pool"
#define SEM_LEDGER "/sem_ledger"
#define SEM_LOG_FILE "/sem_log_file"

#define SHM_TRANSACTIONS_POOL "/shm_transactions_pool"
#define SHM_LEDGER "/shm_ledger"

#define HASH_SIZE 65 // SHA256_DIGEST_LENGTH * 2 + 1
#define TXB_ID_LEN 64

extern int NUM_MINERS;
extern int TRANSACTIONS_PER_BLOCK;
extern int BLOCKCHAIN_BLOCKS;

typedef struct
{
    unsigned long long id;

    unsigned int reward;
    float value;
    time_t timestamp;
} Transaction;

typedef struct
{
    int empty; // indica se a posição está vazia ou não
    unsigned int age;
    Transaction tx;
} PendingTransaction;

typedef struct
{
    unsigned int size;                            // capacidade da pool
    PendingTransaction *transactions_pending_set; // array de transações pendentes
} TransactionPool;

typedef struct
{
    char txb_id[TXB_ID_LEN];
    char previous_block_hash[HASH_SIZE];
    time_t timestamp;
    Transaction *transactions;
    unsigned int nonce;
} TransactionBlock;

// Inline function to compute the size of a TransactionBlock
static inline size_t get_transaction_block_size()
{
    if (TRANSACTIONS_PER_BLOCK == 0)
    {
        perror("Must set the 'TRANSACTIONS_PER_BLOCK' variable before using!\n");
        exit(-1);
    }
    return sizeof(TransactionBlock) + TRANSACTIONS_PER_BLOCK * sizeof(Transaction);
}

FILE *open_log_file();

#endif // CONTROLLER_H
