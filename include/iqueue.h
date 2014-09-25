/* iqueue.h

   Defines interface of a lock-free interthread message queue.

   Every function returning an int indicates success with
   value 0. A returned value > 0 indicates an error code.

   Copyright:
   This program is free software. See accompanying LICENSE file.
   
   Author:
   (C) 2014 Jörg Seebohn
*/
#ifndef IQUEUE_HEADER
#define IQUEUE_HEADER

typedef struct iqueue_t {
   int dummy;

} iqueue_t;

// Initializes queue
// Possible error codes: ENOMEM
int init_iqueue(/*out*/iqueue_t * queue);

// Frees resources of an initialized queue
int free_iqueue(iqueue_t * queue);




#endif
