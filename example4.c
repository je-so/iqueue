// Measure raw speed of message transfer of 1000000 raw pointer
#include "iqueue.h"
#include <errno.h>
#include <stdio.h>
#include <sys/time.h>

static iqueue1_t* s_queue1;
static iqueue_t*  s_queue2;
static struct timeval starttime[2];
static struct timeval endtime[2];

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
      // do nothing: measure only raw speed
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
   pthread_t cthr, sthr;
   pthread_t cthr2, sthr2;
   new_iqueue1(&s_queue1, 10000);
   pthread_create(&sthr, 0, &server1, 0);
   pthread_create(&cthr, 0, &client1, 0);
   pthread_join(cthr, 0);
   pthread_join(sthr, 0);
   delete_iqueue1(&s_queue1);
   time_t sec = endtime[0].tv_sec - starttime[0].tv_sec;
   long  usec = endtime[0].tv_usec - starttime[0].tv_usec;
   long msec = 1000 * (long)sec + usec / 1000;
   printf("iqueue1_t: 1000000 send/recv time in ms: %ld\n", msec);

   new_iqueue(&s_queue2, 10000);
   pthread_create(&sthr, 0, &server2, 0);
   pthread_create(&cthr, 0, &client2, 0);
   pthread_create(&sthr2, 0, &server2, (void*)1);
   pthread_create(&cthr2, 0, &client2, (void*)1);
   pthread_join(cthr, 0);
   pthread_join(sthr, 0);
   pthread_join(cthr2, 0);
   pthread_join(sthr2, 0);
   delete_iqueue(&s_queue2);
   if (starttime[0].tv_sec > starttime[1].tv_sec || (starttime[0].tv_sec == starttime[1].tv_sec && starttime[0].tv_usec > starttime[1].tv_usec)) {
      starttime[0] = starttime[1];
   }
   if (endtime[0].tv_sec < endtime[1].tv_sec || (endtime[0].tv_sec == endtime[1].tv_sec && endtime[0].tv_usec < endtime[1].tv_usec)) {
      endtime[0] = endtime[1];
   }
   sec = endtime[0].tv_sec - starttime[0].tv_sec;
   usec = endtime[0].tv_usec - starttime[0].tv_usec;
   msec = 1000 * (long)sec + usec / 1000;
   printf("iqueue_t: 2*1000000 send/recv time in ms: %ld\n", msec);
   return 0;
}
