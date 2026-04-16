# DEIChain: A Concurrency-Focused Blockchain Simulation

<img src="images/deichain.png" alt="arch" width="500">

## Overview
DEIChain is a simplified blockchain simulation developed for the Operating Systems (Sistemas Operativos) course of the Informatics Engineering Bachelor's degree. 

While real-world blockchains handle cryptocurrencies and decentralized applications, they also present complex computer science challenges. This project abstracts cryptographic complexities to focus entirely on core Operating System concepts: Process Management, Inter-Process Communication (IPC), Concurrency, and Synchronization.

## System Architecture and Components
The simulation replicates a basic blockchain ecosystem where transactions are created, grouped into blocks, mined (via a simplified Proof-of-Work), and validated. The system is composed of several independent processes and threads:

* **Controller:** The main process that reads the configuration, initializes IPC resources, and manages system components. It also handles dynamic scaling of the Validator processes based on system load.
* **Miner:** A multi-threaded process where each thread simulates a miner. Miners pull transactions from the pool, group them, and perform the PoW puzzle.
* **Validator:** An auto-scaling process (1 to 3 instances depending on pool occupancy) that verifies mined blocks and updates the ledger. 
* **Transaction Generator (TxGen):** Standalone processes executed by the user to produce transactions at specified intervals.
* **Statistics:** A process that computes and presents execution metrics upon receiving a specific signal or at termination.

## Inter-Process Communication (IPC)
To simulate the decentralized but interconnected nature of a blockchain, this project heavily relies on robust IPC mechanisms:

* **Shared Memory:**
  * `shm_transactionspool`: Stores pending transactions awaiting integration and validation.
  * `shm_ledger`: Stores the validated blocks that form the blockchain.
* **Named Pipes:** Used for transmitting mined blocks from the Miner threads to the Validator process.
* **Message Queues:** Used by the Validator to send block processing results and rewards to the Statistics process.

## Synchronization and Concurrency Control
Ensuring data integrity across multiple processes and threads required careful synchronization, primarily managed through Semaphores:

* `sem_transactions_pool` & `sem_ledger`: Guarantee mutual exclusion when accessing the shared memory segments.
* `sem_minerwork`: Synchronizes the presence of transactions with miner activity. It acts as an event trigger to wake up miners, preventing wasteful busy-waiting and saving CPU cycles.
* `sem_pipeclosed`: Ensures the named pipe closes gracefully by enforcing that the reading side (Validator) only closes after the writing side (Miner threads).
* `sem_log_file`: Ensures mutual exclusion when writing to the system log file.

## Key Technical Implementations

* **Memory Pointer Interfaces:** Because shared memory is mapped to different address spaces in different processes, storing raw pointers in shared memory is invalid. We developed an "interface" structure that maps blocks and transaction lists properly, allowing the ledger to be accessed almost like a standard array across any process.
* **Resource-Efficient Mining:** Miners avoid the `sleep()` function and busy-waiting. They rely on `sem_minerwork` to be notified of new transactions. To handle edge cases where generators stop, miners use a `timedwait()` to periodically check the pool, ensuring progress without wasting system resources.
* **Graceful Termination & Memory Management:** The miner process implements a dedicated thread for signal handling. To prevent memory leaks during `pthread_cancel()`, we utilized a custom data structure and `pthread_cleanup_push()`, ensuring all dynamically allocated memory is freed safely upon termination.
* **Data Serialization over Pipes:** Transmitting blocks over the named pipe initially resulted in byte misalignment. This was resolved by using the `#pragma pack(push, 1)` directive, guaranteeing struct alignment in memory and ensuring atomic, corruption-free reads and writes.
