#include "Patching.h"
#include <cstring>
#include <memory>
#include <span>
#include <vector>
#include <Windows.h>
#include "wil/result_macros.h"
#include "core/Hooking.hpp"

namespace
{
	std::vector<std::unique_ptr<Hooking::BytePatch>> sPatches;

	void Apply(uint32_t address, std::span<const uint8_t> bytes)
	{
		auto patch = std::make_unique<Hooking::BytePatch>();
		THROW_HR_IF(E_FAIL, !patch->Apply(address, bytes, "TransportMap"));
		sPatches.push_back(std::move(patch));
	}
}

void Patching::OverwriteMemory(void* address, uint8_t newValue)
{
	Apply(reinterpret_cast<uint32_t>(address), {&newValue, 1});
}

void Patching::PatchImmediate32(const uint32_t address, const uint32_t expectedValue, const uint32_t newValue)
{
	uint32_t current;
	std::memcpy(&current, reinterpret_cast<const void*>(address), sizeof(current));
	if (current == newValue)
	{
		return;
	}

	THROW_HR_IF(E_FAIL, current != expectedValue);

	uint8_t bytes[sizeof(newValue)];
	std::memcpy(bytes, &newValue, sizeof(bytes));
	Apply(address, bytes);
}

void Patching::InstallCallHook(uint32_t address, void (*pfnFunc)(void))
{
	uint8_t bytes[5] = {0xE8};
	const uint32_t offset = reinterpret_cast<uint32_t>(pfnFunc) - address - 5;
	std::memcpy(bytes + 1, &offset, sizeof(offset));
	Apply(address, bytes);
}

void Patching::RemoveAll()
{
	while (!sPatches.empty())
	{
		sPatches.pop_back();
	}
}
