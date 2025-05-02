/*
    Projeto de Sistemas Operativos 2024/2025 - DEIChain
    Diogo Nuno Fonseca Rodrigues 2022257625
    Guilherme Teixeira Gonçalves Rosmaninho 2022257636
*/

#include "../include/Controller.h"

#ifndef SHMMANAGEMENT_H
#define SHMMANAGEMENT_H

TransactionPoolInterface interfaceTxPool(void *shm_base);
TransactionBlockInterface interfaceTxBlock(void *shm_base, size_t block_offset);
LedgerInterface interfaceLedger(void *shm_base);
void freeLedger(LedgerInterface *ledger);
void print_ledger(const LedgerInterface *ledger);
void print_transaction_block(const TransactionBlock *block);
void print_transaction_pool(const TransactionPoolInterface *pool);

int get_max_transaction_reward(const TransactionBlock *block,
                               const int txs_per_block);
void serialize_transaction_block(const TransactionBlock *block, char *output, size_t output_size, int transactions_per_block);

#endif // SHMMANAGEMENT_H