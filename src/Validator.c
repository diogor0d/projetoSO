/*
    Projeto de Sistemas Operativos 2024/2025 - DEIChain
    Diogo Nuno Fonseca Rodrigues 2022257625
    Guilherme Teixeira Gonçalves Rosmaninho 2022257636
*/

#define _GNU_SOURCE // para o vasprintf e strcasestr
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
#include <semaphore.h>
#include <pthread.h>
#include <mqueue.h>
#include <errno.h>

#include "../include/Controller.h"
#include "../include/PoW/pow.h"
#include "../include/SHMManagement.h"

static sem_t *sem_log_file = NULL;
static FILE *log_file = NULL;
static char TIPO_PROCESSO_BUFFER[32];              // Larger static buffer to hold the process type string
static char *TIPO_PROCESSO = TIPO_PROCESSO_BUFFER; // Point to the static buffer

// aceder a variavel globais do controller
int LEDGER_SIZE;
size_t TRANSACTIONS_PER_BLOCK;
int shm_transactionspool_size;
int shm_ledger_size;

static sem_t *sem_tx_pool = NULL;
static sem_t *sem_ledger = NULL;
int NUM_MINERS;

static mqd_t statistics_mq;

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

// buffer pipe
char *buffer;

int *tx_found_bitmap;

TransactionBlock nemesis_block = {0};
TransactionBlock streamed_block = {0};

static void
log_info(const char *format, ...)
{
    char *log_message;
    va_list args;

    va_start(args, format);

    // Format the string (outside of critical section)
    if (vasprintf(&log_message, format, args) == -1)
    {
        va_end(args);
        perror("Erro ao formatar mensagem de log");
        free(log_message);
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

void cleanup()
{
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

    freeLedger(&ledgerInterface);

    // Fechar o semáforo
    if (sem_tx_pool != NULL)
    {
        if (sem_close(sem_tx_pool) == -1)
        {
            log_info("Erro ao fechar o semáforo SEM_TRANSACTIONS_POOL");
        }
        else
        {
            log_info("sem_tx_pool fechado com sucesso");
        }
    }

    // ledger
    if (sem_ledger != NULL)
    {
        if (sem_close(sem_ledger) == -1)
        {
            log_info("Erro ao fechar semáforo SEM_LEDGER");
        }
        else
        {
            log_info("sem_ledger fechado com sucesso");
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

static void sigterm(int signum)
{
    (void)signum; // Ignore the signal parameter

    log_info("SIGTERM recebido. A terminar execução...");

    cleanup(); // Call the cleanup function to close the log file and semaphores

    // Exit the process
    exit(EXIT_SUCCESS);
}

// Function to compare two transactions
int txCompare(const Transaction *tx1, const Transaction *tx2)
{
    if (tx1 == NULL || tx2 == NULL)
    {
        return 0; // Return false if either transaction is NULL
    }

    // Compare all fields of the Transaction struct
    if (strcmp(tx1->tx_id, tx2->tx_id) == 0 &&
        tx1->reward == tx2->reward &&
        tx1->value == tx2->value &&
        tx1->timestamp == tx2->timestamp)
    {
        return 1; // Transactions are equal
    }

    return 0; // Transactions are not equal
}

void validator(int num)
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
    snprintf(TIPO_PROCESSO, sizeof(TIPO_PROCESSO_BUFFER), "VALIDATOR %d [%d]", num, getpid());

    // abrir a memoria partilhada para a transactions pool (já existente)
    shm_transactionspool_fd = shm_open(SHM_TRANSACTIONS_POOL, O_RDWR, 0666);
    if (shm_transactionspool_fd == -1)
    {
        log_info("Erro ao abrir memória partilhada para transactions pool");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // mapear a memoria partilhada para o espaço de memória do processo
    shm_transactionspool_base = mmap(NULL, shm_transactionspool_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_transactionspool_fd, 0);
    if (shm_transactionspool_base == MAP_FAILED)
    {
        log_info("Erro ao mapear memória partilhada para transactions pool");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // abrir a memoria partilhada para a ledger (já existente)
    shm_ledger_fd = shm_open(SHM_LEDGER, O_RDWR, 0666);
    if (shm_ledger_fd == -1)
    {
        log_info("Erro ao abrir memória partilhada para ledger");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // mapear a memoria partilhada para o espaço de memória do processo
    shm_ledger_base = mmap(NULL, shm_ledger_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_ledger_fd, 0);
    if (shm_ledger_base == MAP_FAILED)
    {
        log_info("Erro ao mapear memória partilhada para transactions pool");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // Abrir o semáforo para a transactions pool
    sem_tx_pool = sem_open(SEM_TRANSACTIONS_POOL, 0);
    if (sem_tx_pool == SEM_FAILED)
    {
        perror("Erro ao abrir semáforo SEM_TRANSACTIONS_POOL\n");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // Abrir o semáforo para a ledger
    sem_ledger = sem_open(SEM_LEDGER, 0);
    if (sem_ledger == SEM_FAILED)
    {
        log_info("Erro ao abrir semáforo SEM_LEDGER");
        cleanup();
        exit(EXIT_FAILURE);
    }

    validation_pipe_fd = open(VALIDATION_PIPE, O_RDONLY);
    if (validation_pipe_fd < 0)
    {
        log_info("Erro ao abrir o pipe de validação");
        cleanup();
        exit(EXIT_FAILURE);
    }
    log_info("Pipe de validação aberto com sucesso.");

    statistics_mq = mq_open(STATISTICS_MQ, O_WRONLY | O_NONBLOCK);
    if (statistics_mq == (mqd_t)-1)
    {
        log_info("Erro ao abrir a message queue %s", STATISTICS_MQ);
        cleanup();
        exit(EXIT_FAILURE);
    }

    signal(SIGTERM, sigterm); // tratar o sinal SIGTERM para terminar o processo corretamente

    ledgerInterface = interfaceLedger(shm_ledger_base);   // criar a interface para o ledger
    tx_pool = interfaceTxPool(shm_transactionspool_base); // criar a interface para a transactions pool

    // print_ledger(&ledgerInterface); // Print the ledger

    sem_wait(sem_ledger); // bloquear o semáforo para o ledger

    if (*(ledgerInterface.count) == 0)
    {
        nemesis_block.transactions =
            (Transaction *)calloc(TRANSACTIONS_PER_BLOCK, sizeof(Transaction));
        PoWResult r;
        r = proof_of_work(&nemesis_block);
        if (r.error)
        {
            perror("Could not compute the Hash\n");
            free(nemesis_block.transactions);
            cleanup();
            exit(1);
        }

        // print_transaction_block(&nemesis_block); // Print the block

        *(ledgerInterface.count) = 1;
        *(ledgerInterface.blocks[0].txb_id) = 'z';

        strcpy(ledgerInterface.blocks[0].txb_id, nemesis_block.txb_id);
        *(ledgerInterface.blocks[0].timestamp) = nemesis_block.timestamp;
        *(ledgerInterface.blocks[0].nonce) = nemesis_block.nonce;

        // memcpy(ledgerInterface.blocks[0].transactions, nemesis_block.transactions, TRANSACTIONS_PER_BLOCK * sizeof(Transaction));

        strcpy(ledgerInterface.last_block_hash, r.hash);
        // sem_post(sem_ledger); // desbloquear o semáforo para o ledger

        free(nemesis_block.transactions); // Free the allocated memory for transactions
        log_info("Hash inicial (Bloco origem): %s", ledgerInterface.last_block_hash);
    }

    sem_post(sem_ledger); // desbloquear o semáforo para o ledger

    // print_ledger(&ledgerInterface); // Print the ledger

    while (1)
    {

        // tamanho - "header" - transacoes
        size_t MAX_BLOCK_SIZE = sizeof(size_t) + (sizeof(TransactionBlock) - sizeof(Transaction *)) + (TRANSACTIONS_PER_BLOCK * sizeof(Transaction));
        buffer = malloc(MAX_BLOCK_SIZE);
        if (!buffer)
        {
            perror("malloc");
            break;
        }

        ssize_t read_bytes = read(validation_pipe_fd, buffer, MAX_BLOCK_SIZE);
        if (read_bytes <= 0)
        {
            free(buffer);
            buffer = NULL;
            break;
        }

        if ((size_t)read_bytes < sizeof(size_t))
        {
            fprintf(stderr, "Leitura demasiado pequena para o tamanho recebido da transmissao \n");
            free(buffer);
            buffer = NULL;
            break;
        }

        size_t payload_size;
        memcpy(&payload_size, buffer, sizeof(size_t));

        // Check if the payload size matches remaining data
        if ((size_t)read_bytes != sizeof(size_t) + payload_size)
        {
            fprintf(stderr, "Esperado payload com o tamanho %zu, recebido %zu\n", payload_size, read_bytes - sizeof(size_t));
            free(buffer);
            buffer = NULL;
            break;
        }

        // Now parse the payload
        char *payload = buffer + sizeof(size_t);

        size_t header_size = sizeof(TransactionBlock) - sizeof(Transaction *);
        memcpy(&streamed_block, payload, header_size);

        streamed_block.transactions = malloc(sizeof(Transaction) * TRANSACTIONS_PER_BLOCK);
        if (!streamed_block.transactions)
        {
            perror("malloc transactions");
            free(buffer);
            buffer = NULL;
            break;
        }

        memcpy(streamed_block.transactions, payload + header_size, sizeof(Transaction) * TRANSACTIONS_PER_BLOCK);

        StatisticsMessage new_msg = {0};
        int skip_block = 0; // Flag to indicate whether to skip the block
        char last_block_hash[HASH_SIZE];

        // OPTIMIZATION 1: Minimize ledger lock time - get hash and check nonce
        sem_wait(sem_ledger);
        memcpy(last_block_hash, ledgerInterface.last_block_hash, HASH_SIZE);
        sem_post(sem_ledger);

        log_info("Inicio da validação do bloco %s", streamed_block.txb_id);

        // Verify nonce - can be done outside lock
        if (!verify_nonce(&streamed_block))
        {
            log_info("Bloco recebido com nonce inválido\n");
            skip_block = 1;
        }

        // Check hash - can be done outside lock since we copied the hash
        if (strncmp(streamed_block.previous_block_hash, last_block_hash, HASH_SIZE) != 0)
        {
            log_info("Bloco recebido com hash anterior inválida - Bloco %s rejeitado", streamed_block.txb_id);
            skip_block = 1;
        }

        // OPTIMIZATION 2: Create a copy of the transactions we need to check
        // This allows us to minimize the time we hold the transaction pool lock
        Transaction temp_tx_array[TRANSACTIONS_PER_BLOCK];

        // Dynamically allocate tx_found_bitmap
        tx_found_bitmap = calloc(TRANSACTIONS_PER_BLOCK, sizeof(int));
        if (!tx_found_bitmap)
        {
            perror("calloc tx_found_bitmap");
            free(buffer);
            buffer = NULL;
            free(streamed_block.transactions);
            break;
        }

        if (!skip_block)
        {
            // OPTIMIZATION 3: Hold transaction pool lock only for the minimum time needed
            sem_wait(sem_tx_pool);

            // First, check if transactions exist in the pool
            // log_info("Verificando transações do bloco %s", streamed_block.txb_id);
            for (size_t i = 0; i < TRANSACTIONS_PER_BLOCK && !skip_block; i++)
            {
                int found = 0;
                for (size_t j = 0; j < *(tx_pool.size); j++)
                {
                    if (tx_pool.transactions[j].filled == 0)
                    {
                        continue; // Skip empty transactions
                    }

                    if (strcmp(streamed_block.transactions[i].tx_id, tx_pool.transactions[j].tx.tx_id) == 0)
                    {
                        // Check if transaction content matches
                        if (txCompare(&streamed_block.transactions[i], &tx_pool.transactions[j].tx) == 0)
                        {
                            log_info("Conteudo transação %s referenciada difere da transacao na pool. Bloco %s rejeitado",
                                     streamed_block.transactions[i].tx_id, streamed_block.txb_id);
                            skip_block = 1;
                            break;
                        }
                        found = 1;
                        tx_found_bitmap[i] = 1;

                        // Keep a copy so we don't need another lock to process later
                        memcpy(&temp_tx_array[i], &tx_pool.transactions[j].tx, sizeof(Transaction));
                        break;
                    }
                }

                if (!found)
                {
                    log_info("Transação %s não encontrada na transactions pool. Bloco %s rejeitado",
                             streamed_block.transactions[i].tx_id, streamed_block.txb_id);
                    skip_block = 1;
                    break;
                }
            }

            // OPTIMIZATION 4: If the block is valid, remove transactions from pool in the same lock session
            if (!skip_block)
            {
                // log_info("A remover transações do bloco %s da transactions pool", streamed_block.txb_id);
                for (size_t i = 0; i < TRANSACTIONS_PER_BLOCK; i++)
                {
                    for (size_t j = 0; j < *(tx_pool.size); j++)
                    {
                        if (tx_pool.transactions[j].filled == 0)
                        {
                            continue; // Skip empty transactions
                        }

                        if (strcmp(streamed_block.transactions[i].tx_id, tx_pool.transactions[j].tx.tx_id) == 0)
                        {
                            // Remove transaction from pool
                            tx_pool.transactions[j].filled = 0;
                            (*(tx_pool.count))--;
                            break;
                        }
                    }
                }

                // OPTIMIZATION 5: Perform aging in the same critical section
                // log_info("Aging transactions na transactions pool");
                for (size_t i = 0; i < *(tx_pool.size); i++)
                {
                    if (tx_pool.transactions[i].filled == 1)
                    {
                        if (tx_pool.transactions[i].age % 50 == 0)
                        {
                            tx_pool.transactions[i].tx.reward++;
                        }
                        tx_pool.transactions[i].age++;
                    }
                }

                // log_info("Transações restantes na transactions pool: %d", *(tx_pool.count));
            }

            sem_post(sem_tx_pool);
        }

        // Handle the case where the block should be skipped
        if (skip_block)
        {
            free(buffer);
            buffer = NULL;
            free(streamed_block.transactions);
            free(tx_found_bitmap); // Free dynamically allocated bitmap

            // OPTIMIZATION 6: Prepare statistics message outside locks
            snprintf(new_msg.txb_id, TXB_ID_LEN, "%s", streamed_block.txb_id);

            // Extract miner ID
            int miner_id;
            if (sscanf(streamed_block.txb_id, "BLOCK-%d-", &miner_id) == 1)
            {
                snprintf(new_msg.miner_id, sizeof(new_msg.miner_id), "%d", miner_id);
            }
            else
            {
                log_info("Erro ao extrair miner_id de %s", streamed_block.txb_id);
                snprintf(new_msg.miner_id, sizeof(new_msg.miner_id), "-1");
            }

            new_msg.block_index = -1;
            new_msg.earned_amount = -1;

            // Send message to queue (no lock needed)
            if (mq_send(statistics_mq, (const char *)&new_msg, sizeof(StatisticsMessage), 0) == -1)
            {
                log_info("Erro ao enviar mensagem para a message queue %s", STATISTICS_MQ);
            }
            else
            {
                // log_info("Mensagem enviada para a message queue %s", STATISTICS_MQ);
            }

            continue;
        }

        log_info("Bloco %s validado com sucesso", streamed_block.txb_id);

        // Block is valid - prepare to add to ledger
        // Compute hash outside the lock
        char hash[HASH_SIZE];
        compute_sha256(&streamed_block, hash);

        // OPTIMIZATION 7: Short and focused ledger update
        sem_wait(sem_ledger);

        // Double check that ledger state hasn't changed while we were processing
        if (strncmp(streamed_block.previous_block_hash, ledgerInterface.last_block_hash, HASH_SIZE) != 0)
        {
            log_info("Estado da ledger alterou-se durante processamento. Bloco %s rejeitado", streamed_block.txb_id);
            sem_post(sem_ledger);
            free(buffer);
            free(streamed_block.transactions);
            free(tx_found_bitmap); // Free dynamically allocated bitmap
            continue;
        }

        // Add block to ledger
        unsigned int next_index = *(ledgerInterface.last_block_index) + 1;

        strcpy(ledgerInterface.blocks[next_index].txb_id, streamed_block.txb_id);
        strcpy(ledgerInterface.blocks[next_index].previous_block_hash, streamed_block.previous_block_hash);
        *(ledgerInterface.blocks[next_index].timestamp) = streamed_block.timestamp;
        *(ledgerInterface.blocks[next_index].nonce) = streamed_block.nonce;

        for (size_t i = 0; i < TRANSACTIONS_PER_BLOCK; i++)
        {
            strcpy(ledgerInterface.blocks[next_index].transactions[i].tx_id, streamed_block.transactions[i].tx_id);
            ledgerInterface.blocks[next_index].transactions[i].reward = streamed_block.transactions[i].reward;
            ledgerInterface.blocks[next_index].transactions[i].value = streamed_block.transactions[i].value;
            ledgerInterface.blocks[next_index].transactions[i].timestamp = streamed_block.transactions[i].timestamp;
        }

        strcpy(ledgerInterface.last_block_hash, hash);
        (*(ledgerInterface.count))++;
        (*(ledgerInterface.last_block_index))++;

        new_msg.timestamp = time(NULL);

        sem_post(sem_ledger);

        log_info("Bloco %s inserido na blockchain.", streamed_block.txb_id);

        // OPTIMIZATION 8: Prepare statistics message outside locks
        snprintf(new_msg.txb_id, TXB_ID_LEN, "%s", streamed_block.txb_id);

        // Extract miner ID
        int miner_id;
        if (sscanf(streamed_block.txb_id, "BLOCK-%d-", &miner_id) == 1)
        {
            snprintf(new_msg.miner_id, sizeof(new_msg.miner_id), "%d", miner_id);
        }
        else
        {
            log_info("Erro ao extrair miner_id de %s", streamed_block.txb_id);
            snprintf(new_msg.miner_id, sizeof(new_msg.miner_id), "-1");
        }

        new_msg.block_index = next_index;
        new_msg.earned_amount = 0;

        // Calculate total transaction rewards
        for (size_t i = 0; i < TRANSACTIONS_PER_BLOCK; i++)
        {
            new_msg.earned_amount += streamed_block.transactions[i].reward;
        }

        // Send statistics message (no lock needed)
        if (mq_send(statistics_mq, (const char *)&new_msg, sizeof(StatisticsMessage), 0) == -1)
        {
            log_info("Erro ao enviar mensagem para a message queue %s: %s (errno=%d)",
                     STATISTICS_MQ, strerror(errno), errno);
        }
        else
        {
            // log_info("Mensagem enviada para a message queue %s", STATISTICS_MQ);
        }

        free(buffer);
        buffer = NULL;
        free(streamed_block.transactions);
        streamed_block.transactions = NULL;
        free(tx_found_bitmap); // Free dynamically allocated bitmap
        tx_found_bitmap = NULL;
    }

    cleanup();
}
