#include "hunt_stats.h"

#include "CGameMap.h"
#include "CHero.h"
#include "CItem.h"
#include "CRole.h"
#include "discord.h"
#include "game.h"
#include "hunt_settings.h"
#include "itemtype.h"
#include "log.h"
#include "plugins/base_hunt_plugin.h"
#include "plugins/plugin_mgr.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <unordered_set>

namespace HuntStats {

// =====================================================================
// State
// =====================================================================
namespace {

Settings g_settings;
std::mutex g_mtx;  // guards everything below

DWORD     g_sessionStartTick   = 0;
uint64_t  g_kills              = 0;
int       g_deaths             = 0;
int64_t   g_goldDelta          = 0;

OBJID     g_lastHeroId         = 0;
OBJID     g_lastMapId          = 0;
uint32_t  g_lastSilver         = 0;
bool      g_silverBaselined    = false;
bool      g_lastIsDead         = false;
uint64_t  g_lastKillMilestonePosted = 0;

// Monsters previously observed inside the hunt zone — used for the
// "entity disappeared → kill" heuristic.
std::unordered_set<OBJID> g_seenMonsters;

// Inventory snapshot keyed by item UID for new-item diffing.
struct InvSnapshot { uint32_t typeId; uint8_t plus; };
std::unordered_map<OBJID, InvSnapshot> g_lastInventory;
bool g_inventoryBaselined = false;

// Drop accumulation: typeId → count
std::unordered_map<uint32_t, uint32_t> g_drops;

// Helper: is at least one hunt plugin currently enabled?
bool IsHuntActive()
{
    for (auto& p : PluginManager::Get().GetPlugins()) {
        if (auto* h = dynamic_cast<BaseHuntPlugin*>(p.get())) {
            if (h->m_enabled) return true;
        }
    }
    return false;
}

// Reset only the volatile snapshots — used on hero swap / map change.
// Does NOT touch session counters.
void RebaselineSnapshots(CHero* hero)
{
    g_seenMonsters.clear();
    g_lastInventory.clear();
    g_inventoryBaselined = false;
    g_silverBaselined    = false;
    if (hero) {
        g_lastHeroId      = hero->GetID();
        g_lastIsDead      = hero->IsDead() != 0;
        g_lastSilver      = hero->GetSilver();
        g_silverBaselined = true;
        for (const auto& itemRef : hero->m_deqItem) {
            if (itemRef) {
                g_lastInventory[itemRef->GetID()] =
                    InvSnapshot{ itemRef->GetTypeID(), (uint8_t)itemRef->GetPlus() };
            }
        }
        g_inventoryBaselined = true;
    }
}

void ClearSessionCounters()
{
    g_sessionStartTick = GetTickCount();
    g_kills            = 0;
    g_deaths           = 0;
    g_goldDelta        = 0;
    g_drops.clear();
    g_lastKillMilestonePosted = 0;
}

}  // namespace

// =====================================================================
// Public API
// =====================================================================

Settings& GetSettings()
{
    return g_settings;
}

void Reset()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    ClearSessionCounters();
    if (CHero* hero = Game::GetHero())
        RebaselineSnapshots(hero);
    spdlog::info("[stats] Session reset");
}

DWORD GetSessionDurationMs()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_sessionStartTick == 0)
        return 0;
    return GetTickCount() - g_sessionStartTick;
}

uint64_t GetKills()    { std::lock_guard<std::mutex> lk(g_mtx); return g_kills; }
int64_t  GetGoldDelta(){ std::lock_guard<std::mutex> lk(g_mtx); return g_goldDelta; }
int      GetDeaths()   { std::lock_guard<std::mutex> lk(g_mtx); return g_deaths; }
DWORD    GetSessionStartTick() { std::lock_guard<std::mutex> lk(g_mtx); return g_sessionStartTick; }

double GetKillsPerHour()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_sessionStartTick == 0) return 0.0;
    const DWORD elapsed = GetTickCount() - g_sessionStartTick;
    if (elapsed < 30 * 1000) return 0.0;  // wait 30s before showing a rate
    return (double)g_kills * 3600.0 * 1000.0 / (double)elapsed;
}

double GetGoldPerHour()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_sessionStartTick == 0) return 0.0;
    const DWORD elapsed = GetTickCount() - g_sessionStartTick;
    if (elapsed < 30 * 1000) return 0.0;
    return (double)g_goldDelta * 3600.0 * 1000.0 / (double)elapsed;
}

std::vector<DropEntry> GetTopDrops(size_t maxEntries)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    std::vector<DropEntry> out;
    out.reserve(g_drops.size());
    for (const auto& [typeId, count] : g_drops)
        out.push_back({ typeId, count });
    std::sort(out.begin(), out.end(),
        [](const DropEntry& a, const DropEntry& b) { return a.count > b.count; });
    if (out.size() > maxEntries)
        out.resize(maxEntries);
    return out;
}

// =====================================================================
// Per-frame Update
// =====================================================================

namespace {

void NotifyKillMilestone(uint64_t kills)
{
    if (!g_settings.discordOnKillMilestone) return;
    if (g_settings.killMilestoneInterval <= 0) return;
    const uint64_t bucket = kills / (uint64_t)g_settings.killMilestoneInterval;
    if (bucket == 0 || bucket == g_lastKillMilestonePosted) return;
    g_lastKillMilestonePosted = bucket;

    const DWORD elapsed = GetTickCount() - g_sessionStartTick;
    const double minutes = (double)elapsed / 60000.0;
    char msg[256];
    snprintf(msg, sizeof(msg),
        "Hunt milestone: %llu kills in %.1f min (%.0f/hr, gold %+lld)",
        (unsigned long long)kills, minutes,
        (minutes > 0.5) ? ((double)kills * 60.0 / minutes) : 0.0,
        (long long)g_goldDelta);
    SendDiscordNotification(msg, /*mention=*/false);
}

void NotifyDeath(CHero* hero)
{
    if (!g_settings.discordOnDeath) return;
    if (!hero) return;
    CGameMap* map = Game::GetMap();
    char msg[256];
    snprintf(msg, sizeof(msg),
        "[%s] Died on map %u at (%d,%d). Session kills: %llu",
        hero->GetName(),
        map ? map->GetId() : 0,
        hero->m_posMap.x, hero->m_posMap.y,
        (unsigned long long)g_kills);
    SendDiscordNotification(msg, /*mention=*/true);
}

void NotifyNotableDrop(CHero* hero, uint32_t typeId, uint8_t plus)
{
    if (!g_settings.discordOnNotableDrop) return;
    const int quality = (int)(typeId % 10);
    const bool qualityHit = quality >= g_settings.notableDropMinQuality;
    const bool plusHit    = g_settings.notableDropMinPlus > 0
                         && (int)plus >= g_settings.notableDropMinPlus;
    if (!qualityHit && !plusHit) return;

    const std::string itemName = FormatItemName(typeId, plus);
    CGameMap* map = Game::GetMap();
    char msg[384];
    snprintf(msg, sizeof(msg),
        "[%s] Notable drop: %s (id=%u, q=%d, +%u) on map %u at (%d,%d)",
        hero ? hero->GetName() : "?",
        itemName.c_str(), typeId, quality, plus,
        map ? map->GetId() : 0,
        hero ? hero->m_posMap.x : 0,
        hero ? hero->m_posMap.y : 0);
    SendDiscordNotification(msg, /*mention=*/true);
}

// Iterate the entity set and return all monster IDs that are currently inside
// the hunt zone for the current map.  Used to grow the "seen" set; an ID that
// was previously in the set but is no longer present is counted as a kill.
void CollectMonstersInZone(CRoleMgr* mgr, OBJID currentMapId,
                           const AutoHuntSettings& settings,
                           std::unordered_set<OBJID>& outPresent)
{
    if (!mgr) return;
    if (settings.zoneMapId == 0 || settings.zoneMapId != currentMapId)
        return;
    for (const auto& ref : mgr->m_deqRole) {
        CRole* role = ref.get();
        if (!role) continue;
        if (!role->IsMonster()) continue;
        if (role->IsDead()) continue;
        if (!IsPointInHuntZone(settings, currentMapId, role->m_posMap))
            continue;
        outPresent.insert(role->GetID());
    }
}

}  // namespace

void Update()
{
    std::lock_guard<std::mutex> lk(g_mtx);

    // First-ever tick — initialize session timer.
    if (g_sessionStartTick == 0)
        ClearSessionCounters();

    CHero*    hero = Game::GetHero();
    CGameMap* map  = Game::GetMap();
    CRoleMgr* mgr  = Game::GetRoleMgr();
    if (!hero) {
        return;
    }

    const OBJID heroId = hero->GetID();
    if (heroId == 0)
        return;

    // Hero changed (login swap) → wipe snapshots, but keep the running session
    // (the user may want totals across re-logs).  If you want a hard reset on
    // hero swap, call HuntStats::Reset() yourself.
    if (heroId != g_lastHeroId) {
        RebaselineSnapshots(hero);
        return;  // skip the rest this frame — counters need fresh baselines
    }

    const OBJID mapId = map ? map->GetId() : 0;
    if (mapId != g_lastMapId) {
        // Map changed → drop the seen-monsters set (no kill attribution
        // across maps), keep inventory & silver baselines.
        g_seenMonsters.clear();
        g_lastMapId = mapId;
    }

    if (!g_silverBaselined) {
        g_lastSilver      = hero->GetSilver();
        g_silverBaselined = true;
    }
    if (!g_inventoryBaselined) {
        g_lastInventory.clear();
        for (const auto& itemRef : hero->m_deqItem) {
            if (itemRef) {
                g_lastInventory[itemRef->GetID()] =
                    InvSnapshot{ itemRef->GetTypeID(), (uint8_t)itemRef->GetPlus() };
            }
        }
        g_inventoryBaselined = true;
    }

    const AutoHuntSettings& settings = GetAutoHuntSettings();
    const bool active = IsHuntActive();

    // Optional gating: only accumulate while the hero is in the configured zone
    const bool heroInZone = active
        && IsPointInHuntZone(settings, mapId, hero->m_posMap);
    const bool accumulating = active
        && (!g_settings.pauseWhenOutOfZone || heroInZone);

    // ── Death detection (rising edge) ─────────────────────────────────────
    const bool isDead = hero->IsDead() != 0;
    if (isDead && !g_lastIsDead) {
        if (accumulating || active) {  // count deaths whenever hunting
            ++g_deaths;
            NotifyDeath(hero);
        }
    }
    g_lastIsDead = isDead;

    // ── Gold delta ────────────────────────────────────────────────────────
    const uint32_t curSilver = hero->GetSilver();
    if (curSilver != g_lastSilver) {
        const int64_t delta = (int64_t)curSilver - (int64_t)g_lastSilver;
        if (accumulating)
            g_goldDelta += delta;
        g_lastSilver = curSilver;
    }

    // ── Inventory diff → drops ───────────────────────────────────────────
    std::unordered_map<OBJID, InvSnapshot> currentInv;
    currentInv.reserve(hero->m_deqItem.size());
    for (const auto& itemRef : hero->m_deqItem) {
        if (!itemRef) continue;
        const OBJID uid = itemRef->GetID();
        const InvSnapshot snap{ itemRef->GetTypeID(), (uint8_t)itemRef->GetPlus() };
        currentInv[uid] = snap;
        if (g_lastInventory.find(uid) == g_lastInventory.end()) {
            // New item this frame
            if (accumulating) {
                g_drops[snap.typeId]++;
                NotifyNotableDrop(hero, snap.typeId, snap.plus);
            }
        }
    }
    g_lastInventory = std::move(currentInv);

    // ── Kill detection (entity-disappear in zone) ────────────────────────
    std::unordered_set<OBJID> currentMonsters;
    CollectMonstersInZone(mgr, mapId, settings, currentMonsters);

    // IDs that were previously seen but are gone now = vanished
    if (!g_seenMonsters.empty()) {
        for (OBJID prevId : g_seenMonsters) {
            if (currentMonsters.find(prevId) != currentMonsters.end())
                continue;
            // Vanished
            if (accumulating) {
                ++g_kills;
                NotifyKillMilestone(g_kills);
            }
        }
    }
    g_seenMonsters = std::move(currentMonsters);
}

// =====================================================================
// UI
// =====================================================================

static void FormatDurationHMS(DWORD ms, char* out, size_t outSize)
{
    const uint32_t secs = ms / 1000;
    const uint32_t h = secs / 3600;
    const uint32_t m = (secs / 60) % 60;
    const uint32_t s = secs % 60;
    if (h > 0) snprintf(out, outSize, "%uh %02um %02us", h, m, s);
    else if (m > 0) snprintf(out, outSize, "%um %02us", m, s);
    else snprintf(out, outSize, "%us", s);
}

void RenderUI()
{
    constexpr ImGuiTreeNodeFlags kFlags = ImGuiTreeNodeFlags_DefaultOpen;
    if (!ImGui::CollapsingHeader("Session Stats", kFlags))
        return;

    // Snapshot once to keep the panel internally consistent for this frame.
    DWORD elapsed;
    uint64_t kills;
    int64_t gold;
    int deaths;
    double kph, gph;
    std::vector<DropEntry> drops;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        elapsed = (g_sessionStartTick == 0) ? 0 : (GetTickCount() - g_sessionStartTick);
        kills   = g_kills;
        gold    = g_goldDelta;
        deaths  = g_deaths;
        if (elapsed >= 30 * 1000) {
            kph = (double)kills * 3600.0 * 1000.0 / (double)elapsed;
            gph = (double)gold  * 3600.0 * 1000.0 / (double)elapsed;
        } else {
            kph = 0.0;
            gph = 0.0;
        }
        drops.reserve(g_drops.size());
        for (const auto& [typeId, count] : g_drops)
            drops.push_back({ typeId, count });
    }
    std::sort(drops.begin(), drops.end(),
        [](const DropEntry& a, const DropEntry& b) { return a.count > b.count; });

    char durBuf[32];
    FormatDurationHMS(elapsed, durBuf, sizeof(durBuf));

    if (ImGui::BeginTable("##huntstats_top", 2,
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("Session"); ImGui::TableNextColumn(); ImGui::Text("%s", durBuf);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("Kills");
        ImGui::TableNextColumn(); ImGui::Text("%llu  (%.0f/hr)", (unsigned long long)kills, kph);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("Gold");
        ImGui::TableNextColumn(); ImGui::Text("%+lld  (%+.0f/hr)", (long long)gold, gph);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("Deaths"); ImGui::TableNextColumn(); ImGui::Text("%d", deaths);

        ImGui::EndTable();
    }

    ImGui::Spacing();

    if (ImGui::Button("Reset Stats"))
        Reset();
    ImGui::SameLine();
    ImGui::Checkbox("Auto-reset on enable", &g_settings.autoResetOnEnable);
    ImGui::SameLine();
    ImGui::Checkbox("Pause when out of zone", &g_settings.pauseWhenOutOfZone);

    // ── Discord events sub-section ──────────────────────────────────────
    if (ImGui::TreeNode("Discord events")) {
        ImGui::Checkbox("On kill milestone", &g_settings.discordOnKillMilestone);
        if (g_settings.discordOnKillMilestone) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("every N kills##huntstats_milestone",
                            &g_settings.killMilestoneInterval);
            if (g_settings.killMilestoneInterval < 1)
                g_settings.killMilestoneInterval = 1;
        }
        ImGui::Checkbox("On death",                 &g_settings.discordOnDeath);
        ImGui::Checkbox("On notable drop",          &g_settings.discordOnNotableDrop);
        if (g_settings.discordOnNotableDrop) {
            ImGui::Indent();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::SliderInt("min quality (3-9)##huntstats_q",
                             &g_settings.notableDropMinQuality, 3, 9);
            ImGui::SetNextItemWidth(80.0f);
            ImGui::SliderInt("min plus (0-12)##huntstats_p",
                             &g_settings.notableDropMinPlus, 0, 12);
            ImGui::TextDisabled("3=Normal, 6=Refined, 7=Unique, 8=Elite, 9=Super");
            ImGui::Unindent();
        }
        ImGui::Checkbox("On session start",         &g_settings.discordOnSessionStart);
        ImGui::BeginDisabled();
        ImGui::Checkbox("On level-up (not yet supported)",
                        &g_settings.discordOnLevelUp);
        ImGui::EndDisabled();
        ImGui::TreePop();
    }

    // ── Drops table ─────────────────────────────────────────────────────
    if (ImGui::TreeNodeEx("Drops", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (drops.empty()) {
            ImGui::TextDisabled("No drops recorded this session.");
        } else if (ImGui::BeginTable("##huntstats_drops", 3,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                | ImGuiTableFlags_ScrollY,
                ImVec2(0, 200.0f))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Item",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("ID",    ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableHeadersRow();
            for (const auto& d : drops) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const char* baseName = GetItemTypeName(d.typeId);
                ImGui::Text("%s", baseName && *baseName ? baseName : "(unknown)");
                ImGui::TableNextColumn();
                ImGui::Text("%u", d.typeId);
                ImGui::TableNextColumn();
                ImGui::Text("%u", d.count);
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
}

}  // namespace HuntStats
