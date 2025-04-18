/*
    Projeto de Sistemas Operativos 2024/2025 - DEIChain
    Diogo Nuno Fonseca Rodrigues 2022257625
    Guilherme Teixeira Gonçalves Rosmaninho 2022257636
*/

#define _GNU_SOURCE // para o vasprintf
#include <stdio.h>
#include <pthread.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

#include "../include/Controller.h"

static sem_t *sem_log_file = NULL;
static FILE *log_file = NULL;
static char *TIPO_PROCESSO = NULL;

typedef struct
{
    int thread_id;
} MinerThreadArgs;

static void log_info(const char *format, ...)
{

    char *log_message;
    va_list args;

    va_start(args, format);

    // formatar a string
    if (vasprintf(&log_message, format, args) == -1)
    {
        va_end(args);
        perror("Erro ao formatar mensagem de log");
        return;
    }

    va_end(args);

    // bloquear o semáforo
    sem_wait(sem_log_file);

    // obter o tempo atual
    time_t rawtime;
    struct tm *timeinfo;
    char time_str[20]; // Buffer para "dd/mm/yyyy hh:mm:ss"
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(time_str, sizeof(time_str), "%d/%m/%Y %H:%M:%S", timeinfo);

    // Escrever a mensagem de log no ficheiro e no stdout
    fprintf(log_file, "%s %s > %s\n", time_str, TIPO_PROCESSO, log_message);
    fflush(log_file);
    fprintf(stdout, "\033[33m%s %s > \033[0m%s\n", time_str, TIPO_PROCESSO, log_message);
    fflush(stdout);

    // desbloquear o semáforo
    sem_post(sem_log_file);

    // libertar a memoria alocada para a mensagem formatada
    free(log_message);
}

void *miner_thread(void *arg)
{
    MinerThreadArgs *args = (MinerThreadArgs *)arg;
    int thread_id = args->thread_id;

    log_info("Miner thread %d (PID: %d) is running...", thread_id, getpid());

    sleep(500);

    log_info("Miner thread %d has finished.", thread_id);
    pthread_exit(NULL);
}

void miner(int num_miners)
{
    pthread_t threads[num_miners];
    MinerThreadArgs thread_args[num_miners];

    // Abrir o semaforo para logs (já existente)
    sem_log_file = sem_open(SEM_LOG_FILE, 0);
    if (sem_log_file == SEM_FAILED)
    {
        perror("MINER : Erro ao abrir semáforo para LOG_FILE");
        return;
    }

    log_file = open_log_file();
    if (log_file == NULL)
    {
        perror("MINER : Erro ao abrir o ficheiro de log");
        return;
    }

    TIPO_PROCESSO = "MINER";

    for (int i = 0; i < num_miners; i++)
    {
        thread_args[i].thread_id = i + 1;

        if (pthread_create(&threads[i], NULL, miner_thread, &thread_args[i]) != 0)
        {
            log_info("Erro ao criar miner thread");
            return;
        }
    }

    for (int i = 0; i < num_miners; i++)
    {
        // espera pelo fim de cada thread
        pthread_join(threads[i], NULL);
    }

    log_info("Todas as miner threads terminaram.\n");

    // fechar o semaforo para logs
    sem_close(sem_log_file);
}