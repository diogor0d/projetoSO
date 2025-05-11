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
#include <errno.h>
#include <pthread.h>
#include <mqueue.h>

#include "../include/Controller.h"
#include "../include/Miner.h"
#include "../include/Validator.h"
#include "../include/Statistics.h"
#include "../include/SHMManagement.h"

int NUM_MINERS;
size_t TRANSACTIONS_PER_BLOCK;
size_t transactions_per_block; // para compatibilidade com o PoW
int BLOCKCHAIN_BLOCKS;
int TRANSACTION_POOL_SIZE;

// Definições de semáforos e memória partilhada para acesso global

static int shm_transactionspool_fd, shm_ledger_fd, shm_minerworkcondvar_fd;
static void *shm_transactionspool_base = NULL;
static void *shm_ledger_base = NULL;
static void *shm_minerworkcondvar_base = NULL;

static sem_t *sem_transactions_pool, *sem_ledger, *sem_minerwork, *sem_pipeclosed;

int shm_transactionspool_size, shm_ledger_size;

// mesage queue
mqd_t statistics_mq;

// thread gestao
pthread_t validator_launcher_thread;
static int validator_thread_created = 0;
static volatile int stop_thread = 0;
pid_t validators[2]; // validators extra

// declarar array para armazenamento dos PIDs dos processos filhos
pid_t pids[3];
#define NUM_CHILDREN 3

// para facilitar o funcionalidade de logging de forma global
static sem_t *sem_log_file = NULL;
static FILE *log_file = NULL;
static char *TIPO_PROCESSO = NULL;
static int cleanup_called = 0;

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
    fprintf(stdout, "\n\033[33m%s %s > \033[0m%s", time_str, TIPO_PROCESSO, log_message);
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

    if (fscanf(file, "TRANSACTION_POOL_SIZE=%s\n", buffer) != 1 || !is_positive_integer(buffer))
    {
        printf("Valor inválido para TRANSACTION_POOL_SIZE\n");
        fclose(file);
        exit(EXIT_FAILURE);
    }
    TRANSACTION_POOL_SIZE = atoi(buffer);

    if (fscanf(file, "TRANSACTIONS_PER_BLOCK=%s\n", buffer) != 1 || !is_positive_integer(buffer))
    {
        printf("Valor inválido para TRANSACTIONS_PER_BLOCK\n");
        fclose(file);
        exit(EXIT_FAILURE);
    }
    TRANSACTIONS_PER_BLOCK = atoi(buffer);
    transactions_per_block = TRANSACTIONS_PER_BLOCK; // para compatibilidade com o PoW

    if (fscanf(file, "BLOCKCHAIN_BLOCKS=%s\n", buffer) != 1 || !is_positive_integer(buffer))
    {
        printf("Valor inválido para BLOCKCHAIN_BLOCKS\n");
        fclose(file);
        exit(EXIT_FAILURE);
    }
    BLOCKCHAIN_BLOCKS = atoi(buffer);

    fclose(file);
}

static void cleanup()
{
    if (cleanup_called)
        return;
    cleanup_called = 1;

    log_info("A libertar recursos...");

    // terminar validators adicionais
    log_info("A sinalizar validators adicionais...");
    for (int i = 0; i < 2; i++)
    {
        if (validators[i] > 0)
        {
            if (kill(validators[i], SIGTERM) == -1)
            {
                log_info("Erro ao enviar SIGTERM para o validator %d", validators[i]);
            }
            else
            {
                log_info("Enviado SIGTERM para o validator %d com sucesso", validators[i]);
            }
        }
    }

    // agurdar pelo fim dos validators
    for (int i = 0; i < 2; i++)
    {
        if (validators[i] > 0)
        {
            int status;
            if (waitpid(validators[i], &status, 0) == -1)
            {
                log_info("Erro ao esperar pelo validator %d", validators[i]);
            }
            else
            {
                log_info("Validator %d PID %d terminou.", i, validators[i]);
            }
        }
    }

    // terminar thread de gestao de validators
    if (validator_thread_created)
    {
        // First try clean termination
        stop_thread = 1;

        // Give thread a moment to exit normally
        struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
        nanosleep(&ts, NULL);

        // Then force cancellation if needed
        pthread_cancel(validator_launcher_thread);

        // Join with timeout to avoid hanging
        struct timespec join_timeout;
        clock_gettime(CLOCK_REALTIME, &join_timeout);
        join_timeout.tv_sec += 2; // 2 second timeout

        int join_result = pthread_join(validator_launcher_thread, NULL);
        if (join_result == 0)
        {
            log_info("Thread de gestao de validators terminada com sucesso");
        }
        else
        {
            log_info("Erro ao aguardar pelo término da thread de gestao: %d", join_result);
        }
        validator_thread_created = 0;
    }

    // Terminate child processes
    log_info("A sinalizar processos filhos...");
    for (int i = 0; i < NUM_CHILDREN; i++)
    {
        if (pids[i] > 0) // Ensure the PID is valid
        {
            if (i == 1)
            {
                sem_wait(sem_pipeclosed); // Wait for the pipe to be closed before sending SIGTERM
            }
            log_info("Enviado SIGTERM para o processo filho com PID %d", pids[i]);
            if (kill(pids[i], SIGTERM) == -1)
            {
                log_info("Erro ao enviar SIGTERM para o processo filho com PID %d", pids[i]);
            }
        }
    }

    // Wait for child processes to exit
    log_info("A aguardar pelo fim dos processos filhos...");
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
    if (shm_transactionspool_base != NULL)
    {
        if (munmap(shm_transactionspool_base, shm_transactionspool_size) == -1)
        {
            log_info("Erro ao desmapear %s", SHM_TRANSACTIONS_POOL);
        }
        else
        {
            log_info("Desmapeado %s com sucesso", SHM_TRANSACTIONS_POOL);
        }
    }
    if (shm_transactionspool_fd != -1)
    {
        if (close(shm_transactionspool_fd) == -1)
        {
            log_info("Erro ao fechar %s", SHM_TRANSACTIONS_POOL);
        }
        else
        {
            log_info("Fechado %s com sucesso", SHM_TRANSACTIONS_POOL);
        }
    }
    if (shm_unlink(SHM_TRANSACTIONS_POOL) == -1)
    {
        log_info("Erro ao terminar %s", SHM_TRANSACTIONS_POOL);
    }
    else
    {
        log_info("%s terminado com sucesso", SHM_TRANSACTIONS_POOL);
    }

    // Unmap and close shared memory for ledger
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
    if (shm_unlink(SHM_LEDGER) == -1)
    {
        log_info("Erro ao terminar %s", SHM_LEDGER);
    }
    else
    {
        log_info("%s terminado com sucesso", SHM_LEDGER);
    }

    // shm condvar
    if (shm_minerworkcondvar_fd != -1)
    {
        if (close(shm_minerworkcondvar_fd) == -1)
        {
            log_info("Erro ao fechar %s", SHM_MINERWORK_CONDVAR);
        }
        else
        {
            log_info("%s fechado com sucesso", SHM_MINERWORK_CONDVAR);
        }
    }
    if (shm_minerworkcondvar_base != NULL)
    {
        if (munmap(shm_minerworkcondvar_base, sizeof(MinerWorKCondVar)) == -1)
        {
            log_info("Erro ao desmapear %s", SHM_MINERWORK_CONDVAR);
        }
        else
        {
            log_info("Desmapeado %s com sucesso", SHM_MINERWORK_CONDVAR);
        }
    }
    if (shm_unlink(SHM_MINERWORK_CONDVAR) == -1)
    {
        log_info("Erro ao terminar %s", SHM_MINERWORK_CONDVAR);
    }
    else
    {
        log_info("%s terminado com sucesso", SHM_MINERWORK_CONDVAR);
    }

    // Close semaphores
    // transactions pool
    if (sem_transactions_pool != NULL)
    {
        if (sem_close(sem_transactions_pool) == -1)
        {
            log_info("Erro ao fechar semáforo %s", SEM_TRANSACTIONS_POOL);
        }
        else
        {
            log_info("%s fechado com sucesso", SEM_TRANSACTIONS_POOL);
        }
    }

    // miner work
    if (sem_minerwork != NULL)
    {
        if (sem_close(sem_minerwork) == -1)
        {
            log_info("Erro ao fechar semáforo %s", SEM_MINERWORK);
        }
        else
        {
            log_info("%s fechado com sucesso", SEM_MINERWORK);
        }
    }

    // ledger
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

    // sem pipe closed
    if (sem_pipeclosed != NULL)
    {
        if (sem_close(sem_pipeclosed) == -1)
        {
            log_info("Erro ao fechar semáforo %s", SEM_PIPECLOSED);
        }
        else
        {
            log_info("%s fechado com sucesso", SEM_PIPECLOSED);
        }
    }

    // fechar message queue
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

    // Unlink semaphores
    // transactions pool
    if (sem_unlink(SEM_TRANSACTIONS_POOL) == -1)
    {
        log_info("Erro ao terminar semáforo %s", SEM_TRANSACTIONS_POOL);
    }
    else
    {
        log_info("%s terminado com sucesso", SEM_TRANSACTIONS_POOL);
    }
    // ledger
    if (sem_unlink(SEM_LEDGER) == -1)
    {
        log_info("Erro ao terminar semáforo %s", SEM_LEDGER);
    }
    else
    {
        log_info("%s terminado com sucesso", SEM_LEDGER);
    }
    // miner work
    if (sem_unlink(SEM_MINERWORK) == -1)
    {
        log_info("Erro ao terminar semáforo %s", SEM_MINERWORK);
    }
    else
    {
        log_info("%s terminado com sucesso", SEM_MINERWORK);
    }

    // sem pipe closed
    if (sem_unlink(SEM_PIPECLOSED) == -1)
    {
        log_info("Erro ao terminar semáforo %s", SEM_PIPECLOSED);
    }
    else
    {
        log_info("%s terminado com sucesso", SEM_PIPECLOSED);
    }

    // terminar o pipe
    if (unlink(VALIDATION_PIPE) == -1)
    {
        log_info("Erro ao terminar o pipe %s", VALIDATION_PIPE);
    }
    else
    {
        log_info("%s terminado com sucesso", VALIDATION_PIPE);
    }

    // Terminar message queue
    if (mq_unlink(STATISTICS_MQ) == -1)
    {
        log_info("Erro ao terminar message queue %s", STATISTICS_MQ);
    }
    else
    {
        log_info("%s terminado com sucesso", STATISTICS_MQ);
    }

    // log file
    if (sem_log_file != NULL)
    {
        if (sem_close(sem_log_file) == -1)
        {
            printf("\nController: Erro ao fechar semáforo %s\n", SEM_LOG_FILE);
        }
        else
        {
            printf("\nController: %s fechado com sucesso\n", SEM_LOG_FILE);
        }
    }
    // log file
    if (sem_unlink(SEM_LOG_FILE) == -1)
    {
        printf("\nController: Erro ao terminar semáforo %s\n", SEM_LOG_FILE);
    }
    else
    {
        printf("Controller: %s terminado com sucesso\n", SEM_LOG_FILE);
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
    exit(EXIT_SUCCESS);
}

void *validator_launcher(void *arg)
{
    // Set cancellation state
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    // bloquear todos os sinais nesta thread
    sigset_t set;
    sigfillset(&set);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    log_info("Thread de gestão de validadores em execução...");

    // Use a controlled loop instead of infinite loop
    while (!stop_thread)
    {
        TransactionPoolSHM *pool = (TransactionPoolSHM *)shm_transactionspool_base;

        float occupancy = (float)pool->count / TRANSACTION_POOL_SIZE;
        log_info("Ocupação da transactions pool: %.2f%%", occupancy * 100);

        if (occupancy >= 0.6)
        {
            if (validators[0] == 0)
            {
                // criar o processo de validação
                validators[0] = fork();
                if (validators[0] == 0)
                {
                    // reset na mask de sinais para que o o validator n seja afetado pelo bloqueio de sinais da thread
                    sigset_t empty_set;
                    sigemptyset(&empty_set);
                    sigprocmask(SIG_SETMASK, &empty_set, NULL);
                    // processo filho
                    validator(2);
                    exit(EXIT_SUCCESS);
                }
                else if (validators[0] < 0)
                {
                    log_info("Erro ao criar o processo Validator 2");
                }
                else
                {
                    log_info("Processo Validator criado com PID %d", validators[0]);
                }
            }
            if (occupancy > 0.8)
            {
                if (validators[1] == 0)
                {
                    validators[1] = fork();
                    if (validators[1] == 0)
                    {
                        // reset na mask de sinais para que o o validator n seja afetado pelo bloqueio de sinais da thread
                        sigset_t empty_set;
                        sigemptyset(&empty_set);
                        sigprocmask(SIG_SETMASK, &empty_set, NULL);
                        // processo filho
                        validator(3);
                        exit(EXIT_SUCCESS);
                    }
                    else if (validators[1] < 0)
                    {
                        log_info("Erro ao criar o processo Validator 3");
                    }
                    else
                    {
                        log_info("Processo Validator criado com PID %d", validators[1]);
                    }
                }
            }
        }
        else if (occupancy < 0.4)
        {
            for (int i = 0; i < 2; i++)
            {
                if (validators[i] > 0)
                {
                    log_info("A terminar o processo Validator com PID %d", validators[i]);
                    if (kill(validators[i], SIGTERM) == -1)
                    {
                        log_info("Erro ao enviar SIGTERM para o processo Validator com PID %d", validators[i]);
                    }
                    else
                    {
                        log_info("Enviado SIGTERM para o Validator %d com PID %d com sucesso", i, validators[i]);
                        int status;
                        if (waitpid(validators[i], &status, WNOHANG) > 0)
                        {
                            log_info("Validator %d terminated immediately with status %d", i,
                                     WIFEXITED(status) ? WEXITSTATUS(status) : -1);
                            validators[i] = 0;
                        }
                    }
                }
            }

            // Then check for any remaining zombies
            for (int i = 0; i < 2; i++)
            {
                if (validators[i] > 0)
                {
                    int status;
                    pid_t result = waitpid(validators[i], &status, WNOHANG);
                    if (result > 0)
                    {
                        log_info("Validator %d with PID %d terminated", i, validators[i]);
                        validators[i] = 0;
                    }
                }
            }
        }

        sleep(1);
        pthread_testcancel();
    }

    return NULL;
}

int main()
{
    for (int i = 1; i < NSIG; i++) // NSIG is the total number of signals
    {
        // Ignorar sinais que não podem ser tratados ou que não podem ser ignorados

        // VOLTAR A TIRAR O SIGTSTP
        if (i == SIGTSTP || i == SIGKILL || i == SIGCHLD || i == SIGSTOP || i == 32 || i == 33)
        {
            continue;
        }

        if (signal(i, SIG_IGN) == SIG_ERR)
        {
            fprintf(stderr, "Falha ao ignorar o sinal %d\n", i);
        }
    }

    // Ler e processar o ficheiro de configuração
    parse_config();

    log_file = open_log_file();
    if (!log_file)
    {
        perror("CONTROLLER : Erro ao abrir o ficheiro de log");
        exit(EXIT_FAILURE);
    }

    // abertura do semaforo para logs
    sem_log_file = sem_open(SEM_LOG_FILE, O_CREAT, 0666, 1);
    if (sem_log_file == SEM_FAILED)
    {
        perror("Erro ao criar semáforo para LOG_FILE");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // string para identificar o tipo de processo nos logs
    char temp[20];
    snprintf(temp, sizeof(temp), "CONTROLLER [%d]", getpid());
    TIPO_PROCESSO = strdup(temp);

    log_info("Processo Controller iniciado (PID: %d)", getpid());
    log_info("Configurações atuais:");
    log_info("NUM_MINERS: %d", NUM_MINERS);
    log_info("TRANSACTION_POOL_SIZE: %d\n", TRANSACTION_POOL_SIZE);
    log_info("TRANSACTIONS_PER_BLOCK: %d", TRANSACTIONS_PER_BLOCK);
    log_info("BLOCKCHAIN_BLOCKS: %d", BLOCKCHAIN_BLOCKS);

    // Criação dos semáforos

    // semaforo transactions pool
    sem_transactions_pool = sem_open(SEM_TRANSACTIONS_POOL, O_CREAT, 0666, 1);
    if (sem_transactions_pool == SEM_FAILED)
    {
        log_info("Erro ao criar semáforo para TRANSACTIONS_POOL");
        cleanup();
        exit(EXIT_FAILURE);
    }
    log_info("Semáforo %s criado com sucesso", SEM_TRANSACTIONS_POOL);

    // semaforo ledger
    sem_ledger = sem_open(SEM_LEDGER, O_CREAT, 0666, 1);
    if (sem_ledger == SEM_FAILED)
    {
        log_info("Erro ao criar semáforo para LEDGER");
        cleanup();
        exit(EXIT_FAILURE);
    }
    log_info("Semáforo %s criado com sucesso", SEM_LEDGER);

    // semaforo miners
    sem_minerwork = sem_open(SEM_MINERWORK, O_CREAT, 0666, 0);
    if (sem_minerwork == SEM_FAILED)
    {
        log_info("Erro ao criar semáforo para MINER_WORK");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // sem pipe closed
    sem_pipeclosed = sem_open(SEM_PIPECLOSED, O_CREAT, 0666, 0);
    if (sem_pipeclosed == SEM_FAILED)
    {
        log_info("Erro ao criar semáforo para PIPE_CLOSED");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // shared memory transactions pool
    shm_transactionspool_fd = shm_open(SHM_TRANSACTIONS_POOL, O_CREAT | O_RDWR, 0666);
    if (shm_transactionspool_fd == -1)
    {
        log_info("Erro na criação de SHM_TRANSACTIONSPOOL");
        cleanup();
        exit(EXIT_FAILURE);
    }
    log_info("%s criada com sucesso", SHM_TRANSACTIONS_POOL);
    shm_transactionspool_size = sizeof(TransactionPoolSHM) + (TRANSACTION_POOL_SIZE * sizeof(PendingTransaction));
    if (ftruncate(shm_transactionspool_fd, shm_transactionspool_size) == -1)
    {
        log_info("Erro ao definir o tamanho de SHM_TRANSACTIONSPOOL");
        cleanup();
        exit(EXIT_FAILURE);
    }
    log_info("%s redimensionada com sucesso para %d bytes", SHM_TRANSACTIONS_POOL, shm_transactionspool_size);
    // mapear a memoria partilhada
    shm_transactionspool_base = mmap(NULL, shm_transactionspool_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_transactionspool_fd, 0);
    if (shm_transactionspool_base == MAP_FAILED)
    {
        log_info("Erro ao mapear SHM_TRANSACTIONSPOOL");
        cleanup();
        exit(EXIT_FAILURE);
    }
    log_info("%s mapeada com sucesso", SHM_TRANSACTIONS_POOL);
    memset(shm_transactionspool_base, 0, shm_transactionspool_size); // Inicializar a memória partilhada para a transactions pool
    ((TransactionPoolSHM *)shm_transactionspool_base)
        ->size = TRANSACTION_POOL_SIZE;                                                                  // parametros uteis para txgen
    ((TransactionPoolSHM *)shm_transactionspool_base)->num_miners = NUM_MINERS;                          // parametros uteis para txgen
    ((TransactionPoolSHM *)shm_transactionspool_base)->transactions_per_block = TRANSACTIONS_PER_BLOCK;  // parametros uteis para txgen
    ((TransactionPoolSHM *)shm_transactionspool_base)->transactions_offset = sizeof(TransactionPoolSHM); // Inicializar o contador de transações
    log_info("Memória partilhada para a transactions pool inicializada com sucesso");

    // Criar o segmento de memoria partilhada para LEDGER
    shm_ledger_fd = shm_open(SHM_LEDGER, O_CREAT | O_RDWR, 0666);
    if (shm_ledger_fd == -1)
    {
        log_info("Erro na criação de SHM_LEDGER");
        cleanup();
        exit(EXIT_FAILURE);
    }
    log_info("%s criada com sucesso", SHM_LEDGER);
    shm_ledger_size = sizeof(LedgerSHM) + BLOCKCHAIN_BLOCKS * sizeof(TransactionBlockSHM) + BLOCKCHAIN_BLOCKS * TRANSACTIONS_PER_BLOCK * sizeof(Transaction);
    if (ftruncate(shm_ledger_fd, shm_ledger_size) == -1)
    {
        log_info("Erro ao definir o tamanho de SHM_LEDGER");
        cleanup();
        exit(EXIT_FAILURE);
    }
    log_info("%s redimensionada com sucesso para %d bytes", SHM_LEDGER, shm_ledger_size);
    // mapear a memoria partilhada
    shm_ledger_base = mmap(NULL, shm_ledger_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_ledger_fd, 0);
    if (shm_ledger_base == MAP_FAILED)
    {
        log_info("Erro ao mapear SHM_LEDGER");
        cleanup();
        exit(EXIT_FAILURE);
    }
    log_info("%s mapeada com sucesso", SHM_LEDGER);
    memset(shm_ledger_base, 0, shm_ledger_size);                       // Inicializar a memória partilhada para o ledger
    ((LedgerSHM *)shm_ledger_base)->num_blocks = BLOCKCHAIN_BLOCKS;    // Inicializar o número de blocos no ledger
    ((LedgerSHM *)shm_ledger_base)->blocks_offset = sizeof(LedgerSHM); // Inicializar o offset para as transações
    log_info("Memória partilhada para o ledger inicializada com sucesso");

    //  desmapear a memoria partilhada do processo atual (já que não é necessário)
    if (munmap(shm_ledger_base, shm_ledger_size) == -1)
    {
        log_info("Erro ao desmapear SHM_LEDGER");
    }
    else
    {
        log_info("Desmapeado %s com sucesso", SHM_LEDGER);
    }
    if (close(shm_ledger_fd) == -1)
    {
        log_info("Erro ao fechar SHM_LEDGER");
    }
    else
    {
        log_info("Fechado %s com sucesso", SHM_LEDGER);
    }

    // inicializacao da memoria partilhada para a variavel de condicao para funcionamento dos miners
    shm_minerworkcondvar_fd = shm_open(SHM_MINERWORK_CONDVAR, O_CREAT | O_RDWR, 0666);
    if (shm_minerworkcondvar_fd == -1)
    {
        log_info("Erro na criação de %s", SHM_MINERWORK_CONDVAR);
        cleanup();
        exit(EXIT_FAILURE);
    }
    if (ftruncate(shm_minerworkcondvar_fd, sizeof(MinerWorKCondVar)) == -1)
    {
        perror("Erro ao redimensionar SHM_MINERWORK_CONDVAR");
        exit(EXIT_FAILURE);
    }
    log_info("%s redimensionada com sucesso para %d bytes", SHM_MINERWORK_CONDVAR, sizeof(MinerWorKCondVar));
    // mapeamento
    shm_minerworkcondvar_base = mmap(NULL, sizeof(MinerWorKCondVar), PROT_READ | PROT_WRITE, MAP_SHARED, shm_minerworkcondvar_fd, 0);
    if (shm_minerworkcondvar_base == MAP_FAILED)
    {
        perror("Erro ao mapear SHM_TRANSACTIONS_POOL");
        exit(EXIT_FAILURE);
    }
    memset(shm_minerworkcondvar_base, 0, sizeof(MinerWorKCondVar)); // Inicializar a memória partilhada a 0 para a variavel de condicao

    MinerWorKCondVar *miner_work_condvar = (MinerWorKCondVar *)shm_minerworkcondvar_base;

    // mutex
    pthread_mutexattr_t mutex_attr;
    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);

    // inicializar o mutex na memoria partilhada
    if (pthread_mutex_init(&miner_work_condvar->mutex, &mutex_attr) != 0)
    {
        log_info("Erro ao inicializar o mutex na memória partilhada");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // condvar
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);

    // inicializar a variavel de condicao na memoria partilhada
    if (pthread_cond_init(&miner_work_condvar->cond, &cond_attr) != 0)
    {
        log_info("Erro ao inicializar a variável de condição na memória partilhada");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // Destroy the attributes after initialization
    pthread_mutexattr_destroy(&mutex_attr);
    pthread_condattr_destroy(&cond_attr);

    // Criar pipe para comunição entre validator e miner
    if ((mkfifo(VALIDATION_PIPE, O_CREAT | O_EXCL | 0777) < 0) && (errno != EEXIST))
    {
        log_info("Falha ao criar o pipe %s: %s", VALIDATION_PIPE, strerror(errno));
        cleanup();
        exit(EXIT_FAILURE);
    }
    else
    {
        log_info("Pipe %s criado com sucesso", VALIDATION_PIPE);
    }

    // criar a message queue para o statistics
    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = STATISTICS_MQ_SIZE; // número máximo de mensagens na fila
    attr.mq_msgsize = STATISTICS_MQ_MSG_SIZE;
    attr.mq_curmsgs = 0;

    statistics_mq = mq_open(STATISTICS_MQ, O_CREAT | O_RDWR, 0666, &attr);
    if (statistics_mq == (mqd_t)-1)
    {
        log_info("Erro ao criar a message queue %s: %s", STATISTICS_MQ, strerror(errno));
        cleanup();
        exit(EXIT_FAILURE);
    }
    else
    {
        log_info("Message queue %s criada com sucesso", STATISTICS_MQ);
    }

    // Iniciar os processos dos varios componentes

    // IMPLEMENTACAO DE VALIDATOR DESATUALIZADA :
    // - thread para gestao de numero de validators
    // - numero de validators aativo dinamico
    // Iniciar o validator

    // Iniciar o miner
    pids[0] = fork();
    if (pids[0] == -1)
    {
        log_info("Erro ao criar o miner process");
        cleanup();
        exit(EXIT_FAILURE);
    }
    else if (pids[0] == 0)
    {
        log_info("Miner iniciado com PID %d", getpid());
        miner();
        exit(EXIT_SUCCESS);
    }

    pids[1] = fork();
    if (pids[1] == -1)
    {
        log_info("Erro ao criar o validator process");
        cleanup();
        exit(EXIT_FAILURE);
    }
    else if (pids[1] == 0)
    {
        log_info("Validator iniciado com PID %d", getpid());
        validator(1);
        exit(EXIT_SUCCESS);
    }

    // Inciar o processo de statistics
    pids[2] = fork();
    if (pids[2] == -1)
    {
        log_info("Erro ao criar o statistics process");
        cleanup();
        exit(EXIT_FAILURE);
    }
    else if (pids[2] == 0)
    {
        log_info("Statistics iniciado com PID %d", getpid());
        statistics(); // A implementar
        exit(EXIT_SUCCESS);
    }

    // thread the gestao de validators
    if (pthread_create(&validator_launcher_thread, NULL, validator_launcher, NULL) != 0)
    {
        log_info("Erro ao criar a thread de gestão de validators");
        cleanup();
        exit(EXIT_FAILURE);
    }
    else
    {
        log_info("Thread de gestão de validators criada com sucesso.");
        validator_thread_created = 1;
    }

    // Voltar a tratar os sinais de interrupção e paragem
    signal(SIGINT, sigint);

    // esperar pelo fim da thread de gestao de validators
    pthread_join(validator_launcher_thread, NULL);

    // esperar pelos validators adicionais
    for (int i = 0; i < 2; i++)
    {
        if (validators[i] > 0)
        {
            int status;
            if (waitpid(validators[i], &status, 0) == -1)
            {
                log_info("Erro ao esperar pelo processo Validator com PID %d", validators[i]);
            }
            else
            {
                log_info("Processo Validator com PID %d terminou.", validators[i]);
            }
        }
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