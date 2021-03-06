/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "Policies/SingletonImp.h"
#include "SharedDefines.h"
#include "ObjectMgr.h"
#include "ProgressBar.h"
#include "SocialMgr.h"
#include "LFGMgr.h"
#include "World.h"
#include "Group.h"
#include "Player.h"

#include <limits>

INSTANTIATE_SINGLETON_1(LFGMgr);

LFGMgr::LFGMgr()
{
    m_RewardMap.clear();

    for (uint8 i = LFG_TYPE_NONE; i < LFG_TYPE_MAX; ++i)
    {
        m_queueInfoMap[i].clear();
        m_groupQueueInfoMap[i].clear();
        m_queueStatus[i] = LFGQueueStatus();
    }

    m_dungeonMap.clear();
    for (uint32 i = 0; i < sLFGDungeonStore.GetNumRows(); ++i)
    {
        if (LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(i))
            m_dungeonMap.insert(std::make_pair(dungeon->ID, dungeon));
    }
    m_proposalMap.clear();
}

LFGMgr::~LFGMgr()
{
    for (LFGRewardMap::iterator itr = m_RewardMap.begin(); itr != m_RewardMap.end(); ++itr)
        delete itr->second;
    m_RewardMap.clear();

    for (uint8 i = LFG_TYPE_NONE; i < LFG_TYPE_MAX; ++i)
    {
    // TODO - delete ->second from maps
        m_queueInfoMap[i].clear();
        m_groupQueueInfoMap[i].clear();
    }
    m_proposalMap.clear();
}

void LFGMgr::Update(uint32 diff)
{
}

void LFGMgr::LoadRewards()
{
   // (c) TrinityCore, 2010. Rewrited for MaNGOS by /dev/rsa
    for (LFGRewardMap::iterator itr = m_RewardMap.begin(); itr != m_RewardMap.end(); ++itr)
        delete itr->second;
    m_RewardMap.clear();

    uint32 count = 0;
    // ORDER BY is very important for GetRandomDungeonReward!
    QueryResult* result = WorldDatabase.Query("SELECT dungeonId, maxLevel, firstQuestId, firstMoneyVar, firstXPVar, otherQuestId, otherMoneyVar, otherXPVar FROM lfg_dungeon_rewards ORDER BY dungeonId, maxLevel ASC");

    if (!result)
    {
        barGoLink bar(1);
        bar.step();

        sLog.outString();
        sLog.outErrorDb(">> Loaded 0 LFG dungeon rewards. DB table `lfg_dungeon_rewards` is empty!");
        return;
    }

    barGoLink bar(result->GetRowCount());

    Field* fields = NULL;
    do
    {
        bar.step();
        fields = result->Fetch();
        uint32 dungeonId = fields[0].GetUInt32();
        uint32 maxLevel = fields[1].GetUInt8();
        uint32 firstQuestId = fields[2].GetUInt32();
        uint32 firstMoneyVar = fields[3].GetUInt32();
        uint32 firstXPVar = fields[4].GetUInt32();
        uint32 otherQuestId = fields[5].GetUInt32();
        uint32 otherMoneyVar = fields[6].GetUInt32();
        uint32 otherXPVar = fields[7].GetUInt32();

        if (!sLFGDungeonStore.LookupEntry(dungeonId))
        {
            sLog.outErrorDb("LFGMgr: Dungeon %u specified in table `lfg_dungeon_rewards` does not exist!", dungeonId);
            continue;
        }

        if (!maxLevel || maxLevel > sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL))
        {
            sLog.outErrorDb("LFGMgr: Level %u specified for dungeon %u in table `lfg_dungeon_rewards` can never be reached!", maxLevel, dungeonId);
            maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
        }

        if (firstQuestId && !sObjectMgr.GetQuestTemplate(firstQuestId))
        {
            sLog.outErrorDb("LFGMgr: First quest %u specified for dungeon %u in table `lfg_dungeon_rewards` does not exist!", firstQuestId, dungeonId);
            firstQuestId = 0;
        }

        if (otherQuestId && !sObjectMgr.GetQuestTemplate(otherQuestId))
        {
            sLog.outErrorDb("LFGMgr: Other quest %u specified for dungeon %u in table `lfg_dungeon_rewards` does not exist!", otherQuestId, dungeonId);
            otherQuestId = 0;
        }

        m_RewardMap.insert(LFGRewardMap::value_type(dungeonId, new LFGReward(maxLevel, firstQuestId, firstMoneyVar, firstXPVar, otherQuestId, otherMoneyVar, otherXPVar)));
        ++count;
    }
    while (result->NextRow());

    sLog.outString();
    sLog.outString(">> Loaded %u LFG dungeon rewards.", count);
}

LFGReward const* LFGMgr::GetRandomDungeonReward(LFGDungeonEntry const* dungeon, Player* player)
{
    LFGReward const* rew = NULL;
    if (player)
    {
        LFGRewardMapBounds bounds = m_RewardMap.equal_range(dungeon->ID);
        for (LFGRewardMap::const_iterator itr = bounds.first; itr != bounds.second; ++itr)
        {
            // Difficulty check TODO
            rew = itr->second;
            // ordered properly at loading
            if (itr->second->maxLevel >= player->getLevel())
                break;
        }
    }
    return rew;
}

bool LFGMgr::IsRandomDungeon(LFGDungeonEntry const*  dungeon)
{
    if (!dungeon)
        return false;

    return dungeon->type == LFG_TYPE_RANDOM_DUNGEON;
}

void LFGMgr::Join(Player* player)
{
//    LfgDungeonSet* dungeons = NULL;

    ObjectGuid guid;
    Group* group = player->GetGroup();

    if (group)
    {
        if (player->GetObjectGuid() != group->GetLeaderGuid())
        {
            player->GetSession()->SendLfgJoinResult(LFG_JOIN_NOT_MEET_REQS);
            return;
        }
        else
            guid = group->GetObjectGuid();
    }
    else
        guid = player->GetObjectGuid();

    if (guid.IsEmpty())
        return;

    LFGType type = player->GetLFGState()->GetType();

    if (type == LFG_TYPE_NONE)
    {
        DEBUG_LOG("LFGMgr::Join: %u trying to join without dungeon type. Aborting.", guid.GetCounter());
        player->GetSession()->SendLfgJoinResult(LFG_JOIN_DUNGEON_INVALID);
        return;
    }
    else if (group && type == LFG_TYPE_RAID && !group->isRaidGroup())
    {
        DEBUG_LOG("LFGMgr::Join: %u trying to join to raid finder, but group is not raid. Aborting.", guid.GetCounter());
        player->GetSession()->SendLfgJoinResult(LFG_JOIN_MIXED_RAID_DUNGEON);
        return;
    }
    else if (group && type != LFG_TYPE_RAID && group->isRaidGroup())
    {
        DEBUG_LOG("LFGMgr::Join: %u trying to join to dungeon finder, but group is raid. Aborting.", guid.GetCounter());
        player->GetSession()->SendLfgJoinResult(LFG_JOIN_MIXED_RAID_DUNGEON);
        return;
    }

    LFGQueueInfoMap::iterator queue = (guid.IsGroup() ? m_groupQueueInfoMap[type].find(guid) : m_queueInfoMap[type].find(guid));
    LFGJoinResult result            = LFG_JOIN_OK;

    if (queue != (guid.IsGroup() ? m_groupQueueInfoMap[type].end() : m_queueInfoMap[type].end()))
    {
        DEBUG_LOG("LFGMgr::Join: %u trying to join but is already in queue!", guid.GetCounter());
        result = LFG_JOIN_INTERNAL_ERROR;
        player->GetSession()->SendLfgJoinResult(result);
        _Leave(guid);
        _LeaveGroup(guid);
        return;
    }

    result = guid.IsGroup() ? GetGroupJoinResult(group) : GetPlayerJoinResult(player);

    if (result != LFG_JOIN_OK)                              // Someone can't join. Clear all stuf
    {
        DEBUG_LOG("LFGMgr::Join: %u joining with %u members. result: %u", guid.GetCounter(), group ? group->GetMembersCount() : 1, result);
        player->GetLFGState()->Clear();
        player->GetSession()->SendLfgJoinResult(result);
        player->GetSession()->SendLfgUpdateParty(LFG_UPDATETYPE_ROLECHECK_FAILED, type);
        return;
    }

    if (!guid.IsGroup() && player->GetLFGState()->GetRoles() == LFG_ROLE_MASK_NONE)
    {
        sLog.outError("LFGMgr::Join: %u has no roles", guid.GetCounter());
    }


    // Joining process
    if (guid.IsGroup())
    {
        for (GroupReference *itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
        {
            if (Player* player = itr->getSource())
                _Leave(player->GetObjectGuid());
        }
        _LeaveGroup(guid, type);
        _JoinGroup(guid, type);
    }
    else
    {
        _Leave(guid, type);
        _Join(guid, type);
    }

    player->GetLFGState()->SetState((type == LFG_TYPE_RAID) ? LFG_STATE_LFR : LFG_STATE_LFG);

    player->GetSession()->SendLfgJoinResult(LFG_JOIN_OK, 0);

    if (group)
        player->GetSession()->SendLfgUpdateParty(LFG_UPDATETYPE_JOIN_PROPOSAL, type);
    else
        player->GetSession()->SendLfgUpdatePlayer(LFG_UPDATETYPE_JOIN_PROPOSAL, type);

    if(sWorld.getConfig(CONFIG_BOOL_RESTRICTED_LFG_CHANNEL))
        player->JoinLFGChannel();

}

void LFGMgr::_Join(ObjectGuid guid, LFGType type)
{
    if (guid.IsEmpty())
        return;

    LFGQueueInfo* pqInfo = new LFGQueueInfo();

    // Joining process
    DEBUG_LOG("LFGMgr::AddToQueue: player %u joined", guid.GetCounter());
    m_queueInfoMap[type][guid] = pqInfo;

}

void LFGMgr::_JoinGroup(ObjectGuid guid, LFGType type)
{
    if (guid.IsEmpty() || !guid.IsGroup())
        return;

    LFGQueueInfo* pqInfo = new LFGQueueInfo();

    // Joining process
    DEBUG_LOG("LFGMgr::AddToQueue: group %u joined", guid.GetCounter());
    m_groupQueueInfoMap[type][guid] = pqInfo;

}

void LFGMgr::Leave(Group* group)
{
    if (!group)
        return;
    Player* leader = sObjectMgr.GetPlayer(group->GetLeaderGuid());

    if (!leader)
        return;

    Leave(leader);
}

void LFGMgr::Leave(Player* player)
{

    ObjectGuid guid;
    Group* group = player->GetGroup();

    if (group)
    {
        if (player->GetObjectGuid() != group->GetLeaderGuid())
            return;
        else
            guid = group->GetObjectGuid();
    }
    else
        guid = player->GetObjectGuid();

    if (guid.IsEmpty())
        return;

    LFGType type = player->GetLFGState()->GetType();

    guid.IsGroup() ? _LeaveGroup(guid) : _Leave(guid);

    player->GetLFGState()->Clear();
    if (group)
    {
        group->GetLFGState()->Clear();
        player->GetSession()->SendLfgUpdateParty(LFG_UPDATETYPE_REMOVED_FROM_QUEUE, type);
    }
    player->GetSession()->SendLfgUpdatePlayer(LFG_UPDATETYPE_REMOVED_FROM_QUEUE, type);

    if(sWorld.getConfig(CONFIG_BOOL_RESTRICTED_LFG_CHANNEL) && player->GetSession()->GetSecurity() == SEC_PLAYER )
        player->LeaveLFGChannel();
}

void LFGMgr::_Leave(ObjectGuid guid, LFGType excludeType)
{
    // leaving process
    for (uint8 i = LFG_TYPE_NONE; i < LFG_TYPE_MAX; ++i)
    {
        if (i == excludeType)
            continue;

        LFGQueueInfoMap::iterator queue = m_queueInfoMap[i].find(guid);
        if (queue != m_queueInfoMap[i].end())
        {
            delete queue->second;
            m_queueInfoMap[i].erase(guid);
        }
    }
}

void LFGMgr::_LeaveGroup(ObjectGuid guid, LFGType excludeType)
{
    // leaving process
    for (uint8 i = LFG_TYPE_NONE; i < LFG_TYPE_MAX; ++i)
    {
        if (i == excludeType)
            continue;

        LFGQueueInfoMap::iterator queue = m_groupQueueInfoMap[i].find(guid);
        if (queue != m_groupQueueInfoMap[i].end())
        {
            delete queue->second;
            m_groupQueueInfoMap[i].erase(guid);
        }
    }
}

LFGJoinResult LFGMgr::GetPlayerJoinResult(Player* player)
{

   if (player->InBattleGround() || player->InArena() || player->InBattleGroundQueue())
        return LFG_JOIN_USING_BG_SYSTEM;

   if (player->HasAura(LFG_SPELL_DUNGEON_DESERTER))
        return LFG_JOIN_DESERTER;

    if (player->HasAura(LFG_SPELL_DUNGEON_COOLDOWN))
        return LFG_JOIN_RANDOM_COOLDOWN;

    LFGDungeonSet* dungeons = player->GetLFGState()->GetDungeons();

//    if (!dungeons || !dungeons->size())
    if (!dungeons)
        return LFG_JOIN_NOT_MEET_REQS;
    // TODO - Check if all dungeons are valid

    return LFG_JOIN_OK;
}

LFGJoinResult LFGMgr::GetGroupJoinResult(Group* group)
{
    if (!group)
        return LFG_JOIN_PARTY_INFO_FAILED;

    if (group->GetMembersCount() > MAX_GROUP_SIZE)
        return LFG_JOIN_TOO_MUCH_MEMBERS;

    for (GroupReference *itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* player = itr->getSource();

        if (!player->IsInWorld())
            return LFG_JOIN_DISCONNECTED;

        LFGJoinResult result = GetPlayerJoinResult(player);

        if (result != LFG_JOIN_OK)
            return result;
    }

    return LFG_JOIN_OK;
}

LFGLockStatusMap LFGMgr::GetPlayerLockMap(Player* player)
{
    LFGLockStatusMap tmpMap;
    tmpMap.clear();

    if (!player)
        return tmpMap;

    for (uint32 i = 1; i < sLFGDungeonStore.GetNumRows(); ++i)
    {
        if (LFGDungeonEntry const* entry = sLFGDungeonStore.LookupEntry(i))
            if (LFGLockStatusType status = GetPlayerLockStatus(player, entry))
                if (status != LFG_LOCKSTATUS_OK)
                    tmpMap.insert(std::make_pair(entry,status));
    }

    return tmpMap;
}

LFGLockStatusType LFGMgr::GetPlayerLockStatus(Player* player, LFGDungeonEntry const* dungeon)
{
    if (!player || !player->IsInWorld())
        return LFG_LOCKSTATUS_RAID_LOCKED;

    if (dungeon->expansion > player->GetSession()->Expansion())
        return LFG_LOCKSTATUS_INSUFFICIENT_EXPANSION;

    if (dungeon->difficulty > DUNGEON_DIFFICULTY_NORMAL
        && player->GetBoundInstance(dungeon->map, Difficulty(dungeon->difficulty)))
        return  LFG_LOCKSTATUS_RAID_LOCKED;

    if (dungeon->minlevel > player->getLevel())
        return  LFG_LOCKSTATUS_TOO_LOW_LEVEL;

    if (dungeon->maxlevel < player->getLevel())
        return LFG_LOCKSTATUS_TOO_HIGH_LEVEL;

    switch (player->GetAreaLockStatus(dungeon->map, Difficulty(dungeon->difficulty)))
    {
        case AREA_LOCKSTATUS_OK:
            break;
        case AREA_LOCKSTATUS_TOO_LOW_LEVEL:
            return  LFG_LOCKSTATUS_TOO_LOW_LEVEL;
        case AREA_LOCKSTATUS_QUEST_NOT_COMPLETED:
            return LFG_LOCKSTATUS_QUEST_NOT_COMPLETED;
        case AREA_LOCKSTATUS_MISSING_ITEM:
            return LFG_LOCKSTATUS_MISSING_ITEM;
        case AREA_LOCKSTATUS_MISSING_DIFFICULTY:
            return LFG_LOCKSTATUS_RAID_LOCKED;
        case AREA_LOCKSTATUS_INSUFFICIENT_EXPANSION:
            return LFG_LOCKSTATUS_INSUFFICIENT_EXPANSION;
        case AREA_LOCKSTATUS_NOT_ALLOWED:
            return LFG_LOCKSTATUS_RAID_LOCKED;
        case AREA_LOCKSTATUS_RAID_LOCKED:
        case AREA_LOCKSTATUS_UNKNOWN_ERROR:
        default:
            return LFG_LOCKSTATUS_RAID_LOCKED;
    }

    if (dungeon->difficulty > DUNGEON_DIFFICULTY_NORMAL)
    {
        if (AreaTrigger const* at = sObjectMgr.GetMapEntranceTrigger(dungeon->map))
        {
            uint32 gs = player->GetEquipGearScore(true,true);

            if (at->minGS > 0 && gs < at->minGS)
                return LFG_LOCKSTATUS_TOO_LOW_GEAR_SCORE;
            else if (at->maxGS > 0 && gs > at->maxGS)
                return LFG_LOCKSTATUS_TOO_HIGH_GEAR_SCORE;
        }
        else
            return LFG_LOCKSTATUS_RAID_LOCKED;
    }

    if (InstancePlayerBind* bind = player->GetBoundInstance(dungeon->map, Difficulty(dungeon->difficulty)))
    {
        if (DungeonPersistentState* state = bind->state)
            if (state->IsCompleted())
                return LFG_LOCKSTATUS_RAID_LOCKED;
    }

        /* TODO
            LFG_LOCKSTATUS_ATTUNEMENT_TOO_LOW_LEVEL;
            LFG_LOCKSTATUS_ATTUNEMENT_TOO_HIGH_LEVEL;
            LFG_LOCKSTATUS_NOT_IN_SEASON;
        */

    return LFG_LOCKSTATUS_OK;
}

LFGLockStatusType LFGMgr::GetGroupLockStatus(Group* group, LFGDungeonEntry const* dungeon)
{
    if (!group)
        return LFG_LOCKSTATUS_RAID_LOCKED;

    for (GroupReference *itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* player = itr->getSource();

        LFGLockStatusType result = GetPlayerLockStatus(player, dungeon);

        if (result != LFG_LOCKSTATUS_OK)
            return result;
    }
    return LFG_LOCKSTATUS_OK;
}

LFGDungeonSet LFGMgr::GetRandomDungeonsForPlayer(Player* player)
{
    LFGDungeonSet list;
    list.clear();

    for (uint32 i = 0; i < sLFGDungeonStore.GetNumRows(); ++i)
    {
        if (LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(i))
        {
            if (dungeon && 
                dungeon->type == LFG_TYPE_RANDOM_DUNGEON &&
                GetPlayerLockStatus(player, dungeon) == LFG_LOCKSTATUS_OK)
                list.insert(dungeon);
        }
    }
    return list;
}

LFGDungeonEntry const* LFGMgr::GetDungeon(uint32 dungeonID)
{
    LFGDungeonMap::const_iterator itr = m_dungeonMap.find(dungeonID);
    return itr != m_dungeonMap.end() ? itr->second : NULL;
}

void LFGMgr::ClearLFRList(Player* player)
{
    if (!player)
        return;

    LFGDungeonSet* dungeons = player->GetLFGState()->GetDungeons();
    dungeons->clear();
    DEBUG_LOG("LFGMgr::LFR List cleared, player %u leaving LFG queue", player->GetObjectGuid().GetCounter());
    _Leave(player->GetObjectGuid());

}

LFGQueuePlayerSet LFGMgr::GetDungeonPlayerQueue(LFGDungeonEntry const* dungeon, Team team)
{
    LFGQueuePlayerSet tmpSet;
    tmpSet.clear();
    LFGType type = LFG_TYPE_NONE;
    uint32 dungeonID = 0;
    uint8 searchEnd = LFG_TYPE_MAX;
    if (dungeon)
    {
        type = LFGType(dungeon->type);
        dungeonID = dungeon->ID;
        searchEnd = type+1;
    }

    for (uint8 i = type; i < searchEnd; ++i)
    {
        for (LFGQueueInfoMap::iterator itr = m_queueInfoMap[i].begin(); itr != m_queueInfoMap[i].end(); ++itr)
        {
            ObjectGuid guid = itr->first;
            if (!guid.IsPlayer())
                continue;

            Player* player = sObjectMgr.GetPlayer(guid);
            if (!player)
                continue;

            if (team && player->GetTeam() != team)
                continue;

            if (player->GetLFGState()->GetState() < LFG_STATE_LFR ||
                player->GetLFGState()->GetState() > LFG_STATE_PROPOSAL)
                continue;

            if (player->GetLFGState()->GetDungeons()->find(dungeon) == player->GetLFGState()->GetDungeons()->end())
                continue;

            tmpSet.insert(player);
        }
    }
    return tmpSet;
}

LFGQueueGroupSet LFGMgr::GetDungeonGroupQueue(LFGDungeonEntry const* dungeon, Team team)
{
    LFGQueueGroupSet tmpSet;
    tmpSet.clear();
    LFGType type = LFG_TYPE_NONE;
    uint32 dungeonID = 0;
    uint8 searchEnd = LFG_TYPE_MAX;
    if (dungeon)
    {
        type = LFGType(dungeon->type);
        dungeonID = dungeon->ID;
        searchEnd = type+1;
    }

    for (uint8 i = type; i < searchEnd; ++i)
    {
        for (LFGQueueInfoMap::iterator itr = m_groupQueueInfoMap[i].begin(); itr != m_groupQueueInfoMap[i].end(); ++itr)
        {
            ObjectGuid guid = itr->first;
            if (!guid.IsGroup())
                continue;

            Group* group = sObjectMgr.GetGroup(guid);
            if (!group)
                continue;

            Player* player = sObjectMgr.GetPlayer(group->GetLeaderGuid());
            if (!player)
                continue;

            if (team && player->GetTeam() != team)
                continue;

            if (player->GetLFGState()->GetState() < LFG_STATE_LFR ||
                player->GetLFGState()->GetState() > LFG_STATE_PROPOSAL)
                continue;

            if (player->GetLFGState()->GetDungeons()->find(dungeon) == player->GetLFGState()->GetDungeons()->end())
                continue;

            tmpSet.insert(group);
        }
    }
    return tmpSet;
}

void LFGMgr::SendLFGRewards(Player* player)
{
    Group* group = player->GetGroup();
    if (!group || !group->isLFDGroup())
    {
        DEBUG_LOG("LFGMgr::SendLFGReward: %u is not in a group or not a LFGGroup. Ignoring", player->GetObjectGuid().GetCounter());
        return;
    }

    for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
    {
        if (Player* member = itr->getSource())
            if (member->IsInWorld())
                SendLFGReward(member);
    }
}

void LFGMgr::SendLFGReward(Player* player)
{
    LFGDungeonEntry const* dungeon = *player->GetLFGState()->GetDungeons()->begin();

    if (!dungeon || dungeon->type != LFG_TYPE_RANDOM_DUNGEON)
    {
        DEBUG_LOG("LFGMgr::RewardDungeonDoneFor: %u dungeon is not random", player->GetObjectGuid().GetCounter());
        return;
    }

    // Update achievements
//    if (dungeon->difficulty == DUNGEON_DIFFICULTY_HEROIC)
//        player->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_USE_LFD_TO_GROUP_WITH_PLAYERS, 1);

    LFGReward const* reward = GetRandomDungeonReward(dungeon, player);

    if (!reward)
        return;

    uint8 index = 0;
    Quest const* qReward = sObjectMgr.GetQuestTemplate(reward->reward[index].questId);
    if (!qReward)
        return;

    // if we can take the quest, means that we haven't done this kind of "run", IE: First Heroic Random of Day.
    if (player->CanRewardQuest(qReward,false))
        player->RewardQuest(qReward,0,NULL,false);
    else
    {
        index = 1;
        qReward = sObjectMgr.GetQuestTemplate(reward->reward[index].questId);
        if (!qReward)
            return;
        // we give reward without informing client (retail does this)
        player->RewardQuest(qReward,0,NULL,false);
    }

    // Give rewards
    DEBUG_LOG("LFGMgr::RewardDungeonDoneFor: %u done dungeon %u, %s previously done.", player->GetObjectGuid().GetCounter(), dungeon->ID, index > 0 ? " " : " not");
    player->GetSession()->SendLfgPlayerReward(dungeon, reward, qReward, index == 0);
}

bool LFGMgr::CreateProposal(LFGDungeonEntry const* dungeon, Group* group)
{
    if (!dungeon)
        return false;

    uint32 ID = GenerateProposalID();
    LFGProposal proposal = LFGProposal(dungeon);
    proposal.cancelTime = time_t(time(NULL)) + LFG_TIME_PROPOSAL;
    proposal.state = LFG_PROPOSAL_INITIATING;
    proposal.group = group;
    m_proposalMap.insert(std::make_pair(ID, proposal));

    if (group)
    {
        group->GetLFGState()->SetProposal(GetProposal(ID));
    }

    DEBUG_LOG("LFGMgr::CreateProposal: %u, dungeon %u, %s", ID, dungeon->ID, group ? " in group" : " not in group");
}

LFGProposal* LFGMgr::GetProposal(uint32 ID)
{
    LFGProposalMap::iterator itr = m_proposalMap.find(ID);
    return itr != m_proposalMap.end() ? &itr->second : NULL;
}

void LFGMgr::RemoveProposal(uint32 ID)
{
    LFGProposalMap::iterator itr = m_proposalMap.find(ID);
    if (itr == m_proposalMap.end())
        return;

    if (Group* group = (*itr).second.group)
    {
        group->GetLFGState()->SetProposal(NULL);
        m_proposalMap.erase(itr);
    }
}

uint32 LFGMgr::GenerateProposalID()
{
    for (int32 i = 1; i != -1  ; ++i)
    {
        if (m_proposalMap.find(i) == m_proposalMap.end())
            return uint32(i);
    }
    return 0;
}
