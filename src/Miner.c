/*
    Projeto de Sistemas Operativos 2024/2025 - DEIChain
    Diogo Nuno Fonseca Rodrigues 2022257625
    Guilherme Teixeira Gonçalves Rosmaninho 2022257636
*/

#define _GNU_SOURCE // para o vasprintf e strcasestr
#include <stdio.h>
#include <pthread.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <openssl/sha.h>
#include <stdbool.h>
#include <errno.h>

#include "../include/Controller.h"
#include "../include/PoW/pow.h"
#include "../include/SHMManagement.h"

static sem_t *sem_log_file = NULL;
static FILE *log_file = NULL;

static char TIPO_PROCESSO_BUFFER[32];              // Larger static buffer to hold the process type string
static char *TIPO_PROCESSO = TIPO_PROCESSO_BUFFER; // Point to the static buffer

static sem_t *sem_transactionspool = NULL;
static sem_t *sem_minerwork = NULL;
static sem_t *sem_ledger = NULL;
static sem_t *sem_pipeclosed = NULL;

// aceder a variavel globais do controller
int NUM_MINERS;
int LEDGER_SIZE;
size_t TRANSACTIONS_PER_BLOCK;
int shm_transactionspool_size;
int shm_ledger_size;

static LedgerInterface ledgerInterface;
static TransactionPoolInterface tx_pool;

// definições de variaveis da transactions pool para acesso em todas as threads
static void *shm_transactionspool_base = NULL;
static int shm_transactionspool_fd = -1;

// definições de variaveis da ledger para acesso em todas as threads
static void *shm_ledger_base = NULL;
static int shm_ledger_fd = -1;

// variaveis para o pipe de comunicação entre miner e validator
static int validation_pipe_fd = -1;

// flag para paragem das threads ; volatile assegura o bom acesso à variavel em qualquer thread
volatile int stop_threads = 0;

typedef struct
{
    int thread_id;
} MinerThreadArgs;

typedef struct
{
    sigset_t *set;
    pthread_t *threads;
} SignalHandlerArgs;

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

    freeLedger(&ledgerInterface);

    if (shm_transactionspool_fd != -1)
    {
        if (close(shm_transactionspool_fd) == -1)
        {
            log_info("Erro ao fechar %s", SHM_TRANSACTIONS_POOL);
        }
        else
        {
            log_info("%s fechada com sucesso", SHM_TRANSACTIONS_POOL);
        }
    }
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

    if (validation_pipe_fd != -1)
    {

        if (close(validation_pipe_fd) == -1)
        {
            log_info("Erro ao fechar %s", VALIDATION_PIPE);
        }
        else
        {
            log_info("%s fechado com sucesso", VALIDATION_PIPE);
        }
    }

    if (sem_transactionspool != NULL)
    {
        if (sem_close(sem_transactionspool) == -1)
        {
            log_info("Erro ao fechar semáforo %s", SEM_TRANSACTIONS_POOL);
        }
        else
        {
            log_info("%s fechado com sucesso", SEM_TRANSACTIONS_POOL);
        }
    }

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
            printf("\nErro ao fechar semáforo %s", SEM_LOG_FILE);
        }
        else
        {
            printf("\n%s fechado com sucesso", SEM_LOG_FILE);
        }
    }
}

void *signal_handler_thread(void *arg)
{
    SignalHandlerArgs *args = (SignalHandlerArgs *)arg;
    sigset_t *set = args->set;
    pthread_t *threads = args->threads;

    int sig;

    // Wait for SIGTERM
    if (sigwait(set, &sig) == 0)
    {
        if (sig == SIGTERM)
        {
            log_info("SIGTERM recebido. A terminar as miner threads...");

            // terminar todas as miner threads
            for (int i = 0; i < NUM_MINERS; i++)
            {
                log_info("A terminar a thread %d...", i);
                pthread_cancel(threads[i]); // Send cancellation request
            }

            log_info("A aguardar pelo fim de tarefas pendentes...");

            // stop_threads = 1; // Set the stop flag to 1

            for (int i = 0; i < NUM_MINERS; i++)
            {

                pthread_join(threads[i], NULL);
                log_info("Thread %d terminada.", i);
            }

            sem_post(sem_pipeclosed); // desbloquear o semáforo para o pipe closed
            log_info("Sinalizado ao validator que pode terminar (pipe)");
        }
    }
    else
    {
        log_info("Erro no tratamento de sinais na thread de sinais: %s", strerror(errno));
    }

    log_info("Signal handler thread terminada.");

    return NULL;
}

void *miner_thread(void *arg)
{
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    MinerThreadArgs *args = (MinerThreadArgs *)arg;
    int thread_id = args->thread_id;

    log_info("Thread %d em execução...", thread_id);

    // print_ledger(&ledgerInterface); // Print the ledger

    int block_number = 0;

    while (!stop_threads)
    {
        sem_wait(sem_minerwork);
        pthread_testcancel(); // ponto de cancelamento

        // criar variaveis temporarias
        Transaction *selected_transactions = NULL;
        char previous_block_hash[HASH_SIZE];
        bool enough_transactions = false;
        bool ledger_full = false;

        // readonly para verificar se existe algo para processar
        sem_wait(sem_transactionspool);
        unsigned int tx_count = *tx_pool.count;
        sem_post(sem_transactionspool);

        sem_wait(sem_ledger);
        unsigned int ledger_count = *(ledgerInterface.count);
        sem_post(sem_ledger);

        if (tx_count < TRANSACTIONS_PER_BLOCK || ledger_count >= (unsigned int)BLOCKCHAIN_BLOCKS)
        {
            continue;
        }

        // preparar tudo para a secção critica
        selected_transactions = (Transaction *)calloc(TRANSACTIONS_PER_BLOCK, sizeof(Transaction));
        if (selected_transactions == NULL)
        {
            log_info("Thread %d: Falha ao alocar memória para as transações selecionadas", thread_id);
            continue;
        }

        sem_wait(sem_transactionspool);

        // Now recheck conditions inside the lock
        if (*tx_pool.count >= TRANSACTIONS_PER_BLOCK)
        {
            bool *selected_bitmap = (bool *)calloc(*tx_pool.size, sizeof(bool));
            if (selected_bitmap == NULL)
            {
                log_info("Thread %d: Falha ao alocar memória para o bitmap", thread_id);
                sem_post(sem_transactionspool);
                free(selected_transactions);
                continue;
            }

            int selected_count = 0;
            while (selected_count < (int)TRANSACTIONS_PER_BLOCK && *tx_pool.count >= TRANSACTIONS_PER_BLOCK)
            {
                unsigned int random_index = rand() % *tx_pool.size;

                if (selected_bitmap[random_index])
                {
                    continue;
                }

                PendingTransaction *current_transaction = &tx_pool.transactions[random_index];

                if (strcmp(current_transaction->tx.tx_id, "0") != 0 && current_transaction->filled == 1)
                {
                    // Copy transaction data instead of just referencing
                    selected_transactions[selected_count].reward = current_transaction->tx.reward;
                    selected_transactions[selected_count].value = current_transaction->tx.value;
                    selected_transactions[selected_count].timestamp = current_transaction->tx.timestamp;
                    strncpy(selected_transactions[selected_count].tx_id, current_transaction->tx.tx_id, TX_ID_LEN);
                    selected_count++;
                }
            }

            enough_transactions = (selected_count == (int)TRANSACTIONS_PER_BLOCK);
            free(selected_bitmap);
        }

        sem_post(sem_transactionspool);

        if (!enough_transactions)
        {
            free(selected_transactions);
            continue;
        }

        sem_wait(sem_ledger);

        // Recheck ledger conditions inside the lock
        if (*(ledgerInterface.count) < (unsigned int)BLOCKCHAIN_BLOCKS)
        {
            strncpy(previous_block_hash, ledgerInterface.last_block_hash, HASH_SIZE);
            ledger_full = false;
        }
        else
        {
            ledger_full = true;
        }

        sem_post(sem_ledger);

        if (ledger_full)
        {
            log_info("Thread %d: Ledger cheia. Criação de blocos interrompida", thread_id);
            free(selected_transactions);
            break;
        }

        // efetuar pow fora de locks de semaforos : minimizar secções críticas
        TransactionBlock block;
        memset(&block, 0, sizeof(TransactionBlock));
        snprintf(block.txb_id, TXB_ID_LEN, "BLOCK-%d-%d", thread_id, block_number++);
        strncpy(block.previous_block_hash, previous_block_hash, HASH_SIZE);
        block.transactions = selected_transactions;

        // log_info("Thread %d: A calcular PoW para o bloco %s", thread_id, block.txb_id);
        PoWResult r;
        do
        {
            block.timestamp = time(NULL);
            pthread_testcancel();
            r = proof_of_work(&block);
        } while (r.error == 1 && !stop_threads);

        log_info("Thread Miner %d: Novo bloco minerado %s", thread_id, block.txb_id);

        // prepararar dados para o pipe
        int pipe_size = fcntl(validation_pipe_fd, F_GETPIPE_SZ);

        size_t before_transactions = offsetof(TransactionBlock, transactions);
        size_t after_transactions = sizeof(TransactionBlock) - offsetof(TransactionBlock, transactions) - sizeof(Transaction *);
        size_t header_size = before_transactions + after_transactions;
        size_t txs_size = sizeof(Transaction) * TRANSACTIONS_PER_BLOCK;
        size_t total_payload_size = header_size + txs_size;
        size_t total_size = sizeof(size_t) + total_payload_size;

        if ((int)total_size > pipe_size)
        {
            log_info("Thread %d: Tamanho do bloco excede PIPE_BUFFER", thread_id);
        }

        char *buffer = malloc(total_size);
        if (!buffer)
        {
            perror("malloc");
            free(selected_transactions);
            continue;
        }

        // esta estrutura estranha deve-se ao facto a compatibilizar com o codigo do PoW onde a estrutura do bloco de transacoes possui um ponteiro para as transacoes no meio da estrutura, algo que não facilita a serialização para comunicacao por pipe
        // prefixo do tamanho
        memcpy(buffer, &total_payload_size, sizeof(size_t));

        // parte antes das transações
        memcpy(buffer + sizeof(size_t), &block, before_transactions);

        // parte depois das transações
        char *after_transactions_src = (char *)&block + offsetof(TransactionBlock, transactions) + sizeof(Transaction *);
        memcpy(buffer + sizeof(size_t) + before_transactions, after_transactions_src, after_transactions);

        // transacoes
        memcpy(buffer + sizeof(size_t) + header_size, block.transactions, txs_size);

        // escrever para o pipe
        ssize_t written = write(validation_pipe_fd, buffer, total_size);
        if ((size_t)written != total_size)
        {
            log_info("Thread %d: escrita no pipe de validação não efetuada", thread_id);
        }

        free(buffer);
        free(selected_transactions);
    }

    log_info("Miner thread %d terminou.", thread_id);
    return NULL;
}

void miner()
{
    // Abrir o semaforo para logs (já existente)
    sem_log_file = sem_open(SEM_LOG_FILE, 0);
    if (sem_log_file == SEM_FAILED)
    {
        perror("\nMINER : Erro ao abrir semáforo para LOG_FILE");
        exit(EXIT_FAILURE);
    }

    log_file = open_log_file();
    if (log_file == NULL)
    {
        perror("\nMINER : Erro ao abrir o ficheiro de log");
        sem_close(sem_log_file);
        exit(EXIT_FAILURE);
    }
    snprintf(TIPO_PROCESSO, sizeof(TIPO_PROCESSO_BUFFER), "MINER [%d]", getpid());

    // Abrir o semaforo para transactions pool
    sem_transactionspool = sem_open(SEM_TRANSACTIONS_POOL, 0);
    if (sem_transactionspool == SEM_FAILED)
    {
        log_info("\Erro ao abrir semáforo %s", SEM_TRANSACTIONS_POOL);
        cleanup();
        exit(EXIT_FAILURE);
    }

    // abrir semaforo minerwork
    sem_minerwork = sem_open(SEM_MINERWORK, 0);
    if (sem_minerwork == SEM_FAILED)
    {
        log_info("Erro ao abrir semáforo %s", SEM_MINERWORK);
        cleanup();
        exit(EXIT_FAILURE);
    }

    //
    sem_ledger = sem_open(SEM_LEDGER, 0);
    if (sem_ledger == SEM_FAILED)
    {
        log_info("Erro ao abrir semáforo %s", SEM_LEDGER);
        cleanup();
        exit(EXIT_FAILURE);
    }

    // sem pipe closed
    sem_pipeclosed = sem_open(SEM_PIPECLOSED, O_CREAT, 0666, 0);
    if (sem_pipeclosed == SEM_FAILED)
    {
        log_info("Erro ao abrir semáforo %s", SEM_PIPECLOSED);
        cleanup();
        exit(EXIT_FAILURE);
    }

    // abrir o pipe no processo atual para acesso por todas as threads
    validation_pipe_fd = open(VALIDATION_PIPE, O_WRONLY);
    if (validation_pipe_fd < 0)
    {
        log_info("Erro ao abrir o pipe de validação");
        cleanup();
        exit(EXIT_FAILURE);
    }
    log_info("Pipe %s aberto com sucesso.", VALIDATION_PIPE);

    // abrir a memoria partilhada para a transactions pool (já existente)
    shm_transactionspool_fd = shm_open(SHM_TRANSACTIONS_POOL, O_RDWR, 0666);
    if (shm_transactionspool_fd == -1)
    {
        log_info("Erro ao abrir memória partilhada para transactions pool");
        cleanup();
        exit(EXIT_FAILURE);
    }
    log_info("%s aberta com sucesso", SHM_TRANSACTIONS_POOL);

    // mapear a memoria partilhada para o espaço de memória do processo
    shm_transactionspool_base = mmap(NULL, shm_transactionspool_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_transactionspool_fd, 0);
    if (shm_transactionspool_base == MAP_FAILED)
    {
        log_info("Erro ao mapear memória partilhada para transactions pool");
        close(shm_transactionspool_fd);
        cleanup();
        exit(EXIT_FAILURE);
    }
    log_info("%s mapeada com sucesso", SHM_TRANSACTIONS_POOL);

    // abrir a memoria partilhada para a ledger (já existente)
    shm_ledger_fd = shm_open(SHM_LEDGER, O_RDWR, 0666);
    if (shm_ledger_fd == -1)
    {
        log_info("MINER : Erro ao abrir memória partilhada para ledger");
        cleanup();
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

    //////////////////////////////////////

    // Create the ledger Interface from shared memory
    ledgerInterface = interfaceLedger(shm_ledger_base);

    // Print the ledger
    // print_ledger(&ledgerInterface);

    // Free the dynamically allocated memory for blocks

    /////////////////////////////////

    tx_pool = interfaceTxPool(shm_transactionspool_base);

    // bloquear o SIGTERM em todas as threads
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    pthread_t threads[NUM_MINERS];

    SignalHandlerArgs signal_args = {
        .set = &set,
        .threads = threads};

    // criar a thread para tratamento de sinais
    pthread_t signal_thread;
    if (pthread_create(&signal_thread, NULL, signal_handler_thread, &signal_args) != 0)
    {
        log_info("Erro ao criar thread de tratamento de sinais");
        return;
    }
    log_info("Thread de tratamento de sinais criada com sucesso.");

    MinerThreadArgs thread_args[NUM_MINERS];

    for (int i = 0; i < NUM_MINERS; i++)
    {
        thread_args[i].thread_id = i + 1;

        if (pthread_create(&threads[i], NULL, miner_thread, &thread_args[i]) != 0)
        {
            log_info("Erro ao criar miner thread");
            return;
        }
    }

    // esperar pela thread de tratamento de sinais
    pthread_join(signal_thread, NULL);
    log_info("Thread de tratamento de sinais terminada.");

    cleanup();
}