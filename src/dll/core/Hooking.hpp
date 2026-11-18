#pragma once

// Function hooks (MinHook detours) and byte patches that can all be removed again.
//
// Rules:
// - Call everything on the game thread.
// - Hooking::Initialize() runs once in the director, only on game version 641. Install and
//   Apply fail (and log) before that.
// - Hooking::Shutdown() removes every hook and patch that is still installed, newest first.
//   A Detour or BytePatch object that outlives Shutdown is then simply not installed.
// - Destroying a Detour or BytePatch removes it.
//
// __thiscall targets
// ------------------
// Most game functions are member functions: `this` in ECX, the arguments on the stack, the
// callee pops them. A free function cannot be declared __thiscall, so use __fastcall with a
// dummy second parameter for EDX. It has the same ABI for these functions:
//
//     // Game: void cSC4CameraControl::UpdateScrollBounds()           (0x007ccc60)
//     using UpdateScrollBoundsFn = void(__fastcall*)(void* self, void* edx);
//     Hooking::Detour<UpdateScrollBoundsFn> sUpdateScrollBounds;
//
//     void __fastcall UpdateScrollBoundsHook(void* self, void* edx)
//     {
//         sUpdateScrollBounds.Original()(self, edx);
//         // ... change the result ...
//     }
//
//     sUpdateScrollBounds.Install(GameAddresses::CameraControl::kUpdateScrollBounds,
//                                 &UpdateScrollBoundsHook, "UpdateScrollBounds");
//
// With arguments, they follow the dummy: `bool(__fastcall*)(void* self, void* edx, int zoom)`.
// Never put a float or a struct by value in the EDX slot: the dummy must be a pointer-sized
// integer type. __stdcall and __cdecl targets keep their own convention.

#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace Hooking
{
    // MH_Initialize. Safe to call again. Returns false when MinHook fails.
    bool Initialize();
    // Removes every installed hook and patch, then MH_Uninitialize.
    void Shutdown();
    [[nodiscard]] bool IsInitialized();

    // Common base so Shutdown can remove everything. Not for direct use.
    class Removable
    {
    public:
        Removable() = default;
        Removable(const Removable&) = delete;
        Removable& operator=(const Removable&) = delete;
        virtual ~Removable() = default;
        virtual void Remove() = 0;
    };

    namespace detail
    {
        bool InstallDetour(uintptr_t target, void* replacement, void** original, const char* name,
                           Removable* owner);
        void RemoveDetour(uintptr_t target, const char* name, Removable* owner);
    }

    // A MinHook detour on one function. Fn is the function pointer type of the target with
    // the calling convention spelled out, for example `int(__fastcall*)(void*, void*, int)`.
    template <typename Fn>
    class Detour final : public Removable
    {
        static_assert(std::is_pointer_v<Fn> && std::is_function_v<std::remove_pointer_t<Fn>>,
                      "Fn must be a function pointer type");

    public:
        Detour() = default;
        ~Detour() override { Remove(); }

        // Installs and enables the detour. Returns false (and logs) when Hooking is not
        // initialized, when this detour is already installed, or when MinHook refuses.
        bool Install(const uintptr_t target, Fn replacement, const char* name)
        {
            if (target_ != 0 || !replacement) {
                return false;
            }
            void* original = nullptr;
            if (!detail::InstallDetour(target, reinterpret_cast<void*>(replacement), &original,
                                       name, this)) {
                return false;
            }
            target_ = target;
            name_ = name;
            original_ = reinterpret_cast<Fn>(original);
            return true;
        }

        // Disables and removes the detour. Does nothing when it is not installed.
        void Remove() override
        {
            if (target_ == 0) {
                return;
            }
            detail::RemoveDetour(target_, name_, this);
            target_ = 0;
            original_ = nullptr;
        }

        // Trampoline to the original function. nullptr when not installed.
        [[nodiscard]] Fn Original() const { return original_; }
        [[nodiscard]] bool Installed() const { return target_ != 0; }

    private:
        uintptr_t target_ = 0;
        const char* name_ = "";
        Fn original_ = nullptr;
    };

    // Overwrites bytes in the game's code or data and restores the original bytes on Remove.
    // Do not patch bytes that a Detour also covers.
    class BytePatch final : public Removable
    {
    public:
        BytePatch() = default;
        ~BytePatch() override;

        // Returns false (and logs) when Hooking is not initialized, the patch is already
        // applied, the span is empty or the memory cannot be written.
        bool Apply(uintptr_t address, std::span<const uint8_t> bytes, const char* name);
        void Remove() override;
        [[nodiscard]] bool Applied() const { return address_ != 0; }

    private:
        uintptr_t address_ = 0;
        const char* name_ = "";
        std::vector<uint8_t> original_;
    };
}
