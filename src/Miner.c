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

#include "../include/Controller.h"
#include "../include/PoW/pow.h"
#include "../include/SHMManagement.h"

static sem_t *sem_log_file = NULL;
static FILE *log_file = NULL;
static char *TIPO_PROCESSO = NULL;

// aceder a variavel globais do controller
int NUM_MINERS;
int LEDGER_SIZE;
size_t TRANSACTIONS_PER_BLOCK;
int shm_transactionspool_size;
int shm_ledger_size;

LedgerInterface ledgerInterface;
TransactionPoolInterface tx_pool;

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

static void cleanup()
{
    freeLedger(&ledgerInterface);

    if (close(shm_transactionspool_fd) == -1)
    {
        log_info("Erro ao fechar SHM_TRANSACTIONS_POOL");
    }
    if (close(shm_ledger_fd) == -1)
    {
        log_info("Erro ao fechar SHM_LEDGER");
    }

    if (close(validation_pipe_fd) == -1)
    {
        log_info("Erro ao fechar o pipe de validação");
    }

    // Close the log file
    if (log_file)
    {
        fclose(log_file);
    }

    // fechar o semaforo para logs
    sem_close(sem_log_file);
}

void *signal_handler_thread(void *arg)
{
    sigset_t *set = (sigset_t *)arg;
    int sig;

    // Wait for SIGTERM
    if (sigwait(set, &sig) == 0)
    {
        if (sig == SIGTERM)
        {
            log_info("SIGTERM recebido. A terminar as miner threads...");
            stop_threads = 1; // Signal threads to stop
        }
    }

    return NULL;
}

void *miner_thread(void *arg)
{
    MinerThreadArgs *args = (MinerThreadArgs *)arg;
    int thread_id = args->thread_id;

    log_info("Miner thread %d (PID: %d) em execução...", thread_id, getpid());

    // print_ledger(&ledgerInterface); // Print the ledger

    int block_number = 0;

    while (!stop_threads)
    {

        log_info("Transacoes na pool: %d", *tx_pool.count);
        // printf("\nMiner thread %d: Criando bloco %d...", thread_id, block_number);
        //  Create a temporary array to sort transactions

        // Copy transactions from the pool to the temporary array

        // FALTA SINCRONIZACAO

        if (*tx_pool.count >= (unsigned int)TRANSACTIONS_PER_BLOCK && *(ledgerInterface.count) > 0 && *(ledgerInterface.count) < (unsigned int)BLOCKCHAIN_BLOCKS)
        {

            // incrementar o número do bloco

            // print_transaction_pool(&tx_pool);

            block_number++;

            // Select TRANSACTIONS_PER_BLOCK transactions with the least reward
            Transaction *selected_transactions = (Transaction *)calloc(TRANSACTIONS_PER_BLOCK, sizeof(Transaction));
            if (selected_transactions == NULL)
            {
                log_info("Thread %d: Failed to allocate memory for selected transactions", thread_id);
                pthread_exit(NULL);
            }

            // Use a bitmap to track selected transactions
            bool *selected_bitmap = (bool *)calloc(*tx_pool.count, sizeof(bool));
            if (selected_bitmap == NULL)
            {
                log_info("Thread %d: Failed to allocate memory for selected bitmap", thread_id);
                pthread_exit(NULL);
            }

            int selected_count = 0;
            unsigned int pool_index = 0;

            // Use a while loop to select transactions
            while (selected_count < (int)TRANSACTIONS_PER_BLOCK && pool_index < *tx_pool.count)
            {
                unsigned int random_index = rand() % *tx_pool.count; // Generate a random index

                // Check if the transaction has already been selected
                /*  if (selected_bitmap[random_index])
                 {
                     if
                     continue; // Skip already selected transactions
                 } */

                PendingTransaction *current_transaction = &tx_pool.transactions[random_index];

                // Check if the transaction has a valid ID
                if (current_transaction->tx.tx_id != 0)
                {
                    selected_transactions[selected_count++] = current_transaction->tx;
                    selected_bitmap[random_index] = true; // Mark the transaction as selected
                }

                pool_index++; // Move to the next transaction in the pool
            }

            // Free the bitmap after use
            free(selected_bitmap);

            // Create a block
            TransactionBlock block;
            memset(&block, 0, sizeof(TransactionBlock)); // Initialize the block to zero
            snprintf(block.txb_id, TXB_ID_LEN, "BLOCK-%d-%d", thread_id, block_number);
            strncpy(block.previous_block_hash, ledgerInterface.last_block_hash, HASH_SIZE); // alterar para o hash do bloco anterior
            block.transactions = selected_transactions;

            // pow
            srand(time(NULL)); // Seed RNG

            PoWResult r;
            do
            {
                // Timestamp: current time
                block.timestamp = time(NULL);
                r = proof_of_work(&block);

            } while (r.error == 1 && !stop_threads);

            // debug
            char hash[HASH_SIZE];
            compute_sha256(&block, hash); // Compute the hash of the block

            log_info("Hash do bloco enviado %s: %s\n", block.txb_id, hash);
            print_transaction_block(&block); // Print the block

            // Serialize the block to send via pipe
            int pipe_size = fcntl(validation_pipe_fd, F_GETPIPE_SZ);
            if (BLOCK_BUFFER_SIZE > pipe_size)
            {
                log_info("Thread %d: Tamanho do buffer de envio de blocos para validation excede o tamanho do PIPE_BUFFER do sistema: writes atomicos nao garantidos.", thread_id);
            }

            size_t before_transactions = offsetof(TransactionBlock, transactions);
            size_t after_transactions = sizeof(TransactionBlock) - offsetof(TransactionBlock, transactions) - sizeof(Transaction *);
            size_t header_size = before_transactions + after_transactions;
            size_t txs_size = sizeof(Transaction) * TRANSACTIONS_PER_BLOCK;
            size_t total_payload_size = header_size + txs_size;
            size_t total_size = sizeof(size_t) + total_payload_size;

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
            signal(SIGPIPE, SIG_IGN);

            // Write to pipe
            ssize_t written = write(validation_pipe_fd, buffer, total_size);
            if ((size_t)written != total_size)
            {
                log_info("Thread %d: Erro ao escrever no pipe de validação", thread_id);
                sleep(10);
            }

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
            break; // Exit the loop when the ledger is full
        }
        else
        {
            // debug condicao
            log_info("Debug: tx_pool.count = %d, TRANSACTIONS_PER_BLOCK = %zu, ledgerInterface.count = %d, BLOCKCHAIN_BLOCKS = %d",
                     *tx_pool.count, TRANSACTIONS_PER_BLOCK, *ledgerInterface.count, BLOCKCHAIN_BLOCKS);

            // print_ledger(&ledgerInterface); // Print the ledger")
            log_info("Thread %d: Não há transações suficientes para construir um novo bloco", thread_id);
            print_transaction_pool(&tx_pool); // Print the transaction pool
            sleep(1);                         // Sleep for a while before checking again
        }
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

    // abrir o pipe no processo atual para acesso por todas as threads
    validation_pipe_fd = open(VALIDATION_PIPE, O_WRONLY);
    if (validation_pipe_fd < 0)
    {
        perror("\nMINER : Erro ao abrir o pipe de validação");
        exit(EXIT_FAILURE);
    }

    // abrir a memoria partilhada para a transactions pool (já existente)
    shm_transactionspool_fd = shm_open(SHM_TRANSACTIONS_POOL, O_RDWR, 0666);
    if (shm_transactionspool_fd == -1)
    {
        perror("\nMINER : Erro ao abrir memória partilhada para transactions pool");
        exit(EXIT_FAILURE);
    }

    // mapear a memoria partilhada para o espaço de memória do processo
    shm_transactionspool_base = mmap(NULL, shm_transactionspool_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_transactionspool_fd, 0);
    if (shm_transactionspool_base == MAP_FAILED)
    {
        perror("\nMINER : Erro ao mapear memória partilhada para transactions pool");
        close(shm_transactionspool_fd);
        exit(EXIT_FAILURE);
    }

    // abrir a memoria partilhada para a ledger (já existente)
    shm_ledger_fd = shm_open(SHM_LEDGER, O_RDWR, 0666);
    if (shm_ledger_fd == -1)
    {
        perror("\nMINER : Erro ao abrir memória partilhada para ledger");
        exit(EXIT_FAILURE);
    }

    // mapear a memoria partilhada para o espaço de memória do processo
    log_info("Tamanho da ledger: %d", shm_ledger_size);
    shm_ledger_base = mmap(NULL, shm_ledger_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_ledger_fd, 0);
    if (shm_ledger_base == MAP_FAILED)
    {
        perror("\nMINER : Erro ao mapear memória partilhada para transactions pool");
        close(shm_ledger_fd);
        exit(EXIT_FAILURE);
    }

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

    // criar a thread para tratamento de sinais
    pthread_t signal_thread;
    if (pthread_create(&signal_thread, NULL, signal_handler_thread, &set) != 0)
    {
        perror("MINER : Erro ao criar thread de tratamento de sinais");
        return;
    }

    pthread_t threads[NUM_MINERS];
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

    for (int i = 0; i < NUM_MINERS; i++)
    {
        // espera pelo fim de cada thread
        pthread_join(threads[i], NULL);
    }

    // esperar pela thread de tratamento de sinais
    pthread_join(signal_thread, NULL);

    log_info("Todas as miner threads terminaram.");

    cleanup();
}