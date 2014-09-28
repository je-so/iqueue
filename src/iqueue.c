/* iqueue.c

   Implements zero-copy, lock-free interthread message queue.

   Copyright:
   This program is free software. See accompanying LICENSE file.

   Author:
   (C) 2014 Jörg Seebohn
*/
#define _GNU_SOURCE
#include "iqueue.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

// === iqsignal0_t ===

static int init_mutex(/*out*/pthread_mutex_t* mutex)
{
   int err;
   pthread_mutexattr_t attr;

   err = pthread_mutexattr_init(&attr);
   if (err) return err;

   // prevents priority inversion
   err = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);

   if (! err) {
      err = pthread_mutex_init(mutex, &attr);
   }

   (void) pthread_mutexattr_destroy(&attr);
   return err;
}

static int init_cond(/*out*/pthread_cond_t* cond)
{
   int err;

   err = pthread_cond_init(cond, 0);

   return err;
}

int init_iqsignal0(/*out*/iqsignal0_t* signal)
{
   int err;

   err = init_mutex(&signal->lock);
   if (err) return err;

   err = init_cond(&signal->cond);
   if (err) {
      (void) pthread_mutex_destroy(&signal->lock);
   }

   signal->waitcount = 0;

   return err;
}

int free_iqsignal0(iqsignal0_t* signal)
{
   int err = pthread_mutex_destroy(&signal->lock);
   int err2 = pthread_cond_destroy(&signal->cond);

   if (err2) err = err2;

   return err;
}

// === iqsignal_t ===

int init_iqsignal(/*out*/iqsignal_t* signal)
{
   int err;

   err = init_iqsignal0(&signal->sign0);
   if (err) return err;

   signal->signalcount = 0;

   return err;
}

int free_iqsignal(iqsignal_t* signal)
{
   int err = free_iqsignal0(&signal->sign0);
   return err;
}

void wait_iqsignal(iqsignal_t * signal)
{
   pthread_mutex_lock(&signal->sign0.lock);

   if (! signal->signalcount) {
      ++ signal->sign0.waitcount;
      pthread_cond_wait(&signal->sign0.cond, &signal->sign0.lock);
      -- signal->sign0.waitcount;
   }

   pthread_mutex_unlock(&signal->sign0.lock);
}

void signal_iqsignal(iqsignal_t * signal)
{
   pthread_mutex_lock(&signal->sign0.lock);

   ++ signal->signalcount;
   pthread_cond_broadcast(&signal->sign0.cond);

   pthread_mutex_unlock(&signal->sign0.lock);
}

uint32_t clearsignal_iqsignal(iqsignal_t * signal)
{
   pthread_mutex_lock(&signal->sign0.lock);

   uint32_t oldval = signal->signalcount;
   signal->signalcount = 0;

   pthread_mutex_unlock(&signal->sign0.lock);

   return oldval;
}

// === iqueue_t ===

int new_iqueue(/*out*/iqueue_t** queue, uint16_t size)
{
   if (size == 0) {
      return EINVAL;
   }

   size_t queuesize = sizeof(iqueue_t) + (uint32_t)size * sizeof(void*);
   iqueue_t* allocated_queue = (iqueue_t*) malloc(queuesize);

   if (!allocated_queue) {
      return ENOMEM;
   }

   memset(allocated_queue, 0, queuesize);
   allocated_queue->size = size;

   int err;
   int initcount = 0;

   err = init_iqsignal0(&allocated_queue->reader);
   if (err) goto ONERR;
   initcount = 1;

   err = init_iqsignal0(&allocated_queue->writer);
   if (err) goto ONERR;
   // initcount = 2;

   *queue = allocated_queue;

   return 0; /*OK*/
ONERR:
   switch (initcount) {
   case 1: free_iqsignal0(&allocated_queue->reader);
   case 0: break;
   }
   free(allocated_queue);
   return err;
}

int delete_iqueue(iqueue_t** queue)
{
   int err = 0;
   int err2;

   if (*queue) {

      close_iqueue(*queue);

      err = free_iqsignal0(&(*queue)->writer);
      err2 = free_iqsignal0(&(*queue)->reader);
      if (err2) err = err2;

      free(*queue);

      *queue = 0;
   }

   return err;
}

void close_iqueue(iqueue_t* queue)
{
   pthread_mutex_lock(&queue->reader.lock);
   pthread_mutex_lock(&queue->writer.lock);
   queue->closed = 1;
   pthread_mutex_unlock(&queue->writer.lock);
   pthread_mutex_unlock(&queue->reader.lock);

   // Wait until reader/writer woken up
   for (;;) {
      pthread_mutex_lock(&queue->reader.lock);
      pthread_cond_broadcast(&queue->reader.cond);
      size_t isreader = queue->reader.waitcount;
      pthread_mutex_unlock(&queue->reader.lock);

      pthread_mutex_lock(&queue->writer.lock);
      pthread_cond_broadcast(&queue->writer.cond);
      size_t iswriter = queue->writer.waitcount;
      pthread_mutex_unlock(&queue->writer.lock);

      if (!isreader && !iswriter) break;

      sched_yield();
   }
}

static int trysend_nowakeup_iqueue(iqueue_t* queue, void* msg)
{
   if (msg == 0) {
      return EINVAL;
   }

   for (;;) {
      uint32_t oldval  = cmpxchg_atomicu32(&queue->next_and_nrofmsg, 0, 0);
      uint16_t next    = (uint16_t) (oldval >> 16);
      uint16_t nrofmsg = (uint16_t) oldval;
      unsigned pos = next + (unsigned) nrofmsg;
      if (pos >= queue->size) {
         pos -= queue->size;
      }

      if (queue->closed) {
         return EPIPE;
      }

      if (nrofmsg >= queue->size) {
         return EAGAIN;
      }

      ++ nrofmsg;
      uint32_t newval = ((uint32_t)next << 16) + nrofmsg;

      if (0 == cmpxchg_atomicptr(&queue->msg[pos], 0, msg)) {
         if (oldval == cmpxchg_atomicu32(&queue->next_and_nrofmsg, oldval, newval)) {
            break;
         }
         cmpxchg_atomicptr(&queue->msg[pos], msg, 0);
      }
   }

   return 0;
}

static int tryrecv_nowakeup_iqueue(iqueue_t* queue, /*out*/void** msg)
{
   if (msg == 0) {
      return EINVAL;
   }

   for (;;) {
      uint32_t oldval = cmpxchg_atomicu32(&queue->next_and_nrofmsg, 0, 0);
      uint16_t next    = (uint16_t) (oldval >> 16);
      uint16_t nrofmsg = (uint16_t) oldval;
      unsigned pos = next;

      if (queue->closed) {
         return EPIPE;
      }

      if (!nrofmsg) {
         return EAGAIN;
      }

      -- nrofmsg;
      ++ next;
      if (next >= queue->size) {
         next = 0;
      }
      uint32_t newval = ((uint32_t)next << 16) + nrofmsg;

      if (oldval == cmpxchg_atomicu32(&queue->next_and_nrofmsg, oldval, newval)) {
         void* fetchedmsg = queue->msg[pos];
         cmpxchg_atomicptr(&queue->msg[pos], fetchedmsg, 0);
         *msg = fetchedmsg;
         break;
      }
   }

   return 0;
}

#define WAKEUP_READER() \
   if (!err && queue->reader.waitcount) {        \
      pthread_mutex_lock(&queue->reader.lock);   \
      pthread_cond_signal(&queue->reader.cond);  \
      pthread_mutex_unlock(&queue->reader.lock); \
   }

#define WAKEUP_WRITER() \
   if (!err && queue->writer.waitcount) {        \
      pthread_mutex_lock(&queue->writer.lock);   \
      pthread_cond_signal(&queue->writer.cond);  \
      pthread_mutex_unlock(&queue->writer.lock); \
   }

int trysend_iqueue(iqueue_t* queue, void* msg)
{
   int err = trysend_nowakeup_iqueue(queue, msg);

   WAKEUP_READER();

   return err;
}

int tryrecv_iqueue(iqueue_t* queue, /*out*/void** msg)
{
   int err = tryrecv_nowakeup_iqueue(queue, msg);

   WAKEUP_WRITER();

   return err;
}

int send_iqueue(iqueue_t* queue, void* msg)
{
   int err = trysend_nowakeup_iqueue(queue, msg);

   if (EAGAIN == err) {
      pthread_mutex_lock(&queue->writer.lock);
      ++ queue->writer.waitcount;

      for (;;) {
         err = trysend_nowakeup_iqueue(queue, msg);
         if (EAGAIN != err) break;
         pthread_cond_wait(&queue->writer.cond, &queue->writer.lock);
      }

      -- queue->writer.waitcount;
      pthread_mutex_unlock(&queue->writer.lock);
   }

   WAKEUP_READER();

   return err;
}

int recv_iqueue(iqueue_t* queue, /*out*/void** msg)
{
   int err = tryrecv_nowakeup_iqueue(queue, msg);

   if (EAGAIN == err) {
      pthread_mutex_lock(&queue->reader.lock);
      ++ queue->reader.waitcount;

      for (;;) {
         err = tryrecv_nowakeup_iqueue(queue, msg);
         if (EAGAIN != err) break;
         pthread_cond_wait(&queue->reader.cond, &queue->reader.lock);
      }

      -- queue->reader.waitcount;
      pthread_mutex_unlock(&queue->reader.lock);
   }

   WAKEUP_WRITER();

   return err;
}
