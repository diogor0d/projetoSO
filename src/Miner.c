/*
    Projeto de Sistemas Operativos 2024/2025 - DEIChain
    Diogo Nuno Fonseca Rodrigues 2022257625
    Guilherme Teixeira Gonçalves Rosmaninho 2022257636
*/

#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>

#include "../include/Controller.h"

typedef struct
{
    int thread_id;
    sem_t *sem_log_file;
    FILE *log_file;
} MinerThreadArgs;

void *miner_thread(void *arg)
{
    MinerThreadArgs *args = (MinerThreadArgs *)arg;
    int thread_id = args->thread_id;
    sem_t *sem_log_file = args->sem_log_file;
    FILE *log_file = args->log_file;

    log_info(sem_log_file, log_file, "Miner thread %d (PID: %d) is running...", thread_id, getpid());

    sleep(500);

    log_info(sem_log_file, log_file, "Miner thread %d has finished.", thread_id);
    pthread_exit(NULL);
}

void miner(int num_miners)
{
    pthread_t threads[num_miners];
    MinerThreadArgs thread_args[num_miners];

    // Abrir o semaforo para logs (já existente)
    sem_t *sem_log_file = sem_open(SEM_LOG_FILE, 0);
    if (sem_log_file == SEM_FAILED)
    {
        perror("Erro ao abrir semáforo para LOG_FILE");
        return;
    }

    FILE *log_file = open_log_file();

    for (int i = 0; i < num_miners; i++)
    {
        thread_args[i].thread_id = i + 1;
        thread_args[i].sem_log_file = sem_log_file;
        thread_args[i].log_file = log_file;

        if (pthread_create(&threads[i], NULL, miner_thread, &thread_args[i]) != 0)
        {
            log_info(sem_log_file, log_file, "Erro ao criar miner thread");
            return;
        }
    }

    for (int i = 0; i < num_miners; i++)
    {
        // espera pelo fim de cada thread
        pthread_join(threads[i], NULL);
    }

    log_info(sem_log_file, log_file, "Todas as miner threads terminaram.\n");

    // fechar o semaforo para logs
    sem_close(sem_log_file);
}