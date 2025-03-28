//
// Created by diogo on 27/03/2025.
//

#ifndef CONTROLLER_H
#define CONTROLLER_H

typedef struct {
    int id;
    double reward;
    int sender;
    int receiver;
    double value;
    time_t created_at;
} Transaction;

typedef struct {
    int current_block_id;
    Transaction* transactions;
} TransactionPool;

#endif //CONTROLLER_H
