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
void print_ledger(const LedgerInterface *ledger);
void print_transaction_block(const TransactionBlock *block);

#endif // SHMMANAGEMENT_H