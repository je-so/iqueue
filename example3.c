#include "iqueue.h"
#include <stdio.h>

struct echomsg_t {
   const char* str;  // in param
   iqueue1_t*  processed; // used to signal ready
};

iqueue_DECLARE(echoqueue, struct echomsg_t)

void* server(void* queue)
{
   struct echomsg_t* msg = 0;
   echoqueue_t* echoqueue = queue;
   while (0 == recv_echoqueue(echoqueue, &msg)) {
      printf("Echo: %s\n", msg->str);
      // Signal client
      send_iqueue1(msg->processed, msg);
   }
   return 0;
}

void* client(void* queue)
{
   iqueue1_t*   processed;
   new_iqueue1(&processed, 1);
   echoqueue_t* echoqueue = queue;
   struct echomsg_t msg = { "Hello Server", processed };
   send_echoqueue(echoqueue, &msg);
   // Wait for server
   void* msg2 = 0;
   recv_iqueue1(processed, &msg2);
   if (msg2 == &msg) {
      printf("Client: msg has been processed\n");
   }
   delete_iqueue1(&processed);
   return 0;
}

int main(void)
{
   echoqueue_t queue;
   pthread_t cthr, sthr;
   init_echoqueue(&queue, 1);
   pthread_create(&sthr, 0, &server, &queue);
   pthread_create(&cthr, 0, &client, &queue);
   pthread_join(cthr, 0);
   free_echoqueue(&queue);
   pthread_join(sthr, 0);
   return 0;
}
