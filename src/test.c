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
int allocated_bytes(/*out*/size_t * nrofbytes)
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
   for (unsigned i = 3; len > 0; --len) {
      i -= (buffer[len] == '\n');
      if (!i) break;
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
int allocated_bytes(size_t * nrofbytes)
{
   *nrofbytes = 0;
   return 0;
}

#endif

static void* thr_lock(void * param)
{
   iqueue_t* queue = param;

   TEST(0 == pthread_mutex_lock(&queue->writer.lock));
   cmpxchg_atomicint(&queue->closed, 0, 1);
   TEST(0 == pthread_cond_wait(&queue->writer.cond, &queue->writer.lock));
   TEST(0 == pthread_mutex_unlock(&queue->writer.lock));

   TEST(0 == pthread_mutex_lock(&queue->reader.lock));
   cmpxchg_atomicint(&queue->closed, 1, 2);
   TEST(0 == pthread_cond_wait(&queue->reader.cond, &queue->reader.lock));
   TEST(0 == pthread_mutex_unlock(&queue->reader.lock));
   cmpxchg_atomicint(&queue->closed, 2, 3);

   return 0;
}

static void test_initfree(void)
{
   pthread_t thr;
   iqueue_t* queue = 0;

   // TEST new_iqueue
   TEST(0 == new_iqueue(&queue, 12345));
   TEST(0 != queue);
   TEST(12345 == queue->size);
   TEST(0 == queue->readpos);
   TEST(0 == queue->writepos);
   TEST(0 == queue->reader.waitcount);
   TEST(0 == queue->writer.waitcount);
   TEST(0 == queue->closed);
   for (size_t i = 0; i < queue->size; ++i) {
      TEST(0 == queue->msg[i]);
   }
   // test writelock + writecond
   TEST(0 == pthread_create(&thr, 0, &thr_lock, queue));
   for (int i = 0; i < 10000; ++i) {
      if (0 != cmpxchg_atomicint(&queue->closed, 0, 0)) break;
      sched_yield();
   }
   // thr_lock is waiting on writecond
   TEST(1 == cmpxchg_atomicint(&queue->closed, 0, 0));
   TEST(0 == pthread_mutex_lock(&queue->writer.lock));
   TEST(0 == pthread_cond_signal(&queue->writer.cond));
   for (int i = 0; i < 10; ++i) {
      sched_yield();
      TEST(1 == cmpxchg_atomicint(&queue->closed, 0, 0));
   }
   TEST(0 == pthread_mutex_unlock(&queue->writer.lock));
   for (int i = 0; i < 10000; ++i) {
      if (1 != cmpxchg_atomicint(&queue->closed, 0, 0)) break;
      sched_yield();
   }
   // thr_lock is waiting on readcond
   TEST(2 == cmpxchg_atomicint(&queue->closed, 0, 0));
   TEST(0 == pthread_mutex_lock(&queue->reader.lock));
   TEST(0 == pthread_cond_signal(&queue->reader.cond));
   for (int i = 0; i < 10; ++i) {
      sched_yield();
      TEST(2 == cmpxchg_atomicint(&queue->closed, 0, 0));
   }
   TEST(0 == pthread_mutex_unlock(&queue->reader.lock));
   TEST(0 == pthread_join(thr, 0));
   TEST(3 == cmpxchg_atomicint(&queue->closed, 0, 0));
   PASS();

   // TEST delete_iqueue
   TEST(0 == delete_iqueue(&queue));
   TEST(0 == queue);
   PASS();

   // TEST new_iqueue: different size parameter
   for (size_t s = 1; s < 65536; s <<= 1, s += 33) {
      TEST(0 == new_iqueue(&queue, s));
      TEST(0 != queue);
      TEST(s == queue->size);
      TEST(0 == queue->readpos);
      TEST(0 == queue->writepos);
      TEST(0 == queue->reader.waitcount);
      TEST(0 == queue->writer.waitcount);
      TEST(0 == queue->closed);
      for (size_t i = 0; i < queue->size; ++i) {
         TEST(0 == queue->msg[i]);
      }
      TEST(0 == delete_iqueue(&queue));
      TEST(0 == queue);
   }
   PASS();

   // TEST new_iqueue: EINVAL
   TEST(EINVAL == new_iqueue(&queue, 0));
   TEST(EINVAL == new_iqueue(&queue, ((size_t)-1 - sizeof(iqueue_t))/sizeof(void*)));
   PASS();

   // TEST new_iqueue: ENOMEM
   TEST(ENOMEM == new_iqueue(&queue, (size_t)-1/(2*sizeof(void*))));
   PASS();
}

static void* thread_simulate_read(void * param)
{
   iqueue_t * queue = param;

   TEST(0 == queue->reader.waitcount);
   TEST(0 == pthread_mutex_lock(&queue->reader.lock));
   size_t pos = queue->readpos;
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
   iqueue_t * queue = 0;
   iqmsg_t    msg[10];
   pthread_t  thr;

   // prepare
   TEST(0 == new_iqueue(&queue, 10));

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
   for (unsigned i = 0; i < 10; ++i) {
      unsigned n = (i+1) % queue->size;
      TEST(0 == queue->msg[i]);
      TEST(0 == trysend_iqueue(queue, &msg[i]));
      TEST(10 == queue->size);
      TEST(0 == queue->readpos);
      TEST(n == queue->writepos);
      TEST(0 == queue->reader.waitcount);
      TEST(0 == queue->writer.waitcount);
      TEST(0 == queue->closed);
      TEST(&msg[i] == queue->msg[i]);
   }
   PASS();

   // TEST trysend_iqueue: EAGAIN
   TEST(EAGAIN == trysend_iqueue(queue, &msg[1]));
   TEST(&msg[0] == queue->msg[0]);
   TEST(0 == queue->writepos);
   PASS();

   // TEST trysend_iqueue: wakeup waiting reader
   memset(queue->msg, 0, sizeof(queue->msg[0]) * queue->size);
   for (unsigned i = 0; i < 10; ++i) {
      queue->readpos = i;
      TEST(0 == pthread_create(&thr, 0, &thread_simulate_read, queue));
      for (int wc = 0; wc < 10000; ++wc) {
         sched_yield();
         if (add_atomicsize(&queue->reader.waitcount, 0)) break;
      }
      TEST(1 == add_atomicsize(&queue->reader.waitcount, 0));
      TEST(0 == trysend_iqueue(queue, &msg[i]));
      for (int wc = 0; wc < 10000; ++wc) {
         sched_yield();
         if (0 == add_atomicsize(&queue->reader.waitcount, 0)) break;
      }
      TEST(0 == add_atomicsize(&queue->reader.waitcount, 0));
      TEST(0 == pthread_join(thr, 0));
   }
   memset(queue->msg, 0, sizeof(queue->msg[0]) * queue->size);
   queue->readpos = 0;
   PASS();

   // unprepare
   TEST(0 == delete_iqueue(&queue));
}

static void* thread_call_send(void * param)
{
   iqueue_t * queue = param;

   TEST(0 == queue->writer.waitcount);
   TEST(0 == pthread_mutex_lock(&queue->writer.lock));
   size_t pos = queue->writepos;
   iqmsg_t * msg = queue->msg[pos];
   TEST(0 == pthread_mutex_unlock(&queue->writer.lock));

   TEST(0 == send_iqueue(queue, msg));

   return 0;
}

static void test_send_single(void)
{
   iqueue_t * queue = 0;
   iqmsg_t    msg[10];
   pthread_t  thr;

   // prepare
   TEST(0 == new_iqueue(&queue, 10));

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
   for (unsigned i = 0; i < 10; ++i) {
      unsigned n = (i+1) % queue->size;
      TEST(0 == queue->msg[i]);
      TEST(0 == send_iqueue(queue, &msg[i]));
      TEST(10 == queue->size);
      TEST(0 == queue->readpos);
      TEST(n == queue->writepos);
      TEST(0 == queue->reader.waitcount);
      TEST(0 == queue->writer.waitcount);
      TEST(0 == queue->closed);
      TEST(&msg[i] == queue->msg[i]);
   }
   PASS();

   // TEST send_iqueue: waits (reader is simulated)
   for (unsigned i = 0; i < 10; ++i) {
      TEST(0 == pthread_create(&thr, 0, &thread_call_send, queue));
      // simulate wrong wakeup (send does not return)
      for (int wr = 0; wr <= 5; ++wr) {
         for (int wc = 0; wc < 10000; ++wc) {
            sched_yield();
            if (add_atomicsize(&queue->writer.waitcount, 0)) break;
         }
         TEST(1 == add_atomicsize(&queue->writer.waitcount, 0));
         if (wr < 5) {
            TEST(0 == pthread_mutex_lock(&queue->writer.lock));
            TEST(0 == pthread_cond_signal(&queue->writer.cond));
            TEST(0 == pthread_mutex_unlock(&queue->writer.lock));
            for (int wc = 0; wc < 10000; ++wc) {
               // woken up
               if (0 == add_atomicsize(&queue->writer.waitcount, 0)) break;
            }
         }
      }
      TEST(1 == add_atomicsize(&queue->writer.waitcount, 0));
      // simulate reader
      queue->readpos = (i+1) % 10;
      queue->msg[i] = 0;
      // wake up writer
      pthread_mutex_lock(&queue->writer.lock);
      pthread_cond_signal(&queue->writer.cond);
      pthread_mutex_unlock(&queue->writer.lock);
      for (int wc = 0; wc < 10000; ++wc) {
         sched_yield();
         if (0 == add_atomicsize(&queue->writer.waitcount, 0)) break;
      }
      TEST(0 == add_atomicsize(&queue->writer.waitcount, 0));
      TEST(0 == pthread_join(thr, 0));
      // writer has rewritten msg
      TEST(queue->writepos == queue->readpos);
      TEST(&msg[i] == queue->msg[i]);
   }
   PASS();

   // unprepare
   TEST(0 == delete_iqueue(&queue));
}

static void test_tryrecv_single(void)
{
   iqueue_t * queue = 0;
   iqmsg_t    msg[10];
   iqmsg_t  * rcv;
   pthread_t  thr;

   // prepare
   TEST(0 == new_iqueue(&queue, 10));

   // TEST tryrecv_iqueue: EINVAL
   TEST(EINVAL == tryrecv_iqueue(queue, 0));
   PASS();

   // TEST tryrecv_iqueue: EPIPE
   queue->closed = 1;
   TEST(EPIPE == tryrecv_iqueue(queue, &rcv));
   queue->closed = 0;
   PASS();

   // fill queue
   for (unsigned i = 0; i < 10; ++i) {
      TEST(0 == trysend_iqueue(queue, &msg[i]));
   }

   // TEST tryrecv_iqueue: get from queue
   for (unsigned i = 0; i < 10; ++i) {
      unsigned n = (i+1) % queue->size;
      TEST(0 == tryrecv_iqueue(queue, &rcv));
      TEST(rcv == &msg[i]);
      TEST(10 == queue->size);
      TEST(n == queue->readpos);
      TEST(0 == queue->writepos);
      TEST(0 == queue->reader.waitcount);
      TEST(0 == queue->writer.waitcount);
      TEST(0 == queue->closed);
      TEST(0 == queue->msg[i]);
   }
   PASS();

   // TEST tryrecv_iqueue: EAGAIN
   TEST(EAGAIN == tryrecv_iqueue(queue, &rcv));
   TEST(0 == queue->readpos);
   PASS();

   // fill queue
   for (unsigned i = 0; i < 10; ++i) {
      TEST(0 == trysend_iqueue(queue, &msg[i]));
   }

   // TEST tryrecv_iqueue: wakeup waiting writer
   for (unsigned i = 0; i < 10; ++i) {
      TEST(0 == pthread_create(&thr, 0, &thread_call_send, queue));
      for (int wc = 0; wc < 10000; ++wc) {
         sched_yield();
         if (add_atomicsize(&queue->writer.waitcount, 0)) break;
      }
      TEST(1 == add_atomicsize(&queue->writer.waitcount, 0));
      TEST(0 == tryrecv_iqueue(queue, &rcv));
      TEST(rcv == &msg[i]);
      for (int wc = 0; wc < 10000; ++wc) {
         sched_yield();
         if (0 == add_atomicsize(&queue->writer.waitcount, 0)) break;
      }
      TEST(0 == add_atomicsize(&queue->writer.waitcount, 0));
      TEST(0 == pthread_join(thr, 0));
   }
   PASS();

   // unprepare
   TEST(0 == delete_iqueue(&queue));
}

static void* thread_call_recv(void * param)
{
   iqueue_t * queue = param;

   TEST(0 == queue->reader.waitcount);
   iqmsg_t * rcv = 0;
   TEST(0 == recv_iqueue(queue, &rcv));
   TEST(0 != rcv);

   return 0;
}

static void test_recv_single(void)
{
   iqueue_t * queue = 0;
   iqmsg_t    msg[10];
   iqmsg_t  * rcv;
   pthread_t  thr;

   // prepare
   TEST(0 == new_iqueue(&queue, 10));

   // TEST recv_iqueue: EINVAL
   TEST(EINVAL == recv_iqueue(queue, 0));
   PASS();

   // TEST recv_iqueue: EPIPE
   queue->closed = 1;
   TEST(EPIPE == recv_iqueue(queue, &rcv));
   queue->closed = 0;
   PASS();

   // fill queue
   for (unsigned i = 0; i < 10; ++i) {
      TEST(0 == trysend_iqueue(queue, &msg[i]));
   }

   // TEST recv_iqueue: get from queue
   for (unsigned i = 0; i < 10; ++i) {
      unsigned n = (i+1) % queue->size;
      TEST(0 == recv_iqueue(queue, &rcv));
      TEST(rcv == &msg[i]);
      TEST(10 == queue->size);
      TEST(n == queue->readpos);
      TEST(0 == queue->writepos);
      TEST(0 == queue->reader.waitcount);
      TEST(0 == queue->writer.waitcount);
      TEST(0 == queue->closed);
      TEST(0 == queue->msg[i]);
   }
   PASS();

   // TEST recv_iqueue: waits (writer is simulated)
   for (unsigned i = 0; i < 10; ++i) {
      TEST(0 == pthread_create(&thr, 0, &thread_call_recv, queue));
      // simulate wrong wakeup (recv does not return)
      for (int wr = 0; wr <= 5; ++wr) {
         for (int wc = 0; wc < 10000; ++wc) {
            sched_yield();
            if (add_atomicsize(&queue->reader.waitcount, 0)) break;
         }
         TEST(1 == add_atomicsize(&queue->reader.waitcount, 0));
         if (wr < 5) {
            TEST(0 == pthread_mutex_lock(&queue->reader.lock));
            TEST(0 == pthread_cond_signal(&queue->reader.cond));
            TEST(0 == pthread_mutex_unlock(&queue->reader.lock));
            for (int wc = 0; wc < 10000; ++wc) {
               // woken up
               if (0 == add_atomicsize(&queue->reader.waitcount, 0)) break;
            }
         }
      }
      TEST(1 == add_atomicsize(&queue->reader.waitcount, 0));
      // simulate writer
      queue->writepos = (i+1) % 10;
      queue->msg[i] = &msg[i];
      // wake up reader
      pthread_mutex_lock(&queue->reader.lock);
      pthread_cond_signal(&queue->reader.cond);
      pthread_mutex_unlock(&queue->reader.lock);
      for (int wc = 0; wc < 10000; ++wc) {
         sched_yield();
         if (0 == add_atomicsize(&queue->reader.waitcount, 0)) break;
      }
      TEST(0 == add_atomicsize(&queue->reader.waitcount, 0));
      TEST(0 == pthread_join(thr, 0));
      // reader has removed msg
      TEST(queue->writepos == queue->readpos);
      TEST(0 == queue->msg[i]);
   }
   PASS();

   // unprepare
   TEST(0 == delete_iqueue(&queue));
}

static void* thread_epipe_send(void * queue)
{
   iqmsg_t msg;
   int err = send_iqueue(queue, &msg);
   if (err != EPIPE) {
      printf("wrong err = %d\n", err);
   }
   TEST(EPIPE == err);
   return 0;
}

static void* thread_epipe_recv(void * queue)
{
   iqmsg_t* msg = 0;
   int err = recv_iqueue(queue, &msg);
   if (err != EPIPE) {
      printf("wrong err = %d\n", err);
   }
   TEST(EPIPE == err);
   return 0;
}

static void test_close(void)
{
   iqueue_t * queue = 0;
   iqmsg_t    msg;
   pthread_t  thr[100];

   // prepare

   // TEST close_iqueue: sets closed
   TEST(0 == new_iqueue(&queue, 1));
   close_iqueue(queue);
   TEST(1 == queue->closed);
   TEST(0 == delete_iqueue(&queue));
   PASS();

   // TEST close_iqueue: wakes up waiting reader and writer
   // prepare
   TEST(0 == new_iqueue(&queue, 1));
   TEST(0 == send_iqueue(queue, &msg));
   for (unsigned i = 0; i < 50; ++i) {
      TEST(0 == pthread_create(&thr[i], 0, &thread_epipe_send, queue));
      for (int wc = 0; wc < 10000; ++wc) {   // wait until started
         sched_yield();
         if (i+1 == cmpxchg_atomicsize(&queue->writer.waitcount, 0, 0)) break;
      }
   }
   sched_yield();
   TEST(50 == cmpxchg_atomicsize(&queue->writer.waitcount, 0, 0));
   queue->msg[0] = 0; // simulate reading msg without signalling waiting writers
   TEST(50 == cmpxchg_atomicsize(&queue->writer.waitcount, 50, 0));
   for (int i = 0; i < 50; ++i) {
      TEST(0 == pthread_create(&thr[50+i], 0, &thread_epipe_recv, queue));
   }
   for (int i = 0; i < 10000; ++i) {   // wait until all threads wait
      sched_yield();
      if (50 == cmpxchg_atomicsize(&queue->reader.waitcount, 0, 0)) break;
   }
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
   TEST(0 == new_iqueue(&queue, 1));
   TEST(0 == send_iqueue(queue, &msg));
   for (unsigned i = 0; i < 50; ++i) {
      TEST(0 == pthread_create(&thr[i], 0, &thread_epipe_send, queue));
      for (int wc = 0; wc < 10000; ++wc) {   // wait until started
         sched_yield();
         if (i+1 == cmpxchg_atomicsize(&queue->writer.waitcount, 0, 0)) break;
      }
   }
   sched_yield();
   TEST(50 == cmpxchg_atomicsize(&queue->writer.waitcount, 0, 0));
   queue->msg[0] = 0; // simulate readin of msg without signalling waiting writers
   TEST(50 == cmpxchg_atomicsize(&queue->writer.waitcount, 50, 0));
   for (int i = 0; i < 50; ++i) {
      TEST(0 == pthread_create(&thr[50+i], 0, &thread_epipe_recv, queue));
   }
   for (int i = 0; i < 10000; ++i) {   // wait until all threads wait
      sched_yield();
      if (50 == cmpxchg_atomicsize(&queue->reader.waitcount, 0, 0)) break;
   }
   // test
   TEST(50 == cmpxchg_atomicsize(&queue->reader.waitcount, 0, 0));
   TEST(0 == cmpxchg_atomicsize(&queue->writer.waitcount, 0, 50));
   TEST(0 == delete_iqueue(&queue));
   for (int i = 0; i < 100; ++i) {
      TEST(0 == pthread_join(thr[i], 0));
   }
   PASS();
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

      test_initfree();
      test_trysend_single();
      test_send_single();
      test_tryrecv_single();
      test_recv_single();
      test_close();

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
