#include "iqueue.h"
#include <stdio.h>

struct echomsg_t {
   iqmsg_t header;
   const char* str; // in param
   int err;         // out param
};

void* server(void* queue)
{
   iqmsg_t* msg = 0;
   while (0 == recv_iqueue(queue, &msg)) {
      printf("Echo: %s\n", ((struct echomsg_t*)msg)->str);
      // set return value (no error)
      ((struct echomsg_t*)msg)->err = 0;
      // signal client it is safe to delete msg
      setprocessed_iqmsg(msg);
   }
   return 0;
}

void* client(void* queue)
{
   iqsignal_t signal;
   struct echomsg_t msg = { iqmsg_INIT(&signal), "Hello Server", 1 };
   send_iqueue(queue, &msg.header);
   wait_iqsignal(&signal); // wait until msg is processed
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
