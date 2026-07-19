#include "Flags.hpp"

#include <gflags/gflags.h>

DEFINE_uint32(
   executor_threads,
   2,
   "Number of worker threads in the CPU executor (must be 2, 4, or 6)");
