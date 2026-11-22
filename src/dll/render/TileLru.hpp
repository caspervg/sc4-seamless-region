#pragma once

// Render: the memory accounting of the tile cache. Header only, no game calls, unit
// tested in tests/RenderTests.cpp.
//
// Entries are keyed by TileKey and carry a byte size and the stamp of the last draw pass
// that used them. MakeRoom evicts the least recently drawn entries, but never one that was
// drawn in the pass that asked for the new tile or later. That keeps a view that is larger
// than the budget from evicting and reloading its own tiles in a loop.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "store/TileStore.hpp"

struct TileKeyHash
{
    size_t operator()(const TileKey& k) const noexcept
    {
        size_t h = std::hash<std::string_view>{}(k.cityKey);
        for (const int v : {k.rotation, k.zoom, k.tx, k.ty, k.transport ? 1 : 0}) {
            h ^= static_cast<size_t>(v) + 0x9e3779b9u + (h << 6) + (h >> 2);
        }
        return h;
    }
};

struct TileKeyEqual
{
    bool operator()(const TileKey& a, const TileKey& b) const noexcept
    {
        return a.tx == b.tx && a.ty == b.ty && a.zoom == b.zoom && a.rotation == b.rotation &&
               a.transport == b.transport && a.cityKey == b.cityKey;
    }
};

template <typename Value>
class TileLru
{
public:
    void SetBudget(const size_t bytes) { budget_ = bytes; }
    [[nodiscard]] size_t Budget() const { return budget_; }
    [[nodiscard]] size_t Bytes() const { return bytes_; }
    [[nodiscard]] size_t Count() const { return entries_.size(); }

    // Returns the entry and marks it as used in the pass `stamp`, or nullptr.
    [[nodiscard]] Value* Find(const TileKey& key, const uint64_t stamp)
    {
        const auto it = entries_.find(key);
        if (it == entries_.end()) {
            return nullptr;
        }
        it->second.lastUsed = stamp;
        return &it->second.value;
    }

    // Evicts least recently used entries until `bytes` more fit in the budget. Only entries
    // last used before the pass `protectFrom` may go. Returns false when the room cannot be
    // made. onEvict(Value&) runs for every evicted entry.
    // ponytail: linear scan per eviction. Fine for a few thousand tiles, a few evictions
    // per frame. Use an intrusive list if that ever shows in a profile.
    template <typename OnEvict>
    bool MakeRoom(const size_t bytes, const uint64_t protectFrom, OnEvict&& onEvict)
    {
        if (bytes > budget_) {
            return false;
        }
        while (bytes_ + bytes > budget_) {
            auto oldest = entries_.end();
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (it->second.lastUsed < protectFrom &&
                    (oldest == entries_.end() || it->second.lastUsed < oldest->second.lastUsed)) {
                    oldest = it;
                }
            }
            if (oldest == entries_.end()) {
                return false;
            }
            Evict_(oldest, onEvict);
        }
        return true;
    }

    // Adds an entry. Call MakeRoom first. Replaces nothing: the key must be new.
    Value& Insert(const TileKey& key, Value value, const size_t bytes, const uint64_t stamp)
    {
        Entry& entry = entries_[key];
        bytes_ += bytes - entry.bytes;
        entry = Entry{std::move(value), bytes, stamp};
        return entry.value;
    }

    template <typename OnEvict>
    void Clear(OnEvict&& onEvict)
    {
        for (auto& [key, entry] : entries_) {
            onEvict(entry.value);
        }
        entries_.clear();
        bytes_ = 0;
    }

private:
    struct Entry
    {
        Value value{};
        size_t bytes = 0;
        uint64_t lastUsed = 0;
    };
    using Map = std::unordered_map<TileKey, Entry, TileKeyHash, TileKeyEqual>;

    template <typename OnEvict>
    void Evict_(const typename Map::iterator it, OnEvict&& onEvict)
    {
        onEvict(it->second.value);
        bytes_ -= it->second.bytes;
        entries_.erase(it);
    }

    Map entries_;
    size_t budget_ = 0;
    size_t bytes_ = 0;
};
