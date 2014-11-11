iqueue
======

C implementation of interthread message queue.

It is implemented without locks (lock-free)
except for waiting/blocking functions.
It is designed to allow for zero-copy message transfer. Only a pointer to the message is transfered. The message itself is not copied.

**iqueue1_t:** This type supports a single reader thread and a single writer thread.
It is up to 2 times faster than type iqueue_t (see [example4.c](example4.c)).

**iqueue_t:** This type supports multiple readers and writers. Which makes it necessary
to synchronize more state. Compare [trysend_iqueue](https://github.com/je-so/iqueue/blob/master/src/iqueue.c#L222) with [trysend_iqueue1](https://github.com/je-so/iqueue/blob/master/src/iqueue.c#L443).


The following examples use iqueue_t.

## Client Server Example

The client sends an *echomsg_t* and waits until it has been processed. The server responds to the client request with
an error code and signals the client if the message has been processed.
The helper type *iqsignal_t* supports the notification of a client whenever a message has been processed.

```C
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
```

## Example with Busy Waiting

A queue with size 3 is created. The client pushes 3 messages into the queue without waiting.
The client waits in a busy loop until all messages has been processed. It does not call 
*wait_iqsignal* but uses *signalcount_iqsignal* to check for all 3 messages.

Then it checks that every message has been processed correctly.


```C
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
   void* msg = 0;
   while (0 == recv_iqueue(queue, &msg)) {
      ((struct addmsg_t*)msg)->sum  = ((struct addmsg_t*)msg)->arg1;
      ((struct addmsg_t*)msg)->sum += ((struct addmsg_t*)msg)->arg2;
      setprocessed_iqmsg(&((struct addmsg_t*)msg)->header);
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
      send_iqueue(queue, &msg[i]);
   }
   // busy wait
   while (3 != signalcount_iqsignal(&signal)) {
      // ... process other things ...
   }
   for (int i = 0; i < 3; ++i) {
      assert(isprocessed_iqmsg(&msg[i].header));
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
```

## Example with Unix Signals and Statically Typed Messages

Shows usage of macro *iqueue_DECLARE*. Use it to make an iqueue_t type safe. It redeclares the fct_iqueue interface as fct_echoqueue and declares type echoqueue_t which mirrors iqueue_t. The redeclared interface processes messages of type *struct echomsg_t** instead of generic type void*. This example also shows the usage of Unix signals to signal the client of the fact that the server has processed the message.

```C

#define _GNU_SOURCE
#include "iqueue.h"
#include <stdio.h>
#include <signal.h>

struct echomsg_t {
   const char* str;  // in param
   pthread_t thread; // used to signal ready
};

// declare type echoqueue_t and interface fct_echoqueue 
// which processes messages of type struct echomsg_t
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
```

## Internal Workings ##

A new version is coming inspired by http://moodycamel.com/blog/2014/detailed-design-of-a-lock-free-queue.

