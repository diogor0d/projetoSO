/*
    Projeto de Sistemas Operativos 2024/2025 - DEIChain
    Diogo Nuno Fonseca Rodrigues 2022257625
    Guilherme Teixeira Gonçalves Rosmaninho 2022257636
*/

#define _GNU_SOURCE // para o vasprintf
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <signal.h>

#include "../include/Controller.h"
#include "../include/Miner.h"

int NUM_MINERS;
int POOL_SIZE;
int TRANSACTIONS_PER_BLOCK;
int BLOCKCHAIN_BLOCKS;
int TRANSACTION_POOL_SIZE = 10000; // valor por omissão

// Definições de semáforos e memória partilhada para acesso global

sem_t *sem_transactions_pool, *sem_ledger;
void *shm_transactionspool_base = NULL;
void *shm_ledger_base = NULL;
int shm_transactionspool_fd, shm_ledger_fd, shm_transactionspool_size, shm_ledger_size;

// declarar array para armazenamento dos PIDs dos processos filhos
pid_t pids[3];
#define NUM_CHILDREN 3

// para facilitar o funcionalidade de logging de forma global
static sem_t *sem_log_file = NULL;
static FILE *log_file = NULL;
static char *TIPO_PROCESSO = NULL;

FILE *open_log_file()
{
    FILE *file = fopen(LOG_FILE, "a");
    return file;
}

int is_positive_integer(const char *str)
{
    while (*str)
    {
        if (!isdigit(*str))
        {
            return 0;
        }
        str++;
    }
    return 1;
}

// Função para escrever mensagens de log no logfile e no stdout
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

void parse_config()
{
    FILE *file = fopen(CONFIG_FILE, "r");
    if (file == NULL)
    {
        printf("Erro na abertura do ficheiro de configurações.\n");
        exit(EXIT_FAILURE);
    }

    char buffer[128];
    if (fscanf(file, "NUM_MINERS=%s\n", buffer) != 1 || !is_positive_integer(buffer))
    {
        printf("Valor inválido para NUM_MINERS\n");
        fclose(file);
        exit(EXIT_FAILURE);
    }
    NUM_MINERS = atoi(buffer);

    if (fscanf(file, "POOL_SIZE=%s\n", buffer) != 1 || !is_positive_integer(buffer))
    {
        printf("Valor inválido para POOL_SIZE\n");
        fclose(file);
        exit(EXIT_FAILURE);
    }
    POOL_SIZE = atoi(buffer);

    if (fscanf(file, "TRANSACTIONS_PER_BLOCK=%s\n", buffer) != 1 || !is_positive_integer(buffer))
    {
        printf("Valor inválido para TRANSACTIONS_PER_BLOCK\n");
        fclose(file);
        exit(EXIT_FAILURE);
    }
    TRANSACTIONS_PER_BLOCK = atoi(buffer);

    if (fscanf(file, "BLOCKCHAIN_BLOCKS=%s\n", buffer) != 1 || !is_positive_integer(buffer))
    {
        printf("Valor inválido para BLOCKCHAIN_BLOCKS\n");
        fclose(file);
        exit(EXIT_FAILURE);
    }
    BLOCKCHAIN_BLOCKS = atoi(buffer);

    // efetuar a leitura transaction_pool_size apenas se existir
    if (fscanf(file, "TRANSACTION_POOL_SIZE=%s\n", buffer) == 1 && is_positive_integer(buffer))
    {
        TRANSACTION_POOL_SIZE = atoi(buffer);
    }

    fclose(file);
}

void cleanup()
{
    log_info("A libertar recursos...");

    // Terminate child processes
    for (int i = 0; i < NUM_CHILDREN; i++)
    {
        if (pids[i] > 0) // Ensure the PID is valid
        {
            log_info("Enviando SIGTERM para o processo filho com PID %d", pids[i]);
            if (kill(pids[i], SIGTERM) == -1)
            {
                log_info("Erro ao enviar SIGTERM para o processo filho com PID %d", pids[i]);
            }
        }
    }

    // Wait for child processes to exit
    for (int i = 0; i < NUM_CHILDREN; i++)
    {
        if (pids[i] > 0) // Ensure the PID is valid
        {
            int status;
            if (waitpid(pids[i], &status, 0) == -1)
            {
                log_info("Erro ao esperar pelo processo filho com PID %d", pids[i]);
            }
            else
            {
                log_info("Processo filho com PID %d terminou.", pids[i]);
            }
        }
    }

    // Unmap and close shared memory for transaction pool
    if (munmap(shm_transactionspool_base, shm_transactionspool_size) == -1)
    {
        log_info("Erro ao desmapear SHM_TRANSACTIONSPOOL");
    }
    if (close(shm_transactionspool_fd) == -1)
    {
        log_info("Erro ao fechar SHM_TRANSACTIONSPOOL");
    }
    if (shm_unlink(SHM_TRANSACTIONS_POOL) == -1)
    {
        log_info("Erro ao desvincular SHM_TRANSACTIONSPOOL");
    }

    // Unmap and close shared memory for ledger
    if (munmap(shm_ledger_base, shm_ledger_size) == -1)
    {
        log_info("Erro ao desmapear SHM_LEDGER");
    }
    if (close(shm_ledger_fd) == -1)
    {
        log_info("Erro ao fechar SHM_LEDGER");
    }
    if (shm_unlink(SHM_LEDGER) == -1)
    {
        log_info("Erro ao desvincular SHM_LEDGER");
    }

    // Close semaphores
    // transactions pool
    if (sem_close(sem_transactions_pool) == -1)
    {
        log_info("Erro ao fechar semáforo SEM_TRANSACTIONS_POOL");
    }

    // ledger
    if (sem_close(sem_ledger) == -1)
    {
        log_info("Erro ao fechar semáforo SEM_LEDGER");
    }

    // log file
    if (sem_close(sem_log_file) == -1)
    {
        perror("Erro ao fechar semáforo SEM_LOG_FILE");
    }

    // Unlink semaphores
    // transactions pool
    if (sem_unlink(SEM_TRANSACTIONS_POOL) == -1)
    {
        log_info("Erro ao desvincular semáforo SEM_TRANSACTIONS_POOL");
    }
    // ledger
    if (sem_unlink(SEM_LEDGER) == -1)
    {
        log_info("Erro ao desvincular semáforo SEM_LEDGER");
    }
    // log file
    if (sem_unlink(SEM_LOG_FILE) == -1)
    {
        perror("Erro ao desvincular semáforo SEM_LOG_FILE");
    }

    // Close the log file
    if (log_file)
    {
        fclose(log_file);
    }
}

void sigint(int signum)
{
    (void)signum; // ignorar o sinal, não é necessário para o tratamento
    log_info("SIGINT recebido... Paragem de execução em curso...");
    cleanup();
    ;
    exit(EXIT_SUCCESS);
}

int main()
{
    signal(SIGINT, sigint); // redirecionar o sinal SIGINT para permitir a interrupção do programa

    // Ler e processar o ficheiro de configuração
    parse_config();

    log_file = open_log_file();
    if (!log_file)
    {
        perror("CONTROLLER : Erro ao abrir o ficheiro de log");
        exit(EXIT_FAILURE);
    }
    printf("CONTROLLER DEBUG: log_file = %p\n", log_file);

    // abertura do semaforo para logs
    sem_log_file = sem_open(SEM_LOG_FILE, O_CREAT, 0666, 1);
    if (sem_log_file == SEM_FAILED)
    {
        perror("Erro ao criar semáforo para LOG_FILE");
        sem_unlink(SEM_TRANSACTIONS_POOL); // Clean up
        sem_unlink(SEM_LEDGER);
        exit(EXIT_FAILURE);
    }

    // string para identificar o tipo de processo nos logs
    TIPO_PROCESSO = "CONTROLLER";

    log_info("Processo Controller iniciado (PID: %d)", getpid());
    log_info("Configurações atuais:");
    log_info("NUM_MINERS: %d", NUM_MINERS);
    log_info("POOL_SIZE: %d", POOL_SIZE);
    log_info("TRANSACTIONS_PER_BLOCK: %d", TRANSACTIONS_PER_BLOCK);
    log_info("BLOCKCHAIN_BLOCKS: %d", BLOCKCHAIN_BLOCKS);
    log_info("TRANSACTION_POOL_SIZE: %d\n", TRANSACTION_POOL_SIZE);

    // Criação dos semáforos

    // semaforo transactions pool
    sem_transactions_pool = sem_open(SEM_TRANSACTIONS_POOL, O_CREAT, 0666, 1);
    if (sem_transactions_pool == SEM_FAILED)
    {
        log_info("Erro ao criar semáforo para TRANSACTIONS_POOL");
        sem_unlink(SEM_LOG_FILE);
        exit(EXIT_FAILURE);
    }

    // semaforo ledger
    sem_ledger = sem_open(SEM_LEDGER, O_CREAT, 0666, 1);
    if (sem_ledger == SEM_FAILED)
    {
        log_info("Erro ao criar semáforo para LEDGER");
        sem_unlink(SEM_TRANSACTIONS_POOL);
        sem_unlink(SEM_LOG_FILE);
        exit(EXIT_FAILURE);
    }

    // shared memory transactions pool
    shm_transactionspool_fd = shm_open(SHM_TRANSACTIONS_POOL, O_CREAT | O_RDWR, 0666);
    if (shm_transactionspool_fd == -1)
    {
        log_info("Erro na criação de SHM_TRANSACTIONSPOOL");
        exit(EXIT_FAILURE);
    }
    shm_transactionspool_size = sizeof(TransactionPool) + (TRANSACTION_POOL_SIZE * sizeof(Transaction));
    if (ftruncate(shm_transactionspool_fd, shm_transactionspool_size) == -1)
    {
        log_info("Erro ao definir o tamanho de SHM_TRANSACTIONSPOOL");
        exit(EXIT_FAILURE);
    }
    // mapear a memoria partilhada
    shm_transactionspool_base = mmap(NULL, shm_transactionspool_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_transactionspool_fd, 0);
    if (shm_transactionspool_base == MAP_FAILED)
    {
        log_info("Erro ao mapear SHM_TRANSACTIONSPOOL");
        shm_unlink(SHM_TRANSACTIONS_POOL);
        exit(EXIT_FAILURE);
    }
    // bloquear o semaforo antes de escrever na memoria partilhada
    if (sem_wait(sem_transactions_pool) == -1)
    {
        log_info("Erro ao bloquear semáforo SEM_TRANSACTIONS_POOL");
    }
    memset(shm_transactionspool_base, 0, shm_transactionspool_size);
    // Initialize the TransactionPool in shared memory
    TransactionPool *tx_pool = (TransactionPool *)shm_transactionspool_base;
    tx_pool->current_block_id = 0;
    tx_pool->count = 0;
    tx_pool->size = TRANSACTION_POOL_SIZE;
    //  desbloquear o semaforo após a escrita
    if (sem_post(sem_transactions_pool) == -1)
    {
        log_info("Erro ao desbloquear semáforo SEM_TRANSACTIONS_POOL");
    }

    // Criar o segmento de memoria partilhada para LEDGER
    shm_ledger_fd = shm_open(SHM_LEDGER, O_CREAT | O_RDWR, 0666);
    if (shm_ledger_fd == -1)
    {
        log_info("Erro na criação de SHM_LEDGER");
        exit(EXIT_FAILURE);
    }
    shm_ledger_size = 1024; // alterar no futuro para sizeof() com BLOCKCHAIN_BLOCKS * BLOCK_SIZE
    if (ftruncate(shm_ledger_fd, shm_ledger_size) == -1)
    {
        log_info("Erro ao definir o tamanho de SHM_LEDGER");
        exit(EXIT_FAILURE);
    }

    // Iniciar os processos dos varios componentes

    // Iniciar o miner
    pids[0] = fork();
    if (pids[0] == -1)
    {
        log_info("Erro ao criar o miner process");
        exit(EXIT_FAILURE);
    }
    else if (pids[0] == 0)
    {
        signal(SIGINT, SIG_IGN); // Ignorar o sinal SIGINT no processo miner
        log_info("Miner iniciado com PID %d", getpid());
        miner(NUM_MINERS);
        exit(EXIT_SUCCESS);
    }

    // Iniciar o validator
    pids[1] = fork();
    if (pids[1] == -1)
    {
        signal(SIGINT, SIG_IGN); // Ignorar o sinal SIGINT no processo validator
        log_info("Erro ao criar o validator process");
        exit(EXIT_FAILURE);
    }
    else if (pids[1] == 0)
    {
        log_info("Validator iniciado com PID %d", getpid());
        // validator();        // A implementar
        exit(EXIT_SUCCESS);
    }

    // Inciar o processo de statistics
    pids[2] = fork();
    if (pids[2] == -1)
    {
        signal(SIGINT, SIG_IGN); // Ignorar o sinal SIGINT no processo statistics
        log_info("Erro ao criar o statistics process");
        exit(EXIT_FAILURE);
    }
    else if (pids[2] == 0)
    {
        log_info("Statistics iniciado com PID %d", getpid());
        // statistics();       // A implementar
        exit(EXIT_SUCCESS);
    }

    // Controlador aguarda pelo término de todos os processos filhos
    for (int i = 0; i < 3; i++)
    {
        int status;
        if (waitpid(pids[i], &status, 0) == -1)
        {
            log_info("Erro ao esperar pelo processo filho");
        }
        else
        {
            if (WIFEXITED(status))
            {
                log_info("Processo com PID %d terminou com código de saída %d", pids[i], WEXITSTATUS(status));
            }
            else if (WIFSIGNALED(status))
            {
                log_info("Processo com PID %d terminou devido a sinal %d", pids[i], WTERMSIG(status));
            }
        }
    }

    // libertar os recursos em utilização
    cleanup();

    return 0;
}