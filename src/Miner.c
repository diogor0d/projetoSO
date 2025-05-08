/*
    Projeto de Sistemas Operativos 2024/2025 - DEIChain
    Diogo Nuno Fonseca Rodrigues 2022257625
    Guilherme Teixeira Gonçalves Rosmaninho 2022257636
*/

#define _GNU_SOURCE // para o vasprintf
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
static char *TIPO_PROCESSO = NULL;

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

// minerwork condvar
int shm_minerworkcondvar_fd = -1;
static void *shm_minerworkcondvar_base = NULL;
MinerWorKCondVar *minerwork_condvar = NULL;

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
            log_info("Erro ao fechar semáforo %s", SEM_LOG_FILE);
        }
        else
        {
            log_info("%s fechado com sucesso", SEM_LOG_FILE);
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

    log_info("Signal handler thread terminado.");

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

        // sem_wait(sem_minerwork); // Wait for the signal to start working
        //  pthread_testcancel(); // Check for cancellation reques

        // log_info("Transacoes na pool: %d", *tx_pool.count);

        //   Create a temporary array to sort transactions

        pthread_testcancel(); // Explicitly check for cancellation

        sem_wait(sem_minerwork); // Wait for the signal to start working

        pthread_testcancel(); // Explicitly check for cancellation

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);

        ts.tv_sec += 2;

        if (sem_timedwait(sem_transactionspool, &ts) == -1)
        {
            if (errno == ETIMEDOUT)
            {
                log_info("Timeout waiting for transactionspool — skipping block creation.");
                continue; // Don't access shared resource
            }
            else
            {
                perror("sem_timedwait");
                pthread_exit(NULL);
            }
        }
        sem_wait(sem_ledger); // desbloquear o semáforo para o ledger

        pthread_testcancel(); // Explicitly check for cancellation

        // apesar da redundancia desta condição, ela é efetuada para garantir a clareza da operação de cada thread
        if (*tx_pool.count >= (unsigned int)TRANSACTIONS_PER_BLOCK && *(ledgerInterface.count) > 0 && *(ledgerInterface.count) < (unsigned int)BLOCKCHAIN_BLOCKS)
        {

            // incrementar o número do bloco

            // print_transaction_pool(&tx_pool);

            block_number++;

            // Select TRANSACTIONS_PER_BLOCK transactions with the least reward
            Transaction *selected_transactions = (Transaction *)calloc(TRANSACTIONS_PER_BLOCK, sizeof(Transaction));
            if (selected_transactions == NULL)
            {
                log_info("Thread %d: Falha ao alocar memória para as transações selecionadas", thread_id);
                sem_post(sem_transactionspool); // desbloquear o semáforo para a transactions pool
                sem_post(sem_ledger);           // desbloquear o semáforo para o ledger
                pthread_exit(NULL);
            }

            int selected_count = 0;

            bool *selected_bitmap = (bool *)calloc(*tx_pool.size, sizeof(bool));
            if (selected_bitmap == NULL)
            {
                log_info("Thread %d: Falha ao alocar memória para o bitmap de transações selecionadas", thread_id);
                sem_post(sem_transactionspool); // desbloquear o semáforo para a transactions pool
                sem_post(sem_ledger);           // desbloquear o semáforo para o ledger
                pthread_exit(NULL);
            }
            // Skip to the next iteration if not enough transactions

            // Use a while loop to select transactions
            log_info("Thread %d: A selecionar transações aleatórias para o bloco %d", thread_id, block_number);
            while (selected_count < (int)TRANSACTIONS_PER_BLOCK && *tx_pool.count >= TRANSACTIONS_PER_BLOCK)
            {
                pthread_testcancel();                               // Explicitly check for cancellation
                unsigned int random_index = rand() % *tx_pool.size; // Generate a random index

                // Check if the transaction at this index has already been selected
                if (selected_bitmap[random_index])
                {
                    continue; // Skip this transaction if it has already been selected
                }

                PendingTransaction *current_transaction = &tx_pool.transactions[random_index];

                // Check if the transaction is valid (filled and has a valid ID)
                if (strcmp(current_transaction->tx.tx_id, "0") != 0 && current_transaction->filled == 1)
                {
                    // Mark the transaction as selected in the bitmap
                    selected_bitmap[random_index] = true;

                    // Add the transaction to the selected transactions array
                    selected_transactions[selected_count].reward = current_transaction->tx.reward;
                    selected_transactions[selected_count].value = current_transaction->tx.value;
                    selected_transactions[selected_count].timestamp = current_transaction->tx.timestamp;
                    strncpy(selected_transactions[selected_count].tx_id, current_transaction->tx.tx_id, TX_ID_LEN);
                    selected_count++;
                }
            }

            // Free the bitmap after use
            free(selected_bitmap);

            // Create a block
            TransactionBlock block;
            memset(&block, 0, sizeof(TransactionBlock)); // Initialize the block to zero
            snprintf(block.txb_id, TXB_ID_LEN, "BLOCK-%d-%d", thread_id, block_number);

            // sem_wait(sem_ledger); // bloquear o semáforo para o ledger

            strncpy(block.previous_block_hash, ledgerInterface.last_block_hash, HASH_SIZE); // alterar para o hash do bloco anterior
            block.transactions = selected_transactions;

            // pow
            srand(time(NULL)); // Seed RNG

            log_info("Thread %d: A calcular PoW para o bloco %s", thread_id, block.txb_id);
            PoWResult r;
            do
            {
                // Timestamp: current time
                block.timestamp = time(NULL);
                pthread_testcancel(); // Explicitly check for cancellation
                r = proof_of_work(&block);

            } while (r.error == 1 && !stop_threads);

            // debug
            char hash[HASH_SIZE];
            compute_sha256(&block, hash); // Compute the hash of the block

            // log_info("Hash do bloco enviado %s: %s\n", block.txb_id, hash);
            // print_transaction_block(&block); // Print the block

            // Serialize the block to send via pipe
            int pipe_size = fcntl(validation_pipe_fd, F_GETPIPE_SZ);

            size_t before_transactions = offsetof(TransactionBlock, transactions);
            size_t after_transactions = sizeof(TransactionBlock) - offsetof(TransactionBlock, transactions) - sizeof(Transaction *);
            size_t header_size = before_transactions + after_transactions;
            size_t txs_size = sizeof(Transaction) * TRANSACTIONS_PER_BLOCK;
            size_t total_payload_size = header_size + txs_size;
            size_t total_size = sizeof(size_t) + total_payload_size;

            if ((int)total_size > pipe_size)
            {
                log_info("Thread %d: Tamanho do bloco a enviar excede o tamanho do PIPE_BUFFER do sistema: writes/reads atomicos nao garantidos.", thread_id);
            }

            char *buffer = malloc(total_size);
            if (!buffer)
            {
                perror("malloc");
            }

            // Store the size prefix
            memcpy(buffer, &total_payload_size, sizeof(size_t));

            // Copy the part of the block before transactions
            memcpy(buffer + sizeof(size_t), &block, before_transactions);

            // Copy the part after transactions (nonce and any padding)
            char *after_transactions_src = (char *)&block + offsetof(TransactionBlock, transactions) + sizeof(Transaction *);
            memcpy(buffer + sizeof(size_t) + before_transactions, after_transactions_src, after_transactions);

            // Copy transactions array
            memcpy(buffer + sizeof(size_t) + header_size, block.transactions, txs_size);

            // ignorar o sipipe previne que o processo seja terminado quando nenhum validator estiver a ler do pipe, situação que podera ocorrer quando a ledger estiver cheia
            // signal(SIGPIPE, SIG_IGN);

            // Write to pipe
            // printf("Thread %d: Enviando bloco %s para o pipe de validação\n", thread_id, block.txb_id);
            ssize_t written = write(validation_pipe_fd, buffer, total_size);
            if ((size_t)written != total_size)
            {
                log_info("Thread %d: escrita no pipe de validação não efetuada", thread_id);
            }

            // sem_post(sem_ledger); // desbloquear o semáforo para o ledger

            free(buffer);
            free(selected_transactions);

            // criar o bloco
            // TransactionBlock block;
            // snprintf(block.txb_id, TXB_ID_LEN, "BLOCK-%d-%d", thread_id, rand() % 1000);

            // computar PoW
        }
        else if (*ledgerInterface.count >= (unsigned int)BLOCKCHAIN_BLOCKS)
        {
            log_info("Thread %d: Ledger cheia. Criação de blocos interrompida", thread_id);
            // sem_post(sem_transactionspool); // desbloquear o semáforo para a transactions pool
            sem_post(sem_transactionspool); // desbloquear o semáforo para a transactions pool
            sem_post(sem_ledger);           // desbloquear o semáforo para o ledger
            break;                          // Exit the loop when the ledger is full
        }
        else
        {
            // sem_post(sem_transactionspool); // desbloquear o semáforo para a transactions pool
        }

        pthread_testcancel(); // Explicitly check for cancellation

        sem_post(sem_transactionspool); // desbloquear o semáforo para a transactions pool
        sem_post(sem_ledger);           // desbloquear o semáforo para o ledger
    }

    log_info("Miner thread %d terminou.", thread_id);
    pthread_exit(NULL);
}

void miner()
{
    // Abrir o semaforo para logs (já existente)
    sem_log_file = sem_open(SEM_LOG_FILE, 0);
    if (sem_log_file == SEM_FAILED)
    {
        perror("\nMINER : Erro ao abrir semáforo para LOG_FILE");
        return;
    }

    log_file = open_log_file();
    if (log_file == NULL)
    {
        perror("\nMINER : Erro ao abrir o ficheiro de log");
        return;
    }
    TIPO_PROCESSO = "MINER";

    // Abrir o semaforo para transactions pool
    sem_transactionspool = sem_open(SEM_TRANSACTIONS_POOL, 0);
    if (sem_transactionspool == SEM_FAILED)
    {
        log_info("\Erro ao abrir semáforo %s", SEM_TRANSACTIONS_POOL);
        return;
    }

    // abrir semaforo minerwork
    sem_minerwork = sem_open(SEM_MINERWORK, 0);
    if (sem_minerwork == SEM_FAILED)
    {
        log_info("Erro ao abrir semáforo %s", SEM_MINERWORK);
        return;
    }

    //
    sem_ledger = sem_open(SEM_LEDGER, 0);
    if (sem_ledger == SEM_FAILED)
    {
        log_info("Erro ao abrir semáforo %s", SEM_LEDGER);
        return;
    }

    // sem pipe closed
    sem_pipeclosed = sem_open(SEM_PIPECLOSED, O_CREAT, 0666, 0);
    if (sem_pipeclosed == SEM_FAILED)
    {
        log_info("Erro ao abrir semáforo %s", SEM_PIPECLOSED);
        return;
    }

    // abrir o pipe no processo atual para acesso por todas as threads
    validation_pipe_fd = open(VALIDATION_PIPE, O_WRONLY);
    if (validation_pipe_fd < 0)
    {
        log_info("Erro ao abrir o pipe de validação");
        exit(EXIT_FAILURE);
    }
    log_info("Pipe %s aberto com sucesso.", VALIDATION_PIPE);

    // abrir a memoria partilhada para a transactions pool (já existente)
    shm_transactionspool_fd = shm_open(SHM_TRANSACTIONS_POOL, O_RDWR, 0666);
    if (shm_transactionspool_fd == -1)
    {
        log_info("Erro ao abrir memória partilhada para transactions pool");
        exit(EXIT_FAILURE);
    }
    log_info("%s aberta com sucesso", SHM_TRANSACTIONS_POOL);

    // mapear a memoria partilhada para o espaço de memória do processo
    shm_transactionspool_base = mmap(NULL, shm_transactionspool_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_transactionspool_fd, 0);
    if (shm_transactionspool_base == MAP_FAILED)
    {
        log_info("Erro ao mapear memória partilhada para transactions pool");
        close(shm_transactionspool_fd);
        exit(EXIT_FAILURE);
    }
    log_info("%s mapeada com sucesso", SHM_TRANSACTIONS_POOL);

    // abrir a memoria partilhada para a ledger (já existente)
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

    shm_minerworkcondvar_fd = shm_open(SHM_MINERWORK_CONDVAR, O_RDWR, 0666);
    if (shm_minerworkcondvar_fd == -1)
    {
        perror("Erro ao abrir SHM_MINERWORK_CONDVAR");
        exit(EXIT_FAILURE);
    }

    // Map it
    shm_minerworkcondvar_base = mmap(
        NULL,
        sizeof(MinerWorKCondVar),
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        shm_minerworkcondvar_fd,
        0);
    if (shm_minerworkcondvar_base == MAP_FAILED)
    {
        perror("Erro ao mapear SHM_MINERWORK_CONDVAR");
        exit(EXIT_FAILURE);
    }
    minerwork_condvar = (MinerWorKCondVar *)shm_minerworkcondvar_base;

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

    log_info("Todas as miner threads terminaram.");

    // esperar pela thread de tratamento de sinais
    pthread_join(signal_thread, NULL);
    log_info("Thread de tratamento de sinais terminada.");

    cleanup();
}