// Measure raw speed of message transfer of 1000000 raw pointer
#include "iqueue.h"
#include <errno.h>
#include <stdio.h>
#include <sys/time.h>

#define MAXTHREAD 2

static iqueue1_t* s_queue1;
static iqueue_t*  s_queue2;
static struct timeval starttime[MAXTHREAD];
static struct timeval endtime[MAXTHREAD];

void* server1(void* id)
{
   void* msg = 0;
   size_t counter = 0;
   gettimeofday(&starttime[(int)id], 0);
   for (;;) {
      while (tryrecv_iqueue1(s_queue1, &msg)) ;
      // do nothing: measure only raw speed
      ++counter;
      if (counter == 1000000) break;
   }
   return 0;
}

void* client1(void* id)
{
   for (uintptr_t i = 1; i <= 1000000; ++i) {
      while (trysend_iqueue1(s_queue1, (void*)i)) ;
   }
   gettimeofday(&endtime[(int)id], 0);
   return 0;
}

void* server2(void* id)
{
   void* msg = 0;
   size_t counter = 0;
   gettimeofday(&starttime[(int)id], 0);
   for (;;) {
      while (tryrecv_iqueue(s_queue2, &msg)) ;
      // do nothing: measure only raw transfer speed
      ++counter;
      if (counter == 1000000) break;
   }
   return 0;
}

void* client2(void* id)
{
   for (uintptr_t i = 1; i <= 1000000; ++i) {
      while (trysend_iqueue(s_queue2, (void*)i)) ;
   }
   gettimeofday(&endtime[(int)id], 0);
   return 0;
}

int main(void)
{
   pthread_t cthr1, sthr1;
   pthread_t cthr2[MAXTHREAD], sthr2[MAXTHREAD];

   new_iqueue1(&s_queue1, 10000);
   pthread_create(&sthr1, 0, &server1, 0);
   pthread_create(&cthr1, 0, &client1, 0);
   pthread_join(cthr1, 0);
   pthread_join(sthr1, 0);
   delete_iqueue1(&s_queue1);
   time_t sec = endtime[0].tv_sec - starttime[0].tv_sec;
   long  usec = endtime[0].tv_usec - starttime[0].tv_usec;
   long msec = 1000 * (long)sec + usec / 1000;
   printf("iqueue1_t: 1000000 send/recv time in ms: %ld\n", msec);

   new_iqueue(&s_queue2, 10000);
   for (int i = 0; i < MAXTHREAD; ++i) {
      pthread_create(&sthr2[i], 0, &server2, (void*)i);
      pthread_create(&cthr2[i], 0, &client2, (void*)i);
   }
   for (int i = 0; i < MAXTHREAD; ++i) {
      pthread_join(cthr2[i], 0);
      pthread_join(sthr2[i], 0);
   }
   delete_iqueue(&s_queue2);
   for (int i = 1; i < MAXTHREAD; ++i) {
      if (starttime[0].tv_sec > starttime[i].tv_sec || (starttime[0].tv_sec == starttime[i].tv_sec && starttime[0].tv_usec > starttime[i].tv_usec)) {
         starttime[0] = starttime[i];
      }
   }
   for (int i = 1; i < MAXTHREAD; ++i) {
      if (endtime[0].tv_sec < endtime[i].tv_sec || (endtime[0].tv_sec == endtime[i].tv_sec && endtime[0].tv_usec < endtime[i].tv_usec)) {
          endtime[0] = endtime[i];
      }
   }
   sec = endtime[0].tv_sec - starttime[0].tv_sec;
   usec = endtime[0].tv_usec - starttime[0].tv_usec;
   msec = 1000 * (long)sec + usec / 1000;
   printf("iqueue_t: %d*1000000 send/recv time in ms: %ld\n", MAXTHREAD, msec);

   return 0;
}
