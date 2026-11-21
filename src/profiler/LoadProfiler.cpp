// SC4LoadProfiler: a test DLL, not part of the mod. Times the phases of a city load and samples
// where the main thread spends it. Writes SC4LoadProfiler.log next to the DLL after each
// cSC4App::LoadCity, plus SC4LoadProfiler-samples.tsv (address, count, innermost timed phase)
// to map to functions with Ghidra afterwards.
//
// Timers: MinHook detours to a tiny stub per function. The stub notes the time and swaps the
// return address for an exit thunk, so it works for any calling convention and signature. The
// targets are from docs/re-notes and a read-only Ghidra pass (SimCity 4 1.1.641 only).
//
// Sampler: while LoadCity runs, a thread suspends the main thread about once per millisecond
// and records its EIP and the timed phase it is in.

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <MinHook.h>

#include "cRZMessage2COMDirector.h"

namespace
{
    struct Target
    {
        uintptr_t address;
        const char* name;
    };

    constexpr Target kTargets[] = {
        {0x004555f0, "cSC4App::LoadCity"},
        {0x00454620, "cSC4App::Read (the save)"},
        {0x0045cb10, "cSC4City::Read"},
        {0x004a1a20, "cSC4OccupantManager::Read"},
        {0x004e0e70, "DestroyCurrentScreen"},
        {0x004e0dd0, "CreateCityScreen"},
        {0x0045bd20, "cSC4City::Init"},
        {0x0045aa10, "CreateCityComponents"},
        {0x00449af0, "PostCityMessage (with the plugins' handlers)"},
        {0x00514520, "AutomataManager::ScanExemplars"},
        {0x006b0760, "BuildingDevelopment exemplar scan"},
        {0x006a8c20, "cSC4DemandSimulator::Init"},
        {0x009739cb, "ResourceManager::GetResourceList"},
        {0x00973280, "ResourceManager::GetResourceListForType"},
        {0x009735b8, "ResourceManager cache cleanup"},
    };
    constexpr size_t kCount = std::size(kTargets);
    constexpr int kLoadCity = 0;

    void* gOriginal[kCount]{};  // plain array: the inline assembly indexes it

    struct Frame
    {
        uint32_t id;
        uint32_t ret;
        int64_t start;
        int64_t child;
    };
    struct Stat
    {
        int64_t total = 0;
        int64_t self = 0;
        uint32_t calls = 0;
    };

    thread_local std::array<Frame, 256> tFrames;
    thread_local int tDepth = 0;

    DWORD gMainThread = 0;
    HANDLE gMainHandle = nullptr;
    std::atomic<int> gMainPhase{-1};  // innermost timed function of the main thread
    std::array<Stat, kCount> gStats{};
    int64_t gFrequency = 1;
    std::atomic<bool> gSampling{false};
    std::thread gSampler;
    std::vector<std::pair<uint32_t, int>> gSamples;  // EIP, phase
    std::filesystem::path gDir;
    int gLoads = 0;

    int64_t Now()
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }

    double Ms(const int64_t ticks) { return static_cast<double>(ticks) * 1000.0 / static_cast<double>(gFrequency); }

    void SamplerMain()
    {
        while (gSampling.load()) {
            if (SuspendThread(gMainHandle) != static_cast<DWORD>(-1)) {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;
                // No allocation while the main thread is suspended: it may hold the heap lock.
                if (GetThreadContext(gMainHandle, &context) && gSamples.size() < gSamples.capacity()) {
                    gSamples.emplace_back(context.Eip, gMainPhase.load());
                }
                ResumeThread(gMainHandle);
            }
            Sleep(1);
        }
    }

    std::string ModuleOf(const uint32_t address)
    {
        HMODULE module = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(address)), &module)) {
            return "?";
        }
        char path[MAX_PATH]{};
        GetModuleFileNameA(module, path, MAX_PATH);
        return std::filesystem::path(path).filename().string();
    }

    void Report()
    {
        ++gLoads;
        FILE* log = nullptr;
        if (_wfopen_s(&log, (gDir / L"SC4LoadProfiler.log").c_str(), L"a") != 0 || !log) {
            return;
        }
        std::fprintf(log, "\n=== City load %d: %.0f ms ===\n", gLoads, Ms(gStats[kLoadCity].total));
        std::fprintf(log, "%-48s %8s %10s %10s\n", "phase", "calls", "total ms", "self ms");
        for (size_t i = 0; i < kCount; ++i) {
            if (gStats[i].calls) {
                std::fprintf(log, "%-48s %8u %10.0f %10.0f\n", kTargets[i].name, gStats[i].calls, Ms(gStats[i].total),
                             Ms(gStats[i].self));
            }
        }
        // Samples: per phase, and the top addresses outside the game (DLLs).
        std::map<int, int> byPhase;
        std::map<std::string, int> byModule;
        std::unordered_map<uint32_t, std::pair<int, int>> byAddress;  // count, phase
        for (const auto& [eip, phase] : gSamples) {
            ++byPhase[phase];
            ++byModule[eip >= 0x00400000 && eip < 0x00c00000 ? std::string("SimCity 4.exe") : ModuleOf(eip)];
            auto& entry = byAddress[eip];
            ++entry.first;
            entry.second = phase;
        }
        std::fprintf(log, "-- %zu samples (about 1 per ms) by innermost timed phase\n", gSamples.size());
        for (const auto& [phase, count] : byPhase) {
            std::fprintf(log, "%-48s %8d  %5.1f%%\n", phase < 0 ? "(outside the timed phases)" : kTargets[phase].name,
                         count, 100.0 * count / std::max<size_t>(1, gSamples.size()));
        }
        std::fprintf(log, "-- by module\n");
        std::vector<std::pair<int, std::string>> modules;
        for (const auto& [name, count] : byModule) {
            modules.emplace_back(count, name);
        }
        std::ranges::sort(modules, std::greater<>());
        for (const auto& [count, name] : modules) {
            std::fprintf(log, "%-48s %8d  %5.1f%%\n", name.c_str(), count, 100.0 * count / std::max<size_t>(1, gSamples.size()));
        }
        std::fclose(log);

        FILE* tsv = nullptr;
        if (_wfopen_s(&tsv, (gDir / (L"SC4LoadProfiler-samples-" + std::to_wstring(gLoads) + L".tsv")).c_str(), L"w") ==
                0 &&
            tsv) {
            for (const auto& [eip, entry] : byAddress) {
                std::fprintf(tsv, "%08X\t%d\t%d\t%s\n", eip, entry.first, entry.second,
                             eip >= 0x00400000 && eip < 0x00c00000 ? "SimCity 4.exe" : ModuleOf(eip).c_str());
            }
            std::fclose(tsv);
        }
        gStats = {};
        gSamples.clear();
    }
}

extern "C" void __cdecl ProfEnter(const uint32_t id, uint32_t* retSlot);
extern "C" uint32_t __cdecl ProfExit();
void __cdecl ExitThunk();

extern "C" void __cdecl ProfEnter(const uint32_t id, uint32_t* retSlot)
{
    if (tDepth >= static_cast<int>(tFrames.size())) {
        return;  // too deep: not timed (the return address stays)
    }
    const bool main = GetCurrentThreadId() == gMainThread;
    if (id == kLoadCity && main && !gSampling.load()) {
        gSamples.reserve(60000);  // a minute of samples
        gSampling = true;
        gSampler = std::thread(SamplerMain);
    }
    tFrames[tDepth++] = {id, *retSlot, Now(), 0};
    *retSlot = reinterpret_cast<uint32_t>(&ExitThunk);
    if (main) {
        gMainPhase = static_cast<int>(id);
    }
}

extern "C" uint32_t __cdecl ProfExit()
{
    const Frame frame = tFrames[--tDepth];
    const int64_t elapsed = Now() - frame.start;
    Stat& stat = gStats[frame.id];
    stat.total += elapsed;
    stat.self += elapsed - frame.child;
    ++stat.calls;
    if (tDepth > 0) {
        tFrames[tDepth - 1].child += elapsed;
    }
    const bool main = GetCurrentThreadId() == gMainThread;
    if (main) {
        gMainPhase = tDepth > 0 ? static_cast<int>(tFrames[tDepth - 1].id) : -1;
    }
    if (frame.id == kLoadCity && main && tDepth == 0) {
        gSampling = false;
        if (gSampler.joinable()) {
            gSampler.join();
        }
        Report();
    }
    return frame.ret;
}

// Entered by `push id; jmp CommonEnter` with the caller's return address above the id.
__declspec(naked) void CommonEnter()
{
    __asm {
        pushfd
        pushad                      // 36 bytes saved: [esp+36] = id, [esp+40] = return address
        lea eax, [esp + 40]
        push eax
        push dword ptr [esp + 40]   // the id (one push moved it to +40)
        call ProfEnter
        add esp, 8
        popad
        popfd
        push eax                    // [esp] = eax, [esp+4] = id
        mov eax, [esp + 4]
        mov eax, gOriginal[eax * 4]
        mov [esp + 4], eax          // the id slot now holds the original function
        pop eax
        ret                         // into the original, with the exit thunk as its return address
    }
}

__declspec(naked) void __cdecl ExitThunk()
{
    __asm {
        push eax                    // slot for the real return address
        push eax
        push edx
        push ecx
        pushfd
        call ProfExit
        mov [esp + 16], eax
        popfd
        pop ecx
        pop edx
        pop eax
        ret
    }
}

class LoadProfilerDirector final : public cRZMessage2COMDirector
{
public:
    uint32_t GetDirectorID() const override { return 0x5EA1C0FF; }
    bool DoMessage(cIGZMessage2*) override { return true; }

    bool OnStart(cIGZCOM* com) override
    {
        cRZMessage2COMDirector::OnStart(com);
        gMainThread = GetCurrentThreadId();
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &gMainHandle,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        gFrequency = frequency.QuadPart;
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ProfEnter), &self);
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(self, path, MAX_PATH);
        gDir = std::filesystem::path(path).parent_path();

        FILE* log = nullptr;
        _wfopen_s(&log, (gDir / L"SC4LoadProfiler.log").c_str(), L"a");
        if (MH_Initialize() != MH_OK) {
            if (log) {
                std::fprintf(log, "MinHook did not start\n");
                std::fclose(log);
            }
            return true;
        }
        // One stub per target: push id; jmp CommonEnter.
        auto* stubs = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        for (uint32_t i = 0; i < kCount; ++i) {
            uint8_t* stub = stubs + i * 16;
            stub[0] = 0x68;  // push imm32
            std::memcpy(stub + 1, &i, 4);
            stub[5] = 0xE9;  // jmp rel32
            const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&CommonEnter) -
                                                     reinterpret_cast<uintptr_t>(stub + 10));
            std::memcpy(stub + 6, &rel, 4);
            const MH_STATUS created =
                MH_CreateHook(reinterpret_cast<void*>(kTargets[i].address), stub, &gOriginal[i]);
            const MH_STATUS enabled =
                created == MH_OK ? MH_EnableHook(reinterpret_cast<void*>(kTargets[i].address)) : created;
            if (log) {
                std::fprintf(log, "hook %08X %-48s %s\n", static_cast<unsigned>(kTargets[i].address), kTargets[i].name,
                             MH_StatusToString(enabled));
            }
        }
        if (log) {
            std::fclose(log);
        }
        return true;
    }
};

static LoadProfilerDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector()
{
    static bool sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
