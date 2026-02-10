#pragma once
#include "../VmHandlerMatcher.hpp"

void SetVirtualMemoryValue(uint64_t addr, uint64_t value, uint32_t size);
uint64_t GetVirtualMemoryValue(uint64_t addr, uint32_t size);

void EmulateVmCode(const std::vector<HandlerMatch>& handlers);