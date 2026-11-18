#include "core/Hooking.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

#include <MinHook.h>

#include "utils/Logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace
{
    std::recursive_mutex sMutex;
    bool sInitialized = false;
    // Installed hooks and patches, oldest first.
    std::vector<Hooking::Removable*> sInstalled;

    void Track(Hooking::Removable* owner) { sInstalled.push_back(owner); }

    void Untrack(Hooking::Removable* owner)
    {
        std::erase(sInstalled, owner);
    }

    bool WriteBytes(const uintptr_t address, const uint8_t* bytes, const size_t size)
    {
        auto* target = reinterpret_cast<void*>(address);
        DWORD oldProtect = 0;
        if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            return false;
        }
        std::memcpy(target, bytes, size);
        DWORD ignored = 0;
        VirtualProtect(target, size, oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), target, size);
        return true;
    }
}

namespace Hooking
{
    bool Initialize()
    {
        std::scoped_lock lock(sMutex);
        if (sInitialized) {
            return true;
        }
        const MH_STATUS status = MH_Initialize();
        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
            LOG_ERROR("Hooking: MH_Initialize failed: {}", MH_StatusToString(status));
            return false;
        }
        sInitialized = true;
        LOG_INFO("Hooking: initialized");
        return true;
    }

    void Shutdown()
    {
        std::scoped_lock lock(sMutex);
        if (!sInitialized) {
            return;
        }
        // Remove() untracks, so always take the newest.
        while (!sInstalled.empty()) {
            sInstalled.back()->Remove();
        }
        const MH_STATUS status = MH_Uninitialize();
        if (status != MH_OK) {
            LOG_WARN("Hooking: MH_Uninitialize failed: {}", MH_StatusToString(status));
        }
        sInitialized = false;
        LOG_INFO("Hooking: shut down, all hooks removed");
    }

    bool IsInitialized()
    {
        std::scoped_lock lock(sMutex);
        return sInitialized;
    }

    namespace detail
    {
        bool InstallDetour(const uintptr_t target, void* replacement, void** original, const char* name,
                           Removable* owner)
        {
            std::scoped_lock lock(sMutex);
            if (!sInitialized) {
                LOG_ERROR("Hooking: cannot install {} at 0x{:08x}, hooking is not initialized", name, target);
                return false;
            }
            auto* targetPtr = reinterpret_cast<LPVOID>(target);
            MH_STATUS status = MH_CreateHook(targetPtr, replacement, original);
            if (status != MH_OK) {
                LOG_ERROR("Hooking: MH_CreateHook {} at 0x{:08x} failed: {}", name, target, MH_StatusToString(status));
                return false;
            }
            status = MH_EnableHook(targetPtr);
            if (status != MH_OK) {
                LOG_ERROR("Hooking: MH_EnableHook {} at 0x{:08x} failed: {}", name, target, MH_StatusToString(status));
                MH_RemoveHook(targetPtr);
                return false;
            }
            Track(owner);
            LOG_INFO("Hooking: installed {} at 0x{:08x}", name, target);
            return true;
        }

        void RemoveDetour(const uintptr_t target, const char* name, Removable* owner)
        {
            std::scoped_lock lock(sMutex);
            Untrack(owner);
            if (!sInitialized) {
                return;
            }
            auto* targetPtr = reinterpret_cast<LPVOID>(target);
            MH_STATUS status = MH_DisableHook(targetPtr);
            if (status != MH_OK) {
                LOG_WARN("Hooking: MH_DisableHook {} failed: {}", name, MH_StatusToString(status));
            }
            status = MH_RemoveHook(targetPtr);
            if (status != MH_OK) {
                LOG_WARN("Hooking: MH_RemoveHook {} failed: {}", name, MH_StatusToString(status));
                return;
            }
            LOG_INFO("Hooking: removed {}", name);
        }
    }

    BytePatch::~BytePatch() { Remove(); }

    bool BytePatch::Apply(const uintptr_t address, const std::span<const uint8_t> bytes, const char* name)
    {
        std::scoped_lock lock(sMutex);
        if (!sInitialized) {
            LOG_ERROR("Hooking: cannot apply patch {} at 0x{:08x}, hooking is not initialized", name, address);
            return false;
        }
        if (address_ != 0 || bytes.empty() || address == 0) {
            LOG_ERROR("Hooking: patch {} at 0x{:08x} rejected (already applied or empty)", name, address);
            return false;
        }
        const auto* current = reinterpret_cast<const uint8_t*>(address);
        std::vector<uint8_t> saved(current, current + bytes.size());
        if (!WriteBytes(address, bytes.data(), bytes.size())) {
            LOG_ERROR("Hooking: patch {} at 0x{:08x} failed, error {}", name, address, GetLastError());
            return false;
        }
        original_ = std::move(saved);
        address_ = address;
        name_ = name;
        Track(this);
        LOG_INFO("Hooking: applied patch {} at 0x{:08x} ({} bytes)", name, address, bytes.size());
        return true;
    }

    void BytePatch::Remove()
    {
        std::scoped_lock lock(sMutex);
        if (address_ == 0) {
            return;
        }
        Untrack(this);
        if (WriteBytes(address_, original_.data(), original_.size())) {
            LOG_INFO("Hooking: restored patch {}", name_);
        }
        else {
            LOG_ERROR("Hooking: could not restore patch {} at 0x{:08x}, error {}", name_, address_, GetLastError());
        }
        address_ = 0;
        original_.clear();
    }
}
