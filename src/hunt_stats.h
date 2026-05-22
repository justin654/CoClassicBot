#pragma once
#include "base.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// =====================================================================
// HuntStats — session telemetry for the auto-hunt plugins
//
// Tracks: session duration, kill count (entity-disappear heuristic),
// gold delta, item drops table, death count.  Emits optional Discord
// webhook events on kill milestones, deaths, and notable drops.
//
// Level / XP tracking is NOT implemented yet — the stat-table IDs for
// level and experience have not been reversed.  Level-up scaffolding
// exists in this file (settings + Discord plumbing) so it becomes a
// one-line patch once the IDs are known.
// =====================================================================

namespace HuntStats {

struct Settings
{
    // Discord event toggles
    bool discordOnKillMilestone = false;
    int  killMilestoneInterval  = 100;   // post every N kills

    bool discordOnDeath         = false;
    bool discordOnNotableDrop   = false;
    int  notableDropMinQuality  = 6;     // 6=Refined, 7=Unique, 8=Elite, 9=Super
    int  notableDropMinPlus     = 0;     // also post if +N >= threshold (0 = ignore)

    bool discordOnSessionStart  = false;
    bool discordOnLevelUp       = false; // wired but inactive until level field is found

    // Behavior toggles
    bool autoResetOnEnable      = false; // reset stats when hunt plugin toggled on
    bool pauseWhenOutOfZone     = true;  // only accumulate while in the hunt zone
};

Settings& GetSettings();

// ── Drops table entry ─────────────────────────────────────────────────────
struct DropEntry
{
    uint32_t typeId;
    uint32_t count;
};

// ── Per-frame driver ──────────────────────────────────────────────────────
// Called once per frame from BaseHuntPlugin::Update before the m_enabled
// short-circuit.  Internally checks whether any hunt plugin is enabled and
// gates accumulation accordingly so the panel still shows totals when idle.
void Update();

// Force the tracker to treat the next Update() as the start of a new session.
void Reset();

// Accessors for the UI / external readers.
DWORD     GetSessionDurationMs();
uint64_t  GetKills();
int64_t   GetGoldDelta();
int       GetDeaths();
DWORD     GetSessionStartTick();

// kills per hour and gold per hour (returns 0 until at least 30s elapsed
// to avoid divide-by-tiny-number jitter on first frame).
double    GetKillsPerHour();
double    GetGoldPerHour();

// Returns top-N drop entries sorted by count descending.
std::vector<DropEntry> GetTopDrops(size_t maxEntries = 50);

// ── UI ────────────────────────────────────────────────────────────────────
// Render the "Session Stats" collapsing header.  Call from inside a parent
// ImGui frame (e.g. BaseHuntPlugin::RenderGeneralUI).
void RenderUI();

}  // namespace HuntStats
