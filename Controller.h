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
    int id;
    int reward;
    int sender;
    int receiver;
    int age;
    double value;
    time_t created_at;
} Transaction;

typedef struct
{
    int size; // capacidade da pool
    int count;
    int current_block_id;
    Transaction transactions[];
} TransactionPool;

void log_info(sem_t *sem_log_file, const char *format, ...);

#endif // CONTROLLER_H
