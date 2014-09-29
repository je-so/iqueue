#define _GNU_SOURCE
#include "iqueue.h"
#include <stdio.h>
#include <signal.h>

struct echomsg_t {
   const char* str;  // in param
   pthread_t thread; // used to signal ready
};

iqueue_DECLARE(echoqueue, struct echomsg_t)

void* server(void* queue)
{
   struct echomsg_t* msg = 0;
   echoqueue_t* echoqueue = queue;
   while (0 == recv_echoqueue(echoqueue, &msg)) {
      printf("Echo: %s\n", msg->str);
      // Signal client
      pthread_kill(msg->thread, SIGUSR1);
   }
   return 0;
}

void* client(void* queue)
{
   echoqueue_t* echoqueue = queue;
   struct echomsg_t msg = { "Hello Server", pthread_self() };
   send_echoqueue(echoqueue, &msg);
   // Wait for server
   int signr;
   sigset_t sigset;
   sigemptyset(&sigset);
   sigaddset(&sigset, SIGUSR1);
   sigwait(&sigset, &signr);
   return 0;
}

int main(void)
{
   echoqueue_t queue;
   pthread_t cthr, sthr;
   sigset_t sigset;
   sigemptyset(&sigset);
   sigaddset(&sigset, SIGUSR1);
   sigprocmask(SIG_BLOCK, &sigset, 0);
   init_echoqueue(&queue, 1);
   pthread_create(&sthr, 0, &server, &queue);
   pthread_create(&cthr, 0, &client, &queue);
   pthread_join(cthr, 0);
   free_echoqueue(&queue);
   pthread_join(sthr, 0);
   return 0;
}
