#ifndef HYPERCORE_DIAG_H
#define HYPERCORE_DIAG_H

#include <stdint.h>

/* Paired R47 loader/core ABI: argument six is optional diagnostics only.
 * Call solely from normal userland startup boundaries, never from a trap,
 * interrupt, signal handler, or a kernel tracing callback. */
typedef void (*hypercore_diag_fn)(const char* phase, uint64_t value);

void hypercore_diag_stage(const char* phase, uint64_t value);

#endif
