
/* interlock.cpp

   Linux implementation of the interlocked memory operations.

   The OSX build uses the OSAtomic*32Barrier family; those are Darwin-only (and
   deprecated there). The GCC/Clang __atomic builtins give the same full-barrier
   semantics on every architecture Linux runs on, so no assembly is needed.
*/

#include <stdint.h>

#include "interlock.h"

int32_t *InterlockedAllocate ( )
{
	int32_t* p = new int32_t;

    if(p)
		*p = 0;

    return p;
}


void InterlockedDeallocate ( int32_t *v )
{
    if(v)
		delete v;
}


/*
-- Subtract one, return result
*/
int32_t	InterlockedDecrement ( int32_t *v )
{
	return __atomic_sub_fetch(v, 1, __ATOMIC_SEQ_CST);
}


/*
-- Add one, return result
*/
int32_t InterlockedIncrement ( int32_t *v )
{
	return __atomic_add_fetch(v, 1, __ATOMIC_SEQ_CST);
}

/*
-- Add "incr", return initial value
*/
int32_t InterlockedExchangeAdd ( int32_t *v, int32_t incr )
{
	return __atomic_fetch_add(v, incr, __ATOMIC_SEQ_CST);
}
