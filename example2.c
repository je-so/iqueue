#include "iqueue.h"
#include <assert.h>
#include <stdio.h>

struct addmsg_t {
   iqmsg_t header;
   int arg1, arg2; // in param
   int sum;        // out param
};

void* server(void* queue)
{
   iqmsg_t* msg = 0;
   while (0 == recv_iqueue(queue, &msg)) {
      ((struct addmsg_t*)msg)->sum  = ((struct addmsg_t*)msg)->arg1;
      ((struct addmsg_t*)msg)->sum += ((struct addmsg_t*)msg)->arg2;
      setprocessed_iqmsg(msg);
   }
   return 0;
}

void* client(void* queue)
{
   iqsignal_t signal;
   struct addmsg_t msg[3] = {
      { iqmsg_INIT(&signal), 1, 2, 0 },
      { iqmsg_INIT(&signal), 3, 4, 0 },
      { iqmsg_INIT(&signal), 5, 6, 0 }
   };
   for (int i = 0; i < 3; ++i) {
      send_iqueue(queue, &msg[i].header);
   }
   // busy wait
   while (3 != signalcount_iqsignal(&signal)) {
      // ... process other things ...
   }
   for (int i = 0; i < 3; ++i) {
      assert(1 == msg[i].header.processed);
      assert(msg[i].sum == msg[i].arg1 + msg[i].arg2);
   }
   printf("Client: All messages processed\n");
   return 0;
}

int main(void)
{
   iqueue_t* queue;
   pthread_t cthr, sthr;
   new_iqueue(&queue, 3);
   pthread_create(&sthr, 0, &server, queue);
   pthread_create(&cthr, 0, &client, queue);
   pthread_join(cthr, 0);
   delete_iqueue(&queue);
   pthread_join(sthr, 0);
   return 0;
}
