/* $Id: assert.h 46991 2011-09-12 17:36:30Z twu $ */
#ifndef ASSERT_INCLUDED
#define ASSERT_INCLUDED

#include "except.h"

#undef assert

/* #define CHECK_ASSERTIONS 1 */
#ifdef CHECK_ASSERTIONS
extern void assert (int e);
#define assert(e) ((void) ((e) || (RAISE(Assert_Failed),0)))
#else
#define assert(e) ((void) 0)
#endif

#endif
