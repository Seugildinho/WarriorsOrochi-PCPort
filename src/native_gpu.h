#pragma once
#include "runtime/gs/gs_backend.h"
#include <memory>
#include <string>

// Hardware-only D3D11 compute path; unsupported GS states keep the CPU reference.
std::unique_ptr<GSRasterBackend> createOrochiGpuBackend(std::string &error);
int runGpuRegressionTests();
