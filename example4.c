// Measure raw speed of message transfer of 1000000 raw pointer
// Use » time ./example4 «
// Change recv_iqueue1/send_iqueue1 into recv_iqueue/send_iqueue
// to compare time with type iqueue_t 
// (iqueue1_t is compatible with iqueue_t).
#include "iqueue.h"
#include <errno.h>
#include <stdio.h>

void* server(void* queue)
{
   void* msg = 0;
   size_t counter = 0;
   while (0 == recv_iqueue1(queue, &msg)) {
       // do nothing: measure only raw speed
       ++counter;
       if (counter == 1000000) break;
   }
   return 0;
}

void* client(void* queue)
{
   for (uintptr_t i = 1; i <= 1000000; ++i) {
      send_iqueue1(queue, (void*)i);
   }
   return 0;
}

int main(void)
{
   iqueue1_t* queue;
   pthread_t cthr, sthr;
   new_iqueue1(&queue, 10000);
   pthread_create(&sthr, 0, &server, queue);
   pthread_create(&cthr, 0, &client, queue);
   pthread_join(cthr, 0);
   pthread_join(sthr, 0);
   return 0;
}
