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

// === iqsignal_t ===

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

int init_iqsignal(/*out*/iqsignal_t* signal)
{
   int err;

   err = init_mutex(&signal->lock);
   if (err) return err;

   err = init_cond(&signal->cond);
   if (err) {
      (void) pthread_mutex_destroy(&signal->lock);
      return err;
   }

   signal->waitcount = 0;
   signal->signalcount = 0;

   return 0;
}

int free_iqsignal(iqsignal_t* signal)
{
   int err = pthread_mutex_destroy(&signal->lock);
   int err2 = pthread_cond_destroy(&signal->cond);

   if (err2) err = err2;

   return err;
}

void wait_iqsignal(iqsignal_t* signal)
{
   pthread_mutex_lock(&signal->lock);

   if (! signal->signalcount) {
      ++ signal->waitcount;
      pthread_cond_wait(&signal->cond, &signal->lock);
      -- signal->waitcount;
   }

   pthread_mutex_unlock(&signal->lock);
}

void signal_iqsignal(iqsignal_t* signal)
{
   pthread_mutex_lock(&signal->lock);

   ++ signal->signalcount;
   pthread_cond_broadcast(&signal->cond);

   pthread_mutex_unlock(&signal->lock);
}

uint32_t clearsignal_iqsignal(iqsignal_t* signal)
{
   uint32_t oldval;

   pthread_mutex_lock(&signal->lock);
   oldval = signal->signalcount;
   signal->signalcount = 0;
   pthread_mutex_unlock(&signal->lock);

   return oldval;
}

size_t signalcount_iqsignal(iqsignal_t* signal)
{
   size_t signalcount;

   pthread_mutex_lock(&signal->lock);
   signalcount = signal->signalcount;
   pthread_mutex_unlock(&signal->lock);

   return signalcount;
}

// === iqueue_t ===

// length of iqueue_t:sizeused / iqueue_t:sizefree
#define NROFSIZE ((int)(sizeof(((iqueue_t*)0)->sizeused)/sizeof(((iqueue_t*)0)->sizeused[0])))

int new_iqueue(/*out*/iqueue_t** queue, uint32_t capacity)
{
   uint32_t isNOTpowerof2 = (capacity & (capacity-1));
   uint32_t aligned_capacity = capacity < NROFSIZE || isNOTpowerof2 ? NROFSIZE : capacity/2;

   while (aligned_capacity < capacity) {
      aligned_capacity <<= 1;
      if (aligned_capacity >= ((size_t)-1 - sizeof(iqueue_t)) / sizeof(void*)) {
         return EINVAL;
      }
   }

   size_t queuesize = sizeof(iqueue_t) + aligned_capacity * sizeof(void*);
   iqueue_t* allocated_queue = (iqueue_t*) malloc(queuesize);

   if (!allocated_queue) {
      return ENOMEM;
   }

   memset(allocated_queue, 0, queuesize);
   allocated_queue->capacity = aligned_capacity;
   for (int i = 0; i < NROFSIZE; ++i) {
      allocated_queue->sizefree[i] = aligned_capacity / NROFSIZE;
   }

   int err;
   int initcount = 0;

   err = init_iqsignal(&allocated_queue->reader);
   if (err) goto ONERR;
   initcount = 1;

   err = init_iqsignal(&allocated_queue->writer);
   if (err) goto ONERR;
   // initcount = 2;

   *queue = allocated_queue;

   return 0; /*OK*/
ONERR:
   switch (initcount) {
   case 1: free_iqsignal(&allocated_queue->reader);
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

      err = free_iqsignal(&(*queue)->writer);
      err2 = free_iqsignal(&(*queue)->reader);
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

int trysend_iqueue(iqueue_t* queue, void* msg)
{
   uint32_t ifree;

   if (0 == msg) {
      return EINVAL;
   }

   for (int i = 0;; ++i) {
      ifree = queue->ifree;
      if (queue->closed) return EPIPE;
      uint32_t sizefree = fetchadd_atomicu32(&queue->sizefree[ifree], (uint32_t)-1) - 1;
      if (sizefree < queue->capacity) break;
      fetchadd_atomicu32(&queue->sizefree[ifree], 1);
      cmpxchg_atomicu32(&queue->ifree, ifree, (ifree+1) & (NROFSIZE-1));
      if (i == NROFSIZE-1) return EAGAIN;
   }

   uint32_t pos = fetchadd_atomicu32(&queue->writepos, 1);
   pos &= (queue->capacity-1);

   while (0 != cmpxchg_atomicptr(&queue->msg[pos], 0, msg)) ;

   fetchadd_atomicu32(&queue->sizeused[ifree], 1);

   return 0;
}

int tryrecv_iqueue(iqueue_t* queue, /*out*/void** msg)
{
   uint32_t iused;

   for (int i = 0;; ++i) {
      iused = queue->iused;
      if (queue->closed) return EPIPE;
      uint32_t sizeused = fetchadd_atomicu32(&queue->sizeused[iused], (uint32_t)-1) - 1;
      if (sizeused < queue->capacity) break;
      fetchadd_atomicu32(&queue->sizeused[iused], 1);
      cmpxchg_atomicu32(&queue->iused, iused, (iused+1) & (NROFSIZE-1));
      if (i == NROFSIZE-1) return EAGAIN;
   }

   uint32_t pos = fetchadd_atomicu32(&queue->readpos, 1);
   pos &= (queue->capacity-1);

   void* fetchedmsg;
   do {
      fetchedmsg = queue->msg[pos];
   } while (fetchedmsg != cmpxchg_atomicptr(&queue->msg[pos], fetchedmsg, 0) || 0 == fetchedmsg);

   *msg = fetchedmsg;

   fetchadd_atomicu32(&queue->sizefree[iused], 1);

   return 0;
}

#define WAKEUP_READER() \
   if (!err && queue->reader.signalcount) {      \
      pthread_mutex_lock(&queue->reader.lock);   \
      if (queue->reader.signalcount) {           \
         --queue->reader.signalcount;            \
         pthread_cond_signal(&queue->reader.cond); \
      }                                          \
      pthread_mutex_unlock(&queue->reader.lock); \
   }

#define WAKEUP_WRITER() \
   if (!err && queue->writer.signalcount) {      \
      pthread_mutex_lock(&queue->writer.lock);   \
      if (queue->writer.signalcount) {           \
         --queue->writer.signalcount;            \
         pthread_cond_signal(&queue->writer.cond); \
      }                                          \
      pthread_mutex_unlock(&queue->writer.lock); \
   }

int send_iqueue(iqueue_t* queue, void* msg)
{
   int err = trysend_iqueue(queue, msg);

   WAKEUP_READER();

   if (EAGAIN == err) {
      pthread_mutex_lock(&queue->writer.lock);
      ++ queue->writer.waitcount;

      for (;;) {
         err = trysend_iqueue(queue, msg);
         if (EAGAIN != err) break;
         ++ queue->writer.signalcount;
         pthread_cond_wait(&queue->writer.cond, &queue->writer.lock);
      }

      -- queue->writer.waitcount;
      pthread_mutex_unlock(&queue->writer.lock);
   }

   return err;
}

int recv_iqueue(iqueue_t* queue, /*out*/void** msg)
{
   int err = tryrecv_iqueue(queue, msg);

   WAKEUP_WRITER();

   if (EAGAIN == err) {
      pthread_mutex_lock(&queue->reader.lock);
      ++ queue->reader.waitcount;

      for (;;) {
         err = tryrecv_iqueue(queue, msg);
         if (EAGAIN != err) break;
         ++ queue->reader.signalcount;
         pthread_cond_wait(&queue->reader.cond, &queue->reader.lock);
      }

      -- queue->reader.waitcount;
      pthread_mutex_unlock(&queue->reader.lock);
   }

   return err;
}

uint32_t size_iqueue(const iqueue_t* queue)
{
   uint32_t size = 0;
   for (int i = 0; i < NROFSIZE; ++i) {
      uint32_t sizeused = cmpxchg_atomicu32((uint32_t*)(uintptr_t)&queue->sizeused[i], 0, 0);
      size += (sizeused < queue->capacity ? sizeused : 0);
   }
   return size <= queue->capacity ? size : queue->capacity;
}

// === iqueue1_t ===

int new_iqueue1(/*out*/iqueue1_t** queue, uint32_t capacity)
{
   if (capacity == 0 || ((size_t)-1 - sizeof(iqueue1_t))/sizeof(void*) <= capacity) {
      return EINVAL;
   }

   size_t queuesize = sizeof(iqueue1_t) + capacity * sizeof(void*);
   iqueue1_t* allocated_queue = (iqueue1_t*) malloc(queuesize);

   if (!allocated_queue) {
      return ENOMEM;
   }

   memset(allocated_queue, 0, queuesize);
   allocated_queue->capacity = capacity;

   int err;
   int initcount = 0;

   err = init_iqsignal(&allocated_queue->reader);
   if (err) goto ONERR;
   initcount = 1;

   err = init_iqsignal(&allocated_queue->writer);
   if (err) goto ONERR;
   // initcount = 2;

   *queue = allocated_queue;

   return 0; /*OK*/
ONERR:
   switch (initcount) {
   case 1: free_iqsignal(&allocated_queue->reader);
   case 0: break;
   }
   free(allocated_queue);
   return err;
}

int delete_iqueue1(iqueue1_t** queue)
{
   int err = 0;
   int err2;

   if (*queue) {

      close_iqueue1(*queue);

      err = free_iqsignal(&(*queue)->writer);
      err2 = free_iqsignal(&(*queue)->reader);
      if (err2) err = err2;

      free(*queue);

      *queue = 0;
   }

   return err;
}

void close_iqueue1(iqueue1_t* queue)
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

int trysend_iqueue1(iqueue1_t* queue, void* msg)
{
   if (0 == msg) {
      return EINVAL;
   }

   if (queue->closed) {
      return EPIPE;
   }

   uint32_t pos = queue->writepos;
   uint32_t oldpos = pos;
   ++pos;
   if (pos >= queue->capacity) {
      pos = 0;
   }
   queue->writepos = pos;

   if (0 != cmpxchg_atomicptr(&queue->msg[oldpos], 0, msg)) {
      queue->writepos = oldpos;
      return EAGAIN;
   }

   return 0;
}

int tryrecv_iqueue1(iqueue1_t* queue, /*out*/void** msg)
{
   if (queue->closed) {
      return EPIPE;
   }

   uint32_t pos = queue->readpos;
   uint32_t oldpos = pos;
   ++pos;
   if (pos >= queue->capacity) {
      pos = 0;
   }
   queue->readpos = pos;

   void* fetchedmsg = queue->msg[oldpos];

   if (fetchedmsg != cmpxchg_atomicptr(&queue->msg[oldpos], fetchedmsg, 0) || 0 == fetchedmsg) {
      queue->readpos = oldpos;
      return EAGAIN;
   }

   *msg = fetchedmsg;

   return 0;
}

int send_iqueue1(iqueue1_t* queue, void* msg)
{
   int err = trysend_iqueue1(queue, msg);

   WAKEUP_READER();

   if (EAGAIN == err) {
      pthread_mutex_lock(&queue->writer.lock);
      ++ queue->writer.waitcount;

      for (;;) {
         err = trysend_iqueue1(queue, msg);
         if (EAGAIN != err) break;
         ++ queue->writer.signalcount;
         pthread_cond_wait(&queue->writer.cond, &queue->writer.lock);
      }

      -- queue->writer.waitcount;
      pthread_mutex_unlock(&queue->writer.lock);
   }

   return err;
}

int recv_iqueue1(iqueue1_t* queue, /*out*/void** msg)
{
   int err = tryrecv_iqueue1(queue, msg);

   WAKEUP_WRITER();

   if (EAGAIN == err) {
      pthread_mutex_lock(&queue->reader.lock);
      ++ queue->reader.waitcount;

      for (;;) {
         err = tryrecv_iqueue1(queue, msg);
         if (EAGAIN != err) break;
         ++ queue->reader.signalcount;
         pthread_cond_wait(&queue->reader.cond, &queue->reader.lock);
      }

      -- queue->reader.waitcount;
      pthread_mutex_unlock(&queue->reader.lock);
   }

   return err;
}

uint32_t size_iqueue1(const iqueue1_t* queue)
{
   uint32_t rpos = cmpxchg_atomicu32((uint32_t*)(uintptr_t)&queue->readpos, 0, 0);
   uint32_t wpos = cmpxchg_atomicu32((uint32_t*)(uintptr_t)&queue->writepos, 0, 0);

   if (rpos < wpos) {
      return (wpos - rpos);

   } else {
      uint32_t free = (rpos - wpos);
      return (free == 0 && 0 == queue->msg[wpos == 0?queue->capacity-1:wpos-1])
             ? 0
             : (queue->capacity - free);
   }
}
