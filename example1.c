#include "iqueue.h"
#include <stdio.h>

struct echomsg_t {
   iqsignal_t  sign;
   const char* str; // in param
   int err;         // out param
};

void* server(void* queue)
{
   void* msg = 0;
   while (0 == recv_iqueue(queue, &msg)) {
      printf("Echo: %s\n", ((struct echomsg_t*)msg)->str);
      // set return value (no error)
      ((struct echomsg_t*)msg)->err = 0;
      // signal client msg has been processed
      signal_iqsignal(&((struct echomsg_t*)msg)->sign);
   }
   return 0;
}

void* client(void* queue)
{
   struct echomsg_t msg;
   init_iqsignal(&msg.sign);
   msg.str = "Hello Server";
   msg.err = 1;
   send_iqueue(queue, &msg);
   wait_iqsignal(&msg.sign); // wait until msg has been processed
   return (void*) msg.err;
}

int main(void)
{
   iqueue_t* queue;
   pthread_t cthr, sthr;
   void* err;
   new_iqueue(&queue, 1);
   pthread_create(&sthr, 0, &server, queue);
   pthread_create(&cthr, 0, &client, queue);
   pthread_join(cthr, &err);
   printf("err = %d\n", (int)err);
   delete_iqueue(&queue);
   pthread_join(sthr, 0);
   return 0;
}
