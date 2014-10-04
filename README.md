iqueue
======

C implementation of interthread message queue.

It is implemented without locks (lock-free)
except for waiting/blocking functions.
It is designed to allow for zero-copy message transfer.

**iqueue1_t:** This type supports a single reader thread and a single writer thread.
It is up to 4 times faster than type iqueue_t (see [example4.c](example4.c)).

**iqueue_t:** This type supports multiple readers and writers. Which makes it necessary
to synchronize more state. Compare [trysend_nowakeup_iqueue](https://github.com/je-so/iqueue/blob/master/src/iqueue.c#L217) with [trysend_nowakeup_iqueue1](https://github.com/je-so/iqueue/blob/master/src/iqueue.c#L372).


The following examples use iqueue_t.

## Client Server Example

Only a pointer to the message is transfered. The message itself
is not copied. The server responds to the client request with
an error code and signals the client if the message is processed.

```C
#include "iqueue.h"
#include <stdio.h>

struct echomsg_t {
   iqmsg_t header;
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
      // signal client it is safe to delete msg
      setprocessed_iqmsg(&((struct echomsg_t*)msg)->header);
   }
   return 0;
}

void* client(void* queue)
{
   iqsignal_t signal;
   struct echomsg_t msg = { iqmsg_INIT(&signal), "Hello Server", 1 };
   send_iqueue(queue, &msg);
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
```

## Example with Busy Waiting

A queue with size 3 is created. The client pushes 3 messages into the queue without waiting.
The client waits in a busy loop until all messages has been processed.
It checks the results and exits.

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
```

## Example with Unix Signals and Statically Typed Messages

Same as first example. Only the client is signalled with help of Unix signals.
Also macro iqueue_DECLARE is used to declare type echoqueue_t which supports
messages of type *struct echomsg_t*.

```C

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
```

## Internal Workings ##

The diagram shows the queue ring buffer which stores pointers to messages. A value of NULL indicates a free entry.
Additional queue state is stored in the two logic variables *next* and *size*. Both logic variables are encoded 
in one physical variable named next\_size to allow for a single *atomic_compare_exchange* operation.

![](https://github.com/je-so/testcode/blob/master/img/iqueue.png)
