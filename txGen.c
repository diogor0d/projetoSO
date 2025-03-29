/*
    Projeto de Sistemas Operativos 2024/2025 - DEIChain
    Diogo Nuno Fonseca Rodrigues 2022257625
    Guilherme Teixeira Gonçalves Rosmaninho 2022257636
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#include <semaphore.h>

#include "Controller.h"

void generate_transaction(Transaction *tx, int id, int reward)
{
    tx->id = id;
    tx->reward = reward;
    tx->sender = rand() % 1000;
    tx->receiver = rand() % 1000;
    tx->age = 0;
    tx->value = (double)(rand() % 10000) / 100.0;
    tx->created_at = time(NULL);
}

int main(int argc, char *argv[])
{

    printf("%s", SHM_TRANSACTIONS_POOL);

    int shm_fd;
    sem_t *sem_tx_pool;

    // Ler e processar os argumentos da linha de comando
    if (argc != 3)
    {
        fprintf(stderr, "Uso: %s <reward> <sleep time>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    int reward = atoi(argv[1]);
    int sleep_time = atoi(argv[2]);

    // Abrir o semáforo para a transactions pool
    sem_tx_pool = sem_open(SEM_TRANSACTIONS_POOL, 0);
    if (sem_tx_pool == SEM_FAILED)
    {
        perror("Erro ao abrir semáforo SEM_TRANSACTIONS_POOL\n");
        exit(EXIT_FAILURE);
    }

    // Abrir a memória partilhada
    shm_fd = shm_open(SHM_TRANSACTIONS_POOL, O_RDWR, 0666);
    if (shm_fd == -1)
    {
        perror("Erro ao abrir SHM_TRANSACTIONS_POOL\n");
        sem_close(sem_tx_pool);
        exit(EXIT_FAILURE);
    }

    // Como do ponto de vista do txgen o poolsize é desconhecido, fazemos um map inicial para obter as caracteristicas da transaction pool (nomeadamente o tamanho) e obtida esta informação, fazemos um novo map com o tamanho total da memória partilhada a ser utilizada.

    // Mapear apenas o cabeçalho (TransactionPool) para ler o tamanho
    void *shm_base = mmap(NULL, sizeof(TransactionPool), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_base == MAP_FAILED)
    {
        perror("Erro ao mapear SHM_TRANSACTIONS_POOL\n");
        close(shm_fd);
        sem_close(sem_tx_pool);
        exit(EXIT_FAILURE);
    }
    printf("Mapeamento da memoria partilhada inicial efetuado...\n");

    // Ler o tamanho da pool de transações
    TransactionPool *tx_pool = (TransactionPool *)shm_base;
    int pool_size = tx_pool->size;
    printf("Leitura do tamanho da poolsize efetuado...\n");

    printf("Tamanho da transactions pool: %d\n", pool_size);

    // Desmapear a memória inicial
    if (munmap(shm_base, sizeof(TransactionPool)) == -1)
    {
        perror("Erro ao desmapear SHM_TRANSACTIONS_POOL\n");
        close(shm_fd);
        sem_close(sem_tx_pool);
        exit(EXIT_FAILURE);
    }
    printf("Desmapeamento efetuado...\n");

    // Calcular o tamanho total da memória partilhada
    size_t shm_size = sizeof(TransactionPool) + (pool_size * sizeof(Transaction));

    printf("Redimensionamento da memoria partilhada em curso...\n");
    // Mapear novamente com o tamanho total
    shm_base = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_base == MAP_FAILED)
    {
        perror("Erro ao mapear SHM_TRANSACTIONS_POOL com tamanho total\n");
        close(shm_fd);
        sem_close(sem_tx_pool);
        exit(EXIT_FAILURE);
    }

    tx_pool = (TransactionPool *)shm_base;

    srand(time(NULL));

    // numero incremental para determinar o transaction id posteriormente
    int tx_number = 0;

    while (1)
    {

        // Adicionar nova transação à transactions pool
        printf("Adicionando nova transação à transactions pool...\n");

        // Bloquear o semáforo antes de escrever na pool
        if (sem_wait(sem_tx_pool) == -1)
        {
            perror("Erro ao bloquear o semáforo");
            break;
        }

        if (tx_pool->count < pool_size)
        {

            for (int i = 0; i < tx_pool->size; i++)
            {
                if (tx_pool->transactions[i].id == 0) // verificação para assegurar que o lugar na lista está livre
                {
                    printf("Lugar livre\n");

                    Transaction new_tx;
                    int t_id = getpid() * 100000 + (int)time(NULL) % 100000000 + tx_number; // acrescentou-se o tempo para garantir unicidade
                    generate_transaction(&new_tx, t_id, reward);
                    tx_pool->transactions[i] = new_tx;
                    tx_pool->count++;
                    tx_number++;
                    printf("Transação Gerada > ID: %d, Reward: %d\n", new_tx.id, new_tx.reward);
                    break;
                }
            }
        }
        else
        {
            printf("Transactions pool cheia. Aguardando...\n");
        }

        // Desbloquear o semáforo após a escrita
        if (sem_post(sem_tx_pool) == -1)
        {
            perror("Erro ao desbloquear o semáforo");
            break;
        }

        sleep(sleep_time);
    }

    // Unmap e fecho da shared memory
    if (munmap(tx_pool, shm_size) == -1)
    {
        perror("Erro ao desmapear SHM_TRANSACTIONS_POOL\n");
    }
    if (close(shm_fd) == -1)
    {
        perror("Erro ao fechar SHM_TRANSACTIONS_POOL\n");
    }

    // Fechar o semáforo
    sem_close(sem_tx_pool);

    return 0;
}
