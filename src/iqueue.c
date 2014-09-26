#define _GNU_SOURCE
#include "iqueue.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int init_mutex(/*out*/pthread_mutex_t * mutex)
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

static int init_cond(/*out*/pthread_cond_t * cond)
{
   int err;

   err = pthread_cond_init(cond, 0);

   return err;
}

int init_iqsignal(/*out*/iqsignal_t * signal)
{
   int err;

   err = init_mutex(&signal->lock);
   if (err) return err;

   err = init_cond(&signal->cond);
   if (err) {
      (void) pthread_mutex_destroy(&signal->lock);
   }

   return err;
}

int free_iqsignal(iqsignal_t * signal)
{
   int err = pthread_mutex_destroy(&signal->lock);
   int err2 = pthread_cond_destroy(&signal->cond);

   if (err2) err = err2;

   return err;
}

void wait_iqsignal(iqsignal_t * signal)
{
   pthread_mutex_lock(&signal->lock);

   if (! signal->signaled) {
      pthread_cond_wait(&signal->cond, &signal->lock);
   }

   signal->signaled = 0;

   pthread_mutex_unlock(&signal->lock);
}

void signal_iqsignal(iqsignal_t * signal)
{
   pthread_mutex_lock(&signal->lock);

   signal->signaled = 1;
   pthread_cond_signal(&signal->cond);

   pthread_mutex_unlock(&signal->lock);
}

int issignaled_iqsignal(iqsignal_t * signal)
{
   pthread_mutex_lock(&signal->lock);

   int issignaled = signal->signaled;

   pthread_mutex_unlock(&signal->lock);

   return issignaled;
}


int new_iqueue(/*out*/iqueue_t ** queue, size_t size)
{
   if (size == 0 || size >= (SIZE_MAX - sizeof(iqueue_t)) / sizeof(void*)) {
      return EINVAL;
   }

   size_t queuesize = sizeof(iqueue_t) + size * sizeof(void*);
   iqueue_t * allocated_queue = (iqueue_t*) malloc(queuesize);

   if (!allocated_queue) {
      return ENOMEM;
   }

   memset(allocated_queue, 0, queuesize);
   allocated_queue->size = size;

   int err;
   int initcount = 0;

   err = init_mutex(&allocated_queue->readlock);
   if (err) goto ONERR;
   initcount = 1;

   err = init_mutex(&allocated_queue->writelock);
   if (err) goto ONERR;
   initcount = 2;

   err = init_cond(&allocated_queue->readcond);
   if (err) goto ONERR;
   initcount = 3;

   err = init_cond(&allocated_queue->writecond);
   if (err) goto ONERR;
   // initcount = 4;

   *queue = allocated_queue;

   return 0; /*OK*/
ONERR:
   switch (initcount) {
   case 3: pthread_cond_destroy(&allocated_queue->readcond);
   case 2: pthread_mutex_destroy(&allocated_queue->writelock);
   case 1: pthread_mutex_destroy(&allocated_queue->readlock);
   case 0: break;
   }
   free(allocated_queue);
   return err;
}

int delete_iqueue(iqueue_t ** queue)
{
   int err = 0;
   int err2;

   if (*queue) {

      close_iqueue(*queue);

      err = pthread_cond_destroy(&(*queue)->writecond);
      err2 = pthread_cond_destroy(&(*queue)->readcond);
      if (err2) err = err2;
      err2 = pthread_mutex_destroy(&(*queue)->readlock);
      if (err2) err = err2;
      err2 = pthread_mutex_destroy(&(*queue)->writelock);
      if (err2) err = err2;

      free(*queue);

      *queue = 0;
   }

   return err;
}

void close_iqueue(iqueue_t * queue)
{
   __sync_val_compare_and_swap(&queue->closed, 0, 1);

   // Wait until reader/writer woken up
   while (  0 != __sync_val_compare_and_swap(&queue->waitreader, 0, 0)
            || 0 != __sync_val_compare_and_swap(&queue->waitwriter, 0, 0)) {
      pthread_mutex_lock(&queue->readlock);
      pthread_cond_broadcast(&queue->readcond);
      pthread_mutex_unlock(&queue->readlock);

      pthread_mutex_lock(&queue->writelock);
      pthread_cond_broadcast(&queue->writecond);
      pthread_mutex_unlock(&queue->writelock);

      sched_yield();
   }
}

int trysend_iqueue(iqueue_t * queue, iqmsg_t * msg)
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

   if (queue->waitreader) {
      // wake up reader
      pthread_mutex_lock(&queue->readlock);
      pthread_cond_signal(&queue->readcond);
      pthread_mutex_unlock(&queue->readlock);
   }

   return 0;
}

int send_iqueue(iqueue_t * queue, iqmsg_t * msg)
{
   int err = trysend_iqueue(queue, msg);

   while (err == EAGAIN) {
      __sync_fetch_and_add(&queue->waitwriter, 1);
      pthread_mutex_lock(&queue->writelock);

      err = trysend_iqueue(queue, msg);

      if (EAGAIN == err) {
         pthread_cond_wait(&queue->writecond, &queue->writelock);
      }

      pthread_mutex_unlock(&queue->writelock);
      __sync_fetch_and_sub(&queue->waitwriter, 1);

      if (EAGAIN == err) {
         err = trysend_iqueue(queue, msg);
      }
   }

   return err;
}

int tryrecv_iqueue(iqueue_t * queue, /*out*/iqmsg_t ** msg)
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

      void * fetchedmsg = __sync_fetch_and_and(&queue->msg[readpos], 0);
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

   if (queue->waitwriter) {
      // wake up writer
      pthread_mutex_lock(&queue->writelock);
      pthread_cond_signal(&queue->writecond);
      pthread_mutex_unlock(&queue->writelock);
   }

   return 0;
}

int recv_iqueue(iqueue_t * queue, /*out*/iqmsg_t ** msg)
{
   int err = tryrecv_iqueue(queue, msg);

   while (err == EAGAIN) {
      __sync_fetch_and_add(&queue->waitreader, 1);
      pthread_mutex_lock(&queue->readlock);

      err = tryrecv_iqueue(queue, msg);

      if (EAGAIN == err) {
         pthread_cond_wait(&queue->readcond, &queue->readlock);
      }

      pthread_mutex_unlock(&queue->readlock);
      __sync_fetch_and_sub(&queue->waitreader, 1);

      if (EAGAIN == err) {
         err = tryrecv_iqueue(queue, msg);
      }
   }

   return err;
}
