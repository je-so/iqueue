/* test.c

   Implements iqueue_t tests.

   Copyright:
   This program is free software. See accompanying LICENSE file.

   Author:
   (C) 2014 Jörg Seebohn
*/
#define _GNU_SOURCE
#include "iqueue.h"
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// len of iqueue->sizeused
#define LENOFSIZE 256

#define TEST(COND) \
         if (!(COND)) { \
            fprintf(stderr, "\n%s:%d: TEST failed\n", __FILE__, __LINE__); \
            exit(1); \
         }


#define PASS() \
         printf("."); \
         fflush(stdout);

#ifdef __linux
/*
 * Uses GNU malloc_stats extension.
 * This function returns internal collected statistics
 * about memory usage so implementing a thin wrapper
 * for malloc is not necessary.
 *
 * Currently it is only tested on Linux platforms.
 *
 * What malloc_stats does:
 * The GNU C lib function malloc_stats writes textual information
 * to standard err in the following form:
 * > Arena 0:
 * > system bytes     =     135168
 * > in use bytes     =      15000
 * > Total (incl. mmap):
 * > system bytes     =     135168
 * > in use bytes     =      15000
 * > max mmap regions =          0
 * > max mmap bytes   =          0
 *
 * How it is implemented:
 * This function redirects standard error file descriptor
 * to a pipe and reads the content of the pipe into a buffer.
 * It scans backwards until the third last line is
 * reached ("in use bytes") and then returns the converted
 * number at the end of the line as result.
 * */
int allocated_bytes(/*out*/size_t* nrofbytes)
{
   int err;
   int fd     = -1;
   int pfd[2] = { -1, -1 };

   if (pipe(pfd)) {
      err = errno;
      perror("pipe");
      goto ONERR;
   }

   int flags = fcntl(pfd[0], F_GETFL);
   fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);

   fd = dup(STDERR_FILENO);
   if (fd == -1) {
      err = errno;
      perror("dup");
      goto ONERR;
   }

   if (-1 == dup2(pfd[1], STDERR_FILENO)) {
      err = errno;
      perror("dup2");
      goto ONERR;
   }

   malloc_stats();

   char     buffer[256/*must be even*/];
   ssize_t  len    = 0;

   len = read(pfd[0], buffer, sizeof(buffer));
   if (len < 0) {
      err = errno;
      perror("read");
      goto ONERR;
   }

   while (sizeof(buffer) == len) {
      memcpy(buffer, &buffer[sizeof(buffer)/2], sizeof(buffer)/2);
      len = read(pfd[0], &buffer[sizeof(buffer)/2], sizeof(buffer)/2);
      if (len < 0) {
         err = errno;
         if (err == EWOULDBLOCK || err == EAGAIN) {
            len = sizeof(buffer)/2;
            break;
         }
         perror("read");
         goto ONERR;
      }
      len += (int)sizeof(buffer)/2;
   }

   // remove last two lines
   for (unsigned i = 3; i > 0 && len > 0; ) {
      i -= (buffer[--len] == '\n');
   }

   while (len > 0
          && buffer[len-1] >= '0'
          && buffer[len-1] <= '9') {
      -- len;
   }

   size_t used_bytes = 0;
   if (  len > 0
         && buffer[len] >= '0'
         && buffer[len] <= '9'  ) {
      sscanf(&buffer[len], "%zu", &used_bytes);
   }

   if (-1 == dup2(fd, STDERR_FILENO)) {
      err = errno;
      perror("dup2");
      goto ONERR;
   }

   (void) close(fd);
   (void) close(pfd[0]);
   (void) close(pfd[1]);

   *nrofbytes = used_bytes;

   return 0;
ONERR:
   if (pfd[0] != -1) close(pfd[0]);
   if (pfd[1] != -1) close(pfd[1]);
   if (fd != -1) {
      dup2(fd, STDERR_FILENO);
      close(fd);
   }
   return err;
}

#else
// Implement allocated_bytes for your operating system here !
int allocated_bytes(size_t* nrofbytes)
{
   *nrofbytes = 0;
   return 0;
}

#endif

static void* thr_lock(void* param)
{
   iqueue_t* queue = param;

   TEST(0 == pthread_mutex_lock(&queue->writer.lock));
   cmpxchg_atomicu32(&queue->closed, 0, 1);
   TEST(0 == pthread_cond_wait(&queue->writer.cond, &queue->writer.lock));
   TEST(0 == pthread_mutex_unlock(&queue->writer.lock));

   TEST(0 == pthread_mutex_lock(&queue->reader.lock));
   cmpxchg_atomicu32(&queue->closed, 1, 2);
   TEST(0 == pthread_cond_wait(&queue->reader.cond, &queue->reader.lock));
   TEST(0 == pthread_mutex_unlock(&queue->reader.lock));
   cmpxchg_atomicu32(&queue->closed, 2, 3);

   return 0;
}

static void test_initfree(void)
{
   pthread_t thr;
   iqueue_t* queue = 0;

   // TEST new_iqueue + delete_iqueue: capacity <= LENOFSIZE
   for (uint32_t capacity = 0; capacity <= LENOFSIZE; ++capacity) {
      TEST(0 == new_iqueue(&queue, capacity));
      TEST(0 != queue);
      TEST(0 == queue->closed);
      TEST(LENOFSIZE == queue->capacity);
      TEST(0 == queue->iused)
      for (int i = 0; i < LENOFSIZE; ++i) {
         TEST(0 == queue->sizeused[0]);
      }
      TEST(0 == queue->readpos)
      TEST(0 == queue->ifree)
      for (int i = 0; i < LENOFSIZE; ++i) {
         TEST(1 == queue->sizefree[0]);
      }
      TEST(0 == queue->writepos)
      TEST(0 == queue->reader.waitcount);
      TEST(0 == queue->writer.waitcount);
      for (size_t i = 0; i < queue->capacity; ++i) {
         TEST(0 == queue->msg[i]);
      }

      TEST(0 == delete_iqueue(&queue));
      TEST(0 == queue);
   }
   PASS();

   // TEST new_iqueue: capacity > LENOFSIZE (capacity is rounded up to next power of two)
   for (uint32_t capacity = LENOFSIZE; capacity < 1024*1024; capacity *= 2) {
      for (uint32_t d = 1; d < capacity; d += capacity/2) {
         TEST(0 == new_iqueue(&queue, capacity+1));
         TEST(0 != queue);
         TEST(0 == queue->closed);
         TEST(2*capacity == queue->capacity);
         TEST(0 == queue->iused)
         for (int i = 0; i < LENOFSIZE; ++i) {
            TEST(0 == queue->sizeused[0]);
         }
         TEST(0 == queue->readpos)
         TEST(0 == queue->ifree)
         for (int i = 0; i < LENOFSIZE; ++i) {
            TEST((2*capacity/LENOFSIZE) == queue->sizefree[0]);
         }
         TEST(0 == queue->writepos)
         TEST(0 == queue->reader.waitcount);
         TEST(0 == queue->writer.waitcount);
         for (size_t i = 0; i < queue->capacity; ++i) {
            TEST(0 == queue->msg[i]);
         }

         TEST(0 == delete_iqueue(&queue));
         TEST(0 == queue);
      }
   }
   PASS();

   // TEST new_iqueue: locks
   TEST(0 == new_iqueue(&queue, 0));
   // test writelock + writecond
   TEST(0 == pthread_create(&thr, 0, &thr_lock, queue));
   for (int i = 0; i < 100000; ++i) {
      if (0 != cmpxchg_atomicu32(&queue->closed, 0, 0)) break;
      sched_yield();
   }
   // thr_lock is waiting on writecond
   TEST(1 == cmpxchg_atomicu32(&queue->closed, 0, 0));
   TEST(0 == pthread_mutex_lock(&queue->writer.lock));
   TEST(0 == pthread_cond_signal(&queue->writer.cond));
   for (int i = 0; i < 10; ++i) {
      sched_yield();
      TEST(1 == cmpxchg_atomicu32(&queue->closed, 0, 0));
   }
   TEST(0 == pthread_mutex_unlock(&queue->writer.lock));
   for (int i = 0; i < 100000; ++i) {
      if (1 != cmpxchg_atomicu32(&queue->closed, 0, 0)) break;
      sched_yield();
   }
   // thr_lock is waiting on readcond
   TEST(2 == cmpxchg_atomicu32(&queue->closed, 0, 0));
   TEST(0 == pthread_mutex_lock(&queue->reader.lock));
   TEST(0 == pthread_cond_signal(&queue->reader.cond));
   for (int i = 0; i < 10; ++i) {
      sched_yield();
      TEST(2 == cmpxchg_atomicu32(&queue->closed, 0, 0));
   }
   TEST(0 == pthread_mutex_unlock(&queue->reader.lock));
   TEST(0 == pthread_join(thr, 0));
   TEST(3 == cmpxchg_atomicu32(&queue->closed, 0, 0));
   TEST(0 == delete_iqueue(&queue));
   TEST(0 == queue);
   PASS();

   // TEST new_iqueue: EINVAL
   TEST(EINVAL == new_iqueue(&queue, (uint32_t)-1));
   PASS();
}

static void test_query(void)
{
   iqueue_t* queue = 0;

   // prepare
   TEST(0 == new_iqueue(&queue, LENOFSIZE));

   // TEST capacity_iqueue
   TEST(LENOFSIZE == capacity_iqueue(queue));
   PASS();

   // TEST size_iqueue
   TEST(0 == size_iqueue(queue));
   PASS();

   // TEST capacity_iqueue: returns value from capacity
   queue->capacity = 0;
   TEST(0 == capacity_iqueue(queue));
   for (uint16_t i = 1; i; i = (uint16_t)(i << 1)) {
      queue->capacity = i;
      TEST(i == capacity_iqueue(queue));
   }
   queue->capacity = LENOFSIZE;
   PASS();

   // TEST size_iqueue: returns sum of sizeused array
   for (uint16_t size = 0; size <= LENOFSIZE; ++size) {
      memset(queue->sizeused, 0, sizeof(queue->sizeused));
      for (int si = 0; si < size; ++si) {
         queue->sizeused[si] = 1;
      }
      // overflow values are ignored (>= queue->capacity)
      for (int si = size; si < LENOFSIZE; ++si) {
         queue->sizeused[si] = si & 1 ? queue->capacity : (uint32_t)-1;
      }
      TEST(size == size_iqueue(queue));
   }
   PASS();

   // unprepare
   TEST(0 == delete_iqueue(&queue));
}

static void* thread_simulate_read(void* param)
{
   iqueue_t* queue = param;

   TEST(0 == queue->reader.waitcount);
   TEST(0 == pthread_mutex_lock(&queue->reader.lock));
   size_t pos = queue->writepos;
   ++ queue->reader.waitcount;
   TEST(0 == queue->msg[pos]);
   TEST(0 == pthread_cond_wait(&queue->reader.cond, &queue->reader.lock));
   TEST(0 != queue->msg[pos]);
   -- queue->reader.waitcount;
   TEST(0 == pthread_mutex_unlock(&queue->reader.lock));

   return 0;
}

static void test_trysend_single(void)
{
   iqueue_t* queue = 0;
   int       msg[LENOFSIZE];
   pthread_t thr;

   // prepare
   TEST(0 == new_iqueue(&queue, LENOFSIZE));

   // TEST trysend_iqueue: EINVAL
   TEST(EINVAL == trysend_iqueue(queue, 0));
   PASS();

   // TEST trysend_iqueue: EPIPE
   queue->closed = 1;
   TEST(EPIPE == trysend_iqueue(queue, &msg[0]));
   TEST(0 == queue->msg[0]);
   queue->closed = 0;
   PASS();

   // TEST trysend_iqueue: store into queue
   for (unsigned i = 0; i < LENOFSIZE; ++i) {
      TEST(0 == queue->msg[i]);
      TEST(0 == trysend_iqueue(queue, &msg[i]));
      TEST(0 == queue->closed);
      TEST(LENOFSIZE == queue->capacity);
      TEST(0 == queue->iused);
      TEST(0 == queue->readpos);
      TEST(i == queue->ifree);
      TEST((i+1) == queue->writepos);
      for (unsigned si = 0; si < LENOFSIZE; ++si) {
         TEST((si <= i) == queue->sizeused[si]);
         TEST((si > i) == queue->sizefree[si]);
      }
      TEST(0 == queue->reader.waitcount);
      TEST(0 == queue->writer.waitcount);
      TEST(&msg[i] == queue->msg[i]);
   }
   PASS();

   // TEST trysend_iqueue: EAGAIN
   TEST(LENOFSIZE-1 == queue->ifree);
   TEST(EAGAIN == trysend_iqueue(queue, &msg[1]));
   TEST(LENOFSIZE-1 == queue->ifree); // wrapped around 1 time
   TEST(LENOFSIZE == queue->writepos);
   for (int si = 0; si < LENOFSIZE; ++si) {
      TEST(&msg[si] == queue->msg[si]);
      TEST(1 == queue->sizeused[si]);
      TEST(0 == queue->sizefree[si]);
   }
   PASS();

   // TEST trysend_iqueue: (ifree wraps around)
   queue->ifree = LENOFSIZE-1;
   queue->sizeused[LENOFSIZE-2] = 0;
   queue->sizefree[LENOFSIZE-2] = 1;
   queue->msg[LENOFSIZE-2] = 0;
   queue->writepos = LENOFSIZE-2;
   TEST(0 == trysend_iqueue(queue, &msg[LENOFSIZE-2]));
   TEST(LENOFSIZE-2 == queue->ifree);
   TEST(LENOFSIZE-1 == queue->writepos);
   TEST(&msg[LENOFSIZE-2] == queue->msg[LENOFSIZE-2]);
   TEST(1 == queue->sizeused[LENOFSIZE-2]);
   TEST(0 == queue->sizefree[LENOFSIZE-2]);

   // TEST trysend_iqueue: does not wakeup waiting reader
   for (int si = 0; si < LENOFSIZE; ++si) {
      queue->sizeused[si] = 0;
      queue->sizefree[si] = 1;
      queue->msg[si] = 0;
   }
   queue->iused = 0;
   queue->ifree = 0;
   for (uint32_t i = 0; i < LENOFSIZE; ++i) {
      queue->writepos = i;
      TEST(0 == pthread_create(&thr, 0, &thread_simulate_read, queue));
      for (int wc = 0; wc < 100000; ++wc) {
         sched_yield();
         if (cmpxchg_atomicsize(&queue->reader.waitcount, 0, 0)) break;
      }
      TEST(0 == pthread_mutex_lock(&queue->reader.lock));
      TEST(1 == queue->reader.waitcount);
      TEST(0 == pthread_mutex_unlock(&queue->reader.lock));
      TEST(0 == trysend_iqueue(queue, &msg[i]));
      for (int wc = 0; wc < 100; ++wc) {
         sched_yield();
         if (0 == cmpxchg_atomicsize(&queue->reader.waitcount, 0, 0)) break;
      }
      TEST(1 == cmpxchg_atomicsize(&queue->reader.waitcount, 0, 0));
      // do wakeup
      TEST(0 == pthread_mutex_lock(&queue->reader.lock));
      TEST(0 == pthread_cond_signal(&queue->reader.cond));
      TEST(0 == pthread_mutex_unlock(&queue->reader.lock));
      for (int wc = 0; wc < 100000; ++wc) {
         sched_yield();
         if (0 == cmpxchg_atomicsize(&queue->reader.waitcount, 0, 0)) break;
      }
      TEST(0 == cmpxchg_atomicsize(&queue->reader.waitcount, 0, 0));
      TEST(0 == pthread_join(thr, 0));
   }
   PASS();

   // unprepare
   TEST(0 == delete_iqueue(&queue));
}

static void* thread_call_send(void* param)
{
   iqueue_t* queue = param;

   TEST(0 == queue->writer.waitcount);
   TEST(0 == pthread_mutex_lock(&queue->writer.lock));
   uint32_t pos = queue->writepos;
   pos %= queue->capacity;
   void* msg = queue->msg[pos];
   TEST(0 == pthread_mutex_unlock(&queue->writer.lock));

   TEST(0 == send_iqueue(queue, msg));

   return 0;
}

static void test_send_single(void)
{
   iqueue_t* queue = 0;
   int       msg[LENOFSIZE];
   pthread_t thr;

   // prepare
   TEST(0 == new_iqueue(&queue, LENOFSIZE));

   // TEST send_iqueue: EINVAL
   TEST(EINVAL == send_iqueue(queue, 0));
   PASS();

   // TEST send_iqueue: EPIPE
   queue->closed = 1;
   TEST(EPIPE == send_iqueue(queue, &msg[0]));
   TEST(0 == queue->msg[0]);
   queue->closed = 0;
   PASS();

   // TEST send_iqueue: store into queue
   for (unsigned i = 0; i < LENOFSIZE; ++i) {
      TEST(0 == queue->msg[i]);
      TEST(0 == trysend_iqueue(queue, &msg[i]));
      TEST(0 == queue->closed);
      TEST(LENOFSIZE == queue->capacity);
      TEST(0 == queue->iused);
      TEST(0 == queue->readpos);
      TEST(i == queue->ifree);
      TEST((i+1) == queue->writepos);
      for (unsigned si = 0; si < LENOFSIZE; ++si) {
         TEST((si <= i) == queue->sizeused[si]);
         TEST((si > i) == queue->sizefree[si]);
      }
      TEST(0 == queue->reader.waitcount);
      TEST(0 == queue->writer.waitcount);
      TEST(&msg[i] == queue->msg[i]);
   }
   PASS();

   // TEST send_iqueue: waits (reader is simulated)
   for (uint32_t i = 0; i < LENOFSIZE; ++i) {
      TEST(0 == pthread_create(&thr, 0, &thread_call_send, queue));
      // simulate unsolicited wakeup (send does not return)
      for (int wr = 0; wr <= 5; ++wr) {
         for (int wc = 0; wc < 100000; ++wc) {
            sched_yield();
            if (cmpxchg_atomicsize(&queue->writer.waitcount, 0, 0)) break;
         }
         TEST(1 == cmpxchg_atomicsize(&queue->writer.waitcount, 0, 0));
         if (wr < 5) {
            TEST(0 == pthread_mutex_lock(&queue->writer.lock));
            TEST(0 == pthread_cond_signal(&queue->writer.cond));
            TEST(0 == pthread_mutex_unlock(&queue->writer.lock));
            for (int wc = 0; wc < 100; ++wc) {
               sched_yield();
               if (0 == cmpxchg_atomicsize(&queue->writer.waitcount, 0, 0)) break;
            }
         }
      }
      TEST(1 == cmpxchg_atomicsize(&queue->writer.waitcount, 0, 0));
      // simulate reader
      queue->readpos = i+1;
      queue->msg[i] = 0;
      queue->sizeused[i] = 0;
      queue->sizefree[i] = 1;
      // wake up writer
      pthread_mutex_lock(&queue->writer.lock);
      pthread_cond_signal(&queue->writer.cond);
      pthread_mutex_unlock(&queue->writer.lock);
      for (int wc = 0; wc < 100000; ++wc) {
         sched_yield();
         if (0 == cmpxchg_atomicsize(&queue->writer.waitcount, 0, 0)) break;
      }
      TEST(0 == cmpxchg_atomicsize(&queue->writer.waitcount, 0, 0));
      TEST(0 == pthread_join(thr, 0));
      // writer has rewritten msg
      TEST(LENOFSIZE+1+i == queue->writepos);
      TEST(&msg[i] == queue->msg[i]);
      TEST(1 == queue->sizeused[i]);
      TEST(0 == queue->sizefree[i]);
   }
   PASS();

   // unprepare
   TEST(0 == delete_iqueue(&queue));
}

static void test_tryrecv_single(void)
{
   iqueue_t*  queue = 0;
   int        msg[LENOFSIZE];
   void*      rcv;
   pthread_t  thr;

   // prepare
   TEST(0 == new_iqueue(&queue, LENOFSIZE));

   // TEST tryrecv_iqueue: EPIPE
   queue->closed = 1;
   TEST(EPIPE == tryrecv_iqueue(queue, &rcv));
   queue->closed = 0;
   PASS();

   // fill queue
   for (unsigned i = 0; i < LENOFSIZE; ++i) {
      TEST(0 == trysend_iqueue(queue, &msg[i]));
   }
   TEST(LENOFSIZE == queue->writepos);

   // TEST tryrecv_iqueue: get from queue
   for (uint32_t i = 0; i < LENOFSIZE; ++i) {
      TEST(0 == tryrecv_iqueue(queue, &rcv));
      TEST(rcv == &msg[i]);
      TEST(0 == queue->closed);
      TEST(LENOFSIZE == queue->capacity);
      TEST(i == queue->iused);
      TEST(i+1 == queue->readpos);
      TEST(LENOFSIZE-1 == queue->ifree);
      TEST(LENOFSIZE == queue->writepos);
      TEST(0 == queue->reader.waitcount);
      TEST(0 == queue->writer.waitcount);
      for (unsigned si = 0; si < LENOFSIZE; ++si) {
         TEST((si > i) == queue->sizeused[si]);
         TEST((si <= i) == queue->sizefree[si]);
      }
      TEST(0 == queue->reader.waitcount);
      TEST(0 == queue->writer.waitcount);
      TEST(0 == queue->msg[i]);
   }
   PASS();

   // TEST tryrecv_iqueue: EAGAIN
   TEST(LENOFSIZE-1 == queue->iused);
   TEST(EAGAIN == tryrecv_iqueue(queue, &rcv));
   TEST(LENOFSIZE-1 == queue->iused); // wrapped around 1 time
   TEST(LENOFSIZE == queue->readpos);
   for (int si = 0; si < LENOFSIZE; ++si) {
      TEST(0 == queue->msg[si]);
      TEST(0 == queue->sizeused[si]);
      TEST(1 == queue->sizefree[si]);
   }
   PASS();

   // TEST tryrecv_iqueue: (iused wraps around)
   queue->iused = LENOFSIZE-1;
   queue->sizeused[LENOFSIZE-2] = 1;
   queue->sizefree[LENOFSIZE-2] = 0;
   queue->msg[LENOFSIZE-2] = &msg[LENOFSIZE-2];
   queue->readpos = LENOFSIZE-2;
   TEST(0 == tryrecv_iqueue(queue, &rcv));
   TEST(rcv == &msg[LENOFSIZE-2]);
   TEST(LENOFSIZE-2 == queue->iused);
   TEST(LENOFSIZE-1 == queue->readpos);
   TEST(0 == queue->msg[LENOFSIZE-2]);
   TEST(0 == queue->sizeused[LENOFSIZE-2]);
   TEST(1 == queue->sizefree[LENOFSIZE-2]);

   // fill queue
   queue->iused = 0;
   queue->ifree = 0;
   queue->readpos = 0;
   queue->writepos = 0;
   for (unsigned i = 0; i < LENOFSIZE; ++i) {
      TEST(0 == trysend_iqueue(queue, &msg[i]));
   }

   // TEST tryrecv_iqueue: does not wakeup waiting writer
   for (unsigned i = 0; i < LENOFSIZE; ++i) {
      TEST(0 == pthread_create(&thr, 0, &thread_call_send, queue));
      for (int wc = 0; wc < 100000; ++wc) {
         sched_yield();
         if (cmpxchg_atomicsize(&queue->writer.waitcount, 0, 0)) break;
      }
      TEST(1 == cmpxchg_atomicsize(&queue->writer.waitcount, 0, 0));
      TEST(0 == tryrecv_iqueue(queue, &rcv));
      TEST(rcv == &msg[i]);
      for (int wc = 0; wc < 100; ++wc) {
         sched_yield();
         if (0 == cmpxchg_atomicsize(&queue->writer.waitcount, 0, 0)) break;
      }
      TEST(1 == cmpxchg_atomicsize(&queue->writer.waitcount, 0, 0));
      // do wakeup
      TEST(0 == pthread_mutex_lock(&queue->writer.lock));
      TEST(0 == pthread_cond_signal(&queue->writer.cond));
      TEST(0 == pthread_mutex_unlock(&queue->writer.lock));
      for (int wc = 0; wc < 100000; ++wc) {
         sched_yield();
         if (0 == cmpxchg_atomicsize(&queue->writer.waitcount, 0, 0)) break;
      }
      TEST(0 == cmpxchg_atomicsize(&queue->writer.waitcount, 0, 0));
      TEST(0 == pthread_join(thr, 0));
      // msg was written
      TEST(LENOFSIZE+1+i == queue->writepos);
      TEST(&msg[i] == queue->msg[i]);
      TEST(1 == queue->sizeused[i]);
      TEST(0 == queue->sizefree[i]);
   }
   PASS();

   // unprepare
   TEST(0 == delete_iqueue(&queue));
}

static void* thread_call_recv(void* param)
{
   iqueue_t* queue = param;

   TEST(0 == queue->reader.waitcount);
   void* rcv = 0;
   TEST(0 == recv_iqueue(queue, &rcv));
   TEST(0 != rcv);

   return 0;
}

static void test_recv_single(void)
{
   iqueue_t* queue = 0;
   int       msg[LENOFSIZE];
   void*     rcv;
   pthread_t thr;

   // prepare
   TEST(0 == new_iqueue(&queue, LENOFSIZE));

   // TEST recv_iqueue: EPIPE
   queue->closed = 1;
   TEST(EPIPE == recv_iqueue(queue, &rcv));
   queue->closed = 0;
   PASS();

   // fill queue
   for (unsigned i = 0; i < LENOFSIZE; ++i) {
      TEST(0 == trysend_iqueue(queue, &msg[i]));
   }

   // TEST recv_iqueue: get from queue
   for (uint32_t i = 0; i < LENOFSIZE; ++i) {
      TEST(0 == recv_iqueue(queue, &rcv));
      TEST(rcv == &msg[i]);
      TEST(0 == queue->closed);
      TEST(LENOFSIZE == queue->capacity);
      TEST(i == queue->iused);
      TEST(i+1 == queue->readpos);
      TEST(LENOFSIZE-1 == queue->ifree);
      TEST(LENOFSIZE == queue->writepos);
      TEST(0 == queue->reader.waitcount);
      TEST(0 == queue->writer.waitcount);
      for (unsigned si = 0; si < LENOFSIZE; ++si) {
         TEST((si > i) == queue->sizeused[si]);
         TEST((si <= i) == queue->sizefree[si]);
      }
      TEST(0 == queue->reader.waitcount);
      TEST(0 == queue->writer.waitcount);
      TEST(0 == queue->msg[i]);
   }
   PASS();

   // TEST recv_iqueue: waits (writer is simulated)
   for (uint32_t i = 0; i < LENOFSIZE; ++i) {
      TEST(0 == pthread_create(&thr, 0, &thread_call_recv, queue));
      // simulate unsolicited wakeup (recv does not return)
      for (int wr = 0; wr <= 5; ++wr) {
         for (int wc = 0; wc < 100000; ++wc) {
            sched_yield();
            if (cmpxchg_atomicsize(&queue->reader.waitcount, 0, 0)) break;
         }
         TEST(1 == cmpxchg_atomicsize(&queue->reader.waitcount, 0, 0));
         if (wr < 5) {
            TEST(0 == pthread_mutex_lock(&queue->reader.lock));
            TEST(0 == pthread_cond_signal(&queue->reader.cond));
            TEST(0 == pthread_mutex_unlock(&queue->reader.lock));
            for (int wc = 0; wc < 100; ++wc) {
               sched_yield();
               if (0 == cmpxchg_atomicsize(&queue->reader.waitcount, 0, 0)) break;
            }
         }
      }
      TEST(1 == cmpxchg_atomicsize(&queue->reader.waitcount, 0, 0));
      // simulate writer
      queue->writepos = i+1;
      queue->msg[i] = &msg[i];
      queue->sizeused[i] = 1;
      queue->sizefree[i] = 0;
      // wake up reader
      pthread_mutex_lock(&queue->reader.lock);
      pthread_cond_signal(&queue->reader.cond);
      pthread_mutex_unlock(&queue->reader.lock);
      for (int wc = 0; wc < 100000; ++wc) {
         sched_yield();
         if (0 == cmpxchg_atomicsize(&queue->reader.waitcount, 0, 0)) break;
      }
      TEST(0 == cmpxchg_atomicsize(&queue->reader.waitcount, 0, 0));
      TEST(0 == pthread_join(thr, 0));
      // reader has removed msg
      TEST(LENOFSIZE+1+i == queue->readpos);
      TEST(0 == queue->msg[i]);
   }
   PASS();

   // unprepare
   TEST(0 == delete_iqueue(&queue));
}

static void* thread_epipe_send(void* queue)
{
   int msg;
   int err = send_iqueue(queue, &msg);
   if (err != EPIPE) {
      printf("wrong err = %d\n", err);
   }
   TEST(EPIPE == err);
   return 0;
}

static void* thread_epipe_recv(void* queue)
{
   void* msg = 0;
   int err = recv_iqueue(queue, &msg);
   if (err != EPIPE) {
      printf("wrong err = %d\n", err);
   }
   TEST(EPIPE == err);
   return 0;
}

static void test_close(void)
{
   iqueue_t* queue = 0;
   int       msg[LENOFSIZE];
   pthread_t thr[100];

   // TEST close_iqueue: sets closed
   TEST(0 == new_iqueue(&queue, 1));
   close_iqueue(queue);
   TEST(1 == queue->closed);
   TEST(0 == delete_iqueue(&queue));
   PASS();

   // TEST close_iqueue: wakes up waiting reader and writer
   // prepare
   TEST(0 == new_iqueue(&queue, LENOFSIZE));
   // fill queue
   for (int i = 0; i < LENOFSIZE; ++i) {
      TEST(0 == send_iqueue(queue, &msg[i]));
   }
   TEST(0 == queue->reader.waitcount);
   TEST(0 == queue->writer.waitcount);
   for (unsigned i = 0; i < 50; ++i) {
      TEST(0 == pthread_create(&thr[i], 0, &thread_epipe_send, queue));
   }
   for (int wc = 0; wc < 100000; ++wc) {   // wait until started
      sched_yield();
      if (50 == cmpxchg_atomicsize(&queue->writer.waitcount, 0, 0)) break;
   }
   TEST(0 == pthread_mutex_lock(&queue->writer.lock));
   TEST(50 == queue->writer.waitcount);
   TEST(0 == pthread_mutex_unlock(&queue->writer.lock));
   // read msg without waking up writers
   for (int i = 0; i < LENOFSIZE; ++i) {
      void* rcv;
      TEST(0 == tryrecv_iqueue(queue, &rcv));
      TEST(rcv == &msg[i]);
   }
   // simulate no waiting writers
   TEST(50 == cmpxchg_atomicsize(&queue->writer.waitcount, 50, 0));
   for (int i = 0; i < 50; ++i) {
      TEST(0 == pthread_create(&thr[50+i], 0, &thread_epipe_recv, queue));
   }
   // wait until all threads wait
   for (int i = 0; i < 100000; ++i) {
      sched_yield();
      if (50 == cmpxchg_atomicsize(&queue->reader.waitcount, 0, 0)) break;
   }
   TEST(0 == pthread_mutex_lock(&queue->reader.lock));
   TEST(50 == queue->reader.waitcount);
   TEST(0 == pthread_mutex_unlock(&queue->reader.lock));
   // test
   TEST(50 == cmpxchg_atomicsize(&queue->reader.waitcount, 0, 0));
   TEST(0 == cmpxchg_atomicsize(&queue->writer.waitcount, 0, 50));
   close_iqueue(queue);
   TEST(0 == queue->reader.waitcount);
   TEST(0 == queue->writer.waitcount);
   for (int i = 0; i < 100; ++i) {
      TEST(0 == pthread_join(thr[i], 0));
   }
   TEST(0 == delete_iqueue(&queue));
   PASS();

   // TEST delete_iqueue: wakes up waiting reader and writer
   // prepare
   TEST(0 == new_iqueue(&queue, LENOFSIZE));
   // fill queue
   for (int i = 0; i < LENOFSIZE; ++i) {
      TEST(0 == send_iqueue(queue, &msg[i]));
   }
   for (unsigned i = 0; i < 50; ++i) {
      TEST(0 == pthread_create(&thr[i], 0, &thread_epipe_send, queue));
   }
   for (int wc = 0; wc < 100000; ++wc) {   // wait until started
      sched_yield();
      if (50 == cmpxchg_atomicsize(&queue->writer.waitcount, 0, 0)) break;
   }
   TEST(0 == pthread_mutex_lock(&queue->writer.lock));
   TEST(50 == queue->writer.waitcount);
   TEST(0 == pthread_mutex_unlock(&queue->writer.lock));
   // read msg without waking up writers
   for (int i = 0; i < LENOFSIZE; ++i) {
      void* rcv;
      TEST(0 == tryrecv_iqueue(queue, &rcv));
      TEST(rcv == &msg[i]);
   }
   // simulate no waiting writers
   TEST(50 == cmpxchg_atomicsize(&queue->writer.waitcount, 50, 0));
   for (int i = 0; i < 50; ++i) {
      TEST(0 == pthread_create(&thr[50+i], 0, &thread_epipe_recv, queue));
   }
   for (int i = 0; i < 100000; ++i) {   // wait until all threads wait
      sched_yield();
      if (50 == cmpxchg_atomicsize(&queue->reader.waitcount, 0, 0)) break;
   }
   TEST(0 == pthread_mutex_lock(&queue->reader.lock));
   TEST(50 == queue->reader.waitcount);
   TEST(0 == pthread_mutex_unlock(&queue->reader.lock));
   // test
   TEST(50 == cmpxchg_atomicsize(&queue->reader.waitcount, 0, 0));
   TEST(0 == cmpxchg_atomicsize(&queue->writer.waitcount, 0, 50));
   TEST(0 == delete_iqueue(&queue));
   for (int i = 0; i < 100; ++i) {
      TEST(0 == pthread_join(thr[i], 0));
   }
   PASS();
}

static void* thread_callwaitsignal(void* signal)
{
   wait_iqsignal(signal);
   return 0;
}

static void test_iqsignal(void)
{
   iqsignal_t signal;
   pthread_t  thr[100];

   // TEST init_iqsignal
   memset(&signal, 255, sizeof(signal));
   TEST(0 == init_iqsignal(&signal));
   TEST(0 == signal.waitcount);
   TEST(0 == signal.signalcount);
   PASS();

   // TEST signalcount_iqsignal
   for (size_t i = 1; i; i <<= 1) {
      signal.signalcount = i;
      TEST(i == signalcount_iqsignal(&signal));
   }
   signal.signalcount = 0;
   TEST(0 == signalcount_iqsignal(&signal));
   PASS();

   // TEST signal_iqsignal: add 1 to signalcount
   for (size_t i = 1; i < 1000; ++i) {
      signal_iqsignal(&signal);
      TEST(i == signal.signalcount);
   }
   PASS();

   // TEST wait_iqsignal: signalcount != 0
   for (size_t i = 1; i; i <<= 1) {
      signal.signalcount = i;
      wait_iqsignal(&signal);
      TEST(i == signal.signalcount);
      TEST(0 == signal.waitcount);
   }
   PASS();

   // TEST clearsignal_iqsignal
   for (size_t i = 1; i; i <<= 1) {
      signal.signalcount = i;
      TEST(i == clearsignal_iqsignal(&signal));
      TEST(0 == signal.signalcount);
      TEST(0 == signal.waitcount);
   }
   TEST(0 == clearsignal_iqsignal(&signal));
   TEST(0 == signal.signalcount);
   TEST(0 == signal.waitcount);
   PASS();

   // TEST wait_iqsignal: wait
   for (unsigned i = 0; i < 100; ++i) {
      TEST(0 == pthread_create(&thr[i], 0, &thread_callwaitsignal, &signal));
   }
   for (int wc = 0; wc < 100000; ++wc) {
      sched_yield();
      if (100 == cmpxchg_atomicsize(&signal.waitcount, 0, 0)) break;
   }
   // all threads are waiting
   TEST(100 == cmpxchg_atomicsize(&signal.waitcount, 0, 0));
   PASS();

   // TEST signal_iqsignal: wakeup all waiting threads
   signal_iqsignal(&signal);
   TEST(1 == signalcount_iqsignal(&signal));
   for (int i = 0; i < 100000; ++i) {
      sched_yield();
      if (0 == cmpxchg_atomicsize(&signal.waitcount, 0, 0)) break;
   }
   TEST(0 == cmpxchg_atomicsize(&signal.waitcount, 0, 0));
   for (int i = 0; i < 100; ++i) {
      TEST(0 == pthread_join(thr[i], 0));
   }
   PASS();

   // TEST free_iqsignal
   TEST(0 == free_iqsignal(&signal));
   PASS();
}

#define MAXRANGE  80000
#define MAXTHREAD 5
#define QUEUESIZE 4000

static uint32_t   s_threadid;
static int        s_threadtry;
static iqsignal_t s_threadsignal;
static uint8_t    s_flag[MAXTHREAD][MAXRANGE];

struct range_t {
   uint32_t tid;
   uint32_t nr;
};

void* thread_sendrange(void* queue)
{
   uint32_t myid = s_threadid;
   struct range_t msg[2*QUEUESIZE];

   for (;;) {
      myid = s_threadid;
      if (myid == cmpxchg_atomicu32(&s_threadid, myid, myid+1)) break;
   }

   for (uint32_t nr = 0; nr < 2*QUEUESIZE; ++nr) {
      msg[nr].nr = MAXRANGE;
   }

   for (uint32_t nr = 0; nr < MAXRANGE; ++nr) {
      uint32_t m = nr % (2*QUEUESIZE);
      while (MAXRANGE != cmpxchg_atomicu32(&msg[m].nr, MAXRANGE, 0)) {
         sched_yield(); // message in use
      }
      msg[m].tid = myid;
      msg[m].nr  = nr;
      for (int i = 0; i < 200 && 0 != ((iqueue_t*)queue)->writer.waitcount; ++i) {
         sched_yield();
      }

      for (int i = 0; ; ++i) {
         int err = s_threadtry ? trysend_iqueue(queue, &msg[m]) : send_iqueue(queue, &msg[m]);
         if (err == 0) break;
         TEST(s_threadtry && err == EAGAIN);
         sched_yield();
         if (i == 1000000) {
            printf("Sender starvation\n");
            exit(1);
         }
      }
   }

   wait_iqsignal(&s_threadsignal);

   return 0;
}

void* thread_recvrange(void* queue)
{
   void* imsg;

   for (;;) {
      for (int i = 0; ; ++i) {
         int err = s_threadtry ? tryrecv_iqueue(queue, &imsg) : recv_iqueue(queue, &imsg);
         if (err == EPIPE) return 0;
         if (err == 0) break;
         TEST(err == EAGAIN);
         sched_yield();
         if (i == 1000000) {
            printf("Receiver starvation\n");
            exit(1);
         }
      }

      struct range_t* rmsg = (struct range_t*) imsg;
      TEST(rmsg->tid < MAXTHREAD);
      TEST(rmsg->nr  < MAXRANGE);
      s_flag[rmsg->tid][rmsg->nr] = (uint8_t) (s_flag[rmsg->tid][rmsg->nr] + 1);
      // message processed
      cmpxchg_atomicu32(&rmsg->nr, rmsg->nr, MAXRANGE);
   }

   return 0;
}

void test_multi_sendrecv(void)
{
   iqueue_t* queue = 0;
   pthread_t rthr[MAXTHREAD/2];
   pthread_t sthr[MAXTHREAD];

   for (int usetry = 0; usetry <= 1; ++usetry) {
      memset(s_flag, 0, sizeof(s_flag));
      // start threads
      s_threadid  = 0;
      s_threadtry = usetry;
      TEST(0 == init_iqsignal(&s_threadsignal));
      TEST(0 == new_iqueue(&queue, QUEUESIZE));
      for (int i = 0; i < MAXTHREAD; ++i) {
         TEST(0 == pthread_create(&sthr[i], 0, &thread_sendrange, queue));
         if (i < MAXTHREAD/2) {
            TEST(0 == pthread_create(&rthr[i], 0, &thread_recvrange, queue));
         }
      }
      for (int i = 0; i < MAXTHREAD; ++i) {
         for (int r = 0; r < MAXRANGE; ++r) {
            int x = 0;
            while (__sync_fetch_and_add(&s_flag[i][r], 0) == 0) {
               sched_yield();
               ++x;
               if (x == 1000000) {
                  // DEBUG:
                  printf("usetry:%d rwait:%d wwait:%d wready:%d\n", usetry, queue->reader.waitcount, queue->writer.waitcount, s_threadsignal.waitcount);
                  x = 0;
               }
            }
            // == DEBUG: PRINT PROGRESS (change 0 into 1) ==
            if (0 && 0 == r % 1000) {
               printf("%d: %d\n", i, r);
            }
         }
      }
      close_iqueue(queue);
      // stop threads
      for (int i = 0; i < MAXTHREAD/2; ++i) {
         TEST(0 == pthread_join(rthr[i], 0));
      }
      signal_iqsignal(&s_threadsignal);
      for (int i = 0; i < MAXTHREAD; ++i) {
         TEST(0 == pthread_join(sthr[i], 0));
      }
      TEST(0 == delete_iqueue(&queue));
      TEST(0 == free_iqsignal(&s_threadsignal));
      for (int r = 0; r < MAXRANGE; ++r) {
         for (int i = 0; i < MAXTHREAD; ++i) {
            TEST(s_flag[i][r] == 1);
         }
      }
      PASS();
   }
}

static void* thr_lock1(void* param)
{
   iqueue1_t* queue = param;

   TEST(0 == pthread_mutex_lock(&queue->writer.lock));
   cmpxchg_atomicu32(&queue->closed, 0, 1);
   TEST(0 == pthread_cond_wait(&queue->writer.cond, &queue->writer.lock));
   TEST(0 == pthread_mutex_unlock(&queue->writer.lock));

   TEST(0 == pthread_mutex_lock(&queue->reader.lock));
   cmpxchg_atomicu32(&queue->closed, 1, 2);
   TEST(0 == pthread_cond_wait(&queue->reader.cond, &queue->reader.lock));
   TEST(0 == pthread_mutex_unlock(&queue->reader.lock));
   cmpxchg_atomicu32(&queue->closed, 2, 3);

   return 0;
}

static void test_initfree1(void)
{
   pthread_t thr;
   iqueue1_t* queue = 0;

   // TEST new_iqueue
   TEST(0 == new_iqueue1(&queue, 12345));
   TEST(0 != queue);
   TEST(12345 == queue->capacity);
   TEST(0 == queue->readpos);
   TEST(0 == queue->writepos);
   TEST(0 == queue->closed);
   TEST(0 == queue->reader.waitcount);
   TEST(0 == queue->writer.waitcount);
   for (size_t i = 0; i < queue->capacity; ++i) {
      TEST(0 == queue->msg[i]);
   }
   // test writelock + writecond
   TEST(0 == pthread_create(&thr, 0, &thr_lock1, queue));
   for (int i = 0; i < 100000; ++i) {
      if (0 != cmpxchg_atomicu32(&queue->closed, 0, 0)) break;
      sched_yield();
   }
   // thr_lock1 is waiting on writecond
   TEST(1 == cmpxchg_atomicu32(&queue->closed, 0, 0));
   TEST(0 == pthread_mutex_lock(&queue->writer.lock));
   TEST(0 == pthread_cond_signal(&queue->writer.cond));
   for (int i = 0; i < 10; ++i) {
      sched_yield();
      TEST(1 == cmpxchg_atomicu32(&queue->closed, 0, 0));
   }
   TEST(0 == pthread_mutex_unlock(&queue->writer.lock));
   for (int i = 0; i < 100000; ++i) {
      if (1 != cmpxchg_atomicu32(&queue->closed, 0, 0)) break;
      sched_yield();
   }
   // thr_lock is waiting on readcond
   TEST(2 == cmpxchg_atomicu32(&queue->closed, 0, 0));
   TEST(0 == pthread_mutex_lock(&queue->reader.lock));
   TEST(0 == pthread_cond_signal(&queue->reader.cond));
   for (int i = 0; i < 10; ++i) {
      sched_yield();
      TEST(2 == cmpxchg_atomicu32(&queue->closed, 0, 0));
   }
   TEST(0 == pthread_mutex_unlock(&queue->reader.lock));
   TEST(0 == pthread_join(thr, 0));
   TEST(3 == cmpxchg_atomicu32(&queue->closed, 0, 0));
   PASS();

   // TEST delete_iqueue
   TEST(0 == delete_iqueue1(&queue));
   TEST(0 == queue);
   PASS();

   // TEST new_iqueue: different size parameter
   for (size_t s = 1; s < 65536; s = (s << 1) + 33) {
      TEST(0 == new_iqueue1(&queue, (uint16_t) s));
      TEST(0 != queue);
      TEST(s == queue->capacity);
      TEST(0 == queue->readpos);
      TEST(0 == queue->writepos);
      TEST(0 == queue->closed);
      TEST(0 == queue->reader.waitcount);
      TEST(0 == queue->writer.waitcount);
      for (size_t i = 0; i < queue->capacity; ++i) {
         TEST(0 == queue->msg[i]);
      }
      TEST(0 == delete_iqueue1(&queue));
      TEST(0 == queue);
   }
   PASS();

   // TEST new_iqueue: EINVAL
   TEST(EINVAL == new_iqueue1(&queue, 0));
   PASS();
}

static void test_query1(void)
{
   iqueue1_t* queue = 0;

   // prepare
   TEST(0 == new_iqueue1(&queue, 128));

   // TEST capacity_iqueue
   TEST(128 == capacity_iqueue1(queue));
   PASS();

   // TEST size_iqueue
   TEST(0 == size_iqueue1(queue));
   PASS();

   // TEST capacity_iqueue1: returns value from capacity
   queue->capacity = 0;
   TEST(0 == capacity_iqueue1(queue));
   for (uint32_t i = 1; i; i = (uint16_t)(i << 1)) {
      queue->capacity = i;
      TEST(i == capacity_iqueue1(queue));
   }
   queue->capacity = 128;
   PASS();

   // TEST size_iqueue: readpos < writepos
   for (uint32_t i = 1; i; i = (i << 1)) {
      queue->readpos  = 0;
      queue->writepos = i;
      TEST(i == size_iqueue1(queue));
   }
   for (uint32_t i = 1; i; i = (i << 1)) {
      for (uint32_t s = 10; s; --s) {
         queue->readpos = i;
         queue->writepos = (i+s);
         TEST(s == size_iqueue1(queue));
      }
   }
   PASS();

   // TEST size_iqueue: writepos < readpos
   for (uint32_t i = 1; i; i = (i << 1)) {
      for (uint32_t c = 65535; c >= 32768; c = (uint16_t)(c - (~c + 1))) {
         queue->capacity = c;
         queue->readpos  = i;
         queue->writepos = 0;
         TEST((c - i) == size_iqueue1(queue));
      }
   }
   for (uint32_t i = 1; i; i = (i << 1)) {
      for (uint32_t s = 10; s; --s) {
         for (uint32_t c = 65535; c >= 32768; c = (uint16_t)(c - (~c + 1))) {
            queue->capacity = c;
            queue->readpos = (i+s);
            queue->writepos = i;
            TEST((c-s) == size_iqueue1(queue));
         }
      }
   }
   queue->capacity = 128;
   PASS();

   // TEST size_iqueue: writepos == readpos
   for (uint32_t c = 1; c <= 128; ++c) {
      // writepos == 0
      queue->capacity = c;
      queue->readpos  = 0;
      queue->writepos = 0;
      TEST(0 == size_iqueue1(queue));
      queue->msg[c-1] = (void*) 1;
      TEST(c == size_iqueue1(queue));
      queue->msg[c-1] = (void*) 0;
   }
   for (uint32_t i = 1; i < 128; ++i) {
      for (uint32_t c = 1; c <= 128; ++c) {
         // writepos > 0
         queue->capacity = c;
         queue->readpos  = i;
         queue->writepos = i;
         TEST(0 == size_iqueue1(queue));
         queue->msg[i-1] = (void*) 1;
         TEST(c == size_iqueue1(queue));
         queue->msg[i-1] = (void*) 0;
      }
   }
   PASS();

   // unprepare
   TEST(0 == delete_iqueue1(&queue));
}

void* thread_sendrange1(void* queue)
{
   uint32_t myid = 0;
   struct range_t msg[2*QUEUESIZE];

   for (uint32_t nr = 0; nr < 2*QUEUESIZE; ++nr) {
      msg[nr].nr = MAXRANGE;
   }

   for (uint32_t nr = 0; nr < MAXRANGE; ++nr) {
      uint32_t m = nr % (2*QUEUESIZE);
      while (MAXRANGE != cmpxchg_atomicu32(&msg[m].nr, MAXRANGE, 0)) {
         sched_yield(); // message in use
      }
      msg[m].tid = myid;
      msg[m].nr  = nr;

      for (int i = 0; ; ++i) {
         int err = s_threadtry ? trysend_iqueue1(queue, &msg[m]) : send_iqueue1(queue, &msg[m]);
         if (err == 0) break;
         TEST(s_threadtry && err == EAGAIN);
         sched_yield();
         if (i == 1000000) {
            printf("Sender starvation\n");
            exit(1);
         }
      }
   }

   wait_iqsignal(&s_threadsignal);

   return 0;
}

void* thread_recvrange1(void* queue)
{
   void* imsg;

   for (;;) {
      for (int i = 0; ; ++i) {
         int err = s_threadtry ? tryrecv_iqueue1(queue, &imsg) : recv_iqueue1(queue, &imsg);
         if (err == EPIPE) return 0;
         if (err == 0) break;
         TEST(err == EAGAIN);
         sched_yield();
         if (i == 1000000) {
            printf("Receiver starvation\n");
            exit(1);
         }
      }

      struct range_t* rmsg = (struct range_t*) imsg;
      TEST(rmsg->tid < MAXTHREAD);
      TEST(rmsg->nr  < MAXRANGE);
      s_flag[rmsg->tid][rmsg->nr] = (uint8_t) (s_flag[rmsg->tid][rmsg->nr] + 1);
      // message processed
      cmpxchg_atomicu32(&rmsg->nr, rmsg->nr, MAXRANGE);
   }

   return 0;
}

void test_single_sendrecv1(void)
{
   iqueue1_t* queue = 0;
   pthread_t rthr;
   pthread_t sthr;

   for (int usetry = 0; usetry <= 1; ++usetry) {
      memset(s_flag, 0, sizeof(s_flag));
      // start threads
      s_threadid  = 0;
      s_threadtry = usetry;
      TEST(0 == init_iqsignal(&s_threadsignal));
      TEST(0 == new_iqueue1(&queue, QUEUESIZE));
      TEST(0 == pthread_create(&sthr, 0, &thread_sendrange1, queue));
      TEST(0 == pthread_create(&rthr, 0, &thread_recvrange1, queue));
      for (int r = 0; r < MAXRANGE; ++r) {
         int x = 0;
         while (__sync_fetch_and_add(&s_flag[0][r], 0) == 0) {
            sched_yield();
            ++x;
            if (x == 1000000) {
               // DEBUG:
               printf("usetry:%d rwait:%d wwait:%d wready:%d rpos:%d wpos:%d\n", usetry, queue->reader.waitcount, queue->writer.waitcount, s_threadsignal.waitcount, queue->readpos, queue->writepos);
               x = 0;
            }
         }
         // == DEBUG: PRINT PROGRESS (change 0 into 1) ==
         if (0 && 0 == r % 1000) {
            printf("%d: %d\n", 0, r);
         }
      }
      close_iqueue1(queue);
      // stop threads
      TEST(0 == pthread_join(rthr, 0));
      signal_iqsignal(&s_threadsignal);
      TEST(0 == pthread_join(sthr, 0));
      TEST(0 == delete_iqueue1(&queue));
      TEST(0 == free_iqsignal(&s_threadsignal));
      for (int r = 0; r < MAXRANGE; ++r) {
         TEST(s_flag[0][r] == 1);
      }
      PASS();
   }
}

int main(void)
{
   size_t nrofbytes;
   size_t nrofbytes2;

   printf("Running iqueue test: ");
   fflush(stdout);

   // if first call to pthread API allocates memory - repeat the tests
   for (int i = 0; i < 2; ++i) {
      TEST(0 == allocated_bytes(&nrofbytes));

      // iqueue_t

      test_initfree();
      test_query();
      test_trysend_single();
      test_send_single();
      test_tryrecv_single();
      test_recv_single();
      test_close();
      test_iqsignal();
      test_multi_sendrecv();

      // iqueue1_t

      test_initfree1();
      test_query1();
      test_single_sendrecv1();

      TEST(0 == allocated_bytes(&nrofbytes2));
      if (nrofbytes == nrofbytes2) break;
   }

   printf("\n");

   if (nrofbytes != nrofbytes2) {
      printf("Memory leak of '%ld' bytes!\n", (long) (nrofbytes2 - nrofbytes));
      return 1;
   }

   return 0;
}
