#pragma once
#include "stdint.h"

// The part of the nam-dll Patching API (https://github.com/NAMTeam/nam-dll, src/Patching.h) that the patches
// in this folder use, with the same signatures and the same behavior: a failure throws a wil::ResultException.
// Here the patches go through Hooking::BytePatch, so they can be removed again.
// Do not copy this file and Patching.cpp to nam-dll, it has its own.
namespace Patching
{
	void OverwriteMemory(void* address, uint8_t newValue);
	void PatchImmediate32(uint32_t address, uint32_t expectedValue, uint32_t newValue);
	void InstallCallHook(uint32_t address, void (*pfnFunc)(void));

	// Not in nam-dll: restores the original bytes of every patch above, newest first.
	void RemoveAll();
}
