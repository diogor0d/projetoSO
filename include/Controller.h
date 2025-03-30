/*
    Projeto de Sistemas Operativos 2024/2025 - DEIChain
    Diogo Nuno Fonseca Rodrigues 2022257625
    Guilherme Teixeira Gonçalves Rosmaninho 2022257636
*/

#include <semaphore.h>

#ifndef CONTROLLER_H
#define CONTROLLER_H

#define CONFIG_FILE "config.cfg"
#define LOG_FILE "DEIChain_log.txt"

#define SEM_TRANSACTIONS_POOL "/sem_transactions_pool"
#define SEM_LEDGER "/sem_ledger"
#define SEM_LOG_FILE "/sem_log_file"

#define SHM_TRANSACTIONS_POOL "/shm_transactions_pool"
#define SHM_LEDGER "/shm_ledger"

typedef struct
{
    unsigned long long id;
    unsigned int reward;
    unsigned int sender;
    unsigned int receiver;
    unsigned int age;
    double value;
    unsigned long long created_at;
} Transaction;

typedef struct
{
    unsigned int size; // capacidade da pool
    unsigned int count;
    unsigned long long current_block_id;
    Transaction transactions[];
} TransactionPool;

FILE *open_log_file();
void log_info(sem_t *sem_log_file, FILE *log_file, const char *format, ...);

#endif // CONTROLLER_H
