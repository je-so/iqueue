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
// { uint32_t old = *pval; if (old == oldval) *pval = newval; return old; }
static inline uint32_t cmpxchg_atomicu32(uint32_t* pval, uint32_t oldval, uint32_t newval)
{
         return __sync_val_compare_and_swap(pval, oldval, newval);
}

// Does the following operations in one atomic step:
// { size_t old = *pval; if (old == oldval) *pval = newval; return old; }
static inline size_t cmpxchg_atomicsize(size_t* pval, size_t oldval, size_t newval)
{
         return __sync_val_compare_and_swap(pval, oldval, newval);
}

// Does the following operations in one atomic step:
// { uint32_t old = *pval; *pval += add; return old; }
static inline uint32_t fetchadd_atomicu32(uint32_t* pval, uint32_t add)
{
         return __sync_fetch_and_add(pval, add);
}

#endif
