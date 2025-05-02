/*
    Projeto de Sistemas Operativos 2024/2025 - DEIChain
    Diogo Nuno Fonseca Rodrigues 2022257625
    Guilherme Teixeira Gonçalves Rosmaninho 2022257636
*/

#define _GNU_SOURCE // para o vasprintf
#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <openssl/sha.h>

#include "../include/Controller.h"
#include "../include/PoW/pow.h"
#include "../include/SHMManagement.h"

static sem_t *sem_log_file = NULL;
static FILE *log_file = NULL;
static char *TIPO_PROCESSO = NULL;

// aceder a variavel globais do controller
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

void cleanup()
{
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

    free(ledgerInterface.blocks);

    // Close the log file
    if (log_file)
    {
        fclose(log_file);
    }

    // fechar o semaforo para logs
    sem_close(sem_log_file);
}

static void sigterm(int signum)
{
    (void)signum; // Ignore the signal parameter

    log_info("SIGTERM recebido. A terminar o validator...");

    cleanup(); // Call the cleanup function to close the log file and semaphores

    // Exit the process
    exit(EXIT_SUCCESS);
}

void deserialize_transaction_block(const char *input, TransactionBlock *block, int transactions_per_block)
{
    if (input == NULL || block == NULL)
    {
        log_info("Input string ou bloco inválido para desserialização.");
        return;
    }

    // Clear the block structure
    memset(block, 0, sizeof(TransactionBlock));

    // Parse the block metadata
    const char *ptr = input;
    sscanf(ptr, "BID: %s PH: %s TS: %ld NON: %u,",
           block->txb_id,
           block->previous_block_hash,
           &block->timestamp,
           &block->nonce);

    // Allocate memory for transactions
    block->transactions = (Transaction *)calloc(transactions_per_block, sizeof(Transaction));
    if (block->transactions == NULL)
    {
        perror("Failed to allocate memory for transactions during deserialization");
        exit(EXIT_FAILURE);
    }

    // Move the pointer past the block metadata
    ptr = strchr(ptr, ',') + 1;

    // Parse each transaction
    for (int i = 0; i < transactions_per_block; i++)
    {
        char tx_id[TX_ID_LEN];
        unsigned int reward;
        double value;
        time_t timestamp;

        // Parse the transaction data
        sscanf(ptr, "TI %d ID=%63s R=%u V=%lf TS=%ld,", // Use %63s to limit input to TX_ID_LEN - 1
               &i, tx_id, &reward, &value, &timestamp);

        // Populate the transaction
        strncpy(block->transactions[i].tx_id, tx_id, TX_ID_LEN - 1);
        block->transactions[i].tx_id[TX_ID_LEN - 1] = '\0'; // Ensure null termination
        block->transactions[i].reward = reward;
        block->transactions[i].value = value;
        block->transactions[i].timestamp = timestamp;

        // Move the pointer to the next transaction
        ptr = strchr(ptr, ',') + 1;
    }
}

void blkcpy(TransactionBlockInterface *dest, const TransactionBlock *src, int transactions_per_block)
{
    if (dest == NULL || src == NULL)
    {
        log_info("Invalid source or destination for blkcpy.");
        return;
    }

    // Copy the nonce
    *(dest->nonce) = src->nonce;

    // Copy the timestamp
    *(dest->timestamp) = src->timestamp;

    // Copy the txb_id
    strncpy(dest->txb_id, src->txb_id, sizeof(dest->txb_id) - 1);
    dest->txb_id[sizeof(dest->txb_id) - 1] = '\0'; // Ensure null termination

    // Copy the previous block hash
    strncpy(dest->previous_block_hash, src->previous_block_hash, sizeof(dest->previous_block_hash) - 1);
    dest->previous_block_hash[sizeof(dest->previous_block_hash) - 1] = '\0'; // Ensure null termination

    // Copy the transactions
    for (int i = 0; i < transactions_per_block; i++)
    {
        // Copy the transaction ID (string)
        strncpy(dest->transactions[i].tx_id, src->transactions[i].tx_id, TX_ID_LEN - 1);
        dest->transactions[i].tx_id[TX_ID_LEN - 1] = '\0'; // Ensure null termination

        // Copy the other fields
        dest->transactions[i].reward = src->transactions[i].reward;
        dest->transactions[i].value = src->transactions[i].value;
        dest->transactions[i].timestamp = src->transactions[i].timestamp;
    }
}

void validator()
{

    // Abrir o semaforo para logs (já existente)
    sem_log_file = sem_open(SEM_LOG_FILE, 0);
    if (sem_log_file == SEM_FAILED)
    {
        perror("\nVALIDATOR : Erro ao abrir semáforo para LOG_FILE");
        return;
    }

    log_file = open_log_file();
    if (log_file == NULL)
    {
        perror("\nVALIDATOR : Erro ao abrir o ficheiro de log");
        return;
    }
    TIPO_PROCESSO = "VALIDATOR";

    // abrir a memoria partilhada para a transactions pool (já existente)
    shm_transactionspool_fd = shm_open(SHM_TRANSACTIONS_POOL, O_RDWR, 0666);
    if (shm_transactionspool_fd == -1)
    {
        log_info("Erro ao abrir memória partilhada para transactions pool");
        exit(EXIT_FAILURE);
    }

    // mapear a memoria partilhada para o espaço de memória do processo
    shm_transactionspool_base = mmap(NULL, shm_transactionspool_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_transactionspool_fd, 0);
    if (shm_transactionspool_base == MAP_FAILED)
    {
        log_info("Erro ao mapear memória partilhada para transactions pool");
        close(shm_transactionspool_fd);
        exit(EXIT_FAILURE);
    }

    // abrir a memoria partilhada para a ledger (já existente)
    shm_ledger_fd = shm_open(SHM_LEDGER, O_RDWR, 0666);
    if (shm_ledger_fd == -1)
    {
        log_info("Erro ao abrir memória partilhada para ledger");
        exit(EXIT_FAILURE);
    }

    // mapear a memoria partilhada para o espaço de memória do processo
    shm_ledger_base = mmap(NULL, shm_ledger_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_ledger_fd, 0);
    if (shm_ledger_base == MAP_FAILED)
    {
        log_info("Erro ao mapear memória partilhada para transactions pool");
        close(shm_ledger_fd);
        exit(EXIT_FAILURE);
    }

    validation_pipe_fd = open(VALIDATION_PIPE, O_RDONLY);
    if (validation_pipe_fd < 0)
    {
        log_info("Erro ao abrir o pipe de validação");
        exit(EXIT_FAILURE);
    }
    log_info("Pipe de validação aberto com sucesso.");

    signal(SIGTERM, sigterm); // tratar o sinal SIGTERM para terminar o processo corretamente

    ledgerInterface = interfaceLedger(shm_ledger_base); // criar a interface para o ledger

    print_ledger(&ledgerInterface); // Print the ledger

    if (*(ledgerInterface.count) == 0)
    {
        TransactionBlock nemesis_block;
        memset(&nemesis_block, 0, sizeof(TransactionBlock));
        nemesis_block.transactions =
            (Transaction *)calloc(TRANSACTIONS_PER_BLOCK, sizeof(Transaction));
        PoWResult r;
        r = proof_of_work(&nemesis_block);
        if (r.error)
        {
            perror("Could not compute the Hash\n");
            exit(1);
        }

        log_info("Hash do bloco origem: %s\n", r.hash);

        print_transaction_block(&nemesis_block); // Print the block

        *(ledgerInterface.count) = 1;
        *(ledgerInterface.blocks[0].txb_id) = 'z';

        strcpy(ledgerInterface.blocks[0].txb_id, nemesis_block.txb_id);
        *(ledgerInterface.blocks[0].timestamp) = nemesis_block.timestamp;
        *(ledgerInterface.blocks[0].nonce) = nemesis_block.nonce;

        // memcpy(ledgerInterface.blocks[0].transactions, nemesis_block.transactions, TRANSACTIONS_PER_BLOCK * sizeof(Transaction));

        strcpy(ledgerInterface.last_block_hash, r.hash);

        free(nemesis_block.transactions); // Free the allocated memory for transactions
    }

    print_ledger(&ledgerInterface); // Print the ledger

    log_info("Hash inicial (Bloco origem): %s\n", ledgerInterface.last_block_hash);

    char buffer[1024]; // Adjust the buffer size as needed
    ssize_t bytes_read;

    while ((bytes_read = read(validation_pipe_fd, buffer, sizeof(buffer) - 1)) > 0)
    {
        buffer[bytes_read] = '\0'; // Null-terminate the string

        TransactionBlock block;
        deserialize_transaction_block(buffer, &block, TRANSACTIONS_PER_BLOCK); // Deserialize the block

        // Print the deserialized block
        printf("\n\nBloco recebido: %s\n", block.txb_id);
        char HASH_BUFFER[HASH_SIZE];
        compute_sha256(&block, HASH_BUFFER);
        log_info("Hash do bloco recebido: %s\n", HASH_BUFFER);
        print_transaction_block(&block); // Print the block

        // Check if the block is complient with difficult
        if (!verify_nonce(&block))
        {
            log_info("Bloco %s recebido tem NONCE invalida. Descartado.", block.txb_id);
            continue;
        }

        log_info("Novo bloco aprovado: %s", block.txb_id);
        // compute the hash to pass to the next block as previous hash
        compute_sha256(&block, ledgerInterface.last_block_hash);
        // put on ledger
        blkcpy(&ledgerInterface.blocks[*(ledgerInterface.last_block_index) + 1], &block, TRANSACTIONS_PER_BLOCK);
        // print information
        print_ledger(&ledgerInterface); // Print the ledger
    }

    if (bytes_read == -1)
    {
        log_info("Error reading from pipe");
    }
    else if (bytes_read == 0)
    {
        log_info("Pipe closed by writer.\n");
    }

    cleanup();
}
