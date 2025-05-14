/*
    Projeto de Sistemas Operativos 2024/2025 - DEIChain
    Diogo Nuno Fonseca Rodrigues 2022257625
    Guilherme Teixeira Gonçalves Rosmaninho 2022257636
*/

#ifndef CONTROLLER_H
#define CONTROLLER_H

#include <semaphore.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CONFIG_FILE "config.cfg"
#define LOG_FILE "DEIChain_log.txt"

#define SEM_TRANSACTIONS_POOL "/sem_transactions_pool"
#define SEM_LEDGER "/sem_ledger"
#define SEM_LOG_FILE "/sem_log_file"
#define SEM_MINERWORK "/sem_minerwork"
#define SEM_PIPECLOSED "/sem_pipeclosed"

#define SHM_TRANSACTIONS_POOL "/shm_transactions_pool"
#define SHM_LEDGER "/shm_ledger"

#define VALIDATION_PIPE "/tmp/validation_pipe"

#define STATISTICS_MQ "/statistics_mq"
#define STATISTICS_MQ_SIZE 10
#define STATISTICS_MQ_MSG_SIZE 1024

#define TX_ID_LEN 64
#define HASH_SIZE 65 // SHA256_DIGEST_LENGTH * 2 + 1
#define TXB_ID_LEN 64

extern int NUM_MINERS;
extern size_t TRANSACTIONS_PER_BLOCK;
extern size_t transactions_per_block; // para compatibilidade com o PoW (sem o modificar)
extern int BLOCKCHAIN_BLOCKS;
extern int LEDGER_SIZE;
extern int TRANSACTION_POOL_SIZE;
extern int shm_transactionspool_size;
extern int shm_ledger_size;

typedef struct
{
    char tx_id[TX_ID_LEN];
    int reward;
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
    unsigned int size;  // capacidade da pool
    unsigned int count; // número de transações na pool
    unsigned int num_miners;
    unsigned int transactions_per_block;
    size_t transactions_offset; // offset para o início das transações
} TransactionPoolSHM;

// estrutura local para facilitar a logica de acesso à transactions pool na shared memory
typedef struct
{
    unsigned int *size;
    unsigned int *count;
    unsigned int *num_miners;
    unsigned int *transactions_per_block;
    PendingTransaction *transactions;
} TransactionPoolInterface;

// Esta estrutura serve para reter informações acerca da informação guardada na shared memory, uma vez que não é possivel utilizar ponteiros na mesma
typedef struct
{
    char txb_id[TXB_ID_LEN];
    char previous_block_hash[HASH_SIZE];
    time_t timestamp;
    unsigned int nonce;
    size_t transactions_offset;
} TransactionBlockSHM;

typedef struct
{
    char *txb_id;
    char *previous_block_hash;
    time_t *timestamp;
    unsigned int *nonce;
    Transaction *transactions;
} TransactionBlockInterface;

// Por forma a minimizar trabalho adicional, esta estrutura aparentemente redundante, serve apenas para poder recorrer às funções de hashing e de verificação de nonce (pow.c), uma vez que a estrutura TransactionBlockInterface não pode ser utilizada diretamente na shared memory, devido à presença de ponteiros.
#pragma pack(push, 1) // assegurar consistencia no alinhamento das estruturas - importante para a serialização
typedef struct
{
    char txb_id[TXB_ID_LEN]; // Unique block ID (e.g., ThreadID + #)
    char previous_block_hash[HASH_SIZE];
    time_t timestamp;
    unsigned int nonce;
    Transaction *transactions;
} TransactionBlock;

#pragma pack(pop)

// Esta estrutura representa um block instanciado

typedef struct
{
    unsigned int last_block_index;   // índice do último bloco
    char last_block_hash[HASH_SIZE]; // hash do último bloco
    unsigned int num_blocks;         // número total de blocos
    unsigned int count;
    size_t blocks_offset; // offset para o início dos blocos
} LedgerSHM;

typedef struct
{
    unsigned int *last_block_index;
    char *last_block_hash;
    unsigned int *num_blocks;
    unsigned int *count;
    TransactionBlockInterface *blocks;
} LedgerInterface;

#pragma pack(push, 1)
typedef struct
{
    char txb_id[TXB_ID_LEN];
    char miner_id[7];
    int block_index;
    int earned_amount;
    time_t timestamp;
} StatisticsMessage;
#pragma pack(pop)

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
