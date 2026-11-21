// SC4LoadCache: makes city loads faster by remembering, for the game session, work the game
// redoes on every load. Plugins do not change while the game runs, so:
//
// - Key lists by type. ResourceManager::GetResourceList walks the index of every DBPF segment
//   (about 270 ms per call, 13 calls per city load). A list asked for by type is kept and
//   handed out again while the segments registered at start-up are unchanged and the segments
//   added since (city saves) have no key of that type.
// - The Type 21 scan. On every city load the network props manager loads every exemplar to
//   find the ones with ExemplarType 0x21 (about 4.5 s). After the first scan only those and
//   keys not seen before are passed on.
//
// Windows SimCity 4 1.1.641 only. Writes SC4LoadCache.log next to the DLL.

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <MinHook.h>

#include "cIGZFrameWork.h"
#include "cRZMessage2COMDirector.h"

namespace
{
    // ---- game addresses (docs/re-notes/loadcache.md) ----
    constexpr uintptr_t kResourceManagerPtr = 0x00b43ca8;
    constexpr uintptr_t kGetResourceList = 0x009739cb;   // thiscall(rm, list** out, filter*) -> count
    constexpr uintptr_t kType21Scan = 0x006100d0;        // fastcall(manager)
    constexpr uintptr_t kType21Callback = 0x0060fb90;    // cdecl(const Key*, manager)
    constexpr uintptr_t kGetPropertyUint = 0x005fd480;   // cdecl(holder, id, uint32_t* out) -> bool
    constexpr uintptr_t kOperatorNew = 0x005e55e0;       // cdecl(size)
    constexpr uintptr_t kKeyListCtor = 0x00974e7a;       // thiscall(memory) -> list
    constexpr uintptr_t kTypeFilterVtable = 0x00ad8970;  // {vtable, refs, type}
    constexpr uint32_t kExemplarType = 0x6534284a;
    constexpr uint32_t kPropertyHolderIid = 0xa52160f5;
    constexpr uint32_t kExemplarTypeProperty = 0x10;
    constexpr uint32_t kNetworkPropsType = 0x21;
    constexpr uint16_t kGameVersion = 641;

    // Resource manager fields: the segment deque begin iterator at +0x58 {cur, first, last,
    // node}, its end cur at +0x68, buffers of 32 pointers; the critical section at +0x80.
    constexpr size_t kSegmentsBegin = 0x58;
    constexpr size_t kSegmentsEnd = 0x68;
    constexpr size_t kLock = 0x80;
    constexpr int kDequeBuffer = 32;

    struct Key
    {
        uint32_t type, group, instance;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash
    {
        size_t operator()(const Key& k) const noexcept
        {
            return (static_cast<size_t>(k.instance) * 0x9E3779B1u) ^ (k.group * 0x85EBCA77u) ^ k.type;
        }
    };

    // vtable slot `offset` of a game COM object.
    template <typename F>
    F Slot(void* object, const size_t offset)
    {
        return reinterpret_cast<F>((*static_cast<void***>(object))[offset / 4]);
    }
    void AddRef(void* object) { Slot<uint32_t(__thiscall*)(void*)>(object, 0x4)(object); }
    void Release(void* object) { Slot<uint32_t(__thiscall*)(void*)>(object, 0x8)(object); }
    uint32_t ListSize(void* list) { return Slot<uint32_t(__thiscall*)(void*)>(list, 0x24)(list); }

    void* NewKeyList()
    {
        void* memory = reinterpret_cast<void*(__cdecl*)(size_t)>(kOperatorNew)(0x40);
        if (!memory) {
            return nullptr;
        }
        void* list = reinterpret_cast<void*(__thiscall*)(void*)>(kKeyListCtor)(memory);
        AddRef(list);
        return list;
    }

    // ---- log ----
    std::filesystem::path gLogPath;
    std::mutex gLogMutex;
    void Log(const char* format, ...)
    {
        std::scoped_lock lock(gLogMutex);
        FILE* file = nullptr;
        if (_wfopen_s(&file, gLogPath.c_str(), L"a") != 0 || !file) {
            return;
        }
        va_list args;
        va_start(args, format);
        std::vfprintf(file, format, args);
        va_end(args);
        std::fputc('\n', file);
        std::fclose(file);
    }

    double MsSince(const std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    // ---- key lists by type ----
    using GetResourceListFn = uint32_t(__fastcall*)(void* rm, void* edx, void** out, void* filter);
    GetResourceListFn gGetResourceList = nullptr;

    struct CachedList
    {
        void* list = nullptr;  // one reference held here
        uint32_t count = 0;    // the return value of the call that made it
        uint32_t size = 0;     // its size then: a caller that changed the list makes it unusable
    };
    std::mutex gListMutex;
    std::unordered_set<void*> gStartupSegments;  // registered before PostAppInit
    std::unordered_map<uint32_t, CachedList> gLists;
    bool gListsOff = false;
    uint64_t gListHits = 0, gListMisses = 0;

    // The segments of the resource manager, in its order. Call with its lock held.
    std::vector<void*> Segments(void* rm)
    {
        auto* base = static_cast<uint8_t*>(rm);
        void** cur = *reinterpret_cast<void***>(base + kSegmentsBegin);
        void** last = *reinterpret_cast<void***>(base + kSegmentsBegin + 8);
        void*** node = *reinterpret_cast<void****>(base + kSegmentsBegin + 12);
        void** end = *reinterpret_cast<void***>(base + kSegmentsEnd);
        std::vector<void*> segments;
        while (cur != end && segments.size() < 1'000'000) {
            segments.push_back(*cur);
            if (++cur == last) {
                cur = *++node;
                last = cur + kDequeBuffer;
            }
        }
        return segments;
    }

    struct RmLock
    {
        void* cs;
        explicit RmLock(void* rm) : cs(static_cast<uint8_t*>(rm) + kLock) { Slot<void(__thiscall*)(void*)>(cs, 0x4)(cs); }
        ~RmLock() { Slot<void(__thiscall*)(void*)>(cs, 0x8)(cs); }
    };

    uint32_t __fastcall GetResourceListHook(void* rm, void* edx, void** out, void* filter)
    {
        if (!filter || *static_cast<uintptr_t*>(filter) != kTypeFilterVtable || !out) {
            return gGetResourceList(rm, edx, out, filter);
        }
        const uint32_t type = static_cast<uint32_t*>(filter)[2];
        const auto start = std::chrono::steady_clock::now();
        RmLock rmLock(rm);  // the game's order: its lock first, then ours
        std::scoped_lock lock(gListMutex);
        if (gStartupSegments.empty() || gListsOff) {
            return gGetResourceList(rm, edx, out, filter);
        }
        const std::vector<void*> segments = Segments(rm);
        size_t startup = 0;
        std::vector<void*> added;
        for (void* segment : segments) {
            if (gStartupSegments.contains(segment)) {
                ++startup;
            }
            else {
                added.push_back(segment);
            }
        }
        if (startup != gStartupSegments.size()) {
            // A segment of the start-up set is gone: plugins changed after all. Stay out of it.
            Log("Key lists: %zu of %zu start-up segments left, the cache is off", startup, gStartupSegments.size());
            for (auto& [t, cached] : gLists) {
                Release(cached.list);
            }
            gLists.clear();
            gListsOff = true;
            return gGetResourceList(rm, edx, out, filter);
        }
        // Keys of this type in the segments added since start-up (a city save, usually none).
        uint32_t addedKeys = 0;
        if (!added.empty()) {
            if (void* scratch = NewKeyList()) {
                for (void* segment : added) {
                    addedKeys += Slot<uint32_t(__thiscall*)(void*, void*, void*)>(segment, 0x48)(segment, scratch, filter);
                }
                Release(scratch);
            }
            else {
                addedKeys = 1;
            }
        }
        if (addedKeys == 0) {
            if (const auto it = gLists.find(type); it != gLists.end()) {
                if (ListSize(it->second.list) == it->second.size) {
                    AddRef(it->second.list);
                    *out = it->second.list;
                    ++gListHits;
                    return it->second.count;
                }
                Log("Key lists: type %08X was changed by a caller, made again", type);
                Release(it->second.list);
                gLists.erase(it);
            }
        }
        const uint32_t count = gGetResourceList(rm, edx, out, filter);
        ++gListMisses;
        if (addedKeys == 0 && *out) {
            AddRef(*out);
            gLists[type] = {*out, count, ListSize(*out)};
        }
        Log("Key lists: type %08X built in %.0f ms (%u keys, %zu segments, %zu added since start-up with %u keys)%s",
            type, MsSince(start), count, segments.size(), added.size(), addedKeys, addedKeys == 0 ? ", kept" : "");
        return count;
    }

    // ---- Type 21 scan ----
    using Type21ScanFn = void(__fastcall*)(void* manager);
    using Type21CallbackFn = void(__cdecl*)(const Key* key, void* manager);
    Type21ScanFn gType21Scan = nullptr;
    // ExemplarType == 0x21 per exemplar key seen in a scan.
    std::unordered_map<Key, bool, KeyHash> gIsType21;

    struct ScanContext
    {
        void* manager;
        uint32_t keys = 0, skipped = 0, checked = 0, passed = 0;
    };

    bool IsType21(const Key& key)
    {
        void* rm = *reinterpret_cast<void**>(kResourceManagerPtr);
        void* resource = nullptr;
        using GetResourceFn = bool(__thiscall*)(void*, const Key&, uint32_t, void**, uint32_t, void*);
        if (!Slot<GetResourceFn>(rm, 0xc)(rm, key, kPropertyHolderIid, &resource, 0, nullptr) || !resource) {
            return false;
        }
        void* holder = Slot<void*(__thiscall*)(void*)>(resource, 0x1c)(resource);
        uint32_t type = 0;
        const bool found =
            holder && reinterpret_cast<bool(__cdecl*)(void*, uint32_t, uint32_t*)>(kGetPropertyUint)(
                          holder, kExemplarTypeProperty, &type);
        Release(resource);
        return found && type == kNetworkPropsType;
    }

    void __cdecl Type21Callback(const Key* key, void* context)
    {
        auto* scan = static_cast<ScanContext*>(context);
        ++scan->keys;
        const auto it = gIsType21.find(*key);
        bool type21;
        if (it != gIsType21.end()) {
            type21 = it->second;
            scan->skipped += type21 ? 0 : 1;
        }
        else {
            type21 = IsType21(*key);
            gIsType21.emplace(*key, type21);
            ++scan->checked;
        }
        if (type21) {
            reinterpret_cast<Type21CallbackFn>(kType21Callback)(key, scan->manager);
            ++scan->passed;
        }
    }

    // The game's scan (0x006100d0) with our callback in between.
    void __fastcall Type21ScanHook(void* manager)
    {
        void* rm = *reinterpret_cast<void**>(kResourceManagerPtr);
        if (!rm) {
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        void* list = nullptr;
        using ForTypeFn = uint32_t(__thiscall*)(void*, void**, uint32_t);
        if (Slot<ForTypeFn>(rm, 0x48)(rm, &list, kExemplarType) != 0 && list) {
            ScanContext scan{manager};
            using EnumKeysFn = void(__thiscall*)(void*, void(__cdecl*)(const Key*, void*), void*);
            Slot<EnumKeysFn>(list, 0x1c)(list, &Type21Callback, &scan);
            Log("Type 21 scan: %u exemplars, %u skipped, %u checked, %u passed to the game, %.0f ms", scan.keys,
                scan.skipped, scan.checked, scan.passed, MsSince(start));
        }
        if (list) {
            Release(list);
        }
    }

    uint16_t GameVersion()
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        DWORD handle = 0;
        const DWORD size = GetFileVersionInfoSizeW(path, &handle);
        std::vector<uint8_t> data(size);
        VS_FIXEDFILEINFO* info = nullptr;
        UINT length = 0;
        if (size == 0 || !GetFileVersionInfoW(path, 0, size, data.data()) ||
            !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) {
            return 0;
        }
        return static_cast<uint16_t>(info->dwFileVersionLS >> 16);
    }
}

class LoadCacheDirector final : public cRZMessage2COMDirector
{
public:
    uint32_t GetDirectorID() const override { return 0x5EA1C0FE; }
    bool DoMessage(cIGZMessage2*) override { return true; }

    bool OnStart(cIGZCOM* com) override
    {
        cRZMessage2COMDirector::OnStart(com);
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Log), &self);
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(self, path, MAX_PATH);
        gLogPath = std::filesystem::path(path).parent_path() / L"SC4LoadCache.log";
        std::error_code ec;
        std::filesystem::remove(gLogPath, ec);

        const uint16_t version = GameVersion();
        if (version != kGameVersion) {
            Log("Game version %u is not 641: nothing installed", version);
            return true;
        }
        if (MH_Initialize() != MH_OK) {
            Log("MinHook did not start: nothing installed");
            return true;
        }
        const auto hook = [](const uintptr_t target, void* detour, void** original, const char* name) {
            MH_STATUS status = MH_CreateHook(reinterpret_cast<void*>(target), detour, original);
            if (status == MH_OK) {
                status = MH_EnableHook(reinterpret_cast<void*>(target));
            }
            Log("Hook %s at %08X: %s", name, static_cast<unsigned>(target), MH_StatusToString(status));
        };
        hook(kGetResourceList, reinterpret_cast<void*>(&GetResourceListHook), reinterpret_cast<void**>(&gGetResourceList),
             "GetResourceList");
        hook(kType21Scan, reinterpret_cast<void*>(&Type21ScanHook), reinterpret_cast<void**>(&gType21Scan),
             "Type 21 scan");
        if (cIGZFrameWork* framework = RZGetFrameWork()) {
            framework->AddHook(this);
        }
        return true;
    }

    bool PostAppInit() override
    {
        // Every plugin segment is registered by now; later ones are city saves.
        void* rm = *reinterpret_cast<void**>(kResourceManagerPtr);
        if (rm && gGetResourceList) {
            RmLock rmLock(rm);
            std::scoped_lock lock(gListMutex);
            for (void* segment : Segments(rm)) {
                gStartupSegments.insert(segment);
            }
            Log("%zu segments at start-up", gStartupSegments.size());
        }
        return true;
    }

    bool PreAppShutdown() override
    {
        Log("Key lists: %llu handed out again, %llu built", gListHits, gListMisses);
        return true;
    }
};

static LoadCacheDirector sDirector;

cRZCOMDllDirector* RZGetCOMDllDirector()
{
    static bool sAddedRef = false;
    if (!sAddedRef) {
        sDirector.AddRef();
        sAddedRef = true;
    }
    return &sDirector;
}
