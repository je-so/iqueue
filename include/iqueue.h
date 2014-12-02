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

// defines the size of the cache line
// if this value is undefined cache line alignment is turned off !
#define SIZE_CACHELINE 64

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include "atomic.h"

// defines padding function which prevents false sharing of variables
// http://en.wikipedia.org/wiki/False_sharing
#ifdef SIZE_CACHELINE
#   define PAD(_NR, _NROFBYTES_LESS) uint8_t _align ## _NR [(SIZE_CACHELINE) - (_NROFBYTES_LESS)];
#else
#   define PAD(_NR, _NROFBYTES_LESS) 
#endif

typedef struct iqsignal_t {
   pthread_mutex_t lock;
   pthread_cond_t  cond;
   size_t waitcount;
   size_t signalcount;
} iqsignal_t;

// Supports multi reader / multi writer
typedef struct iqueue_t {
   uint32_t closed;
   uint32_t capacity;
   PAD(0, 2*sizeof(uint32_t))
   uint32_t iused; // index into sizeused
   PAD(1, sizeof(uint32_t))
   uint32_t readpos;
   PAD(2, sizeof(uint32_t))
   uint32_t ifree; // index into sizefree
   PAD(3, sizeof(uint32_t))
   uint32_t writepos;
   PAD(4, sizeof(uint32_t))
   uint32_t sizeused[256/*must be power of two*/];
   uint32_t sizefree[256/*same size as sizeused*/];
   iqsignal_t reader;
   iqsignal_t writer;
   void*    msg[/*capacity*/];
} iqueue_t;

// Supports single reader / single writer
typedef struct iqueue1_t {
   uint32_t closed;
   uint32_t capacity;
   PAD(0, 2*sizeof(uint32_t))
   uint32_t readpos;
   PAD(1, sizeof(uint32_t))
   uint32_t writepos;
   iqsignal_t reader;
   iqsignal_t writer;
   void*   msg[/*capacity*/];
} iqueue1_t;

// === iqueue_t ===

// Initializes queue
// Possible error codes: EINVAL (capacity too big) or ENOMEM
int new_iqueue(/*out*/iqueue_t** queue, uint32_t capacity);

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
// Waiting readers are woken up.
int send_iqueue(iqueue_t* queue, void* msg);

// Receives msg from queue. EAGAIN is returned if queue is empty.
// EPIPE is returned if queue is closed.
int tryrecv_iqueue(iqueue_t* queue, /*out*/void** msg);

// Receives msg from queue. Blocks if queue is empty.
// EPIPE is returned if queue is closed.
// Waiting writers are woken up.
int recv_iqueue(iqueue_t* queue, /*out*/void** msg);

// Returns maximum number of storable messages.
static inline uint32_t capacity_iqueue(const iqueue_t* queue)
{
         return queue->capacity;
}

// Returns number of stored (unread) messages.
uint32_t size_iqueue(const iqueue_t* queue);

// === iqsignal_t ===

// Initializes new signal synchronization facility.
int init_iqsignal(/*out*/iqsignal_t* signal);

// Frees resources associated with signal. Make sure that there are no more waiting threads else undefined behaviour.
int free_iqsignal(iqsignal_t* signal);

// Waits until signalcount_iqsignal(signal) returns != 0.
void wait_iqsignal(iqsignal_t* signal);

// Clears signalcount to 0 and returns previous value
uint32_t clearsignal_iqsignal(iqsignal_t* signal);

// Increments signalcount by one and wakes up all threads waiting with wait_iqsignal(signal).
void signal_iqsignal(iqsignal_t* signal);

// Returns the how many times signal_iqsignal(signal) was called (Nr of processed messages).
size_t signalcount_iqsignal(iqsignal_t* signal);

// === iqueue1_t ===

// Initializes queue
// Possible error codes: EINVAL (capacity == 0) or ENOMEM
int new_iqueue1(/*out*/iqueue1_t** queue, uint32_t capacity);

// Frees all resources of queue. Close is called automatically.
int delete_iqueue1(iqueue1_t** queue);

// Marks queue as closed and wakes up any waiting reader/writer.
// Blocks until all read/writer has left queue.
void close_iqueue1(iqueue1_t* queue);

// Stores msg in queue. EAGAIN is returned if queue is full.
// EPIPE is returned if queue is closed.
int trysend_iqueue1(iqueue1_t* queue, void* msg);

// Stores msg in queue. Blocks if queue is full.
// EPIPE is returned if queue is closed.
// A waiting reader are woken up.
int send_iqueue1(iqueue1_t* queue, void* msg);

// Receives msg from queue. EAGAIN is returned if queue is empty.
// EPIPE is returned if queue is closed.
int tryrecv_iqueue1(iqueue1_t* queue, /*out*/void** msg);

// Receives msg from queue. Blocks if queue is empty.
// EPIPE is returned if queue is closed.
// A waiting writer are woken up.
int recv_iqueue1(iqueue1_t* queue, /*out*/void** msg);

// Returns maximum number of storable messages.
static inline uint32_t capacity_iqueue1(const iqueue1_t* queue)
{
         return queue->capacity;
}

uint32_t size_iqueue1(const iqueue1_t* queue);

// === support for statically typed queues ===

/* Declares/implements queue type affix##_t and whole iqueue interface
 * where msg type (void*) is replaced with type (msg_t*).
 * Use init_##affix / free_##affix to initialize/free queue. */
#define iqueue_DECLARE(affix, msg_t) \
         typedef struct affix ##_t { \
            iqueue_t* queue;          \
         } affix ##_t;               \
         static inline int init_##affix(affix##_t* queue, uint32_t capacity) \
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

#undef PAD

#endif
