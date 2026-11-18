#pragma once
#include "runtime/gs/gs_backend.h"
#include <memory>

// Runs a raster backend on its own thread. Commands that only change GS memory
// are queued in order; anything that returns data or exposes memory to the
// caller first drains the queue, so results match the wrapped backend exactly.
std::unique_ptr<GSRasterBackend> createOrochiThreadedBackend(std::unique_ptr<GSRasterBackend> inner);
int runGsThreadRegressionTests();
