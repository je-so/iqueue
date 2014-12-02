// Measure raw speed of message transfer of 1000000 raw pointer
#include "iqueue.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

// ====================

void server1(iqueue1_t* queue, int nrops)
{
   for (int i = 1; i <= nrops; ++i) {
      void* msg;
      while (tryrecv_iqueue1(queue, &msg)) ;
   }
}

void client1(iqueue1_t* queue, int nrops)
{
   for (int i = 1; i <= nrops; ++i) {
      while (trysend_iqueue1(queue, (void*)(intptr_t)i)) ;
   }
}

void server2(iqueue_t* queue, int nrops)
{
   for (int i = 1; i <= nrops; ++i) {
      void* msg;
      while (tryrecv_iqueue(queue, &msg)) ;
   }
}

void client2(iqueue_t* queue, int nrops)
{
   for (int i = 1; i <= nrops; ++i) {
      while (trysend_iqueue(queue, (void*)(intptr_t)i)) ;
   }
}

// ===================== 

// customize iperf perfomance test framework

iqueue1_t* s_queue1;
iqueue_t*  s_queue2;

typedef struct iperf_param_t {
   int    tid; // threadid or processid of test instance (0,1,2,...)
   int    nrinstance; // nr of all test instances (threads or processes)
   int    nrops; // initially set to 1; should be overwritten by prepare
                 // reflects the number of performaned "operations"
} iperf_param_t;

int iperf_prepare(iperf_param_t* param)
{
   int err = 0;
   param->nrops = 1000000;

   if (!s_queue1 && !s_queue2) {
      if (param->nrinstance <= 2) {
         err = new_iqueue1(&s_queue1, 1000000);
      } else {
         err = new_iqueue(&s_queue2, 1000000);
      }
   }

   return err;
}

int iperf_run(iperf_param_t* param)
{
   // performs nrops recv or send operations
   if (s_queue1) {
      if (0 == (param->tid%2)) {
         server1(s_queue1, param->nrops);
      } else {
         client1(s_queue1, param->nrops);
      }
   } else {
      if (0 == (param->tid%2)) {
         server2(s_queue2, param->nrops);
      } else {
         client2(s_queue2, param->nrops);
      }
   }
   return 0;
}

// =====================

// following source adapted and taken from testcode/iperf 

typedef struct instance_t {
   pthread_t     thr;
   pid_t         child;
   iperf_param_t param;
   int preparedfd;
   int startfd;
   int resultfd;
} instance_t;

static int nrinstance;
static int preparedfd[2];
static int startfd[2];
static int resultfd[2];
static instance_t* instance = 0;
static struct timeval starttime;
static struct timeval endtime;

static void print_error(int tid, int err)
{
   fprintf(stderr, "\nERROR %d (tid: %d): %s\n", err, tid, strerror(err));
}

static void abort_test(int tid, int err)
{
   print_error(tid, err);
   exit(err);
}

static void* instance_main(void* _param)
{
   int err;
   ssize_t bytes;
   instance_t* param = _param;

   err = iperf_prepare(&param->param);
   if (err) abort_test(param->param.tid, err);

   // signal waiting starter that instance is prepared
   bytes = write(param->preparedfd, "", 1);
   if (bytes != 1) {
      abort_test(param->param.tid, EIO);
   }

   // wait for start signal
   char dummy;
   bytes = read(param->startfd, &dummy, 1);
   if (bytes != 1) {
      abort_test(param->param.tid, EIO);
   }

   err = iperf_run(&param->param);
   if (err) abort_test(param->param.tid, err);

   // signal result (time) to waiting starter
   struct timeval now;
   gettimeofday(&now, 0);
   char result[64] = {0};
   sprintf(result, "%d %lld %lld", param->param.nrops, (long long)now.tv_sec, (long long)now.tv_usec);
   bytes = write(param->resultfd, result, sizeof(result));
   if (bytes != sizeof(result)) {
      abort_test(param->param.tid, EIO);
   }

   // printf("tid: %d time: %s\n", param->param.tid, result);

   return 0;
}

static void prepare_instances(void)
{
   int err;

   err = pipe(preparedfd);
   if (err) abort_test(-1, errno);
   err = pipe(startfd);
   if (err) abort_test(-1, errno);
   err = pipe(resultfd);
   if (err) abort_test(-1, errno);

   // start all instances
   for (int tid = 0; tid < nrinstance; ++tid) {
      instance[tid].param.tid = tid;
      instance[tid].param.nrinstance = nrinstance;
      instance[tid].param.nrops = 1;
      instance[tid].preparedfd = preparedfd[1];
      instance[tid].startfd = startfd[0];
      instance[tid].resultfd = resultfd[1];

      err = pthread_create(&instance[tid].thr, 0, &instance_main, &instance[tid]);
      if (err) abort_test(-1, err);
   }

   // wait until all instances prepared themselves
   for (int tid = 0; tid < nrinstance; ++tid) {
      char byte;
      ssize_t bytes = read(preparedfd[0], &byte, 1);
      if (bytes != 1) {
         abort_test(-1, EIO);
      }
   }
}

static void run_instances(/*out*/long long* nrops_total)
{
   gettimeofday(&starttime, 0);

   // send start signal
   {
      char buffer[256];
      ssize_t bytes = write(startfd[1], buffer, (size_t)nrinstance);
      if (bytes != nrinstance) {
         abort_test(-1, EIO);
      }
   }

   // wait for result
   *nrops_total = 0;
   for (int tid = 0; tid < nrinstance; ++tid) {
      char result[64];
      ssize_t bytes = read(resultfd[0], result, sizeof(result));
      if (bytes != sizeof(result)) {
         abort_test(-1, EIO);
      }
      int nrops = 0;
      long long sec = 0;
      long long usec = -1;
      sscanf(result, "%d %lld %lld", &nrops, &sec, &usec);
      if (usec == -1) {
         abort_test(-1, EINVAL);
      }
      if ( tid == 0
           || ( endtime.tv_sec < sec
                || (endtime.tv_sec == sec && endtime.tv_usec < usec))) {
         endtime.tv_sec = (time_t) sec;
         endtime.tv_usec = (suseconds_t) usec;
      }
      *nrops_total += nrops;
   }

   // wait for end of instance
   for (int tid = 0; tid < nrinstance; ++tid) {
      pthread_join(instance[tid].thr, 0);
   }

   for (int i = 0; i < 2; ++i) {
      close(preparedfd[i]);
      close(startfd[i]);
      close(resultfd[i]);
   }
}

int main(int argc, const char* argv[])
{
   int err = EINVAL;

   if (argc == 2) {
      sscanf(argv[1], "%d", &nrinstance);
      nrinstance = (nrinstance + 1) & ~0x1; // make nrinstance even
      if (2 <= nrinstance && nrinstance <= 256) err = 0;
   }

   if (err) {
      printf("Usage: %s [nr-threads]\n", argv[0]);
      printf("With: 1 < nr-threads < 257\n");
      exit(err);
   }

   printf("Run %d test threads (%d clients / %d servers)\n", nrinstance, nrinstance/2, nrinstance/2);

   instance = (instance_t*) malloc(sizeof(instance_t) * (size_t)nrinstance);
   if (! instance) err = ENOMEM;

   if (err) {
      print_error(-1, err);

   } else {
      prepare_instances();
      long long nrops = 1;
      run_instances(&nrops);

      long long sec = endtime.tv_sec - starttime.tv_sec;
      long long usec = endtime.tv_usec - starttime.tv_usec;
      usec = 1000000ll * sec + usec;
      printf("\nRESULT: %lld usec for %lld operations (%lld operations/msec)\n", usec, nrops, nrops*1000ll/usec);
   }

   return err;
}
