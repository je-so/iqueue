/* iqueue.h

   Defines interface of a lock-free interthread message queue.

   Every function returning an int indicates success with
   value 0. A returned value > 0 indicates an error code.

   Copyright:
   This program is free software. See accompanying LICENSE file.

   Author:
   (C) 2014 Jörg Seebohn
*/
#ifndef IQUEUE_HEADER
#define IQUEUE_HEADER

#include <pthread.h>

typedef struct iqsignal_t {
   pthread_mutex_t lock;
   pthread_cond_t  cond;
   size_t signalcount;
} iqsignal_t;

typedef struct iqmsg_t {
   iqsignal_t * signal;
   int processed;
} iqmsg_t;

typedef struct iqueue_t {
   size_t  size;
   size_t  readpos;
   size_t  writepos;
   int     waitreader;
   int     waitwriter;
   int     closed;
   pthread_mutex_t readlock;
   pthread_mutex_t writelock;
   pthread_cond_t  readcond;
   pthread_cond_t  writecond;
   void*   msg[/*size*/];
} iqueue_t;

// === iqueue_t ===

// Initializes queue
// Possible error codes: ENOMEM
int new_iqueue(/*out*/iqueue_t ** queue, size_t size);

// Frees all resources of queue. Close is called automatically.
int delete_iqueue(iqueue_t ** queue);

// Marks queue as closed and wakes up any waiting reader/writer.
// Blocks until all read/writer has left queue.
void close_iqueue(iqueue_t * queue);

// Stores msg in queue. EAGAIN is returned if queue is full.
// EPIPE is returned if queue is closed.
int trysend_iqueue(iqueue_t * queue, iqmsg_t * msg);

// Stores msg in queue. Blocks if queue is full.
// EPIPE is returned if queue is closed.
int send_iqueue(iqueue_t * queue, iqmsg_t * msg);

// Receives msg from queue. EAGAIN is returned if queue is empty.
// EPIPE is returned if queue is closed.
int tryrecv_iqueue(iqueue_t * queue, /*out*/iqmsg_t ** msg);

// Receives msg from queue. Blocks if queue is empty.
// EPIPE is returned if queue is closed.
int recv_iqueue(iqueue_t * queue, /*out*/iqmsg_t ** msg);

// === iqsignal_t ===

// Initializes new signal synchronization facility.
int init_iqsignal(/*out*/iqsignal_t * signal);

// Frees resources associated with signal. Make sure that there are no more waiting threads else undefined behaviour.
int free_iqsignal(iqsignal_t * signal);

// Waits until signalcount_iqsignal(signal) returns != 0.
// Clears signalcount before return.
void wait_iqsignal(iqsignal_t * signal);

// Increments signalcount by one and wakes up all threads waiting with wait_iqsignal(signal).
void signal_iqsignal(iqsignal_t * signal);

// Returns the how many times signal_iqsignal(signal) was called (Nr of processed messages).
size_t signalcount_iqsignal(iqsignal_t * signal);

// === iqmsg_t ===

// Static initializer for iqmsg_t. Parameter signal is used by a receiver to signal the message as processed.
// If you do not want to receive signals set this value to 0.
#define iqmsg_INIT(signal) \
         { signal, 0 }

// Initializer for msg. Parameter signal is used by a receiver to signal the message as processed.
// If you do not want to receive signals set this value to 0.
static inline void init_iqmsg(iqmsg_t * msg, iqsignal_t * signal)
{
         msg->signal = signal;
         msg->processed = 0;
}

static inline int isprocessed_iqmsg(iqmsg_t * msg)
{
         return __sync_val_compare_and_swap(&msg->processed, 0, 0);
}

static inline void setprocessed_iqmsg(iqmsg_t * msg)
{
         __sync_val_compare_and_swap(&msg->processed, 0, 1);
         if (msg->signal) {
            signal_iqsignal(msg->signal);
         }
}

#endif
