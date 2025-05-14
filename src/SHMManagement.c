
#include "../include/Controller.h"

#include <string.h>

size_t TRANSACTIONS_PER_BLOCK;
int BLOCKCHAIN_BLOCKS;
int TRANSACTION_POOL_SIZE;

// criar uma interface para a transactions pool: permite um acesso mais transparente à shared memory de transactions pool
TransactionPoolInterface interfaceTxPool(void *shm_base)
{
    TransactionPoolInterface pool;

    // Map the shared memory header
    TransactionPoolSHM *shm_pool = (TransactionPoolSHM *)shm_base;

    // Set pointers to the fields in shared memory
    pool.size = &shm_pool->size;
    pool.count = &shm_pool->count;
    pool.transactions = (PendingTransaction *)((char *)shm_base + shm_pool->transactions_offset);

    return pool;
}

// criar uma interface para a transactions pool: permite um acesso mais transparente a um transaction block na shared memory
TransactionBlockInterface interfaceTxBlock(void *shm_base, size_t block_offset)
{
    TransactionBlockInterface block;

    // Map the shared memory block
    TransactionBlockSHM *shm_block = (TransactionBlockSHM *)((char *)shm_base + block_offset);
    shm_block->transactions_offset = sizeof(TransactionBlockSHM);

    // Set pointers to the fields in shared memory
    block.txb_id = shm_block->txb_id;
    block.previous_block_hash = shm_block->previous_block_hash;
    block.timestamp = &shm_block->timestamp;
    block.nonce = &shm_block->nonce;

    // Set the pointer to the transactions array in shared memory
    block.transactions = (Transaction *)((char *)shm_block + shm_block->transactions_offset);

    return block;
}

// criar uma interface para a ledger: permite um acesso mais transparente à shared memory de ledger
LedgerInterface interfaceLedger(void *shm_base)
{
    LedgerInterface ledger;

    // Map the shared memory ledger
    LedgerSHM *shm_ledger = (LedgerSHM *)shm_base;

    // Set pointers to the fields in shared memory
    ledger.last_block_index = &shm_ledger->last_block_index;
    ledger.last_block_hash = shm_ledger->last_block_hash;
    ledger.num_blocks = &shm_ledger->num_blocks;
    ledger.count = &shm_ledger->count;

    // Allocate memory for the array of local TransactionBlock structures
    ledger.blocks = (TransactionBlockInterface *)malloc(*ledger.num_blocks * sizeof(TransactionBlockInterface));
    if (ledger.blocks == NULL)
    {
        perror("Erro ao alocar memória para os blocos do ledger");
        exit(EXIT_FAILURE);
    }

    // Map each block in the shared memory to a local TransactionBlockInterface
    size_t blocks_offset = shm_ledger->blocks_offset;
    for (unsigned int i = 0; i < *ledger.num_blocks; i++)
    {
        ledger.blocks[i] = interfaceTxBlock(shm_base, blocks_offset);

        // Increment the offset to the next block
        blocks_offset += sizeof(TransactionBlockSHM) + (TRANSACTIONS_PER_BLOCK * sizeof(Transaction));
    }

    return ledger;
}

// liberta a memoria alocada para a interface do ledger
void freeLedger(LedgerInterface *ledger)
{
    if (ledger == NULL)
    {
        return;
    }

    // Free the memory allocated for the blocks array
    if (ledger->blocks != NULL)
    {
        free(ledger->blocks);
        ledger->blocks = NULL;
    }

    // Reset other pointers to NULL for safety
    ledger->last_block_index = NULL;
    ledger->last_block_hash = NULL;
    ledger->num_blocks = NULL;
}

// as seguintes funcoes servem maioritariamente para debug
void print_transaction_block_interface(const TransactionBlockInterface *block)
{
    if (block == NULL)
    {
        printf("TransactionBlockInterface is NULL.\n");
        return;
    }

    printf("Transaction Block Interface:\n");
    printf("  Block ID: %s\n", block->txb_id);
    printf("  Previous Block Hash: %s\n", block->previous_block_hash);
    printf("  Timestamp: %ld\n", block->timestamp ? *block->timestamp : 0);
    printf("  Nonce: %u\n", block->nonce ? *block->nonce : 0);

    if (block->transactions == NULL)
    {
        printf("  Transactions: NULL\n");
        return;
    }

    printf("  Transactions:\n");
    for (unsigned int i = 0; i < TRANSACTIONS_PER_BLOCK; i++)
    {
        Transaction *tx = &block->transactions[i];
        printf("    Transaction %u: ID=%s, Reward=%d, Value=%.2f, Timestamp=%ld\n",
               i + 1, tx->tx_id, tx->reward, tx->value, tx->timestamp);
    }
}

void print_ledger(const LedgerInterface *ledger)
{
    if (ledger == NULL || ledger->blocks == NULL)
    {
        printf("Ledger is empty or not initialized.\n");
        return;
    }

    printf("\nLedger:\n");
    printf("Last Block Index: %u\n", *ledger->last_block_index);
    printf("Last Block Hash: %s\n", ledger->last_block_hash);
    printf("Number of Blocks: %u\n", *ledger->num_blocks);
    printf("Count: %u\n", *ledger->count);

    for (unsigned int i = 0; i < *ledger->count; i++)
    {
        TransactionBlockInterface block = ledger->blocks[i];
        if (strcmp(block.txb_id, "") == 0)
        {
            continue;
        }

        printf("Block %u: ID=%s, Previous Hash=%s, Timestamp=%ld, Nonce=%u\n", i, block.txb_id, block.previous_block_hash, *block.timestamp, *block.nonce);

        for (unsigned int j = 0; j < TRANSACTIONS_PER_BLOCK; j++)
        {
            Transaction tx = block.transactions[j];
            printf("    Transaction %u: ID=%s, Reward=%u, Value=%.2f, Timestamp=%ld\n",
                   j, tx.tx_id, tx.reward, tx.value, tx.timestamp);
        }
    }
}

void print_transaction_block(const TransactionBlock *block)
{
    if (block == NULL)
    {
        printf("TransactionBlock is NULL.\n");
        return;
    }

    printf("\nTransaction Block:\n");
    printf("  Block ID: %s\n", block->txb_id);
    printf("  Previous Block Hash: %s\n", block->previous_block_hash);
    printf("  Timestamp: %ld\n", block->timestamp);
    printf("  Nonce: %u\n", block->nonce);

    if (block->transactions == NULL)
    {
        printf("  Transactions: NULL\n");
        return;
    }

    printf("  Transactions:\n");
    for (int i = 0; i < (int)TRANSACTIONS_PER_BLOCK; i++)
    {
        printf("    Transaction %d: ID=%s, Reward=%u, Value=%.2f, Timestamp=%ld\n",
               i + 1, block->transactions[i].tx_id, block->transactions[i].reward,
               block->transactions[i].value, block->transactions[i].timestamp);
    }
}

void print_transaction(const Transaction *tx)
{
    if (tx == NULL)
    {
        printf("Transaction is NULL.\n");
        return;
    }

    printf("Transaction: ID=%s, Reward=%d, Value=%.2f, Timestamp=%ld\n",
           tx->tx_id, tx->reward, tx->value, tx->timestamp);
}

void print_transaction_pool(TransactionPoolInterface *tx_pool)
{
    if (tx_pool == NULL || tx_pool->size == NULL || tx_pool->count == NULL || tx_pool->transactions == NULL)
    {
        printf("Transaction pool is not initialized.\n");
        return;
    }

    printf("\nTransaction Pool:\n");
    printf("  Size: %u\n", *tx_pool->size);
    printf("  Count: %u\n", *tx_pool->count);

    for (unsigned int i = 0; i < *tx_pool->size; i++)
    {
        PendingTransaction *pt = &tx_pool->transactions[i];
        if (pt->filled)
        {
            printf("  Transaction %u: ID=%s, Reward=%u, Value=%.2f, Timestamp=%ld, Age=%u\n", i + 1, pt->tx.tx_id, pt->tx.reward, pt->tx.value, pt->tx.timestamp, pt->age);
        }
        else
        {
            printf("  Transaction %u: Empty\n", i + 1);
        }
    }
}
