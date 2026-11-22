#include "capture/SeasonOverride.hpp"

#include <bit>
#include <chrono>
#include <limits>

#include "cIGZVariant.h"
#include "cISC4City.h"
#include "cISC4FloraOccupant.h"
#include "cISC4Occupant.h"
#include "cISC4OccupantManager.h"
#include "cISC4PropOccupant.h"
#include "cISCProperty.h"
#include "cISCPropertyHolder.h"
#include "cS3DVector3.h"
#include "SC4List.h"
#include "capture/SeasonRules.hpp"
#include "core/GameAccess.hpp"

namespace
{
    // cSC4PropDeveloper (docs/re-notes/seasons.md, sections 1 and Re-validation): std::list
    // heads of the props with a date range and of those with a time of day range. Each head
    // points to the sentinel node.
    constexpr uint32_t kTimedProps = 0xa5c;
    constexpr uint32_t kTimeOfDayProps = 0xa60;
    // cSC4PropOccupant: the DateInfo block (interval +0, duration +4), null without a range.
    constexpr uint32_t kDateInfo = 0x54;
    constexpr uint32_t kPropStartingDate = 0xCA7515CC;  // uint8 month, day
    constexpr uint8_t kDateBit = 0x2;
    constexpr uint8_t kTimeOfDayBit = 0x1;
    constexpr float kNoon = 12.0f;

    // cSC4FloraOccupant: static flora data (FloraParameters p[i] at +0x10 + 4i).
    constexpr uint32_t kFloraOccupantType = 0x74758926;
    constexpr uint32_t kFloraIid = 0xc83dd057;
    constexpr uint32_t kFloraStaticData = 0x54;
    constexpr uint32_t kFloraP0 = 0x10;
    constexpr uint32_t kFloraCycle = 0x24;  // p[5]
    // The occupant message the flora simulator posts after a stage change: the model maker
    // builds the model of the new state on the next pump.
    constexpr uint32_t kOccupantChanged = 0x99ef1145;

    struct ListNode
    {
        ListNode* next;
        ListNode* prev;
        cISC4PropOccupant* prop;
    };

    template <typename F>
    void ForEachProp(const void* developer, const uint32_t offset, F&& f)
    {
        const auto* head = GameAccess::ReadField<const ListNode*>(developer, offset);
        if (!head) {
            return;
        }
        for (const ListNode* node = head->next; node && node != head; node = node->next) {
            if (node->prop) {
                f(node->prop);
            }
        }
    }

    // 0..99 from the position: the same prop gets the same roll in every render.
    int StableRoll(cISC4PropOccupant* prop)
    {
        cS3DVector3 position;
        cISC4Occupant* occupant = prop->AsOccupant();
        if (!occupant || !occupant->GetPosition(&position)) {
            return 0;
        }
        uint32_t h = std::bit_cast<uint32_t>(position.fX) * 73856093u;
        h ^= std::bit_cast<uint32_t>(position.fY) * 19349663u;
        h ^= std::bit_cast<uint32_t>(position.fZ) * 83492791u;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        return static_cast<int>(h % 100u);
    }
}

SeasonOverride::Counts SeasonOverride::Apply(cISC4City* city, const Options& options)
{
    Restore(true);
    Counts counts;
    if (!city) {
        return counts;
    }
    const auto start = std::chrono::steady_clock::now();
    if (options.date || options.noon) {
        ApplyProps_(city, options, counts);
    }
    if (options.date && options.flora) {
        ApplyFlora_(city, options, counts);
    }
    counts.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return counts;
}

std::optional<std::pair<int, int>> SeasonOverride::StartDate_(cISC4PropOccupant* prop)
{
    const uint32_t type = prop->GetPropType();
    if (const auto it = starts_.find(type); it != starts_.end()) {
        return it->second;
    }
    std::optional<std::pair<int, int>> date;
    cISC4Occupant* occupant = prop->AsOccupant();
    const cISCPropertyHolder* holder = occupant ? occupant->AsPropertyHolder() : nullptr;
    const cISCProperty* property = holder ? holder->GetProperty(kPropStartingDate) : nullptr;
    const cIGZVariant* value = property ? property->GetPropertyValue() : nullptr;
    if (value && (value->GetType() & ~cIGZVariant::TypeArray) == cIGZVariant::Uint8 && value->GetCount() >= 2) {
        if (const uint8_t* bytes = value->RefUint8()) {
            date = std::make_pair(static_cast<int>(bytes[0]), static_cast<int>(bytes[1]));
        }
    }
    starts_.emplace(type, date);
    return date;
}

void SeasonOverride::ApplyProps_(cISC4City* city, const Options& options, Counts& counts)
{
    const void* developer = city->GetPropDeveloper();
    if (!developer) {
        return;
    }
    // A prop can be in both lists: bit 0 timed, bit 1 time of day.
    std::unordered_map<cISC4PropOccupant*, uint8_t> lists;
    if (options.date) {
        ForEachProp(developer, kTimedProps, [&](cISC4PropOccupant* prop) { lists[prop] |= 1; });
    }
    if (options.noon) {
        ForEachProp(developer, kTimeOfDayProps, [&](cISC4PropOccupant* prop) { lists[prop] |= 2; });
    }
    for (const auto& [prop, in] : lists) {
        const uint8_t bits = prop->GetPropTimerBitfield();
        uint8_t wanted = bits;
        if (in & 1) {
            const auto* info = GameAccess::ReadField<const uint8_t*>(prop, kDateInfo);
            const auto startDate = info ? StartDate_(prop) : std::nullopt;
            const uint32_t interval = info ? *reinterpret_cast<const uint32_t*>(info) : 0;
            // Other intervals (weekly events, sentinels) and invalid starts keep the live state.
            if (startDate && SeasonRules::IsYearlyInterval(interval) &&
                SeasonRules::IsValidDate(startDate->first, startDate->second)) {
                const uint32_t duration = *reinterpret_cast<const uint32_t*>(info + 4);
                const bool on = SeasonRules::IsTimedPropOn(startDate->first, startDate->second, duration,
                                                           options.month, options.day);
                wanted = on ? wanted | kDateBit : wanted & ~kDateBit;
            }
        }
        if (in & 2) {
            float from = 0.0f, to = 0.0f;
            prop->GetTimeOfDayRange(from, to);
            wanted = SeasonRules::IsInTimeRange(kNoon, from, to) ? wanted | kTimeOfDayBit : wanted & ~kTimeOfDayBit;
        }
        if (wanted == bits) {
            continue;  // the game shows it as the date and time want already
        }
        const int target = SeasonRules::PropState(wanted, bits, prop->GetRandomChance(), StableRoll(prop));
        const uint8_t state = prop->GetState();
        if (target < 0 || target == state) {
            continue;
        }
        prop->AddRef();
        props_.push_back({prop, state, bits, static_cast<uint8_t>(target)});
        prop->SetState(static_cast<uint32_t>(target));
        ++counts.props;
    }
}

void SeasonOverride::ApplyFlora_(cISC4City* city, const Options& options, Counts& counts)
{
    cISC4OccupantManager* occupants = city->GetOccupantManager();
    if (!occupants) {
        return;
    }
    const float boxMin[3] = {-1.0f, -100000.0f, -1.0f};
    const float boxMax[3] = {city->SizeX() + 1.0f, 100000.0f, city->SizeZ() + 1.0f};
    SC4List<cISC4Occupant*> found;
    if (!occupants->GetOccupantsByBBox(found, boxMin, boxMax, kFloraOccupantType,
                                       std::numeric_limits<uint32_t>::max())) {
        return;
    }
    for (cISC4Occupant* occupant : found) {
        cISC4FloraOccupant* flora = nullptr;
        if (!occupant || !occupant->QueryInterface(kFloraIid, reinterpret_cast<void**>(&flora)) || !flora) {
            continue;
        }
        ++counts.floraSeen;
        bool keep = false;
        if (const auto* data = GameAccess::ReadField<const uint8_t*>(flora, kFloraStaticData)) {
            const float p0 = *reinterpret_cast<const float*>(data + kFloraP0);
            const float cycle = *reinterpret_cast<const float*>(data + kFloraCycle);
            const int32_t state = flora->GetState();
            // Stages 3 and 4 (old, burnt) are real growth: left alone.
            if (SeasonRules::IsSeasonalFlora(cycle) && state >= 0 && state <= 2) {
                const int32_t target = SeasonRules::SeasonalFloraStage(p0, cycle, options.month);
                if (target != state) {
                    flora->SetState(target);
                    occupant->PostOccupantMessage(kOccupantChanged, 0);
                    flora_.push_back({flora, state, target});
                    keep = true;  // the record keeps the reference of the query
                    ++counts.flora;
                }
            }
        }
        if (!keep) {
            flora->Release();
        }
    }
    occupants->ReleaseOccupantList(found);
}

void SeasonOverride::Restore(const bool live)
{
    for (auto it = props_.rbegin(); it != props_.rend(); ++it) {
        // A state or bits the game changed meanwhile stay as the game set them.
        if (live && it->prop->GetState() == it->target && it->prop->GetPropTimerBitfield() == it->bits) {
            it->prop->SetState(it->oldState);
        }
        it->prop->Release();
    }
    props_.clear();
    for (auto it = flora_.rbegin(); it != flora_.rend(); ++it) {
        if (live && it->flora->GetState() == it->target) {
            it->flora->SetState(it->oldState);
            if (cISC4Occupant* occupant = it->flora->AsOccupant()) {
                occupant->PostOccupantMessage(kOccupantChanged, 0);
            }
        }
        it->flora->Release();
    }
    flora_.clear();
}
