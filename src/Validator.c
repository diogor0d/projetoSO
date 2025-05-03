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

    while (*(ledgerInterface.count) < *(ledgerInterface.num_blocks))
    {
        TransactionBlock streamed_block;

        size_t payload_size;
        ssize_t read_bytes = read(validation_pipe_fd, &payload_size, sizeof(size_t));
        if (read_bytes == 0)
        {
            // EOF — other side closed pipe
            printf("Pipe closed. Exiting.\n");
            break;
        }
        else if (read_bytes != sizeof(size_t))
        {
            perror("read (size)");
            break;
        }

        char *buffer = malloc(payload_size);
        if (!buffer)
        {
            perror("malloc");
            break;
        }

        read_bytes = read(validation_pipe_fd, buffer, payload_size);
        if (read_bytes != payload_size)
        {
            perror("read (block)");
            free(buffer);
            break;
        }

        size_t header_size = sizeof(TransactionBlock) - sizeof(Transaction *);
        memcpy(&streamed_block, buffer, header_size);

        streamed_block.transactions = malloc(sizeof(Transaction) * TRANSACTIONS_PER_BLOCK);
        if (!streamed_block.transactions)
        {
            perror("malloc transactions");
            free(buffer);
            break;
        }

        memcpy(streamed_block.transactions, buffer + header_size, sizeof(Transaction) * TRANSACTIONS_PER_BLOCK);

        log_info("Bloco recebido: %s", streamed_block.txb_id);

        if (!verify_nonce(&streamed_block))
        {
            log_info("Bloco recebido com nonce inválido\n");
        }

        char hash[HASH_SIZE];
        compute_sha256(&streamed_block, hash); // Compute the hash of the block

        strcpy(ledgerInterface.blocks[*(ledgerInterface.last_block_index) + 1].txb_id, streamed_block.txb_id);
        strcpy(ledgerInterface.blocks[*(ledgerInterface.last_block_index) + 1].previous_block_hash, streamed_block.previous_block_hash);
        *(ledgerInterface.blocks[*(ledgerInterface.last_block_index) + 1].timestamp) = streamed_block.timestamp;
        *(ledgerInterface.blocks[*(ledgerInterface.last_block_index) + 1].nonce) = streamed_block.nonce;

        /* for (size_t i = 0; i < TRANSACTIONS_PER_BLOCK; i++)
        {
            ledgerInterface.blocks[*(ledgerInterface.last_block_index) + 1].transactions[i] = streamed_block.transactions[i];
        } */

        strcpy(ledgerInterface.last_block_hash, hash);
        (*(ledgerInterface.count))++;
        (*(ledgerInterface.last_block_index))++;
        log_info("Hash do bloco recebido: %s\n", hash);

        log_info("Bloco aprovado:");
        print_transaction_block(&streamed_block); // Your custom print function

        print_ledger(&ledgerInterface); // Print the ledger

        free(buffer);
        free(streamed_block.transactions);
    }

    cleanup();
}
