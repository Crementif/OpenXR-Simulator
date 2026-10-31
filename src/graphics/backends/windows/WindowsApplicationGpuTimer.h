#pragma once
#include "graphics/ApplicationGpuTimer.h"
#include <d3d11.h>
#include <d3d12.h>

namespace simxr
{
std::unique_ptr<ApplicationGpuTimer> CreateApplicationGpuTimer(ID3D11Device* device);
std::unique_ptr<ApplicationGpuTimer> CreateApplicationGpuTimer(ID3D12Device* device, ID3D12CommandQueue* queue);
}
