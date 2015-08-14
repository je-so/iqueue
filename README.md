iqueue
======

C implementation of interthread message queue. It is implemented without locks (lock-free). 
It is designed to allow for zero-copy message transfer. Only a pointer to the message is transfered. The message itself is not copied.

The names of the lock-free functions begin with a try (trysend_iqueue and tryrecv_iqueue). There are also blocking versions named send_iqueue / recv_iqueue which use pthread condition variables to wait for the queue becoming nonfull or non-empty.

**iqueue1_t:** This type supports a single reader thread and a single writer thread.
It is up to 8 times faster than type iqueue_t.

**iqueue_t:** This type supports multiple readers and writers. Which makes it necessary
to synchronize more state. Compare [trysend_iqueue](https://github.com/je-so/iqueue/blob/master/src/iqueue.c#L222) with [trysend_iqueue1](https://github.com/je-so/iqueue/blob/master/src/iqueue.c#L443).

To prevent [false sharing](http://en.wikipedia.org/wiki/False_sharing) the size of all variables are padded up to the size of one cache line. The following list shows the performance on a 2 GHZ x86 quad core (see [example4.c](example4.c)):
* (iqueue1_t) **40000** (unpadded **5500**) msg/msec; messages transfered from one client to one server.
* (iqueue_t) **3000** (unpadded **1500**) msg/msec; performance drops in case of 4 threads (2 clients + 2 servers).
* (iqueue_t) **6000** (unpadded **2000**) msg/msec; performance settles to this value in case of 44 threads.

The value [SIZE_CACHELINE](include/iqueue.h#L19) defines the size of one cache line. If this value is undefined no padding is done at all.

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

Then the client checks that every message has been processed correctly.


```C
#include "iqueue.h"
#include <assert.h>
#include <stdio.h>

struct addmsg_t {
   iqsignal_t* sign;
   int arg1, arg2; // in param
   int sum;        // out param
};

void* server(void* queue)
{
   void* msg = 0;
   while (0 == recv_iqueue(queue, &msg)) {
      ((struct addmsg_t*)msg)->sum  = ((struct addmsg_t*)msg)->arg1;
      ((struct addmsg_t*)msg)->sum += ((struct addmsg_t*)msg)->arg2;
      signal_iqsignal(((struct addmsg_t*)msg)->sign);
   }
   return 0;
}

void* client(void* queue)
{
   iqsignal_t signal;
   struct addmsg_t msg[3] = {
      { &signal, 1, 2, 0 },
      { &signal, 3, 4, 0 },
      { &signal, 5, 6, 0 }
   };
   init_iqsignal(&signal);
   for (int i = 0; i < 3; ++i) {
      send_iqueue(queue, &msg[i]);
   }
   // busy wait
   while (3 != signalcount_iqsignal(&signal)) {
      // ... process other things ...
   }
   for (int i = 0; i < 3; ++i) {
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

## Example with Statically Typed Messages

This example shows the usage of macro *iqueue_DECLARE*. It is used to make an iqueue_t type safe. It redeclares the iqueue interface as yyy_echoqueue and declares type echoqueue_t which contains a pointer to iqueue_t. The redeclared interface processes messages of type *struct echomsg_t** instead of generic type void*. This example also uses the single reader/writer queue iqueue1_t to signal the client of the fact that the server has processed the message.

```C
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
```

## Internal Workings ##

This version is inspired by http://moodycamel.com/blog/2014/detailed-design-of-a-lock-free-queue.
iqueue_t uses a ring buffer to store its messages. Its size is always a power of two. 

For every read or write a counter readpos/writepos is incremented atomically so that no two threads read from or write to the same position. If a thread has read or written its msg into the buffer the number of free or used items is incremented by one. The number of free/used items are used to determine if a read or write is possible.

There is a possible race. A thread increments readpos/writepos and gets preempted before it could read from or write the msg into the buffer. Another thread reads or writes and increments the number of free/used items. Then another thread tries to overwrite to the not read message or read the not written message.

To prevent this race a reading thread waits until the message slot in buffer is not **null** and sets the slot to **null** after reading atomically. A writer waits until the slot becomes null and then writes the message atomically.

Therefore iqueue_t does not support sending **null** pointers as message.

To make iqueue_t fast the number of free/used items in the ring buffer are managed by an array of values (see sizeused/sizefree in https://github.com/je-so/iqueue/blob/master/include/iqueue.h#L30). This allows the use of simple atomic decrement operations without worrying about over decrementing (Whishlist: Atomic increment/decrement operations which do not increment beyond a MAX value and which do not decrement below 0).

If you use more than 128 threads you should increment the size of these arrays.
