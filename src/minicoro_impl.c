/* Compiles minicoro's implementation exactly once for the whole library. */
#define MINICORO_IMPL
#define MCO_USE_VMEM_ALLOCATOR
#include "minicoro.h"
