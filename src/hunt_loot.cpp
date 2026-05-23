#include "hunt_loot.h"
#include "hunt_town.h"
#include "hooks.h"
#include "CHero.h"
#include "CGameMap.h"
#include "CItem.h"
#include "itemtype.h"
#include "pathfinder.h"
#include "log.h"
#include <algorithm>
#include <unordered_set>

// ── File-local constants ──────────────────────────────────────────────────────
static constexpr int   kLootPickupAttemptLimit   = 2;
static constexpr DWORD kLootTargetSwitchIntervalMs = 100;

static constexpr int kMinLootPickupIgnoreMs = 0;
static constexpr int kMaxLootPickupIgnoreMs = 300000;
static constexpr int kMinItemActionIntervalMs = 100;
static constexpr int kMaxItemActionIntervalMs = 5000;
static constexpr int kMinLootSpawnGraceMs = 0;
static constexpr int kMaxLootSpawnGraceMs = 5000;
static constexpr DWORD kDropRecordMatchWindowMs = 30000;  // match items against drop records within 30s

// ── File-local helpers ────────────────────────────────────────────────────────
namespace {

DWORD ClampMs(int value, int minVal, int maxVal)
{
    return static_cast<DWORD>(std::clamp(value, minVal, maxVal));
}

DWORD GetItemActionIntervalMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.itemActionIntervalMs, kMinItemActionIntervalMs, kMaxItemActionIntervalMs);
}

DWORD GetLootPickupIgnoreMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.lootPickupIgnoreMs, kMinLootPickupIgnoreMs, kMaxLootPickupIgnoreMs);
}

DWORD GetLootSpawnGraceMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.lootSpawnGraceMs, kMinLootSpawnGraceMs, kMaxLootSpawnGraceMs);
}

bool IsConfirmedDrop(const Position& pos, DWORD now)
{
    for (const auto& drop : GetLootDropRecords()) {
        if ((now - drop.tick) > kDropRecordMatchWindowMs)
            continue;
        if (std::abs(pos.x - drop.pos.x) <= 1 && std::abs(pos.y - drop.pos.y) <= 1)
            return true;
    }
    return false;
}

bool TickIsFuture(DWORD targetTick, DWORD now)
{
    return static_cast<int32_t>(targetTick - now) > 0;
}

bool IsMovementCommand(const CCommand& cmd)
{
    return cmd.iType == _COMMAND_WALK
        || cmd.iType == _COMMAND_RUN
        || cmd.iType == _COMMAND_WALKFORWARD
        || cmd.iType == _COMMAND_RUNFORWARD
        || cmd.iType == _COMMAND_JUMP;
}

bool IsMovementCommandStillAdvancing(const CHero* hero)
{
    if (!hero)
        return false;
    const CCommand& cmd = hero->GetCommand();
    if (!IsMovementCommand(cmd))
        return false;
    return cmd.posTarget.x != hero->m_posMap.x || cmd.posTarget.y != hero->m_posMap.y;
}

} // namespace

// ── HuntLootManager implementation ───────────────────────────────────────────

std::shared_ptr<CMapItem> HuntLootManager::FindBestLoot(
    CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    std::function<bool(OBJID, DWORD)> isLootPickupIgnoredFn,
    std::function<bool(OBJID mapId, const Position&)> isPointInZoneFn) const
{
    if (!hero || !map) return nullptr;
    if (hero->IsBagFull()) {
        spdlog::trace("[hunt-loot] FindBestLoot: bag full, skip");
        return nullptr;
    }

    const DWORD now = GetTickCount();
    const DWORD spawnGraceMs = GetLootSpawnGraceMs(settings);
    std::shared_ptr<CMapItem> best;
    float bestDist = (std::numeric_limits<float>::max)();
    int totalItems = 0, skippedFilter = 0, skippedIgnored = 0, skippedZone = 0, skippedSpawnGrace = 0, skippedNotOurDrop = 0;

    for (const auto& itemRef : map->m_vecItems) {
        if (!itemRef) continue;
        ++totalItems;
        const auto seenResult = m_lootSeenTicks.try_emplace(itemRef->m_id, now);
        const DWORD seenAge = now - seenResult.first->second;
        if (seenAge < spawnGraceMs) { ++skippedSpawnGrace; continue; }
        if (isLootPickupIgnoredFn && isLootPickupIgnoredFn(itemRef->m_id, now)) { ++skippedIgnored; continue; }
        if (isPointInZoneFn && !isPointInZoneFn(map->GetId(), itemRef->m_pos)) { ++skippedZone; continue; }

        // Phase 2a: gold-value floor.  Universal pickup filter — applies even
        // to confirmed drops.  Items explicitly listed in lootItemIds bypass
        // the floor (user-curated whitelist always wins).
        if (settings.minimumLootGoldValue > 0
            && !HuntTownService::IsSelectedLootItem(settings, itemRef->m_idType)
            && !(settings.lootMoney && HuntTownService::IsMoneyMapItem(*itemRef))) {
            const ItemTypeInfo* info = GetItemTypeInfo(itemRef->m_idType);
            if (info && info->price < (uint32_t)settings.minimumLootGoldValue) {
                ++skippedFilter;
                continue;
            }
        }

        // Confirmed drops (our kill via system message) are always looted.
        // Other items must pass the item filter (loot list, quality, or plus).
        const bool confirmed = IsConfirmedDrop(itemRef->m_pos, now);
        if (!confirmed) {
            if (!HuntTownService::ShouldLootMapItem(settings, *itemRef)) { ++skippedFilter; continue; }
            if (!HuntTownService::IsSelectedLootItem(settings, itemRef->m_idType)
                && !(settings.lootMoney && HuntTownService::IsMoneyMapItem(*itemRef))) {
                ++skippedNotOurDrop;
                continue;
            }
        }

        const float dist = hero->m_posMap.DistanceTo(itemRef->m_pos);
        if (dist < bestDist) {
            bestDist = dist;
            best = itemRef;
        }
    }

    if (best) {
        spdlog::trace("[hunt-loot] FindBestLoot: picked id={} type={} pos=({},{}) dist={:.1f} | ground={} filteredOut={} ignored={} outOfZone={} spawnGrace={} notOurDrop={}",
            best->m_id, best->m_idType, best->m_pos.x, best->m_pos.y, bestDist,
            totalItems, skippedFilter, skippedIgnored, skippedZone, skippedSpawnGrace, skippedNotOurDrop);
    }

    return best;
}

bool HuntLootManager::IsLootPickupIgnored(OBJID itemId, DWORD now) const
{
    const auto it = m_lootPickupAttempts.find(itemId);
    if (it == m_lootPickupAttempts.end())
        return false;
    return TickIsFuture(it->second.ignoreUntilTick, now);
}

void HuntLootManager::RecordLootPickupAttempt(OBJID itemId, DWORD now,
    const AutoHuntSettings& settings)
{
    LootPickupAttemptState& state = m_lootPickupAttempts[itemId];
    if (!TickIsFuture(state.ignoreUntilTick, now)) {
        state.ignoreUntilTick = 0;
        if (state.attempts >= kLootPickupAttemptLimit)
            state.attempts = 0;
    }

    if (state.attempts < UINT8_MAX)
        ++state.attempts;

    if (state.attempts >= kLootPickupAttemptLimit) {
        const DWORD ignoreMs = GetLootPickupIgnoreMs(settings);
        state.attempts = 0;
        state.ignoreUntilTick = now + ignoreMs;
        spdlog::info("[hunt-loot] Item id={} hit pickup attempt limit ({}), ignoring for {}ms",
            itemId, kLootPickupAttemptLimit, ignoreMs);
    } else {
        spdlog::trace("[hunt-loot] Item id={} pickup attempt {}/{}", itemId, state.attempts, kLootPickupAttemptLimit);
    }
}

void HuntLootManager::PruneLootPickupAttempts(CGameMap* map)
{
    if (m_lootPickupAttempts.empty() && m_lootSeenTicks.empty())
        return;
    if (!map) {
        ResetLootPickupAttempts();
        return;
    }

    std::unordered_set<OBJID> activeItemIds;
    activeItemIds.reserve(map->m_vecItems.size());
    for (const auto& itemRef : map->m_vecItems) {
        if (itemRef)
            activeItemIds.insert(itemRef->m_id);
    }

    for (auto it = m_lootPickupAttempts.begin(); it != m_lootPickupAttempts.end();) {
        if (activeItemIds.find(it->first) == activeItemIds.end()) {
            it = m_lootPickupAttempts.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = m_lootSeenTicks.begin(); it != m_lootSeenTicks.end();) {
        if (activeItemIds.find(it->first) == activeItemIds.end()) {
            it = m_lootSeenTicks.erase(it);
        } else {
            ++it;
        }
    }
}

void HuntLootManager::ResetLootPickupAttempts()
{
    m_lootPickupAttempts.clear();
    m_lootSeenTicks.clear();
    m_lastLootItemId = 0;
}

bool HuntLootManager::TryPickupLootItem(CHero* hero, const AutoHuntSettings& settings,
    const std::shared_ptr<CMapItem>& item, DWORD now,
    std::function<bool(DWORD)> updatePendingJumpFn)
{
    if (!hero || !item)
        return false;

    const DWORD spawnGraceMs = GetLootSpawnGraceMs(settings);
    const auto seenResult = m_lootSeenTicks.try_emplace(item->m_id, now);
    const DWORD seenAge = now - seenResult.first->second;
    if (seenAge < spawnGraceMs) {
        spdlog::trace("[hunt-loot] Pickup wait id={} type={} reason=spawn_grace age={}ms grace={}ms",
            item->m_id, item->m_idType, seenAge, spawnGraceMs);
        return false;
    }

    const int actualDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, item->m_pos.x, item->m_pos.y);
    if (actualDist != 0) {
        spdlog::trace("[hunt-loot] Pickup wait id={} type={} dist={} reason=not_on_tile",
            item->m_id, item->m_idType, actualDist);
        return false;
    }

    if (hero->IsJumping() || IsMovementCommandStillAdvancing(hero)) {
        spdlog::trace("[hunt-loot] Pickup wait id={} type={} reason=movement_not_settled pos=({},{}) target=({},{})",
            item->m_id, item->m_idType, hero->m_posMap.x, hero->m_posMap.y, item->m_pos.x, item->m_pos.y);
        return false;
    }

    if (updatePendingJumpFn && updatePendingJumpFn(now)) {
        spdlog::trace("[hunt-loot] Pickup wait id={} type={} reason=pending_jump age={}ms",
            item->m_id, item->m_idType, now);
        return false;
    }

    if (Pathfinder::Get().IsActive()) {
        Pathfinder::Get().Stop();
        spdlog::trace("[hunt-loot] Pickup wait id={} type={} reason=stopping_pathfinder",
            item->m_id, item->m_idType);
        return false;
    }

    const DWORD pathfinderJumpAge = now - Pathfinder::Get().GetLastJumpTick();
    if (Pathfinder::Get().GetLastJumpTick() != 0 && pathfinderJumpAge < 500) {
        spdlog::trace("[hunt-loot] Pickup wait id={} type={} reason=pathfinder_settle age={}ms",
            item->m_id, item->m_idType, pathfinderJumpAge);
        return false;
    }

    const DWORD interval = m_lastLootItemId == item->m_id
        ? GetItemActionIntervalMs(settings)
        : kLootTargetSwitchIntervalMs;
    const DWORD elapsed = now - m_lastLootTick;
    if (elapsed < interval) {
        spdlog::trace("[hunt-loot] TryPickup throttled id={} type={} elapsed={}ms interval={}ms",
            item->m_id, item->m_idType, elapsed, interval);
        return false;
    }

    hero->PickupItem(*item);
    m_lastLootTick   = now;
    m_lastLootItemId = item->m_id;
    RecordLootPickupAttempt(item->m_id, now, settings);
    spdlog::trace("[hunt-loot] PickupItem id={} type={} pos=({},{}) sent_on_tile",
        item->m_id, item->m_idType, item->m_pos.x, item->m_pos.y);
    return true;
}

// =============================================================================
// Phase 2a: bag-full trash drop
// =============================================================================

bool HuntLootManager::IsBagItemTrash(const AutoHuntSettings& settings, const CItem& item)
{
    const uint32_t typeId = item.GetTypeID();

    // ── Hard safety filters: never drop these ─────────────────────────────────
    // 1) Anything in any user-curated keep list.
    if (HuntTownService::IsSelectedLootItem(settings, typeId))           return false;
    if (HuntTownService::IsSelectedWarehouseItem(settings, typeId))      return false;
    if (HuntTownService::IsSelectedPriorityReturnItem(settings, typeId)) return false;
    // 2) Equipment is never auto-dropped from the bag.
    if (item.IsEquipment())                                              return false;
    // 3) Anything plussed (+1 or higher) — these are valuable upgrades.
    if (item.GetPlus() > 0)                                              return false;
    // 4) Don't drop arrows the archer plugin maintains.
    if (settings.arrowTypeId != 0 && typeId == settings.arrowTypeId)     return false;

    // ── Now apply the user's trash criteria (OR-of-thresholds) ───────────────
    bool flagged = false;
    if (settings.autoDropMinKeepQuality > 0) {
        const int quality = (int)(typeId % 10);
        if (quality < settings.autoDropMinKeepQuality)
            flagged = true;
    }
    if (!flagged && settings.autoDropMinKeepPrice > 0) {
        if (const ItemTypeInfo* info = GetItemTypeInfo(typeId)) {
            if (info->price < (uint32_t)settings.autoDropMinKeepPrice)
                flagged = true;
        }
    }
    return flagged;
}

bool HuntLootManager::TryDropTrashItem(CHero* hero, const AutoHuntSettings& settings, DWORD now)
{
    if (!hero) return false;
    if (!settings.autoDropTrashWhenFull) return false;
    // Both criteria off → nothing to do.
    if (settings.autoDropMinKeepQuality <= 0 && settings.autoDropMinKeepPrice <= 0)
        return false;

    // Only fire when the bag is at/over the user's storage threshold.
    const int bagThreshold = std::clamp(settings.bagStoreThreshold, 1, CHero::MAX_BAG_ITEMS);
    if ((int)hero->m_deqItem.size() < bagThreshold)
        return false;

    // Throttle: re-use itemActionIntervalMs so we don't spam drop packets.
    const DWORD interval = GetItemActionIntervalMs(settings);
    if ((now - m_lastTrashDropTick) < interval)
        return false;

    // Pick the lowest-quality trash item (deterministic; avoids sticky loops).
    CItem* victim = nullptr;
    int    victimScore = 0;
    for (const auto& itemRef : hero->m_deqItem) {
        if (!itemRef) continue;
        if (!IsBagItemTrash(settings, *itemRef)) continue;
        // Lower quality first, then lower price (rough proxy for worst-first).
        const uint32_t typeId = itemRef->GetTypeID();
        const int      qualityScore = (int)(typeId % 10) * 1000000;
        const ItemTypeInfo* info = GetItemTypeInfo(typeId);
        const int      priceScore = info ? (int)std::min<uint32_t>(info->price, 999999u) : 999999;
        const int      score = qualityScore + priceScore;
        if (!victim || score < victimScore) {
            victim = itemRef.get();
            victimScore = score;
        }
    }
    if (!victim) return false;

    hero->DropItem(victim->GetID(), hero->m_posMap);
    m_lastTrashDropTick   = now;
    m_lastTrashDropItemId = victim->GetID();
    spdlog::info("[hunt-loot] DropTrash id={} type={} name='{}' quality={} plus={} bag={}/{}",
        victim->GetID(), victim->GetTypeID(), victim->GetName(),
        victim->GetQuality(), victim->GetPlus(),
        (int)hero->m_deqItem.size(), CHero::MAX_BAG_ITEMS);
    return true;
}
