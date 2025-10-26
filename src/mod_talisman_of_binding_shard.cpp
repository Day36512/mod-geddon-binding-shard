/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU AGPL v3: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 *
 * Adds a (configurable) once-per-server drop of Talisman of Binding Shard (17782) to an NPC's CORPSE loot.
 * Defaults to Baron Geddon (entry 12056), but can be changed via worldserver.conf:
 *
 *   GeddonShard.Enable         = 1         # bool
 *   GeddonShard.NpcEntry       = 12056     # uint32 creature_template.entry
 *   GeddonShard.Chance         = 1.0       # float (%)
 *   GeddonShard.AllowRepeat    = 0         # bool
 *   GeddonShard.ResetOnStartup = 0         # bool
 */

#include "ScriptMgr.h"
#include "Config.h"
#include "Creature.h"
#include "Player.h"
#include "World.h"
#include "ObjectMgr.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "LootMgr.h"
#include "Chat.h"
#include "WorldSessionMgr.h"

namespace
{
    // Defaults
    static constexpr uint32 NPC_BARON_GEDDON = 12056;
    static constexpr uint32 ITEM_TALISMAN = 17782;

    // Config keys
    static constexpr char const* CONF_ENABLE = "GeddonShard.Enable";
    static constexpr char const* CONF_NPC_ENTRY = "GeddonShard.NpcEntry";
    static constexpr char const* CONF_CHANCE = "GeddonShard.Chance";
    static constexpr char const* CONF_ALLOW_REPEAT = "GeddonShard.AllowRepeat";
    static constexpr char const* CONF_RESET = "GeddonShard.ResetOnStartup";

    // Persistence
    static constexpr char const* TABLE_NAME = "mod_geddon_once_drop";
    static constexpr char const* KEY_NAME = "geddon_17782_once";

    struct ShardConfig
    {
        bool   enable = true;
        uint32 npcEntry = NPC_BARON_GEDDON;
        double chancePct = 1.0;   // 1%
        bool   allowRepeat = false;
        bool   resetOnStart = false;
    };

    static ShardConfig       gConf;
    static std::atomic<bool> gAlreadyDropped{ false };
    static std::mutex        gDbMutex;

    void EnsureTable()
    {
        WorldDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `mod_geddon_once_drop` ("
            "  `keyname`        VARCHAR(64)      NOT NULL,"
            "  `dropped`        TINYINT(1)       NOT NULL DEFAULT 0,"
            "  `last_drop_time` BIGINT UNSIGNED  NOT NULL DEFAULT 0,"
            "  `last_killer`    VARCHAR(64)               DEFAULT NULL,"
            "  PRIMARY KEY (`keyname`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8"
        );

        WorldDatabase.DirectExecute(
            "INSERT IGNORE INTO `mod_geddon_once_drop` (`keyname`,`dropped`,`last_drop_time`,`last_killer`) "
            "VALUES ('geddon_17782_once', 0, 0, NULL)"
        );
    }

    void LoadDroppedState()
    {
        if (QueryResult res = WorldDatabase.Query(
            Acore::StringFormat("SELECT `dropped` FROM `{}` WHERE `keyname`='{}' LIMIT 1", TABLE_NAME, KEY_NAME).c_str()))
        {
            Field* f = res->Fetch();
            bool dropped = f[0].Get<uint8>() != 0;
            gAlreadyDropped.store(dropped, std::memory_order_relaxed);
        }
        else
        {
            gAlreadyDropped.store(false, std::memory_order_relaxed);
        }
    }

    void MaybeResetState()
    {
        if (!gConf.resetOnStart)
            return;

        std::lock_guard<std::mutex> _g(gDbMutex);
        WorldDatabase.DirectExecute(
            Acore::StringFormat(
                "UPDATE `{}` SET `dropped`=0, `last_drop_time`=0, `last_killer`=NULL WHERE `keyname`='{}'",
                TABLE_NAME, KEY_NAME
            ).c_str()
        );
        gAlreadyDropped.store(false, std::memory_order_relaxed);
        LOG_INFO("module", "[GeddonShard] ResetOnStartup=1 -> cleared once-per-server memory.");
    }

    void PersistDropped(Player* killer)
    {
        if (gConf.allowRepeat)
            return;

        uint64 now = static_cast<uint64>(std::time(nullptr));
        std::string name = killer ? killer->GetName() : std::string();

        std::lock_guard<std::mutex> _g(gDbMutex);
        if (name.empty())
        {
            WorldDatabase.DirectExecute(
                Acore::StringFormat(
                    "UPDATE `{}` SET `dropped`=1, `last_drop_time`={}, `last_killer`=NULL WHERE `keyname`='{}'",
                    TABLE_NAME, now, KEY_NAME
                ).c_str()
            );
        }
        else
        {
            if (name.size() > 63) name.resize(63);
            for (char& ch : name) if (ch == '\'' || ch == '\"') ch = '_';

            WorldDatabase.DirectExecute(
                Acore::StringFormat(
                    "UPDATE `{}` SET `dropped`=1, `last_drop_time`={}, `last_killer`='{}' WHERE `keyname`='{}'",
                    TABLE_NAME, now, name, KEY_NAME
                ).c_str()
            );
        }

        gAlreadyDropped.store(true, std::memory_order_relaxed);
    }

    bool RollDrop()
    {
        if (gConf.chancePct <= 0.0) return false;
        if (gConf.chancePct >= 100.0) return true;

        uint32 const scale = 10000; // 100.00%
        uint32 const roll = urand(1, scale);
        uint32 const need = static_cast<uint32>(gConf.chancePct * 100.0 + 0.5);
        return roll <= need;
    }

    inline LootStoreItem MakeLootStoreItem(uint32 itemId)
    {
        return LootStoreItem(itemId, /*reference*/0, /*chance*/100.0f,
            /*needs_quest*/false, /*lootmode*/LOOT_MODE_DEFAULT,
            /*groupid*/0, /*mincount*/1, /*maxcount*/1);
    }

    void AddOneToLoot(Loot* loot, uint32 itemId)
    {
        if (!loot) return;
        loot->AddItem(MakeLootStoreItem(itemId));
    }

    bool LootHasItem(Loot* loot, uint32 itemId)
    {
        if (!loot) return false;
        for (auto const& it : loot->items)
            if (it.itemid == itemId)
                return true;
        for (auto const& it : loot->quest_items)
            if (it.itemid == itemId)
                return true;
        return false;
    }


    void AnnounceDrop(Player* killer, Creature const* killed)
    {
        std::string who = killer ? killer->GetName() : std::string("Someone");

        std::string bossName;
        if (killed)
        {
            bossName = killed->GetName();
        }
        else if (CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(gConf.npcEntry))
        {
            bossName = ct->Name;
        }
        else
        {
            bossName = "their foe";
        }

        std::string msg = Acore::StringFormat("{} has defeated {} and claimed the legendary Talisman of Binding Shard!", who, bossName);

        WorldPacket data;
        ChatHandler::BuildChatPacket(
            data,
            CHAT_MSG_SYSTEM,
            LANG_UNIVERSAL,
            nullptr,          // sender WorldObject*
            nullptr,          // receiver WorldObject*
            msg
        );
        sWorldSessionMgr->SendGlobalMessage(&data);
    }
} // namespace

class GeddonShard_World : public WorldScript
{
public:
    GeddonShard_World() : WorldScript("GeddonShard_World") {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        gConf.enable = sConfigMgr->GetOption<bool>(CONF_ENABLE, true);
        gConf.npcEntry = sConfigMgr->GetOption<uint32>(CONF_NPC_ENTRY, NPC_BARON_GEDDON);
        gConf.chancePct = sConfigMgr->GetOption<float>(CONF_CHANCE, 1.0f);
        gConf.allowRepeat = sConfigMgr->GetOption<bool>(CONF_ALLOW_REPEAT, false);
        gConf.resetOnStart = sConfigMgr->GetOption<bool>(CONF_RESET, false);

        if (gConf.npcEntry == 0)
            gConf.npcEntry = NPC_BARON_GEDDON;

        if (gConf.chancePct < 0.0)  gConf.chancePct = 0.0;
        if (gConf.chancePct > 100.) gConf.chancePct = 100.;

        EnsureTable();
        MaybeResetState();
        LoadDroppedState();

        std::string npcName = "Unknown";
        if (CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(gConf.npcEntry))
            npcName = ct->Name;

        LOG_INFO("module",
            "[GeddonShard] Enable={} NpcEntry={}({}) Chance={:.3f}% AllowRepeat={} ResetOnStartup={} AlreadyDropped={}",
            uint32(gConf.enable), gConf.npcEntry, npcName, gConf.chancePct,
            uint32(gConf.allowRepeat), uint32(gConf.resetOnStart),
            gAlreadyDropped.load());
    }
};

class GeddonShard_Player : public PlayerScript
{
public:
    GeddonShard_Player() : PlayerScript("GeddonShard_Player") {}

    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        if (!gConf.enable || !killer || !killed)
            return;

        if (killed->GetEntry() != gConf.npcEntry)
            return;

        if (!gConf.allowRepeat && gAlreadyDropped.load(std::memory_order_relaxed))
            return;

        if (LootHasItem(&killed->loot, ITEM_TALISMAN))
            return;

        if (!RollDrop())
            return;

        AddOneToLoot(&killed->loot, ITEM_TALISMAN);
        PersistDropped(killer);
        AnnounceDrop(killer, killed);

        LOG_INFO("module", "[GeddonShard] Added item {} to {}'s corpse loot{}.",
            ITEM_TALISMAN, killed->GetName().c_str(), gConf.allowRepeat ? " (AllowRepeat=1)" : "");
    }
};

void AddSC_GeddonBindingShardScripts()
{
    new GeddonShard_World();
    new GeddonShard_Player();
}
