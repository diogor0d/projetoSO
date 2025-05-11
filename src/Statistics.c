/*
    Projeto de Sistemas Operativos 2024/2025 - DEIChain
    Diogo Nuno Fonseca Rodrigues 2022257625
    Guilherme Teixeira Gonçalves Rosmaninho 2022257636
*/

#define _GNU_SOURCE // para o vasprintf
#include <stdarg.h>
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include "../include/Controller.h" // where STATISTICS_MQ and StatisticsMessage are defined

static sem_t *sem_log_file = NULL;
static FILE *log_file = NULL;
static char *TIPO_PROCESSO = NULL;

static mqd_t statistics_mq;
static char *buf;

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
    fprintf(stdout, "\n\033[33m%s %s > \033[0m%s\033[0m", time_str, TIPO_PROCESSO, log_message);
    fflush(stdout);

    // desbloquear o semáforo
    sem_post(sem_log_file);

    // libertar a memoria alocada para a mensagem formatada
    free(log_message);
}

static void cleanup()
{
    if (statistics_mq != -1)
    {
        if (mq_close(statistics_mq) == -1)
        {
            log_info("Erro ao fechar message queue %s", STATISTICS_MQ);
        }
        else
        {
            log_info("%s fechado com sucesso", STATISTICS_MQ);
        }
    }

    if (buf)
    {
        free(buf);
        buf = NULL;
    }

    // Close the log file
    if (log_file)
    {
        fclose(log_file);
    }

    // fechar o semaforo para logs
    if (sem_log_file != NULL)
    {
        if (sem_close(sem_log_file) == -1)
        {
            log_info("Erro ao fechar semáforo %s", SEM_LOG_FILE);
        }
        else
        {
            log_info("%s fechado com sucesso", SEM_LOG_FILE);
        }
    }
}

static void sigterm(int signum)
{
    (void)signum; // Ignore the signal parameter

    log_info("SIGTERM recebido. A terminar execução...");

    cleanup(); // Call the cleanup function to close the log file and semaphores

    // Exit the process
    exit(EXIT_SUCCESS);
}

void statistics()
{
    // Abrir o semaforo para logs (já existente)
    sem_log_file = sem_open(SEM_LOG_FILE, 0);
    if (sem_log_file == SEM_FAILED)
    {
        perror("\nVALIDATOR : Erro ao abrir semáforo para LOG_FILE");
        cleanup();
        return;
    }

    log_file = open_log_file();
    if (log_file == NULL)
    {
        perror("\nVALIDATOR : Erro ao abrir o ficheiro de log");
        cleanup();
        return;
    }
    char temp[64];
    snprintf(temp, sizeof(temp), "STATISTICS [%d]", getpid());
    TIPO_PROCESSO = strdup(temp);

    /* 1) Open the queue for reading */
    statistics_mq = mq_open(STATISTICS_MQ, O_RDONLY);
    if (statistics_mq == (mqd_t)-1)
    {
        log_info("Erro ao abrir a message queue");
        cleanup();
        exit(EXIT_FAILURE);
    }

    /* 2) Query its attributes so we know how big to make our buffer */
    struct mq_attr attr;
    if (mq_getattr(statistics_mq, &attr) == -1)
    {
        log_info("mq_getattr");
        cleanup();
        exit(EXIT_FAILURE);
    }

    /* 3) Allocate a buffer of exactly mq_msgsize bytes */
    buf = malloc(attr.mq_msgsize);
    if (!buf)
    {
        log_info("malloc");
        cleanup();
        exit(EXIT_FAILURE);
    }

    log_info("A receber mensagens da message queue '%s' (msgsize=%ld)...\n",
             STATISTICS_MQ, attr.mq_msgsize);

    signal(SIGTERM, sigterm); // processar sigterm após inicio seguro

    /* 4) Loop receiving */
    while (1)
    {
        ssize_t bytes_received =
            mq_receive(statistics_mq, buf, attr.mq_msgsize, NULL);

        if (bytes_received >= 0)
        {
            if ((size_t)bytes_received < sizeof(StatisticsMessage))
            {
                log_info("Aviso: mensagem recebida (%zd bytes) menor que sizeof(StatisticsMessage) (%zu bytes)\n",
                         bytes_received, sizeof(StatisticsMessage));
                continue;
            }

            /* Cast into your struct and print */
            StatisticsMessage *msg = (StatisticsMessage *)buf;
            log_info("Bloco recebido: %s, Miner ID: %s, Índice do bloco: %d, Recompensa obtida: %d", msg->txb_id, msg->miner_id, msg->block_index, msg->earned_amount);
        }
        else
        {
            if (errno == EINTR)
            {
                /* Interrupted by signal — just retry */
                continue;
            }
            log_info("Erro ao receber mensagem da message queue");
            break;
        }
    }

    cleanup();
}
