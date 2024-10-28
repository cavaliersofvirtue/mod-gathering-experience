#include "ScriptMgr.h"
#include "Player.h"
#include "Creature.h"
#include "ObjectMgr.h"
#include "GameEventMgr.h"
#include "SkillDiscovery.h"
#include "Config.h"
#include "Chat.h"
#include <iostream> // Include for logging
#include "ChatCommand.h"
#include "StringFormat.h"
#include "DatabaseEnv.h"
#include "Log.h"

using namespace Acore::ChatCommands;

// Define the maximum level for gathering scaling
const uint32 GATHERING_MAX_LEVEL = 80;
const uint32 MAX_EXPERIENCE_GAIN = 25000;
const uint32 MIN_EXPERIENCE_GAIN = 10;
const char* GATHERING_EXPERIENCE_VERSION = "1.0.0";

class GatheringExperienceModule : public PlayerScript, public WorldScript
{
private:
    struct GatheringItem {
        uint32 baseXP;
        uint32 requiredSkill;
        uint8 profession;
        std::string name;
    };

    std::map<uint32, GatheringItem> gatheringItems;
    std::map<uint32, float> rarityMultipliers;
    std::map<uint32, float> zoneMultipliers;
    bool enabled;
    bool dataLoaded;

public:
    static GatheringExperienceModule* instance;

    GatheringExperienceModule() : 
        PlayerScript("GatheringExperienceModule"), 
        WorldScript("GatheringExperienceModule"),
        enabled(false),
        dataLoaded(false)
    {
        instance = this;
    }

    void OnStartup() override
    {
        LOG_INFO("server.loading", "GatheringExperienceModule - Loading data from database...");
        LoadDataFromDB();
    }

    void OnBeforeConfigLoad(bool /*reload*/) override
    {
        enabled = sConfigMgr->GetOption<bool>("GatheringExperience.Enable", true);
        if (!enabled)
        {
            LOG_INFO("server.loading", "Gathering Experience Module is disabled by config.");
            return;
        }

        LOG_INFO("server.loading", "Gathering Experience Module configuration loaded.");
    }

    void LoadDataFromDB()
    {
        try 
        {
            // Clear existing data - make this more explicit
            LOG_INFO("server.loading", "Clearing existing gathering data from memory...");
            gatheringItems.clear();
            rarityMultipliers.clear();
            zoneMultipliers.clear();
            dataLoaded = false;  // Reset the dataLoaded flag

            // Load gathering items
            if (QueryResult result = WorldDatabase.Query("SELECT item_id, base_xp, required_skill, profession, name FROM gathering_experience"))
            {
                uint32 count = 0;
                do
                {
                    Field* fields = result->Fetch();
                    uint32 itemId = fields[0].Get<uint32>();
                    GatheringItem item;
                    item.baseXP = fields[1].Get<uint32>();
                    item.requiredSkill = fields[2].Get<uint32>();
                    item.profession = fields[3].Get<uint8>();
                    item.name = fields[4].Get<std::string>();
                    gatheringItems[itemId] = item;
                    count++;
                } while (result->NextRow());
                LOG_INFO("server.loading", "Loaded {} gathering items", count);
            }
            else
            {
                LOG_INFO("server.loading", "No gathering items found in database");
            }

            // Load rarity multipliers
            if (QueryResult result = WorldDatabase.Query("SELECT item_id, multiplier FROM gathering_experience_rarity"))
            {
                uint32 count = 0;
                do
                {
                    Field* fields = result->Fetch();
                    rarityMultipliers[fields[0].Get<uint32>()] = fields[1].Get<float>();
                    count++;
                } while (result->NextRow());
                LOG_INFO("server.loading", "Loaded {} rarity multipliers", count);
            }

            // Load zone multipliers
            if (QueryResult result = WorldDatabase.Query("SELECT zone_id, multiplier FROM gathering_experience_zones"))
            {
                uint32 count = 0;
                do
                {
                    Field* fields = result->Fetch();
                    zoneMultipliers[fields[0].Get<uint32>()] = fields[1].Get<float>();
                    count++;
                } while (result->NextRow());
                LOG_INFO("server.loading", "Loaded {} zone multipliers", count);
            }

            dataLoaded = true;
            LOG_INFO("server.loading", "GatheringExperienceModule - Database loading complete");
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("server.loading", "Error loading GatheringExperienceModule data: {}", e.what());
            // In case of error, ensure data is cleared
            gatheringItems.clear();
            rarityMultipliers.clear();
            zoneMultipliers.clear();
            dataLoaded = false;
        }
    }

    void OnLogin(Player* player) override
    {
        if (!enabled)
            return;

        if (sConfigMgr->GetOption<bool>("GatheringExperience.Announce", true))
        {
            std::string message = "This server is running the |cff4CFF00Gathering Experience|r module v" + std::string(GATHERING_EXPERIENCE_VERSION) + " by Thaxtin.";
            ChatHandler(player->GetSession()).SendSysMessage(message.c_str());
        }
    }

    // Function to calculate scaled experience based on player level and item base XP
    uint32 CalculateExperience(Player* player, uint32 baseXP, uint32 requiredSkill, uint32 currentSkill, uint32 itemId)
    {
        if (!enabled)
            return 0;

        uint32 playerLevel = player->GetLevel();

        // No XP gain for characters at or above the max level
        if (playerLevel >= GATHERING_MAX_LEVEL)
            return 0;

        // Calculate skill multiplier
        float skillMultiplier;
        if (IsFishingItem(itemId)) {
            // Base multiplier of 2.5 with 0.4% increase per skill point
            skillMultiplier = 2.0f + (currentSkill * 0.005f);
            skillMultiplier = std::min(skillMultiplier, 4.0f);
        }
        else if (IsSkinningItem(itemId)) {
            // New skinning calculation - similar to fishing but slightly lower multipliers
            skillMultiplier = 1.5f + (currentSkill * 0.004f);
            skillMultiplier = std::min(skillMultiplier, 3.0f);
        }
        else {
            // Original diminishing returns for mining/herbalism
            uint32 skillDifference = (currentSkill > requiredSkill) ? (currentSkill - requiredSkill) : 0;
            skillMultiplier = 1.0f - (skillDifference * 0.02f);
            skillMultiplier = std::max(skillMultiplier, 0.1f);
        }

        // Apply level scaling formula
        float levelMultiplier = (requiredSkill <= 150) ? 1.0f : 
                                (0.5f + (0.5f * (1.0f - static_cast<float>(playerLevel) / GATHERING_MAX_LEVEL)));

        // Get rarity multiplier
        float rarityMultiplier = GetRarityMultiplier(itemId);

        // Apply zone-based multiplier for fishing
        float zoneMultiplier = IsFishingItem(itemId) ? GetFishingZoneMultiplier(player->GetZoneId()) : 1.0f;

        // Calculate final XP with scaling and diminishing returns
        uint32 scaledXP = static_cast<uint32>(baseXP * skillMultiplier * levelMultiplier * rarityMultiplier * zoneMultiplier);

        // Higher minimum XP for fishing (scales with skill level)
        uint32 minXP = IsFishingItem(itemId) ? 
            std::max(50u, static_cast<uint32>(50 * (1.0f + currentSkill * 0.002f))) : 
            MIN_EXPERIENCE_GAIN;
        
        // Apply minimum and maximum XP constraints
        uint32 finalXP = std::clamp(scaledXP, minXP, MAX_EXPERIENCE_GAIN);

        LOG_DEBUG("server.loading", "CalculateExperience - ItemId: {}, BaseXP: {}, CurrentSkill: {}, RequiredSkill: {}, SkillMultiplier: {:.2f}, LevelMultiplier: {:.2f}, RarityMultiplier: {:.2f}, ZoneMultiplier: {:.2f}, ScaledXP: {}, FinalXP: {}", 
                itemId, baseXP, currentSkill, requiredSkill, skillMultiplier, levelMultiplier, rarityMultiplier, zoneMultiplier, scaledXP, finalXP);

        return finalXP;
    }

    // Function to get base XP and required skill for different mining, herbalism items, and skinning items
    std::pair<uint32, uint32> GetGatheringBaseXPAndRequiredSkill(uint32 itemId)
    {
        auto it = gatheringItems.find(itemId);
        if (it != gatheringItems.end())
        {
            return {it->second.baseXP, it->second.requiredSkill};
        }
        return {0, 0};
    }

    // Function to apply rarity-based multipliers for special items
    float GetRarityMultiplier(uint32 itemId)
    {
        auto it = rarityMultipliers.find(itemId);
        return it != rarityMultipliers.end() ? it->second : 1.0f;
    }

    // Check if the item is related to gathering (mining, herbalism, skinning, or fishing)
    bool IsGatheringItem(uint32 itemId)
    {
        // Check if the item exists in mining, herbalism, or skinning XP maps
        auto [baseXP, requiredSkill] = GetGatheringBaseXPAndRequiredSkill(itemId);
        return baseXP > 0;
    }

    float GetFishingZoneMultiplier(uint32 zoneId)
    {
        auto it = zoneMultipliers.find(zoneId);
        return it != zoneMultipliers.end() ? it->second : 1.0f;
    }

    void OnLootItem(Player* player, Item* item, uint32 /*count*/, ObjectGuid /*lootguid*/) override
    {
        if (!enabled)
            return;

        uint32 itemId = item->GetEntry();
        
        // Add debug logging
        LOG_DEBUG("server.loading", "OnLootItem triggered for item {} - Checking if it's in gathering items...", itemId);
        LOG_DEBUG("server.loading", "Current number of gathering items in memory: {}", gatheringItems.size());
        
        auto [baseXP, requiredSkill] = GetGatheringBaseXPAndRequiredSkill(itemId);
        
        if (baseXP == 0)
        {
            LOG_DEBUG("server.loading", "Item {} not recognized as a gathering item", itemId);
            return;
        }

        uint32 currentSkill = 0;
        SkillType skillType = SKILL_NONE;

        // Check for each profession
        if (IsFishingItem(itemId) && player->HasSkill(SKILL_FISHING))
        {
            currentSkill = player->GetSkillValue(SKILL_FISHING);
            skillType = SKILL_FISHING;
        }
        else if (IsMiningItem(itemId) && player->HasSkill(SKILL_MINING))
        {
            currentSkill = player->GetSkillValue(SKILL_MINING);
            skillType = SKILL_MINING;
        }
        else if (IsHerbalismItem(itemId) && player->HasSkill(SKILL_HERBALISM))
        {
            currentSkill = player->GetSkillValue(SKILL_HERBALISM);
            skillType = SKILL_HERBALISM;
        }
        else if (IsSkinningItem(itemId) && player->HasSkill(SKILL_SKINNING))
        {
            currentSkill = player->GetSkillValue(SKILL_SKINNING);
            skillType = SKILL_SKINNING;
        }

        if (skillType == SKILL_NONE)
        {
            LOG_DEBUG("server.loading", "Player {} lacks required skill for item {}", player->GetName(), itemId);
            return;
        }

        uint32 xp = CalculateExperience(player, baseXP, requiredSkill, currentSkill, itemId);
        
        LOG_INFO("server.loading", "Player {} gained {} XP from {} (skill {}) {} ({})",
                 player->GetName(),         // Player name
                 xp,                        // XP gained
                 GetSkillName(skillType),   // Skill name
                 currentSkill,              // Current skill level
                 item->GetTemplate()->Name1, // Item name
                 itemId);                   // Item ID

        player->GiveXP(xp, nullptr);
    }

    // Helper function to get skill name
    const char* GetSkillName(SkillType skillType)
    {
        switch (skillType)
        {
            case SKILL_MINING:
                return "Mining";
            case SKILL_HERBALISM:
                return "Herbalism";
            case SKILL_SKINNING:
                return "Skinning";
            case SKILL_FISHING:
                return "Fishing";
            default:
                return "Unknown";
        }
    }

    // Modify the profession check functions to not log unless in debug mode
    bool IsFishingItem(uint32 itemId)
    {
        auto it = gatheringItems.find(itemId);
        return it != gatheringItems.end() && it->second.profession == 4;
    }

    bool IsMiningItem(uint32 itemId)
    {
        auto it = gatheringItems.find(itemId);
        return it != gatheringItems.end() && it->second.profession == 1;
    }

    bool IsHerbalismItem(uint32 itemId)
    {
        auto it = gatheringItems.find(itemId);
        return it != gatheringItems.end() && it->second.profession == 2;
    }

    bool IsSkinningItem(uint32 itemId)
    {
        auto it = gatheringItems.find(itemId);
        return it != gatheringItems.end() && it->second.profession == 3;
    }

    bool IsEnabled() const { return enabled; }
    
    void SetEnabled(bool state) 
    { 
        enabled = state;
        LOG_INFO("server.loading", "Gathering Experience Module {} by toggle command", 
            enabled ? "enabled" : "disabled");
    }
};

// Initialize the static member
GatheringExperienceModule* GatheringExperienceModule::instance = nullptr;

class GatheringExperienceCommandScript : public CommandScript
{
public:
    GatheringExperienceCommandScript() : CommandScript("GatheringExperienceCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable gatheringCommandTable =
        {
            { "version",     HandleGatheringVersionCommand,   SEC_GAMEMASTER,  Console::Yes },
            { "reload",      HandleGatheringReloadCommand,    SEC_GAMEMASTER,  Console::Yes },
            { "add",        HandleGatheringAddCommand,       SEC_GAMEMASTER,  Console::Yes },
            { "remove",     HandleGatheringRemoveCommand,    SEC_GAMEMASTER,  Console::Yes },
            { "modify",     HandleGatheringModifyCommand,    SEC_GAMEMASTER,  Console::Yes },
            { "list",       HandleGatheringListCommand,      SEC_GAMEMASTER,  Console::Yes }
        };

        static ChatCommandTable commandTable =
        {
            { "gathering", gatheringCommandTable }
        };

        return commandTable;
    }

    static bool HandleGatheringVersionCommand(ChatHandler* handler, const char* /*args*/)
    {
        handler->PSendSysMessage("Gathering Experience Module Version: {}", GATHERING_EXPERIENCE_VERSION);
        return true;
    }

    static bool HandleGatheringReloadCommand(ChatHandler* handler, const char* /*args*/)
    {
        if (GatheringExperienceModule::instance)
        {
            GatheringExperienceModule::instance->LoadDataFromDB();
            handler->PSendSysMessage("Gathering Experience data reloaded from database.");
            return true;
        }
        
        handler->PSendSysMessage("Failed to reload Gathering Experience data.");
        return false;
    }

    static bool HandleGatheringAddCommand(ChatHandler* handler, const char* args)
    {
        char* itemIdStr = strtok((char*)args, " ");
        char* baseXPStr = strtok(nullptr, " ");
        char* reqSkillStr = strtok(nullptr, " ");
        char* professionStr = strtok(nullptr, " ");
        char* nameStr = strtok(nullptr, "\n");

        if (!itemIdStr || !baseXPStr || !reqSkillStr || !professionStr || !nameStr)
        {
            handler->SendSysMessage("Usage: .gathering add <itemId> <baseXP> <requiredSkill> <profession> <name>");
            handler->SendSysMessage("Professions: Mining, Herbalism, Skinning, Fishing");
            return false;
        }

        uint32 itemId = atoi(itemIdStr);
        uint32 baseXP = atoi(baseXPStr);
        uint32 reqSkill = atoi(reqSkillStr);
        std::string profName = professionStr;

        uint8 professionId = GetProfessionIdByName(profName);
        if (professionId == 0)
        {
            handler->PSendSysMessage("Invalid profession: {}", profName);
            handler->SendSysMessage("Valid professions: Mining, Herbalism, Skinning, Fishing");
            return false;
        }

        WorldDatabase.Execute("INSERT INTO gathering_experience (item_id, base_xp, required_skill, profession, name) VALUES ({}, {}, {}, {}, '{}')",
            itemId, baseXP, reqSkill, professionId, nameStr);

        // Force reload of data after adding
        if (GatheringExperienceModule::instance)
        {
            GatheringExperienceModule::instance->LoadDataFromDB();
            handler->PSendSysMessage("Added gathering item {} to database for profession {} and reloaded data.", itemId, profName);
        }
        else
        {
            handler->PSendSysMessage("Added gathering item {} to database for profession {}, but failed to reload data.", itemId, profName);
        }
        
        return true;
    }

    static bool HandleGatheringRemoveCommand(ChatHandler* handler, const char* args)
    {
        char* itemIdStr = strtok((char*)args, " ");
        if (!itemIdStr)
        {
            handler->SendSysMessage("Usage: .gathering remove <itemId>");
            return false;
        }

        uint32 itemId = atoi(itemIdStr);

        // Get item details before removing
        QueryResult result = WorldDatabase.Query(
            "SELECT ge.item_id, ge.base_xp, ge.required_skill, gep.name as prof_name, ge.name "
            "FROM gathering_experience ge "
            "JOIN gathering_experience_professions gep ON ge.profession = gep.profession_id "
            "WHERE ge.item_id = {}", itemId);

        if (!result)
        {
            handler->PSendSysMessage("Item ID {} not found in gathering database.", itemId);
            return false;
        }

        // Show what we're about to remove
        Field* fields = result->Fetch();
        handler->PSendSysMessage("Removing - ItemID: {}, BaseXP: {}, ReqSkill: {}, Profession: {}, Name: {}",
            fields[0].Get<uint32>(),
            fields[1].Get<uint32>(),
            fields[2].Get<uint32>(),
            fields[3].Get<std::string>(),
            fields[4].Get<std::string>());

        WorldDatabase.Execute("DELETE FROM gathering_experience WHERE item_id = {}", itemId);
        
        // Force reload of data after removal
        if (GatheringExperienceModule::instance)
        {
            GatheringExperienceModule::instance->LoadDataFromDB();
            handler->PSendSysMessage("Removed gathering item {} from database and reloaded data.", itemId);
        }
        else
        {
            handler->PSendSysMessage("Removed gathering item {} from database, but failed to reload data.", itemId);
        }
        
        return true;
    }

    static bool HandleGatheringModifyCommand(ChatHandler* handler, const char* args)
    {
        char* itemIdStr = strtok((char*)args, " ");
        char* fieldStr = strtok(nullptr, " ");
        char* valueStr = strtok(nullptr, " ");

        if (!itemIdStr || !fieldStr || !valueStr)
        {
            handler->SendSysMessage("Usage: .gathering modify <itemId> <field> <value>");
            handler->SendSysMessage("Fields: basexp, reqskill, profession, name");
            handler->SendSysMessage("Example: .gathering modify 2770 profession Mining");
            return false;
        }

        uint32 itemId = atoi(itemIdStr);
        std::string field = fieldStr;
        std::string value = valueStr;

        // First check if item exists
        QueryResult checkItem = WorldDatabase.Query("SELECT 1 FROM gathering_experience WHERE item_id = {}", itemId);
        if (!checkItem)
        {
            handler->PSendSysMessage("Item ID {} not found in gathering database.", itemId);
            return false;
        }

        std::string query = "UPDATE gathering_experience SET ";
        if (field == "basexp")
        {
            query += Acore::StringFormat("base_xp = {}", atoi(value.c_str()));
        }
        else if (field == "reqskill")
        {
            query += Acore::StringFormat("required_skill = {}", atoi(value.c_str()));
        }
        else if (field == "profession")
        {
            uint8 professionId = GetProfessionIdByName(value);
            if (professionId == 0)
            {
                handler->PSendSysMessage("Invalid profession: {}", value);
                handler->SendSysMessage("Valid professions: Mining, Herbalism, Skinning, Fishing");
                return false;
            }
            query += Acore::StringFormat("profession = {}", professionId);
        }
        else if (field == "name")
        {
            query += Acore::StringFormat("name = '{}'", value.c_str());
        }
        else
        {
            handler->SendSysMessage("Invalid field specified.");
            handler->SendSysMessage("Valid fields: basexp, reqskill, profession, name");
            return false;
        }

        query += Acore::StringFormat(" WHERE item_id = {}", itemId);
        WorldDatabase.Execute(query);
        
        // Force reload of data after modification
        if (GatheringExperienceModule::instance)
        {
            GatheringExperienceModule::instance->LoadDataFromDB();
            handler->PSendSysMessage("Modified gathering item {} in database and reloaded data.", itemId);
        }
        else
        {
            handler->PSendSysMessage("Modified gathering item {} in database, but failed to reload data.", itemId);
        }
        
        // Show updated item details
        QueryResult result = WorldDatabase.Query(
            "SELECT ge.item_id, ge.base_xp, ge.required_skill, gep.name as prof_name, ge.name "
            "FROM gathering_experience ge "
            "JOIN gathering_experience_professions gep ON ge.profession = gep.profession_id "
            "WHERE ge.item_id = {}", itemId);

        if (result)
        {
            Field* fields = result->Fetch();
            handler->PSendSysMessage("Updated values - ItemID: {}, BaseXP: {}, ReqSkill: {}, Profession: {}, Name: {}",
                fields[0].Get<uint32>(),
                fields[1].Get<uint32>(),
                fields[2].Get<uint32>(),
                fields[3].Get<std::string>(),
                fields[4].Get<std::string>());
        }

        return true;
    }

    static bool HandleGatheringListCommand(ChatHandler* handler, const char* args)
    {
        if (!*args)
        {
            // List all items
            QueryResult result = WorldDatabase.Query(
                "SELECT ge.item_id, ge.base_xp, ge.required_skill, gep.name as prof_name, ge.name "
                "FROM gathering_experience ge "
                "JOIN gathering_experience_professions gep ON ge.profession = gep.profession_id "
                "ORDER BY gep.name, ge.required_skill");

            if (!result)
            {
                handler->SendSysMessage("No gathering items found.");
                return true;
            }

            handler->SendSysMessage("Current gathering items:");
            do
            {
                Field* fields = result->Fetch();
                handler->PSendSysMessage("ItemID: {}, BaseXP: {}, ReqSkill: {}, Profession: {}, Name: {}",
                    fields[0].Get<uint32>(),    // ItemID
                    fields[1].Get<uint32>(),    // BaseXP
                    fields[2].Get<uint32>(),    // ReqSkill
                    fields[3].Get<std::string>(), // Profession name
                    fields[4].Get<std::string>()); // Item name
            } while (result->NextRow());
        }
        else
        {
            // List items for specific profession
            std::string profName = args;
            // Convert to lowercase for case-insensitive comparison
            std::transform(profName.begin(), profName.end(), profName.begin(), ::tolower);

            QueryResult result = WorldDatabase.Query(
                "SELECT ge.item_id, ge.base_xp, ge.required_skill, gep.name as prof_name, ge.name "
                "FROM gathering_experience ge "
                "JOIN gathering_experience_professions gep ON ge.profession = gep.profession_id "
                "WHERE LOWER(gep.name) = '{}' "
                "ORDER BY ge.required_skill", profName);

            if (!result)
            {
                handler->PSendSysMessage("No gathering items found for profession: {}", args);
                return true;
            }

            handler->PSendSysMessage("Current gathering items for {}:", args);
            do
            {
                Field* fields = result->Fetch();
                handler->PSendSysMessage("ItemID: {}, BaseXP: {}, ReqSkill: {}, Name: {}",
                    fields[0].Get<uint32>(),    // ItemID
                    fields[1].Get<uint32>(),    // BaseXP
                    fields[2].Get<uint32>(),    // ReqSkill
                    fields[4].Get<std::string>()); // Item name
            } while (result->NextRow());
        }

        return true;
    }

    // Helper function to get profession ID from name
    static uint8 GetProfessionIdByName(const std::string& name)
    {
        QueryResult result = WorldDatabase.Query(
            "SELECT profession_id FROM gathering_experience_professions WHERE LOWER(name) = LOWER('{}')",
            name);

        if (result)
            return result->Fetch()[0].Get<uint8>();
        return 0;
    }

    // Helper function to validate profession name
    static bool IsValidProfession(const std::string& name)
    {
        return GetProfessionIdByName(name) != 0;
    }

    static bool HandleGatheringHelpCommand(ChatHandler* handler, const char* /*args*/)
    {
        handler->SendSysMessage("Gathering Experience Module Commands:");
        handler->SendSysMessage("  .gathering version - Shows module version");
        handler->SendSysMessage("  .gathering reload - Reloads data from database");
        handler->SendSysMessage("  .gathering add <itemId> <baseXP> <reqSkill> <profession> <name>");
        handler->SendSysMessage("  .gathering remove <itemId>");
        handler->SendSysMessage("  .gathering modify <itemId> <field> <value>");
        handler->SendSysMessage("    Fields: basexp, reqskill, profession, name");
        handler->SendSysMessage("  .gathering list [profession]");
        handler->SendSysMessage("  Valid professions: Mining, Herbalism, Skinning, Fishing");
        return true;
    }
};

void AddGatheringExperienceModuleScripts()
{
    LOG_INFO("server.loading", "Adding GatheringExperienceModule scripts");
    new GatheringExperienceModule();
    new GatheringExperienceCommandScript();
    LOG_INFO("server.loading", "GatheringExperienceModule scripts added");
}

void Addmod_gathering_experience()
{
    AddGatheringExperienceModuleScripts();
}
