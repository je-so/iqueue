#define _GNU_SOURCE
#include "iqueue.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
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

   signal->signalcount = 0;

   pthread_mutex_unlock(&signal->sign0.lock);
}

void signal_iqsignal(iqsignal_t * signal)
{
   pthread_mutex_lock(&signal->sign0.lock);

   ++ signal->signalcount;
   pthread_cond_broadcast(&signal->sign0.cond);

   pthread_mutex_unlock(&signal->sign0.lock);
}

size_t signalcount_iqsignal(iqsignal_t* signal)
{
   pthread_mutex_lock(&signal->sign0.lock);

   size_t signalcount = signal->signalcount;

   pthread_mutex_unlock(&signal->sign0.lock);

   return signalcount;
}

// === iqueue_t ===

int new_iqueue(/*out*/iqueue_t** queue, size_t size)
{
   if (size == 0 || size >= (SIZE_MAX - sizeof(iqueue_t)) / sizeof(void*)) {
      return EINVAL;
   }

   size_t queuesize = sizeof(iqueue_t) + size * sizeof(void*);
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
   __sync_val_compare_and_swap(&queue->closed, 0, 1);

   // Wait until reader/writer woken up
   while (  0 != __sync_val_compare_and_swap(&queue->reader.waitcount, 0, 0)
            || 0 != __sync_val_compare_and_swap(&queue->writer.waitcount, 0, 0)) {
      pthread_mutex_lock(&queue->reader.lock);
      pthread_cond_broadcast(&queue->reader.cond);
      pthread_mutex_unlock(&queue->reader.lock);

      pthread_mutex_lock(&queue->writer.lock);
      pthread_cond_broadcast(&queue->writer.cond);
      pthread_mutex_unlock(&queue->writer.lock);

      sched_yield();
   }
}

int trysend_iqueue(iqueue_t* queue, iqmsg_t* msg)
{
   if (msg == 0) {
      return EINVAL;
   }

   size_t newpos;
   size_t writepos = __sync_val_compare_and_swap(&queue->writepos, (size_t)0, (size_t)0);

   for (;;) {

      if (queue->closed) {
         return EPIPE;
      }

      newpos = writepos + 1;
      if (newpos >= queue->size) {
         newpos = 0;
      }

      if (0 == __sync_val_compare_and_swap(
                  &queue->msg[writepos],
                  /*old value*/ (void*)0,
                  /*new value*/ (void*)msg)) {
         break;
      }

      if (0 != queue->msg[newpos]) {
         return EAGAIN;
      }

      size_t oldval = __sync_val_compare_and_swap(&queue->writepos, writepos, newpos);
      writepos = (oldval == writepos) ? newpos : oldval;
   }

   __sync_val_compare_and_swap(&queue->writepos, writepos, newpos);

   if (queue->reader.waitcount) {
      // wake up reader
      pthread_mutex_lock(&queue->reader.lock);
      pthread_cond_signal(&queue->reader.cond);
      pthread_mutex_unlock(&queue->reader.lock);
   }

   return 0;
}

int send_iqueue(iqueue_t* queue, iqmsg_t* msg)
{
   int err = trysend_iqueue(queue, msg);

   while (err == EAGAIN) {
      __sync_fetch_and_add(&queue->writer.waitcount, 1);
      pthread_mutex_lock(&queue->writer.lock);

      err = trysend_iqueue(queue, msg);

      if (EAGAIN == err) {
         pthread_cond_wait(&queue->writer.cond, &queue->writer.lock);
      }

      pthread_mutex_unlock(&queue->writer.lock);
      __sync_fetch_and_sub(&queue->writer.waitcount, 1);

      if (EAGAIN == err) {
         err = trysend_iqueue(queue, msg);
      }
   }

   return err;
}

int tryrecv_iqueue(iqueue_t* queue, /*out*/iqmsg_t** msg)
{
   if (msg == 0) {
      return EINVAL;
   }

   size_t newpos;
   size_t readpos = __sync_val_compare_and_swap(&queue->readpos, (size_t)0, (size_t)0);

   for (;;) {

      if (queue->closed) {
         return EPIPE;
      }

      newpos = readpos + 1;
      if (newpos >= queue->size) {
         newpos = 0;
      }

      void* fetchedmsg = __sync_fetch_and_and(&queue->msg[readpos], 0);
      if (0 != fetchedmsg) {
         *msg = fetchedmsg;
         break;
      }

      if (0 == queue->msg[newpos]) {
         return EAGAIN;
      }

      size_t oldval = __sync_val_compare_and_swap(&queue->readpos, readpos, newpos);
      readpos = (oldval == readpos) ? newpos : oldval;
   }

   __sync_val_compare_and_swap(&queue->readpos, readpos, newpos);

   if (queue->writer.waitcount) {
      // wake up writer
      pthread_mutex_lock(&queue->writer.lock);
      pthread_cond_signal(&queue->writer.cond);
      pthread_mutex_unlock(&queue->writer.lock);
   }

   return 0;
}

int recv_iqueue(iqueue_t* queue, /*out*/iqmsg_t** msg)
{
   int err = tryrecv_iqueue(queue, msg);

   while (err == EAGAIN) {
      __sync_fetch_and_add(&queue->reader.waitcount, 1);
      pthread_mutex_lock(&queue->reader.lock);

      err = tryrecv_iqueue(queue, msg);

      if (EAGAIN == err) {
         pthread_cond_wait(&queue->reader.cond, &queue->reader.lock);
      }

      pthread_mutex_unlock(&queue->reader.lock);
      __sync_fetch_and_sub(&queue->reader.waitcount, 1);

      if (EAGAIN == err) {
         err = tryrecv_iqueue(queue, msg);
      }
   }

   return err;
}
