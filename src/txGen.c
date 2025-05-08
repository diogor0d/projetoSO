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
#include <signal.h>
#include <pthread.h>

#include "../include/Controller.h"

static int tx_number = 0;                  // incremental value for transaction id
static int shm_fd = 0;                     // file descriptor para a shared memory
static sem_t *sem_tx_pool = NULL;          // semáforo para a transactions pool
static sem_t *sem_minerwork = NULL;        // semáforo para o miner work
static void *shm_base = NULL;              // ponteiro para a memória partilhada
static size_t shm_size = 0;                // tamanho da memória partilhada
static TransactionPoolSHM *tx_pool = NULL; // ponteiro para a pool de transações

// cond var
static int shm_minerworkcondvar_fd = -1;
static void *shm_minerworkcondvar_base = NULL;
static MinerWorKCondVar *minerwork_condvar = NULL; // ponteiro para a cond var

static int generated_transactions = 0; // contador de blocos gerados

unsigned long long current_time_in_milliseconds()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);                                     // obtem o tempo atual
    return (unsigned long long)(ts.tv_sec) * 1000 + (ts.tv_nsec) / 1000000; // converter pala milisegundos
}

void cleanup()
{
    // Fechar o semáforo
    if (sem_tx_pool != NULL)
    {
        if (sem_close(sem_tx_pool) == -1)
        {
            perror("Erro ao fechar o semáforo SEM_TRANSACTIONS_POOL\n");
        }
    }

    if (sem_minerwork != NULL)
    {
        if (sem_close(sem_minerwork) == -1)
        {
            perror("Erro ao fechar o semáforo SEM_MINERWORK\n");
        }
    }

    // Unmap e fecho da shared memory
    if (shm_fd != -1)
    {
        if (shm_base != NULL)
        {
            if (munmap(shm_base, shm_size) == -1)
            {
                perror("Erro ao desmapear SHM_TRANSACTIONS_POOL\n");
            }
        }
        if (close(shm_fd) == -1)
        {
            perror("Erro ao fechar SHM_TRANSACTIONS_POOL\n");
        }
    }

    // limpar recursos da cond var
    if (shm_minerworkcondvar_fd != -1)
    {
        if (shm_minerworkcondvar_base != NULL)
        {
            if (munmap(shm_minerworkcondvar_base, sizeof(MinerWorKCondVar)) == -1)
            {
                perror("Erro ao desmapear SHM_MINERWORK_CONDVAR\n");
            }
        }
        if (close(shm_minerworkcondvar_fd) == -1)
        {
            perror("Erro ao fechar SHM_MINERWORK_CONDVAR\n");
        }
    }
}

void sigint(int signum)
{
    (void)signum; // ignorar o sinal, não é necessário para o tratamento
    printf("\nA terminar o programa...\n");

    cleanup();

    exit(0);
}

void generate_transaction(PendingTransaction *p_tx, int reward)
{

    Transaction new_tx;

    snprintf(new_tx.tx_id, TX_ID_LEN, "TX-%d-%d", getpid(), tx_number);
    tx_number++; // incrementar o número da transação
    new_tx.reward = reward;
    new_tx.value = (double)(rand() % 10000) / 100.0;
    new_tx.timestamp = time(NULL);

    p_tx->tx = new_tx; // atribuir a nova transação ao PendingTransaction
    p_tx->filled = 1;  // marcar como ocupada
    p_tx->age = 0;     // inicializar a idade da transação
}

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

int main(int argc, char *argv[])
{

    signal(SIGINT, sigint); // redirecionar o sinal SIGINT para permitir a interrupção do programa

    // Ler e processar os argumentos da linha de comando
    if (argc != 3)
    {
        printf("Uso: %s <reward> <sleep time>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    int reward = atoi(argv[1]);
    int sleep_time = atoi(argv[2]);

    // Verificar se o reward está entre 1 e 3

    if (reward < 1 || reward > 3)
    {
        printf("O reward deve ser um valor entre 1 e 3.\n");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // Verificar se o sleep time está entre 200 e 3000

    if (sleep_time < 200 || sleep_time > 3000)
    {
        printf("O sleep time deve ser um valor entre 200 e 3000.\n");
        cleanup();
        exit(EXIT_FAILURE);
    }

    printf("A iniciar o txGen...\n");

    // Abrir o semáforo para a transactions pool
    sem_tx_pool = sem_open(SEM_TRANSACTIONS_POOL, 0);
    if (sem_tx_pool == SEM_FAILED)
    {
        perror("Erro ao abrir semáforo SEM_TRANSACTIONS_POOL\nSimulação não está em execução?\n");
        cleanup();
        exit(EXIT_FAILURE);
    }

    sem_minerwork = sem_open(SEM_MINERWORK, 0);
    if (sem_minerwork == SEM_FAILED)
    {
        perror("Erro ao abrir semáforo SEM_MINERWORK\n");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // Abrir a memória partilhada
    shm_fd = shm_open(SHM_TRANSACTIONS_POOL, O_RDWR, 0666);
    if (shm_fd == -1)
    {
        perror("Erro ao abrir SHM_TRANSACTIONS_POOL\n");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // Como do ponto de vista do txgen o poolsize é desconhecido, fazemos um map inicial para obter as caracteristicas da transaction pool (nomeadamente o tamanho) e obtida esta informação, fazemos um novo map com o tamanho total da memória partilhada a ser utilizada.

    // Mapear apenas o cabeçalho (TransactionPool) para ler o tamanho
    shm_base = mmap(NULL, sizeof(TransactionPoolSHM), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_base == MAP_FAILED)
    {
        perror("Erro ao mapear SHM_TRANSACTIONS_POOL\n");
        cleanup();
        exit(EXIT_FAILURE);
    }
    printf("Mapeamento da memoria partilhada inicial efetuado...\n");

    // Ler o tamanho da pool de transações
    tx_pool = (TransactionPoolSHM *)shm_base;
    unsigned int pool_size = tx_pool->size;
    printf("Leitura do tamanho da pool efetuado...\n");
    unsigned int num_miners = tx_pool->num_miners;
    unsigned int transactions_per_block = tx_pool->transactions_per_block;
    printf("Número de miners: %d\n", num_miners);
    printf("Transações por bloco: %d\n", transactions_per_block);

    printf("Tamanho da transactions pool: %d\n", pool_size);

    // Desmapear a memória inicial
    if (munmap(shm_base, sizeof(TransactionPoolSHM)) == -1)
    {
        perror("Erro ao desmapear SHM_TRANSACTIONS_POOL\n");
        cleanup();
        exit(EXIT_FAILURE);
    }
    printf("Desmapeamento efetuado...\n");

    // Calcular o tamanho total da memória partilhada
    shm_size = sizeof(TransactionPoolSHM) + (pool_size * sizeof(PendingTransaction));

    printf("Redimensionamento da memoria partilhada em curso...\n");
    // Mapear novamente com o tamanho total
    shm_base = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_base == MAP_FAILED)
    {
        perror("Erro ao mapear SHM_TRANSACTIONS_POOL com tamanho total\n");
        cleanup();
        exit(EXIT_FAILURE);
    }

    tx_pool = (TransactionPoolSHM *)shm_base;

    // abrir cond var
    shm_minerworkcondvar_fd = shm_open(SHM_MINERWORK_CONDVAR, O_RDWR, 0666);
    if (shm_minerworkcondvar_fd == -1)
    {
        perror("Erro ao abrir SHM_MINERWORK_CONDVAR\n");
        cleanup();
        exit(EXIT_FAILURE);
    }

    shm_minerworkcondvar_base = mmap(NULL, sizeof(MinerWorKCondVar), PROT_READ | PROT_WRITE, MAP_SHARED, shm_minerworkcondvar_fd, 0);
    if (shm_minerworkcondvar_base == MAP_FAILED)
    {
        perror("Erro ao mapear SHM_MINERWORK_CONDVAR\n");
        cleanup();
        exit(EXIT_FAILURE);
    }
    minerwork_condvar = (MinerWorKCondVar *)shm_minerworkcondvar_base;

    srand(time(NULL));

    // O facto das transações estarem na shared memory impede o uso direto de ponteiros, que apenas dizem respeito ao espaço de endereçamento do processo atual. Portanto, é necessário recorrer a offsets para manipular o acesso aos dados em cada processo que acede à shared memory. Para isso, utiliza-se uma interface local de acesso à transactions pool que aponta para os dados na memoria mapeada para a shared memory.
    TransactionPoolInterface tx_pool_interface = interfaceTxPool(shm_base);

    printf("A iniciar a geração de transações...\n");
    while (1)
    {
        // Bloquear o semáforo antes de escrever na pool
        if (sem_wait(sem_tx_pool) == -1)
        {
            perror("Erro ao bloquear o semáforo");
            break;
        }

        if (*tx_pool_interface.count < *tx_pool_interface.size)
        {

            for (unsigned int i = 0; i < *tx_pool_interface.size; i++)
            {
                if (tx_pool_interface.transactions[i].filled == 0) // verificação para assegurar que o lugar na lista está livre
                {

                    PendingTransaction new_tx;
                    generate_transaction(&new_tx, reward);
                    tx_pool_interface.transactions[i] = new_tx;
                    (*tx_pool_interface.count)++;
                    printf("Transação Gerada > ID: %s, Reward: %d\n", new_tx.tx.tx_id, new_tx.tx.reward);
                    generated_transactions++;
                    break;
                }
            }
        }
        else
        {
            printf("Transactions pool cheia. Aguardando...\n");
        }

        // apresentar contagem da tx pool
        printf("Transações na transactions pool: %d\n", *tx_pool_interface.count);
        printf("Trasacoes geradas: %d\n", generated_transactions);

        if (generated_transactions > (int)transactions_per_block && generated_transactions % transactions_per_block == 0)
        {
            int sval;
            sem_getvalue(sem_minerwork, &sval);
            // sval = number of posts not yet consumed.
            // We want at most NUM_MINERS outstanding.
            int to_post = num_miners - sval;
            if (to_post <= 0)
                continue; // already enough “permits” queued
            for (int i = 0; i < to_post; i++)
            {
                sem_post(sem_minerwork);
            }
        }

        // Desbloquear o semáforo após a escrita
        if (sem_post(sem_tx_pool) == -1)
        {
            perror("Erro ao desbloquear o semáforo");
            break;
        }

        usleep(sleep_time * 1000);
    }

    cleanup();

    return 0;
}
