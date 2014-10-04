/* iqueue.h

   Defines interface of a zero-copy, lock-free interthread message queue.

   Every function returning an int indicates success with
   value 0. A returned value > 0 indicates an error code.

   Copyright:
   This program is free software. See accompanying LICENSE file.

   Author:
   (C) 2014 Jörg Seebohn
*/
#ifndef IQUEUE_H
#define IQUEUE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include "atomic.h"

typedef struct iqsignal0_t {
   pthread_mutex_t lock;
   pthread_cond_t  cond;
   size_t waitcount;
} iqsignal0_t;

typedef struct iqsignal_t {
   iqsignal0_t sign0;
   size_t signalcount;
} iqsignal_t;

typedef struct iqmsg_t {
   iqsignal_t* signal;
   uint32_t processed;
} iqmsg_t;

// Supports multi reader / multi writer
typedef struct iqueue_t {
   uint16_t capacity;
   uint32_t next_size;
   uint32_t closed;
   iqsignal0_t reader;
   iqsignal0_t writer;
   void*   msg[/*capacity*/];
} iqueue_t;

// Supports single reader / single writer
typedef struct iqueue1_t {
   uint16_t capacity;
   uint16_t readpos;
   uint16_t writepos;
   uint32_t closed;
   iqsignal0_t reader;
   iqsignal0_t writer;
   void*   msg[/*capacity*/];
} iqueue1_t;

// === iqueue_t ===

// Initializes queue
// Possible error codes: EINVAL (capacity == 0) or ENOMEM
int new_iqueue(/*out*/iqueue_t** queue, uint16_t capacity);

// Frees all resources of queue. Close is called automatically.
int delete_iqueue(iqueue_t** queue);

// Marks queue as closed and wakes up any waiting reader/writer.
// Blocks until all read/writer has left queue.
void close_iqueue(iqueue_t* queue);

// Stores msg in queue. EAGAIN is returned if queue is full.
// EPIPE is returned if queue is closed.
int trysend_iqueue(iqueue_t* queue, void* msg);

// Stores msg in queue. Blocks if queue is full.
// EPIPE is returned if queue is closed.
int send_iqueue(iqueue_t* queue, void* msg);

// Receives msg from queue. EAGAIN is returned if queue is empty.
// EPIPE is returned if queue is closed.
int tryrecv_iqueue(iqueue_t* queue, /*out*/void** msg);

// Receives msg from queue. Blocks if queue is empty.
// EPIPE is returned if queue is closed.
int recv_iqueue(iqueue_t* queue, /*out*/void** msg);

// Returns maximum number of storable messages.
static inline uint16_t capacity_iqueue(const iqueue_t* queue)
{
         return queue->capacity;
}

// Returns number of stored (unread) messages.
static inline uint16_t size_iqueue(const iqueue_t* queue)
{
         return (uint16_t) cmpxchg_atomicu32((uint32_t*)(uintptr_t)&queue->next_size, 0, 0);
}

// === iqsignal_t ===

// Initializes new signal synchronization facility.
int init_iqsignal(/*out*/iqsignal_t* signal);

// Frees resources associated with signal. Make sure that there are no more waiting threads else undefined behaviour.
int free_iqsignal(iqsignal_t* signal);

// Waits until signalcount_iqsignal(signal) returns != 0.
void wait_iqsignal(iqsignal_t* signal);

// Clears signalcount to 0 and returns previous value
uint32_t clearsignal_iqsignal(iqsignal_t * signal);

// Increments signalcount by one and wakes up all threads waiting with wait_iqsignal(signal).
void signal_iqsignal(iqsignal_t* signal);

// Returns the how many times signal_iqsignal(signal) was called (Nr of processed messages).
static inline size_t signalcount_iqsignal(iqsignal_t* signal)
{
         return cmpxchg_atomicsize(&signal->signalcount, 0, 0);
}

// === iqmsg_t ===

// Static initializer for iqmsg_t. Parameter signal is used by a receiver to signal the message as processed.
// If you do not want to receive signals set this value to 0.
#define iqmsg_INIT(signal) \
         { signal, 0 }

// Initializer for msg. Parameter signal is used by a receiver to signal the message as processed.
// If you do not want to receive signals set this value to 0.
static inline void init_iqmsg(iqmsg_t* msg, iqsignal_t* signal)
{
         msg->signal = signal;
         msg->processed = 0;
}

static inline uint32_t isprocessed_iqmsg(iqmsg_t* msg)
{
         return cmpxchg_atomicu32(&msg->processed, 0, 0);
}

static inline void setprocessed_iqmsg(iqmsg_t* msg)
{
         cmpxchg_atomicu32(&msg->processed, 0, 1);
         if (msg->signal) {
            signal_iqsignal(msg->signal);
         }
}

// === iqueue1_t ===

#define static_assert(C,S) \
         ((void)(sizeof(char[(C)?1:-1])))

// Initializes queue
// Possible error codes: EINVAL (capacity == 0) or ENOMEM
static inline int new_iqueue1(/*out*/iqueue1_t** queue, uint16_t capacity)
{
         iqueue_t* tmp;
         int err = new_iqueue(&tmp, capacity);
         static_assert( offsetof(iqueue1_t, closed) == offsetof(iqueue_t, closed),
                        "iqueue_t can be cast to iqueue1_t");
         *queue = (iqueue1_t*) tmp;
         return err;
}

// Frees all resources of queue. Close is called automatically.
static inline int delete_iqueue1(iqueue1_t** queue)
{
         iqueue_t* tmp = (iqueue_t*) *queue;
         int err = delete_iqueue(&tmp);
         *queue = 0;
         return err;
}

// Marks queue as closed and wakes up any waiting reader/writer.
// Blocks until all read/writer has left queue.
static inline void close_iqueue1(iqueue1_t* queue)
{
         close_iqueue((iqueue_t*) queue);
}

// Stores msg in queue. EAGAIN is returned if queue is full.
// EPIPE is returned if queue is closed.
int trysend_iqueue1(iqueue1_t* queue, void* msg);

// Stores msg in queue. Blocks if queue is full.
// EPIPE is returned if queue is closed.
int send_iqueue1(iqueue1_t* queue, void* msg);

// Receives msg from queue. EAGAIN is returned if queue is empty.
// EPIPE is returned if queue is closed.
int tryrecv_iqueue1(iqueue1_t* queue, /*out*/void** msg);

// Receives msg from queue. Blocks if queue is empty.
// EPIPE is returned if queue is closed.
int recv_iqueue1(iqueue1_t* queue, /*out*/void** msg);

// Returns maximum number of storable messages.
static inline uint16_t capacity_iqueue1(const iqueue1_t* queue)
{
         return queue->capacity;
}

// Returns number of stored (unread) messages.
uint16_t size_iqueue1(const iqueue1_t* queue);

// === support for statically typed queues ===

/* Declares/implements queue type affix##_t and whole iqueue interface
 * where msg type (void*) is replaced with type (msg_t*).
 * Use init_##affix / free_##affix to initialize/free queue. */
#define iqueue_DECLARE(affix, msg_t) \
         typedef struct affix ##_t { \
            iqueue_t* queue;          \
         } affix ##_t;               \
         static inline int init_##affix(affix##_t* queue, uint16_t capacity) \
         { \
            return new_iqueue(&queue->queue, capacity); \
         } \
         static inline int free_##affix(affix##_t* queue) \
         { \
            return delete_iqueue(&queue->queue); \
         } \
         static inline void close_##affix(affix##_t* queue) \
         { \
            close_iqueue(queue->queue); \
         } \
         static inline int trysend_##affix(affix##_t* queue, msg_t* msg) \
         { \
            return trysend_iqueue(queue->queue, msg); \
         } \
         static inline int send_##affix(affix##_t* queue, msg_t* msg) \
         { \
            return send_iqueue(queue->queue, msg); \
         } \
         static inline int tryrecv_##affix(affix##_t* queue, msg_t** msg) \
         { \
            void* tmp; \
            int err = tryrecv_iqueue(queue->queue, &tmp); \
            *msg = tmp; \
            return err; \
         } \
         static inline int recv_##affix(affix##_t* queue, msg_t** msg) \
         { \
            void* tmp; \
            int err = recv_iqueue(queue->queue, &tmp); \
            *msg = tmp; \
            return err; \
         }

#endif
