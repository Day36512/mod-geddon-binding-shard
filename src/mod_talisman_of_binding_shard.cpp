/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU AGPL v3: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 *
 * Configurable Boss Loot
 *
 * Expanded from the original mod_talisman_of_binding_shard behavior.
 * This module allows server owners to inject arbitrary item drops into arbitrary creature corpse loot.
 *
 * Features:
 * - Multiple independent loot rules.
 * - Any creature entry can drop any item entry.
 * - Per-rule chance percentage.
 * - Per-rule stack/count range.
 * - Per-rule once-per-server lockout, or repeatable behavior.
 * - Optional duplicate prevention.
 * - Optional global announcement when the injected item is looted.
 * - Backward-compatible fallback to the old GeddonShard.* config keys when BossLoot.RuleCount = 0.
 */

#include "ScriptMgr.h"
#include "Config.h"
#include "Creature.h"
#include "GameObject.h"
#include "Item.h"
#include "Player.h"
#include "World.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "LootMgr.h"
#include "Chat.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // Original module defaults
    static constexpr uint32 NPC_BARON_GEDDON = 12056;
    static constexpr uint32 ITEM_TALISMAN = 17782;

    static constexpr char const* CONF_ENABLE = "BossLoot.Enable";
    static constexpr char const* CONF_RULE_COUNT = "BossLoot.RuleCount";
    static constexpr char const* CONF_RESET_ALL_ON_STARTUP = "BossLoot.ResetOnStartup";

    static constexpr char const* LEGACY_CONF_ENABLE = "GeddonShard.Enable";
    static constexpr char const* LEGACY_CONF_NPC_ENTRY = "GeddonShard.NpcEntry";
    static constexpr char const* LEGACY_CONF_CHANCE = "GeddonShard.Chance";
    static constexpr char const* LEGACY_CONF_ALLOW_REPEAT = "GeddonShard.AllowRepeat";
    static constexpr char const* LEGACY_CONF_RESET = "GeddonShard.ResetOnStartup";

    static constexpr char const* TABLE_NAME = "mod_configurable_boss_loot_once";
    static constexpr char const* LEGACY_TABLE_NAME = "mod_geddon_once_drop";
    static constexpr char const* LEGACY_KEY_NAME = "geddon_17782_once";

    struct BossLootRule
    {
        uint32 index = 0;
        bool enable = true;
        uint32 npcEntry = 0;
        uint32 itemEntry = 0;
        double chancePct = 0.0;
        uint32 minCount = 1;
        uint32 maxCount = 1;
        bool allowRepeat = true;
        bool preventDuplicate = true;
        bool resetOnStart = false;
        bool announce = false;
        std::string onceKey;
        std::string announceMessage;
    };

    struct PendingInjectedDrop
    {
        ObjectGuid lootGuid;
        std::string onceKey;
        uint32 npcEntry = 0;
        uint32 itemEntry = 0;
        bool allowRepeat = true;
        bool announce = false;
        std::string bossName;
        std::string announceMessage;
    };

    static bool gEnabled = true;
    static std::vector<BossLootRule> gRules;

    // onceKey -> dropped
    static std::unordered_map<std::string, bool> gDroppedState;

    static std::vector<PendingInjectedDrop> gPendingDrops;

    static std::mutex gConfigMutex;
    static std::mutex gStateMutex;
    static std::mutex gPendingMutex;
    static std::mutex gDbMutex;

    std::string ConfigKey(uint32 index, char const* leaf)
    {
        return Acore::StringFormat("BossLoot.Rule.{}.{}", index, leaf);
    }

    std::string SqlSafe(std::string value, std::size_t maxLen)
    {
        if (value.size() > maxLen)
            value.resize(maxLen);

        for (char& ch : value)
        {
            if (ch == '\'' || ch == '"' || ch == '\\' || ch == '`')
                ch = '_';
        }

        return value;
    }

    std::string Trim(std::string value)
    {
        auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };

        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());

        return value;
    }

    void ReplaceAll(std::string& text, std::string const& from, std::string const& to)
    {
        if (from.empty())
            return;

        std::size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos)
        {
            text.replace(pos, from.length(), to);
            pos += to.length();
        }
    }

    double ClampChance(double value)
    {
        if (value < 0.0)
            return 0.0;

        if (value > 100.0)
            return 100.0;

        return value;
    }

    std::string MakeAutoOnceKey(uint32 ruleIndex, uint32 npcEntry, uint32 itemEntry)
    {
        return Acore::StringFormat("bossloot_rule{}_npc{}_item{}", ruleIndex, npcEntry, itemEntry);
    }

    std::string GetCreatureName(uint32 entry)
    {
        if (CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(entry))
            return creatureTemplate->Name;

        return Acore::StringFormat("Creature {}", entry);
    }

    std::string GetItemName(uint32 entry)
    {
        if (ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(entry))
            return itemTemplate->Name1;

        return Acore::StringFormat("Item {}", entry);
    }

    std::string GetLootSourceName(Player* player, ObjectGuid lootGuid, uint32 fallbackNpcEntry)
    {
        if (player)
        {
            if (lootGuid.IsCreature())
            {
                if (Creature* creature = ObjectAccessor::GetCreature(*player, lootGuid))
                    return creature->GetName();
            }
            else if (lootGuid.IsGameObject())
            {
                if (GameObject* gameObject = ObjectAccessor::GetGameObject(*player, lootGuid))
                    return gameObject->GetName();
            }
        }

        return GetCreatureName(fallbackNpcEntry);
    }

    std::vector<BossLootRule> GetRulesSnapshot(bool& enabled)
    {
        std::lock_guard<std::mutex> guard(gConfigMutex);
        enabled = gEnabled;
        return gRules;
    }

    bool IsAlreadyDropped(std::string const& onceKey)
    {
        std::lock_guard<std::mutex> guard(gStateMutex);

        auto itr = gDroppedState.find(onceKey);
        return itr != gDroppedState.end() && itr->second;
    }

    bool ReserveOnceDrop(std::string const& onceKey)
    {
        std::lock_guard<std::mutex> guard(gStateMutex);

        bool& dropped = gDroppedState[onceKey];
        if (dropped)
            return false;

        dropped = true;
        return true;
    }

    void MarkDroppedInMemory(std::string const& onceKey, bool dropped)
    {
        std::lock_guard<std::mutex> guard(gStateMutex);
        gDroppedState[onceKey] = dropped;
    }

    bool RollDrop(double chancePct)
    {
        chancePct = ClampChance(chancePct);

        if (chancePct <= 0.0)
            return false;

        if (chancePct >= 100.0)
            return true;

        // 100.0000% precision. This keeps tiny legendary rates sane without floating weirdness.
        uint32 constexpr scale = 1000000;
        uint32 const roll = urand(1, scale);
        uint32 const need = static_cast<uint32>((chancePct / 100.0) * static_cast<double>(scale) + 0.5);

        return roll <= need;
    }

    inline LootStoreItem MakeLootStoreItem(uint32 itemId, uint32 minCount, uint32 maxCount)
    {
        return LootStoreItem(itemId,
            /*reference*/0,
            /*chance*/100.0f,
            /*needs_quest*/false,
            /*lootmode*/LOOT_MODE_DEFAULT,
            /*groupid*/0,
            /*mincount*/minCount,
            /*maxcount*/maxCount);
    }

    void AddItemToLoot(Loot* loot, uint32 itemId, uint32 minCount, uint32 maxCount)
    {
        if (!loot)
            return;

        loot->AddItem(MakeLootStoreItem(itemId, minCount, maxCount));
    }

    bool LootHasItem(Loot* loot, uint32 itemId)
    {
        if (!loot)
            return false;

        for (auto const& lootItem : loot->items)
        {
            if (lootItem.itemid == itemId)
                return true;
        }

        for (auto const& lootItem : loot->quest_items)
        {
            if (lootItem.itemid == itemId)
                return true;
        }

        return false;
    }

    void RememberPendingDrop(Creature* killed, BossLootRule const& rule)
    {
        if (!killed)
            return;

        PendingInjectedDrop pending;
        pending.lootGuid = killed->GetGUID();
        pending.onceKey = rule.onceKey;
        pending.npcEntry = rule.npcEntry;
        pending.itemEntry = rule.itemEntry;
        pending.allowRepeat = rule.allowRepeat;
        pending.announce = rule.announce;
        pending.bossName = killed->GetName();
        pending.announceMessage = rule.announceMessage;

        std::lock_guard<std::mutex> guard(gPendingMutex);
        gPendingDrops.push_back(pending);
    }

    bool TakePendingDrop(ObjectGuid lootGuid, uint32 itemEntry, PendingInjectedDrop& out)
    {
        std::lock_guard<std::mutex> guard(gPendingMutex);

        auto itr = std::find_if(gPendingDrops.begin(), gPendingDrops.end(),
            [&](PendingInjectedDrop const& pending)
            {
                return pending.lootGuid == lootGuid && pending.itemEntry == itemEntry;
            });

        if (itr == gPendingDrops.end())
            return false;

        out = *itr;
        gPendingDrops.erase(itr);
        return true;
    }

    void ClearPendingDrops()
    {
        std::lock_guard<std::mutex> guard(gPendingMutex);
        gPendingDrops.clear();
    }

    // Database persistence
    void EnsureTable()
    {
        std::lock_guard<std::mutex> guard(gDbMutex);

        WorldDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `mod_configurable_boss_loot_once` ("
            "  `keyname`        VARCHAR(191)     NOT NULL,"
            "  `dropped`        TINYINT(1)       NOT NULL DEFAULT 0,"
            "  `last_drop_time` BIGINT UNSIGNED  NOT NULL DEFAULT 0,"
            "  `last_killer`    VARCHAR(64)               DEFAULT NULL,"
            "  `last_looter`    VARCHAR(64)               DEFAULT NULL,"
            "  `npc_entry`      INT UNSIGNED     NOT NULL DEFAULT 0,"
            "  `item_entry`     INT UNSIGNED     NOT NULL DEFAULT 0,"
            "  PRIMARY KEY (`keyname`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8"
        );
    }

    void EnsureRowsForRules(std::vector<BossLootRule> const& rules)
    {
        std::lock_guard<std::mutex> guard(gDbMutex);

        for (BossLootRule const& rule : rules)
        {
            if (!rule.enable || rule.allowRepeat || rule.onceKey.empty())
                continue;

            std::string const key = SqlSafe(rule.onceKey, 191);

            WorldDatabase.DirectExecute(
                Acore::StringFormat(
                    "INSERT IGNORE INTO `{}` (`keyname`, `dropped`, `last_drop_time`, `last_killer`, `last_looter`, `npc_entry`, `item_entry`) "
                    "VALUES ('{}', 0, 0, NULL, NULL, {}, {})",
                    TABLE_NAME, key, rule.npcEntry, rule.itemEntry
                ).c_str()
            );

            WorldDatabase.DirectExecute(
                Acore::StringFormat(
                    "UPDATE `{}` SET `npc_entry`={}, `item_entry`={} WHERE `keyname`='{}'",
                    TABLE_NAME, rule.npcEntry, rule.itemEntry, key
                ).c_str()
            );
        }
    }

    void ResetStatesForRules(std::vector<BossLootRule> const& rules, bool resetAll)
    {
        std::lock_guard<std::mutex> guard(gDbMutex);

        for (BossLootRule const& rule : rules)
        {
            if (!rule.enable || rule.allowRepeat || rule.onceKey.empty())
                continue;

            if (!resetAll && !rule.resetOnStart)
                continue;

            std::string const key = SqlSafe(rule.onceKey, 191);

            WorldDatabase.DirectExecute(
                Acore::StringFormat(
                    "UPDATE `{}` SET `dropped`=0, `last_drop_time`=0, `last_killer`=NULL, `last_looter`=NULL WHERE `keyname`='{}'",
                    TABLE_NAME, key
                ).c_str()
            );

            MarkDroppedInMemory(rule.onceKey, false);

            LOG_INFO("module", "[BossLoot] ResetOnStartup cleared once-drop state for key '{}'.", rule.onceKey);
        }
    }

    void MigrateLegacyGeddonStateIfNeeded(std::vector<BossLootRule> const& rules)
    {
        bool hasLegacyKeyRule = false;
        for (BossLootRule const& rule : rules)
        {
            if (rule.enable && !rule.allowRepeat && rule.onceKey == LEGACY_KEY_NAME)
            {
                hasLegacyKeyRule = true;
                break;
            }
        }

        if (!hasLegacyKeyRule)
            return;

        QueryResult tableExists = WorldDatabase.Query(Acore::StringFormat("SHOW TABLES LIKE '{}'", LEGACY_TABLE_NAME).c_str());
        if (!tableExists)
            return;

        QueryResult legacyState = WorldDatabase.Query(
            Acore::StringFormat("SELECT `dropped`, `last_drop_time`, `last_killer` FROM `{}` WHERE `keyname`='{}' LIMIT 1", LEGACY_TABLE_NAME, LEGACY_KEY_NAME).c_str());

        if (!legacyState)
            return;

        Field* fields = legacyState->Fetch();
        bool const dropped = fields[0].Get<uint8>() != 0;
        if (!dropped)
            return;

        uint64 const lastDropTime = fields[1].Get<uint64>();
        std::string lastKiller;
        if (!fields[2].IsNull())
            lastKiller = SqlSafe(fields[2].Get<std::string>(), 64);

        std::lock_guard<std::mutex> guard(gDbMutex);

        if (lastKiller.empty())
        {
            WorldDatabase.DirectExecute(
                Acore::StringFormat(
                    "UPDATE `{}` SET `dropped`=1, `last_drop_time`={} WHERE `keyname`='{}' AND `dropped`=0",
                    TABLE_NAME, lastDropTime, LEGACY_KEY_NAME
                ).c_str()
            );
        }
        else
        {
            WorldDatabase.DirectExecute(
                Acore::StringFormat(
                    "UPDATE `{}` SET `dropped`=1, `last_drop_time`={}, `last_killer`='{}' WHERE `keyname`='{}' AND `dropped`=0",
                    TABLE_NAME, lastDropTime, lastKiller, LEGACY_KEY_NAME
                ).c_str()
            );
        }
    }

    std::unordered_map<std::string, bool> LoadDroppedStatesForRules(std::vector<BossLootRule> const& rules)
    {
        std::unordered_map<std::string, bool> states;

        std::lock_guard<std::mutex> guard(gDbMutex);

        for (BossLootRule const& rule : rules)
        {
            if (!rule.enable || rule.allowRepeat || rule.onceKey.empty())
                continue;

            std::string const key = SqlSafe(rule.onceKey, 191);
            bool dropped = false;

            if (QueryResult result = WorldDatabase.Query(
                Acore::StringFormat("SELECT `dropped` FROM `{}` WHERE `keyname`='{}' LIMIT 1", TABLE_NAME, key).c_str()))
            {
                Field* fields = result->Fetch();
                dropped = fields[0].Get<uint8>() != 0;
            }

            states[rule.onceKey] = dropped;
        }

        return states;
    }

    void PersistDroppedKillPhase(BossLootRule const& rule, Player* killer)
    {
        if (rule.allowRepeat || rule.onceKey.empty())
            return;

        uint64 const now = static_cast<uint64>(std::time(nullptr));
        std::string killerName = killer ? killer->GetName() : std::string();
        killerName = SqlSafe(killerName, 64);

        std::string const key = SqlSafe(rule.onceKey, 191);

        std::lock_guard<std::mutex> guard(gDbMutex);

        if (killerName.empty())
        {
            WorldDatabase.DirectExecute(
                Acore::StringFormat(
                    "UPDATE `{}` SET `dropped`=1, `last_drop_time`={}, `last_killer`=NULL, `npc_entry`={}, `item_entry`={} WHERE `keyname`='{}'",
                    TABLE_NAME, now, rule.npcEntry, rule.itemEntry, key
                ).c_str()
            );
        }
        else
        {
            WorldDatabase.DirectExecute(
                Acore::StringFormat(
                    "UPDATE `{}` SET `dropped`=1, `last_drop_time`={}, `last_killer`='{}', `npc_entry`={}, `item_entry`={} WHERE `keyname`='{}'",
                    TABLE_NAME, now, killerName, rule.npcEntry, rule.itemEntry, key
                ).c_str()
            );
        }
    }

    void PersistDroppedLootPhase(PendingInjectedDrop const& pending, Player* looter)
    {
        if (pending.allowRepeat || pending.onceKey.empty() || !looter)
            return;

        uint64 const now = static_cast<uint64>(std::time(nullptr));
        std::string looterName = SqlSafe(looter->GetName(), 64);
        std::string const key = SqlSafe(pending.onceKey, 191);

        std::lock_guard<std::mutex> guard(gDbMutex);

        WorldDatabase.DirectExecute(
            Acore::StringFormat(
                "UPDATE `{}` SET `last_drop_time`={}, `last_looter`='{}', `npc_entry`={}, `item_entry`={} WHERE `keyname`='{}'",
                TABLE_NAME, now, looterName, pending.npcEntry, pending.itemEntry, key
            ).c_str()
        );
    }

    // Config loading
    BossLootRule LoadConfiguredRule(uint32 index)
    {
        BossLootRule rule;
        rule.index = index;
        rule.enable = sConfigMgr->GetOption<bool>(ConfigKey(index, "Enable"), true);
        rule.npcEntry = sConfigMgr->GetOption<uint32>(ConfigKey(index, "NpcEntry"), 0);
        rule.itemEntry = sConfigMgr->GetOption<uint32>(ConfigKey(index, "ItemEntry"), 0);
        rule.chancePct = ClampChance(sConfigMgr->GetOption<float>(ConfigKey(index, "Chance"), 0.0f));
        rule.minCount = sConfigMgr->GetOption<uint32>(ConfigKey(index, "MinCount"), 1);
        rule.maxCount = sConfigMgr->GetOption<uint32>(ConfigKey(index, "MaxCount"), 1);
        rule.allowRepeat = sConfigMgr->GetOption<bool>(ConfigKey(index, "AllowRepeat"), true);
        rule.preventDuplicate = sConfigMgr->GetOption<bool>(ConfigKey(index, "PreventDuplicate"), true);
        rule.resetOnStart = sConfigMgr->GetOption<bool>(ConfigKey(index, "ResetOnStartup"), false);
        rule.announce = sConfigMgr->GetOption<bool>(ConfigKey(index, "Announce"), false);
        rule.onceKey = Trim(sConfigMgr->GetOption<std::string>(ConfigKey(index, "OnceKey"), ""));
        rule.announceMessage = sConfigMgr->GetOption<std::string>(ConfigKey(index, "AnnounceMessage"), "{player} has looted {item} from {boss}!");

        if (rule.minCount == 0)
            rule.minCount = 1;

        if (rule.maxCount == 0)
            rule.maxCount = 1;

        if (rule.maxCount < rule.minCount)
            std::swap(rule.minCount, rule.maxCount);

        if (!rule.allowRepeat && rule.onceKey.empty())
            rule.onceKey = MakeAutoOnceKey(index, rule.npcEntry, rule.itemEntry);

        return rule;
    }

    BossLootRule LoadLegacyRule()
    {
        BossLootRule rule;
        rule.index = 1;
        rule.enable = sConfigMgr->GetOption<bool>(LEGACY_CONF_ENABLE, true);
        rule.npcEntry = sConfigMgr->GetOption<uint32>(LEGACY_CONF_NPC_ENTRY, NPC_BARON_GEDDON);
        rule.itemEntry = ITEM_TALISMAN;
        rule.chancePct = ClampChance(sConfigMgr->GetOption<float>(LEGACY_CONF_CHANCE, 1.0f));
        rule.minCount = 1;
        rule.maxCount = 1;
        rule.allowRepeat = sConfigMgr->GetOption<bool>(LEGACY_CONF_ALLOW_REPEAT, false);
        rule.preventDuplicate = true;
        rule.resetOnStart = sConfigMgr->GetOption<bool>(LEGACY_CONF_RESET, false);
        rule.announce = true;
        rule.onceKey = LEGACY_KEY_NAME;
        rule.announceMessage = "{player} has looted the legendary {item} from {boss}!";

        if (rule.npcEntry == 0)
            rule.npcEntry = NPC_BARON_GEDDON;

        return rule;
    }

    std::vector<BossLootRule> LoadRulesFromConfig(bool& enabled, bool& resetAllOnStartup)
    {
        enabled = sConfigMgr->GetOption<bool>(CONF_ENABLE, true);
        resetAllOnStartup = sConfigMgr->GetOption<bool>(CONF_RESET_ALL_ON_STARTUP, false);

        uint32 ruleCount = sConfigMgr->GetOption<uint32>(CONF_RULE_COUNT, 0);
        std::vector<BossLootRule> rules;

        if (ruleCount == 0)
        {
            // Backward compatible mode: if the owner has not opted into BossLoot.Rule.*,
            // the old GeddonShard.* config still behaves like before.
            enabled = sConfigMgr->GetOption<bool>(LEGACY_CONF_ENABLE, true);
            rules.push_back(LoadLegacyRule());
            return rules;
        }

        // Hard cap prevents an accidental silly config from making startup unpleasant.
        if (ruleCount > 256)
        {
            LOG_WARN("module", "[BossLoot] BossLoot.RuleCount={} is excessive. Clamping to 256 rules.", ruleCount);
            ruleCount = 256;
        }

        rules.reserve(ruleCount);

        for (uint32 i = 1; i <= ruleCount; ++i)
        {
            BossLootRule rule = LoadConfiguredRule(i);

            if (rule.enable && (rule.npcEntry == 0 || rule.itemEntry == 0))
            {
                LOG_WARN("module", "[BossLoot] Skipping active rule {} because NpcEntry or ItemEntry is 0.", i);
                continue;
            }

            rules.push_back(rule);
        }

        return rules;
    }

    void AnnounceDrop(Player* looter, PendingInjectedDrop const& pending, ObjectGuid lootGuid, uint32 count)
    {
        if (!pending.announce)
            return;

        std::string playerName = looter ? looter->GetName() : std::string("Someone");
        std::string bossName = pending.bossName.empty() ? GetLootSourceName(looter, lootGuid, pending.npcEntry) : pending.bossName;
        std::string itemName = GetItemName(pending.itemEntry);

        std::string message = pending.announceMessage.empty()
            ? std::string("{player} has looted {item} from {boss}!")
            : pending.announceMessage;

        ReplaceAll(message, "{player}", playerName);
        ReplaceAll(message, "{boss}", bossName);
        ReplaceAll(message, "{item}", itemName);
        ReplaceAll(message, "{itemEntry}", std::to_string(pending.itemEntry));
        ReplaceAll(message, "{npcEntry}", std::to_string(pending.npcEntry));
        ReplaceAll(message, "{count}", std::to_string(count));

        WorldPacket data;
        ChatHandler::BuildChatPacket(
            data,
            CHAT_MSG_SYSTEM,
            LANG_UNIVERSAL,
            nullptr,
            nullptr,
            message
        );

        sWorldSessionMgr->SendGlobalMessage(&data);
    }
}

class ConfigurableBossLoot_World : public WorldScript
{
public:
    ConfigurableBossLoot_World() : WorldScript("ConfigurableBossLoot_World") { }

    void OnAfterConfigLoad(bool reload) override
    {
        bool enabled = true;
        bool resetAllOnStartup = false;
        std::vector<BossLootRule> rules = LoadRulesFromConfig(enabled, resetAllOnStartup);

        EnsureTable();
        EnsureRowsForRules(rules);

        // Treat ResetOnStartup literally: reset only on startup, not on .reload config.
        if (!reload)
            ResetStatesForRules(rules, resetAllOnStartup);

        MigrateLegacyGeddonStateIfNeeded(rules);

        std::unordered_map<std::string, bool> loadedStates = LoadDroppedStatesForRules(rules);

        {
            std::lock_guard<std::mutex> guard(gConfigMutex);
            gEnabled = enabled;
            gRules = rules;
        }

        {
            std::lock_guard<std::mutex> guard(gStateMutex);
            gDroppedState = loadedStates;
        }

        ClearPendingDrops();

        LOG_INFO("module", "[BossLoot] Enable={} RulesLoaded={} ResetAllOnStartup={} Reload={}",
            uint32(enabled), uint32(rules.size()), uint32(resetAllOnStartup), uint32(reload));

        for (BossLootRule const& rule : rules)
        {
            bool const alreadyDropped = !rule.allowRepeat && IsAlreadyDropped(rule.onceKey);

            LOG_INFO("module",
                "[BossLoot] Rule {} Enable={} NPC={}({}) Item={}({}) Chance={:.4f}% Count={}..{} AllowRepeat={} PreventDuplicate={} OnceKey='{}' AlreadyDropped={} Announce={}",
                rule.index,
                uint32(rule.enable),
                rule.npcEntry,
                GetCreatureName(rule.npcEntry),
                rule.itemEntry,
                GetItemName(rule.itemEntry),
                rule.chancePct,
                rule.minCount,
                rule.maxCount,
                uint32(rule.allowRepeat),
                uint32(rule.preventDuplicate),
                rule.onceKey,
                uint32(alreadyDropped),
                uint32(rule.announce));
        }
    }
};

class ConfigurableBossLoot_Player : public PlayerScript
{
public:
    ConfigurableBossLoot_Player() : PlayerScript("ConfigurableBossLoot_Player") { }

    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        if (!killer || !killed)
            return;

        bool enabled = true;
        std::vector<BossLootRule> rules = GetRulesSnapshot(enabled);

        if (!enabled || rules.empty())
            return;

        uint32 const killedEntry = killed->GetEntry();

        for (BossLootRule const& rule : rules)
        {
            if (!rule.enable)
                continue;

            if (rule.npcEntry != killedEntry)
                continue;

            if (rule.preventDuplicate && LootHasItem(&killed->loot, rule.itemEntry))
            {
                LOG_DEBUG("module", "[BossLoot] Rule {} skipped: {} already has item {} in corpse loot.",
                    rule.index, killed->GetName().c_str(), rule.itemEntry);
                continue;
            }

            if (!rule.allowRepeat && IsAlreadyDropped(rule.onceKey))
                continue;

            if (!RollDrop(rule.chancePct))
                continue;

            // Reserve before adding to loot so two simultaneous kills cannot both win the same once-per-server rule.
            if (!rule.allowRepeat && !ReserveOnceDrop(rule.onceKey))
                continue;

            AddItemToLoot(&killed->loot, rule.itemEntry, rule.minCount, rule.maxCount);
            RememberPendingDrop(killed, rule);
            PersistDroppedKillPhase(rule, killer);

            LOG_INFO("module", "[BossLoot] Rule {} added item {} x{}..{} to {} ({}) corpse loot{}.",
                rule.index,
                rule.itemEntry,
                rule.minCount,
                rule.maxCount,
                killed->GetName().c_str(),
                killedEntry,
                rule.allowRepeat ? "" : Acore::StringFormat(" [onceKey='{}']", rule.onceKey).c_str());
        }
    }

    void OnPlayerLootItem(Player* looter, Item* item, uint32 count, ObjectGuid lootGuid) override
    {
        if (!looter || !item)
            return;

        bool enabled = true;
        {
            std::lock_guard<std::mutex> guard(gConfigMutex);
            enabled = gEnabled;
        }

        if (!enabled)
            return;

        PendingInjectedDrop pending;
        if (!TakePendingDrop(lootGuid, item->GetEntry(), pending))
            return;

        AnnounceDrop(looter, pending, lootGuid, count);
        PersistDroppedLootPhase(pending, looter);
    }
};

void AddSC_GeddonBindingShardScripts()
{
    new ConfigurableBossLoot_World();
    new ConfigurableBossLoot_Player();
}
