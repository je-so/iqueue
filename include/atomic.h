/* atomic.h

   Defines atomic operations with help of GNU gcc builtin operations.

   Copyright:
   This program is free software. See accompanying LICENSE file.

   Author:
   (C) 2014 Jörg Seebohn
*/
#ifndef ATOMIC_H
#define ATOMIC_H

// See https://gcc.gnu.org/onlinedocs/gcc-4.1.2/gcc/Atomic-Builtins.html for a description

// Does the following operations in one atomic step:
// { void* old = *pval; if (old == oldval) *pval = newval; return old; }
static inline void* cmpxchg_atomicptr(void** pval, void* oldval, void* newval)
{
         return __sync_val_compare_and_swap(pval, oldval, newval);
}

// Does the following operations in one atomic step:
// { size_t old = *pval; if (old == oldval) *pval = newval; return old; }
static inline size_t cmpxchg_atomicsize(size_t* pval, size_t oldval, size_t newval)
{
         return __sync_val_compare_and_swap(pval, oldval, newval);
}

static inline int cmpxchg_atomicint(int* pval, int oldval, int newval)
{
         return __sync_val_compare_and_swap(pval, oldval, newval);
}

// Does the following operations in one atomic step:
// { size_t old = *pval; *pval += value; return old; }
static inline size_t add_atomicsize(size_t* pval, size_t value)
{
         return __sync_fetch_and_add(pval, value);
}

// Does the following operations in one atomic step:
// { void* old = *pval; *pval &= value; return old; }
static inline void* and_atomicptr(void** pval, void* value)
{
         return __sync_fetch_and_and(pval, value);
}

// Does the following operations in one atomic step:
// { size_t old = *pval; *pval -= value; return old; }
static inline size_t sub_atomicsize(size_t* pval, size_t value)
{
         return __sync_fetch_and_sub(pval, value);
}


#endif
