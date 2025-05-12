/*
    Projeto de Sistemas Operativos 2024/2025 - DEIChain
    Diogo Nuno Fonseca Rodrigues 2022257625
    Guilherme Teixeira Gonçalves Rosmaninho 2022257636
*/

#define _GNU_SOURCE // para o vasprintf e strcasestr
#include <stdarg.h>
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <semaphore.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "../include/Controller.h" // where STATISTICS_MQ and StatisticsMessage are defined
#include "../include/SHMManagement.h"

static sem_t *sem_log_file = NULL;
static FILE *log_file = NULL;
static char TIPO_PROCESSO_BUFFER[32];              // Larger static buffer to hold the process type string
static char *TIPO_PROCESSO = TIPO_PROCESSO_BUFFER; // Point to the static buffer

static int shm_ledger_fd = -1;
static void *shm_ledger_base = NULL;
static sem_t *sem_ledger = NULL;
int shm_ledger_size;
static LedgerInterface ledger;

static mqd_t statistics_mq;
static char *buf;

typedef struct
{
    int *miner_valid_blocks;    // blocos validos de cada miner
    int *miner_invalid_blocks;  // blocos invalidos de cada miner
    double total_tx_time;       // tempo total de todas as transacoes (para calculo de media)
    int *miner_credits;         // creditos de cada miner
    int total_blocks_processed; // total de blocos processados validos e invalidos
    int blocks_in_chain;        // blocos na blockchain
} Statistics;

Statistics stats = {0};

static void log_info(const char *format, ...)
{
    char *log_message;
    va_list args;

    va_start(args, format);

    // Format the string (outside of critical section)
    if (vasprintf(&log_message, format, args) == -1)
    {
        va_end(args);
        perror("Erro ao formatar mensagem de log");
        return;
    }

    va_end(args);

    // Get current time (outside of critical section)
    time_t rawtime;
    struct tm *timeinfo;
    char time_str[20]; // Buffer for "dd/mm/yyyy hh:mm:ss"
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(time_str, sizeof(time_str), "%d/%m/%Y %H:%M:%S", timeinfo);

    // Determine color based on content (outside of critical section)
    const char *color_code = "\033[0m"; // Default: no color

    if (strcasestr(log_message, "erro") != NULL || strcasestr(log_message, "rejeitado"))
    {
        color_code = "\033[31m"; // Red
    }
    else if (strcasestr(log_message, "sucesso") != NULL || strcasestr(log_message, "execução") != NULL || strcasestr(log_message, "iniciado") != NULL)
    {
        color_code = "\033[32m"; // Green
    }

    // CRITICAL SECTION BEGINS - Only lock when actually writing
    sem_wait(sem_log_file);

    // Write to log file
    fprintf(log_file, "%s %s > %s\n", time_str, TIPO_PROCESSO, log_message);
    fflush(log_file);

    // Write to stdout
    fprintf(stdout, "\n\033[33m%s %s > \033[0m%s%s\033[0m",
            time_str, TIPO_PROCESSO, color_code, log_message);
    fflush(stdout);

    sem_post(sem_log_file);
    // CRITICAL SECTION ENDS

    // Free memory (outside of critical section)
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

    if (stats.miner_invalid_blocks)
    {
        free(stats.miner_invalid_blocks);
        stats.miner_invalid_blocks = NULL;
    }

    if (stats.miner_valid_blocks)
    {
        free(stats.miner_valid_blocks);
        stats.miner_valid_blocks = NULL;
    }
    if (stats.miner_credits)
    {
        free(stats.miner_credits);
        stats.miner_credits = NULL;
    }

    if (shm_ledger_fd != -1)
    {
        if (close(shm_ledger_fd) == -1)
        {
            log_info("Erro ao fechar %s", SHM_LEDGER);
        }
        else
        {
            log_info("%s fechada com sucesso", SHM_LEDGER);
        }
    }
    if (shm_ledger_base != NULL)
    {
        if (munmap(shm_ledger_base, shm_ledger_size) == -1)
        {
            log_info("Erro ao desmapear %s", SHM_LEDGER);
        }
        else
        {
            log_info("Desmapeado %s com sucesso", SHM_LEDGER);
        }
    }
    if (sem_ledger != NULL)
    {
        if (sem_close(sem_ledger) == -1)
        {
            log_info("Erro ao fechar semáforo %s", SEM_LEDGER);
        }
        else
        {
            log_info("%s fechado com sucesso", SEM_LEDGER);
        }
    }

    // Close the log file
    if (log_file)
    {
        fclose(log_file);
    }

    if (buf)
    {
        free(buf);
        buf = NULL;
    }

    freeLedger(&ledger);

    // fechar o semaforo para logs
    if (sem_log_file != NULL)
    {
        if (sem_close(sem_log_file) == -1)
        {
            printf("\nErro ao fechar semáforo %s", SEM_LOG_FILE);
        }
        else
        {
            printf("\n%s fechado com sucesso", SEM_LOG_FILE);
        }
    }
}

void printStatistics()
{
    // print statistics struct
    log_info("Estatísticas:");
    log_info("Total de blocos processados: %d", stats.total_blocks_processed);
    log_info("Total de blocos válidos: %d", stats.blocks_in_chain);
    log_info("Tempo médio para aprovação de uma transação: %.2f segundos", stats.total_tx_time / (stats.blocks_in_chain * TRANSACTIONS_PER_BLOCK));
    for (int i = 0; i < NUM_MINERS; i++)
    {
        log_info("Miner %d: Blocos válidos: %d, Blocos inválidos: %d, Créditos: %d", i + 1, stats.miner_valid_blocks[i], stats.miner_invalid_blocks[i], stats.miner_credits[i]);
    }
}

static void sigterm(int signum)
{
    (void)signum; // Ignore the signal parameter

    log_info("SIGTERM recebido. A terminar execução...");

    printStatistics(); // Print statistics before exiting

    cleanup(); // Call the cleanup function to close the log file and semaphores

    // Exit the process
    exit(EXIT_SUCCESS);
}

static void handle_sigusr1(int signum)
{
    (void)signum; // Silence unused parameter warning
    log_info("SIGUSR1 recebido - impressão de estatísticas...");

    printStatistics();
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
    snprintf(TIPO_PROCESSO, sizeof(TIPO_PROCESSO_BUFFER), "STATISTICS [%d]", getpid());

    /* 1) Open the queue for reading */
    statistics_mq = mq_open(STATISTICS_MQ, O_RDONLY);
    if (statistics_mq == (mqd_t)-1)
    {
        log_info("Erro ao abrir a message queue");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // Criar o segmento de memoria partilhada para LEDGER
    shm_ledger_fd = shm_open(SHM_LEDGER, O_RDWR, 0666);
    if (shm_ledger_fd == -1)
    {
        log_info("MINER : Erro ao abrir memória partilhada para ledger");
        exit(EXIT_FAILURE);
    }
    log_info("%s aberta com sucesso", SHM_LEDGER);

    // mapear a memoria partilhada para o espaço de memória do processo
    shm_ledger_base = mmap(NULL, shm_ledger_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_ledger_fd, 0);
    if (shm_ledger_base == MAP_FAILED)
    {
        log_info("MINER : Erro ao mapear memória partilhada para transactions pool");
        close(shm_ledger_fd);
        exit(EXIT_FAILURE);
    }
    log_info("%s mapeada com sucesso", SHM_LEDGER);

    ledger = interfaceLedger(shm_ledger_base);

    sem_ledger = sem_open(SEM_LEDGER, 0);
    if (sem_ledger == SEM_FAILED)
    {
        log_info("Erro ao abrir semáforo %s", SEM_LEDGER);
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

    log_info("A receber mensagens da message queue '%s' (msgsize=%ld)...",
             STATISTICS_MQ, attr.mq_msgsize);

    stats.miner_invalid_blocks = (int *)calloc(NUM_MINERS, sizeof(int));
    stats.miner_valid_blocks = (int *)calloc(NUM_MINERS, sizeof(int));
    stats.miner_credits = (int *)calloc(NUM_MINERS, sizeof(int));
    stats.total_tx_time = 0;
    stats.total_blocks_processed = 0;
    stats.blocks_in_chain = 0;

    signal(SIGTERM, sigterm); // processar sigterm após inicio seguro
    signal(SIGUSR1, handle_sigusr1);

    /* 4) Loop receiving */
    while (1)
    {
        ssize_t bytes_received = mq_receive(statistics_mq, buf, attr.mq_msgsize, NULL);

        if (bytes_received >= 0)
        {
            if ((size_t)bytes_received < sizeof(StatisticsMessage))
            {
                log_info("Aviso: mensagem recebida (%zd bytes) menor que sizeof(StatisticsMessage) (%zu bytes)\n", bytes_received, sizeof(StatisticsMessage));
                continue;
            }

            /* Cast into your struct and print */
            StatisticsMessage *msg = (StatisticsMessage *)buf;

            // log_info("Bloco recebido: %s, Miner ID: %s, Índice do bloco: %d, Recompensa obtida: %d, Timestamp %ld", msg->txb_id, msg->miner_id, msg->block_index, msg->earned_amount, msg->timestamp);

            if (msg->block_index == -1)
            {
                int miner_id = atoi(msg->miner_id) - 1;
                stats.miner_invalid_blocks[miner_id]++;
                stats.total_blocks_processed++;
            }
            else
            {
                int miner_id = atoi(msg->miner_id) - 1;
                stats.miner_valid_blocks[miner_id]++;
                stats.miner_credits[miner_id] += msg->earned_amount;

                stats.total_blocks_processed++;
                stats.blocks_in_chain++;

                sem_wait(sem_ledger);
                if (ledger.blocks[msg->block_index].transactions != NULL)
                    for (long unsigned int i = 0; i < TRANSACTIONS_PER_BLOCK; i++)
                    {
                        stats.total_tx_time += (msg->timestamp - ledger.blocks[msg->block_index].transactions[i].timestamp);
                    }
                sem_post(sem_ledger);
            }
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
