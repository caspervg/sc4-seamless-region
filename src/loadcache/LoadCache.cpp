// SC4LoadCache: makes city loads faster by remembering, for the game session, work the game
// redoes on every load. Plugins do not change while the game runs, so:
//
// - Key lists by type. ResourceManager::GetResourceList walks the index of every DBPF segment
//   (about 270 ms per call, 13 calls per city load). For the segments registered at start-up
//   the keys of each type are kept per segment; a list is put together from those, in the
//   game's segment order, and segments added since (city saves) are asked as before.
// - Exemplar scans. Nine scans load every exemplar to read its ExemplarType (property 0x10) and
//   keep only one type, the Type 21 scan alone about 4.5 s per load. The type of each exemplar
//   is remembered, and a scan's callback is skipped for exemplars of another type. Exemplars not
//   seen before go to the game as before.
//
// Windows SimCity 4 1.1.641 only (docs/re-notes/loadcache.md). Writes SC4LoadCache.log next to
// the DLL.

#include <windows.h>

#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <MinHook.h>

#include "cIGZFrameWork.h"
#include "cRZMessage2COMDirector.h"

namespace
{
    // ---- game addresses ----
    constexpr uintptr_t kResourceManagerPtr = 0x00b43ca8;
    constexpr uintptr_t kGetResourceList = 0x009739cb;   // thiscall(rm, list** out, filter*) -> count
    constexpr uintptr_t kGetPropertyUint = 0x005fd480;   // cdecl(holder, id, uint32_t* out) -> bool
    constexpr uintptr_t kOperatorNew = 0x005e55e0;       // cdecl(size)
    constexpr uintptr_t kKeyListCtor = 0x00974e7a;       // thiscall(memory) -> list
    constexpr uintptr_t kTypeFilterVtable = 0x00ad8970;  // {vtable, refs, type}
    constexpr uint32_t kPropertyHolderIid = 0xa52160f5;
    constexpr uint32_t kExemplarTypeProperty = 0x10;
    constexpr uint16_t kGameVersion = 641;

    // Resource manager fields: the segment deque begin iterator at +0x58 {cur, first, last,
    // node}, its end cur at +0x68, buffers of 32 pointers. Two critical sections: +0x80 for
    // resources (GetResource, GetResourceList), +0x50 for the segment deque (register,
    // unregister, FindDBSegment). The game takes +0x80 first.
    constexpr size_t kSegmentsBegin = 0x58;
    constexpr size_t kSegmentsEnd = 0x68;
    constexpr size_t kResourceLock = 0x80;
    constexpr size_t kSegmentLock = 0x50;
    constexpr int kDequeBuffer = 32;
    // Larger lists are not kept: the textures (about 2.6 million keys, 31 MB for the session)
    // are asked for once per load and cost 0.3 s; the 32-bit game needs the address space more.
    constexpr uint32_t kMaxKeptKeys = 500'000;

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

    // The game's key list (vtable 0x00aada64): a deque of keys in insertion order, duplicates
    // kept.
    using KeyCallback = void(__cdecl*)(const Key*, void*);
    void ListInsert(void* list, const Key& key) { Slot<bool(__thiscall*)(void*, const Key&)>(list, 0x10)(list, key); }
    void ListEnum(void* list, KeyCallback callback, void* context)
    {
        Slot<void(__thiscall*)(void*, KeyCallback, void*)>(list, 0x1c)(list, callback, context);
    }
    uint32_t ListSize(void* list) { return Slot<uint32_t(__thiscall*)(void*)>(list, 0x24)(list); }
    // Segment slot +0x48: appends the keys the filter takes, returns their count.
    uint32_t SegmentKeys(void* segment, void* list, void* filter)
    {
        return Slot<uint32_t(__thiscall*)(void*, void*, void*)>(segment, 0x48)(segment, list, filter);
    }

    // As GetResourceList makes one: the game's allocator and constructor, one reference.
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

    // The keys of one type in the start-up segments: per segment its run in `keys` and the
    // count it returned.
    struct TypeKeys
    {
        struct Run
        {
            uint32_t first = 0, size = 0, returned = 0;
        };
        std::vector<Key> keys;
        std::unordered_map<void*, Run> runs;
    };
    std::mutex gListMutex;
    // Registered before PostAppInit. Written once there, read without a lock afterwards.
    std::unordered_set<void*> gStartupSegments;
    std::unordered_map<uint32_t, TypeKeys> gTypes;
    uint64_t gListHits = 0, gListMisses = 0;

    struct RmLock
    {
        void* cs;
        RmLock(void* rm, const size_t offset) : cs(static_cast<uint8_t*>(rm) + offset)
        {
            Slot<void(__thiscall*)(void*)>(cs, 0x4)(cs);
        }
        ~RmLock() { Slot<void(__thiscall*)(void*)>(cs, 0x8)(cs); }
        RmLock(const RmLock&) = delete;
        RmLock& operator=(const RmLock&) = delete;
    };

    // The segments of the resource manager, in its order.
    std::vector<void*> Segments(void* rm)
    {
        RmLock segmentLock(rm, kSegmentLock);
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

    void __cdecl CollectKey(const Key* key, void* keys) { static_cast<std::vector<Key>*>(keys)->push_back(*key); }

    // The game's GetResourceList (0x009739cb) for a type filter: the same list, in the same
    // order, with the keys of start-up segments from the cache.
    uint32_t __fastcall GetResourceListHook(void* rm, void* edx, void** out, void* filter)
    {
        if (!filter || *static_cast<uintptr_t*>(filter) != kTypeFilterVtable || !out) {
            return gGetResourceList(rm, edx, out, filter);
        }
        const uint32_t type = static_cast<uint32_t*>(filter)[2];
        const auto start = std::chrono::steady_clock::now();
        RmLock rmLock(rm, kResourceLock);  // the game's order: its lock first, then ours
        std::scoped_lock lock(gListMutex);
        if (gStartupSegments.empty()) {
            return gGetResourceList(rm, edx, out, filter);
        }
        const std::vector<void*> segments = Segments(rm);
        void* list = NewKeyList();
        if (!list) {
            return gGetResourceList(rm, edx, out, filter);
        }
        *out = list;
        uint32_t count = 0;
        if (const auto known = gTypes.find(type); known != gTypes.end()) {
            size_t asked = 0;
            for (void* segment : segments) {
                const auto run = known->second.runs.find(segment);
                if (run == known->second.runs.end()) {
                    count += SegmentKeys(segment, list, filter);  // not a start-up segment
                    ++asked;
                    continue;
                }
                const Key* key = known->second.keys.data() + run->second.first;
                for (uint32_t i = 0; i < run->second.size; ++i) {
                    ListInsert(list, key[i]);
                }
                count += run->second.returned;
            }
            if (++gListHits <= 100) {
                Log("Key lists: type %08X from the cache in %.1f ms (%u keys, %zu segments asked)", type, MsSince(start),
                    count, asked);
            }
            return count;
        }
        // First time for this type: the game's loop, noting each start-up segment's run.
        TypeKeys entry;
        uint32_t size = 0;
        bool whole = true;
        for (void* segment : segments) {
            const uint32_t returned = SegmentKeys(segment, list, filter);
            count += returned;
            const uint32_t after = ListSize(list);
            whole &= after >= size;
            if (gStartupSegments.contains(segment)) {
                entry.runs[segment] = {size, after >= size ? after - size : 0, returned};
            }
            size = after;
        }
        ++gListMisses;
        // The list is complete here; a failure below only means nothing is kept.
        try {
            if (whole && size > kMaxKeptKeys) {
                whole = false;
            }
            if (whole) {
                // The runs index the whole list (the keys of later segments stay in it unused).
                entry.keys.reserve(size);
                ListEnum(list, &CollectKey, &entry.keys);
                whole = entry.keys.size() == size;
            }
            if (whole) {
                gTypes.emplace(type, std::move(entry));
            }
        }
        catch (...) {
            whole = false;
        }
        Log("Key lists: type %08X built in %.0f ms (%u keys, %zu segments)%s", type, MsSince(start), count,
            segments.size(), whole ? ", kept" : ", not kept");
        return count;
    }

    // ---- exemplar scans ----
    // ExemplarType of every exemplar a scan has seen. kNoType: it did not load or has none.
    constexpr uint32_t kNoType = 0xFFFFFFFF;
    std::mutex gTypeMutex;
    std::unordered_map<Key, uint32_t, KeyHash> gExemplarTypes;
    uint64_t gScanSkipped = 0, gScanPassed = 0;

    // The type to remember for an exemplar, or nothing: when it did not load (maybe for a
    // while only) or when the record the game uses is not in a start-up segment (a city save,
    // which can differ from city to city).
    std::optional<uint32_t> ReadExemplarType(const Key& key)
    {
        void* rm = *reinterpret_cast<void**>(kResourceManagerPtr);
        if (!rm) {
            return std::nullopt;
        }
        // FindDBSegment (slot +0x80): the first segment with the record, as GetResource takes it.
        void* segment = nullptr;
        if (!Slot<bool(__thiscall*)(void*, const Key&, void**)>(rm, 0x80)(rm, key, &segment) || !segment) {
            return std::nullopt;
        }
        const bool startup = gStartupSegments.contains(segment);
        Release(segment);
        if (!startup) {
            return std::nullopt;
        }
        void* resource = nullptr;
        using GetResourceFn = bool(__thiscall*)(void*, const Key&, uint32_t, void**, uint32_t, void*);
        if (!Slot<GetResourceFn>(rm, 0xc)(rm, key, kPropertyHolderIid, &resource, 0, nullptr) || !resource) {
            return std::nullopt;
        }
        void* holder = Slot<void*(__thiscall*)(void*)>(resource, 0x1c)(resource);
        uint32_t type = 0;
        const bool found =
            holder && reinterpret_cast<bool(__cdecl*)(void*, uint32_t, uint32_t*)>(kGetPropertyUint)(
                          holder, kExemplarTypeProperty, &type);
        Release(resource);
        return found ? type : kNoType;
    }

    // A scan callback, cdecl (const key*, context). Each loads the exemplar, reads property 0x10
    // and only releases it again when the type is not one it takes (each one checked in Ghidra,
    // docs/re-notes/loadcache.md).
    struct Scan
    {
        uintptr_t address;
        const char* name;
        std::array<uint32_t, 3> types;  // the types it takes, kNoType for none
        size_t contextTypeOffset;       // or, when not 0: the type it takes is at context + this
    };
    constexpr Scan kScans[] = {
        {0x0060fb90, "network props (type 0x21)", {0x21, kNoType, kNoType}, 0},
        {0x00513a60, "automata", {kNoType, kNoType, kNoType}, 0xa40},
        {0x006a7840, "demand (type 3)", {0x3, kNoType, kNoType}, 0},
        {0x006b03b0, "building development (type 0xf)", {0xf, kNoType, kNoType}, 0},
        {0x00699e20, "type 4", {0x4, kNoType, kNoType}, 0},
        {0x0069d130, "types 2, 0x11, 0x15", {0x2, 0x11, 0x15}, 0},
        {0x0078cb40, "type 0x29", {0x29, kNoType, kNoType}, 0},
        {0x0080e430, "type 0xe", {0xe, kNoType, kNoType}, 0},
        {0x004a42a0, "type 0x1e", {0x1e, kNoType, kNoType}, 0},
    };
    constexpr size_t kScanCount = std::size(kScans);
    KeyCallback gScanOriginal[kScanCount]{};

    template <size_t I>
    void __cdecl ScanHook(const Key* key, void* context)
    {
        constexpr const Scan& scan = kScans[I];
        uint32_t type = kNoType;
        bool known = false;
        {
            std::scoped_lock lock(gTypeMutex);
            if (const auto it = gExemplarTypes.find(*key); it != gExemplarTypes.end()) {
                type = it->second;
                known = true;
            }
        }
        if (known) {
            const bool takes =
                type != kNoType &&
                (scan.contextTypeOffset != 0
                     ? type == *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(context) +
                                                                   scan.contextTypeOffset)
                     : type == scan.types[0] || type == scan.types[1] || type == scan.types[2]);
            if (!takes) {
                ++gScanSkipped;
                return;
            }
        }
        gScanOriginal[I](key, context);
        ++gScanPassed;
        if (!known) {
            // The callback loaded it just now, so this read comes from the game's cache.
            if (const std::optional<uint32_t> read = ReadExemplarType(*key)) {
                try {
                    std::scoped_lock lock(gTypeMutex);
                    gExemplarTypes.emplace(*key, *read);
                }
                catch (...) {
                }
            }
        }
    }

    template <size_t... I>
    std::array<void*, kScanCount> ScanHooks(std::index_sequence<I...>)
    {
        return {reinterpret_cast<void*>(&ScanHook<I>)...};
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
        const std::array<void*, kScanCount> scanHooks = ScanHooks(std::make_index_sequence<kScanCount>());
        for (size_t i = 0; i < kScanCount; ++i) {
            hook(kScans[i].address, scanHooks[i], reinterpret_cast<void**>(&gScanOriginal[i]), kScans[i].name);
        }
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
            RmLock rmLock(rm, kResourceLock);
            std::scoped_lock lock(gListMutex);
            for (void* segment : Segments(rm)) {
                // Held for the session: no later segment can come at the same address.
                if (gStartupSegments.insert(segment).second) {
                    ::AddRef(segment);
                }
            }
            Log("%zu segments at start-up", gStartupSegments.size());
        }
        return true;
    }

    bool PreAppShutdown() override
    {
        Log("Key lists: %llu from the cache, %llu built. Exemplar scans: %llu callbacks skipped, %llu passed, %zu "
            "exemplar types known",
            gListHits, gListMisses, gScanSkipped, gScanPassed, gExemplarTypes.size());
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
