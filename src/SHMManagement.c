
#include "../include/Controller.h"

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

TransactionBlockInterface interfaceTxBlock(void *shm_base, size_t block_offset)
{
    TransactionBlockInterface block;

    // Map the shared memory block
    TransactionBlockSHM *shm_block = (TransactionBlockSHM *)((char *)shm_base + block_offset);

    // Set pointers to the fields in shared memory
    block.txb_id = shm_block->txb_id;
    block.previous_block_hash = shm_block->previous_block_hash;
    block.timestamp = &shm_block->timestamp;
    block.nonce = &shm_block->nonce;
    block.num_transactions = &shm_block->num_transactions;

    // Set the pointer to the transactions array in shared memory
    block.transactions = (Transaction *)((char *)shm_base + shm_block->transactions_offset);

    return block;
}

LedgerInterface interfaceLedger(void *shm_base)
{
    LedgerInterface ledger;

    // Map the shared memory ledger
    LedgerSHM *shm_ledger = (LedgerSHM *)shm_base;

    // Set pointers to the fields in shared memory
    ledger.last_block_index = &shm_ledger->last_block_index;
    ledger.last_block_hash = shm_ledger->last_block_hash;
    ledger.num_blocks = &shm_ledger->num_blocks;

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
        blocks_offset += sizeof(TransactionBlockSHM) + (*ledger.blocks[i].num_transactions * sizeof(Transaction));
    }

    return ledger;
}

void print_ledger(const LedgerInterface *ledger)
{
    if (ledger == NULL || ledger->blocks == NULL)
    {
        printf("Ledger is empty or not initialized.\n");
        return;
    }

    printf("Ledger:\n");
    printf("Last Block Index: %u\n", *ledger->last_block_index);
    printf("Last Block Hash: %s\n", ledger->last_block_hash);
    printf("Number of Blocks: %u\n", *ledger->num_blocks);

    for (unsigned int i = 0; i < *ledger->num_blocks; i++)
    {
        TransactionBlockInterface block = ledger->blocks[i];

        printf("\nBlock %u:\n", i);
        printf("  Block ID: %s\n", block.txb_id);
        printf("  Previous Block Hash: %s\n", block.previous_block_hash);
        printf("  Timestamp: %ld\n", *block.timestamp);
        printf("  Nonce: %u\n", *block.nonce);
        printf("  Number of Transactions: %u\n", *block.num_transactions);

        for (unsigned int j = 0; j < *block.num_transactions; j++)
        {
            Transaction tx = block.transactions[j];
            printf("    Transaction %u:\n", j);
            printf("      ID: %llu\n", tx.id);
            printf("      Reward: %u\n", tx.reward);
            printf("      Value: %.2f\n", tx.value);
            printf("      Timestamp: %ld\n", tx.timestamp);
        }
    }
}

// Function to print a TransactionBlock
void print_transaction_block(const TransactionBlock *block)
{
    if (block == NULL)
    {
        printf("TransactionBlock is NULL.\n");
        return;
    }

    printf("Transaction Block:\n");
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
    for (int i = 0; i < TRANSACTIONS_PER_BLOCK; i++)
    {
        printf("    Transaction %d:\n", i + 1);
        printf("      ID: %llu\n", block->transactions[i].id);
        printf("      Reward: %u\n", block->transactions[i].reward);
        printf("      Value: %.2f\n", block->transactions[i].value);
        printf("      Timestamp: %ld\n", block->transactions[i].timestamp);
    }
}
