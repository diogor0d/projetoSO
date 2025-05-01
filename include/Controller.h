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

#define VALIDATION_PIPE "/tmp/validation_pipe"

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
    int filled; // indica se a posição está vazia ou não
    unsigned int age;
    Transaction tx;
} PendingTransaction;

typedef struct
{
    unsigned int size;          // capacidade da pool
    unsigned int count;         // número de transações na pool
    size_t transactions_offset; // offset para o início das transações
} TransactionPoolSHM;

// estrutura local para facilitar a logica de acesso à transactions pool na shared memory
typedef struct
{
    unsigned int *size;               // Pointer to the size field in shared memory
    unsigned int *count;              // Pointer to the count field in shared memory
    PendingTransaction *transactions; // Pointer to the array of transactions in shared memory
} TransactionPoolInterface;

// Esta estrutura serve para reter informações acerca da informação guardada na shared memory, uma vez que não é possivel utilizar ponteiros na mesma
typedef struct
{
    char txb_id[TXB_ID_LEN];
    char previous_block_hash[HASH_SIZE];
    time_t timestamp;
    unsigned int nonce;
    unsigned int num_transactions;
    size_t transactions_offset;
} TransactionBlockSHM;

typedef struct
{
    char *txb_id;                   // Pointer to the block ID in shared memory
    char *previous_block_hash;      // Pointer to the previous block hash in shared memory
    time_t *timestamp;              // Pointer to the timestamp in shared memory
    unsigned int *nonce;            // Pointer to the nonce in shared memory
    unsigned int *num_transactions; // Pointer to the number of transactions in shared memory
    Transaction *transactions;      // Pointer to the array of transactions in shared memory
} TransactionBlockInterface;

// Por forma a minimizar trabalho adicional, esta estrutura aparentemente redundante, serve apenas para poder recorrer às funções de hashing e de verificação de nonce (pow.c), uma vez que a estrutura TransactionBlockInterface não pode ser utilizada diretamente na shared memory, devido à presença de ponteiros.
typedef struct
{
    char txb_id[TXB_ID_LEN]; // Unique block ID (e.g., ThreadID + #)
    char previous_block_hash[HASH_SIZE];
    time_t timestamp;          // Pointer to the timestamp in shared memory
    unsigned int nonce;        // Pointer to the nonce in shared memory
    Transaction *transactions; // Pointer to the array of transactions in shared memory
} TransactionBlock;

// Esta estrutura representa um block instanciado

typedef struct
{
    unsigned int last_block_index;   // índice do último bloco
    char last_block_hash[HASH_SIZE]; // hash do último bloco
    unsigned int num_blocks;         // número total de blocos
    size_t blocks_offset;            // offset para o início dos blocos
} LedgerSHM;

typedef struct
{
    unsigned int *last_block_index;    // Pointer to the index of the last block
    char *last_block_hash;             // Pointer to the hash of the last block
    unsigned int *num_blocks;          // Pointer to the total number of blocks
    TransactionBlockInterface *blocks; // Pointer to the array of blocks in shared memory
} LedgerInterface;

// Inline function to compute the size of a TransactionBlock
static inline size_t
get_transaction_block_size()
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
