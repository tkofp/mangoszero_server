/**
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2025 MaNGOS <https://www.getmangos.eu>
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
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */

#include "Common.h"
#include "SharedDefines.h"
#include "Player.h"
#include "BattleGroundMgr.h"
#include "BattleGroundAV.h"
#include "BattleGroundAB.h"
#include "BattleGroundWS.h"
#include "MapManager.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "ProgressBar.h"
#include "Chat.h"
#include "World.h"
#include "WorldPacket.h"
#include "Language.h"
#include "GameEventMgr.h"
#include "DisableMgr.h"
#include "GameTime.h"
#ifdef ENABLE_ELUNA
#include "LuaEngine.h"
#endif /* ENABLE_ELUNA */

#include "Policies/Singleton.h"

INSTANTIATE_SINGLETON_1(BattleGroundMgr);

/*********************************************************/
/***            BATTLEGROUND QUEUE SYSTEM              ***/
/*********************************************************/

BattleGroundQueue::BattleGroundQueue()
{
    for (uint8 i = 0; i < PVP_TEAM_COUNT; ++i)
    {
        for (uint8 j = 0; j < MAX_BATTLEGROUND_BRACKETS; ++j)
        {
            m_SumOfWaitTimes[i][j] = 0;
            m_WaitTimeLastPlayer[i][j] = 0;
            for (uint8 k = 0; k < COUNT_OF_PLAYERS_TO_AVERAGE_WAIT_TIME; ++k)
            {
                m_WaitTimes[i][j][k] = 0;
            }
        }
    }
}

BattleGroundQueue::~BattleGroundQueue()
{
    m_QueuedPlayers.clear();
    for (uint8 i = 0; i < MAX_BATTLEGROUND_BRACKETS; ++i)
    {
        for (uint8 j = 0; j < BG_QUEUE_GROUP_TYPES_COUNT; ++j)
        {
            for (GroupsQueueType::iterator itr = m_QueuedGroups[i][j].begin(); itr != m_QueuedGroups[i][j].end(); ++itr)
            {
                delete(*itr);
            }
            m_QueuedGroups[i][j].clear();
        }
    }
}

/*********************************************************/
/***      BATTLEGROUND QUEUE SELECTION POOLS           ***/
/*********************************************************/

// selection pool initialization, used to clean up from prev selection
void BattleGroundQueue::SelectionPool::Init()
{
    SelectedGroups.clear();
    PlayerCount = 0;
}

// remove group info from selection pool
// returns true when we need to try to add new group to selection pool
// returns false when selection pool is ok or when we kicked smaller group than we need to kick
// sometimes it can be called on empty selection pool
bool BattleGroundQueue::SelectionPool::KickGroup(uint32 size)
{
    // find maxgroup or LAST group with size == size and kick it
    bool found = false;
    GroupsQueueType::iterator groupToKick = SelectedGroups.begin();
    for (GroupsQueueType::iterator itr = groupToKick; itr != SelectedGroups.end(); ++itr)
    {
        if (abs((int32)((*itr)->Players.size() - size)) <= 1)
        {
            groupToKick = itr;
            found = true;
        }
        else if (!found && (*itr)->Players.size() >= (*groupToKick)->Players.size())
        {
            groupToKick = itr;
        }
    }
    // if pool is empty, do nothing
    if (GetPlayerCount())
    {
        // update player count
        GroupQueueInfo* ginfo = (*groupToKick);
        SelectedGroups.erase(groupToKick);
        PlayerCount -= ginfo->Players.size();
        // return false if we kicked smaller group or there are enough players in selection pool
        if (ginfo->Players.size() <= size + 1)
        {
            return false;
        }
    }
    return true;
}

// add group to selection pool
// used when building selection pools
// returns true if we can invite more players, or when we added group to selection pool
// returns false when selection pool is full
bool BattleGroundQueue::SelectionPool::AddGroup(GroupQueueInfo* ginfo, uint32 desiredCount)
{
    // if group is larger than desired count - don't allow to add it to pool
    if (!ginfo->IsInvitedToBGInstanceGUID && desiredCount >= PlayerCount + ginfo->Players.size())
    {
        SelectedGroups.push_back(ginfo);
        // increase selected players count
        PlayerCount += ginfo->Players.size();
        return true;
    }
    if (PlayerCount < desiredCount)
    {
        return true;
    }
    return false;
}

/*********************************************************/
/***               BATTLEGROUND QUEUES                 ***/
/*********************************************************/

// add group or player (grp == NULL) to bg queue with the given leader and bg specifications
GroupQueueInfo* BattleGroundQueue::AddGroup(Player* leader, Group* grp, BattleGroundTypeId BgTypeId, BattleGroundBracketId bracketId, bool isPremade)
{
    // create new ginfo
    GroupQueueInfo* ginfo = new GroupQueueInfo;
    ginfo->BgTypeId                  = BgTypeId;
    ginfo->IsInvitedToBGInstanceGUID = 0;
    ginfo->JoinTime                  = GameTime::GetGameTimeMS();
    ginfo->RemoveInviteTime          = 0;
    ginfo->GroupTeam                 = leader->GetTeam();

    ginfo->Players.clear();

    // compute index (if group is premade or joined a rated match) to queues
    uint32 index = 0;
    if (!isPremade)
    {
        index += PVP_TEAM_COUNT;                             // BG_QUEUE_PREMADE_* -> BG_QUEUE_NORMAL_*
    }

    if (ginfo->GroupTeam == HORDE)
    {
        ++index; // BG_QUEUE_*_ALLIANCE -> BG_QUEUE_*_HORDE
    }

    DEBUG_LOG("Adding Group to BattleGroundQueue bgTypeId : %u, bracket_id : %u, index : %u", BgTypeId, bracketId, index);

    uint32 lastOnlineTime = GameTime::GetGameTimeMS();

    // add players from group to ginfo
    {
        // ACE_Guard<ACE_Recursive_Thread_Mutex> guard(m_Lock);
        if (grp)
        {
            for (GroupReference* itr = grp->GetFirstMember(); itr != NULL; itr = itr->next())
            {
                Player* member = itr->getSource();
                if (!member)
                {
                    continue; // this should never happen
                }
                PlayerQueueInfo& pl_info = m_QueuedPlayers[member->GetObjectGuid()];
                pl_info.LastOnlineTime   = lastOnlineTime;
                pl_info.GroupInfo        = ginfo;
                // add the pinfo to ginfo's list
                ginfo->Players[member->GetObjectGuid()]  = &pl_info;
            }
        }
        else
        {
            PlayerQueueInfo& pl_info = m_QueuedPlayers[leader->GetObjectGuid()];
            pl_info.LastOnlineTime   = lastOnlineTime;
            pl_info.GroupInfo        = ginfo;
            ginfo->Players[leader->GetObjectGuid()]  = &pl_info;
        }

        // add GroupInfo to m_QueuedGroups
        m_QueuedGroups[bracketId][index].push_back(ginfo);

        // announce to world, this code needs mutex
        if (!isPremade && sWorld.getConfig(CONFIG_UINT32_BATTLEGROUND_QUEUE_ANNOUNCER_JOIN))
        {
            if (BattleGround* bg = sBattleGroundMgr.GetBattleGroundTemplate(ginfo->BgTypeId))
            {
                char const* bgName = bg->GetName();
                uint32 MinPlayers = bg->GetMinPlayersPerTeam();
                uint32 qHorde = 0;
                uint32 qAlliance = 0;
                uint32 q_min_level = leader->GetMinLevelForBattleGroundBracketId(bracketId, BgTypeId);
                GroupsQueueType::const_iterator itr;
                for (itr = m_QueuedGroups[bracketId][BG_QUEUE_NORMAL_ALLIANCE].begin(); itr != m_QueuedGroups[bracketId][BG_QUEUE_NORMAL_ALLIANCE].end(); ++itr)
                    if (!(*itr)->IsInvitedToBGInstanceGUID)
                    {
                        qAlliance += (*itr)->Players.size();
                    }
                for (itr = m_QueuedGroups[bracketId][BG_QUEUE_NORMAL_HORDE].begin(); itr != m_QueuedGroups[bracketId][BG_QUEUE_NORMAL_HORDE].end(); ++itr)
                    if (!(*itr)->IsInvitedToBGInstanceGUID)
                    {
                        qHorde += (*itr)->Players.size();
                    }

                // Show queue status to player only (when joining queue)
                if (sWorld.getConfig(CONFIG_UINT32_BATTLEGROUND_QUEUE_ANNOUNCER_JOIN) == 1)
                {
                    ChatHandler(leader).PSendSysMessage(LANG_BG_QUEUE_ANNOUNCE_SELF, bgName, q_min_level, q_min_level + 10,
                                                        qAlliance, (MinPlayers > qAlliance) ? MinPlayers - qAlliance : (uint32)0, qHorde, (MinPlayers > qHorde) ? MinPlayers - qHorde : (uint32)0);
                }
                // System message
                else
                {
                    sWorld.SendWorldText(LANG_BG_QUEUE_ANNOUNCE_WORLD, bgName, q_min_level, q_min_level + 10,
                                         qAlliance, (MinPlayers > qAlliance) ? MinPlayers - qAlliance : (uint32)0, qHorde, (MinPlayers > qHorde) ? MinPlayers - qHorde : (uint32)0);
                }
            }
        }
        // release mutex
    }

    return ginfo;
}

void BattleGroundQueue::PlayerInvitedToBGUpdateAverageWaitTime(GroupQueueInfo* ginfo, BattleGroundBracketId bracket_id)
{
    uint32 timeInQueue = getMSTimeDiff(ginfo->JoinTime, GameTime::GetGameTimeMS());
    uint8 team_index = TEAM_INDEX_ALLIANCE;                    // default set to BG_TEAM_ALLIANCE - or non rated arenas!

    if (ginfo->GroupTeam == HORDE)
    {
        team_index = TEAM_INDEX_HORDE;
    }

    // store pointer to arrayindex of player that was added first
    uint32* lastPlayerAddedPointer = &(m_WaitTimeLastPlayer[team_index][bracket_id]);
    // remove his time from sum
    m_SumOfWaitTimes[team_index][bracket_id] -= m_WaitTimes[team_index][bracket_id][(*lastPlayerAddedPointer)];
    // set average time to new
    m_WaitTimes[team_index][bracket_id][(*lastPlayerAddedPointer)] = timeInQueue;
    // add new time to sum
    m_SumOfWaitTimes[team_index][bracket_id] += timeInQueue;
    // set index of last player added to next one
    (*lastPlayerAddedPointer)++;
    (*lastPlayerAddedPointer) %= COUNT_OF_PLAYERS_TO_AVERAGE_WAIT_TIME;
}

uint32 BattleGroundQueue::GetAverageQueueWaitTime(GroupQueueInfo* ginfo, BattleGroundBracketId bracket_id)
{
    uint8 team_index = TEAM_INDEX_ALLIANCE;                    // default set to BG_TEAM_ALLIANCE - or non rated arenas!
    if (ginfo->GroupTeam == HORDE)
    {
        team_index = TEAM_INDEX_HORDE;
    }
    // check if there is enought values(we always add values > 0)
    if (m_WaitTimes[team_index][bracket_id][COUNT_OF_PLAYERS_TO_AVERAGE_WAIT_TIME - 1])
    {
        return (m_SumOfWaitTimes[team_index][bracket_id] / COUNT_OF_PLAYERS_TO_AVERAGE_WAIT_TIME);
    }
    else
        // if there aren't enough values return 0 - not available
    {
        return 0;
    }
}

// remove player from queue and from group info, if group info is empty then remove it too
void BattleGroundQueue::RemovePlayer(ObjectGuid guid, bool decreaseInvitedCount)
{
    // Player *plr = sObjectMgr.GetPlayer(guid);
    // ACE_Guard<ACE_Recursive_Thread_Mutex> guard(m_Lock);

    int32 bracket_id = -1;                                  // signed for proper for-loop finish
    QueuedPlayersMap::iterator itr;

    // remove player from map, if he's there
    itr = m_QueuedPlayers.find(guid);
    if (itr == m_QueuedPlayers.end())
    {
        sLog.outError("BattleGroundQueue: couldn't find for remove: %s", guid.GetString().c_str());
        return;
    }

    GroupQueueInfo* group = itr->second.GroupInfo;
    GroupsQueueType::iterator group_itr, group_itr_tmp;
    // mostly people with the highest levels are in battlegrounds, thats why
    // we count from MAX_BATTLEGROUND_QUEUES - 1 to 0
    // variable index removes useless searching in other team's queue
    uint32 index = BattleGround::GetTeamIndexByTeamId(group->GroupTeam);

    for (int8 bracket_id_tmp = MAX_BATTLEGROUND_BRACKETS - 1; bracket_id_tmp >= 0 && bracket_id == -1; --bracket_id_tmp)
    {
        // we must check premade and normal team's queue - because when players from premade are joining bg,
        // they leave groupinfo so we can't use its players size to find out index
        for (uint8 j = index; j < BG_QUEUE_GROUP_TYPES_COUNT; j += BG_QUEUE_NORMAL_ALLIANCE)
        {
            for (group_itr_tmp = m_QueuedGroups[bracket_id_tmp][j].begin(); group_itr_tmp != m_QueuedGroups[bracket_id_tmp][j].end(); ++group_itr_tmp)
            {
                if ((*group_itr_tmp) == group)
                {
                    bracket_id = bracket_id_tmp;
                    group_itr = group_itr_tmp;
                    // we must store index to be able to erase iterator
                    index = j;
                    break;
                }
            }
        }
    }
    // player can't be in queue without group, but just in case
    if (bracket_id == -1)
    {
        sLog.outError("BattleGroundQueue: ERROR Can not find groupinfo for %s", guid.GetString().c_str());
        return;
    }
    DEBUG_LOG("BattleGroundQueue: Removing %s, from bracket_id %u", guid.GetString().c_str(), (uint32)bracket_id);

    // ALL variables are correctly set
    // We can ignore leveling up in queue - it should not cause crash
    // remove player from group
    // if only one player there, remove group

    // remove player queue info from group queue info
    GroupQueueInfoPlayers::iterator pitr = group->Players.find(guid);
    if (pitr != group->Players.end())
    {
        group->Players.erase(pitr);
    }

    // if invited to bg, and should decrease invited count, then do it
    if (decreaseInvitedCount && group->IsInvitedToBGInstanceGUID)
    {
        BattleGround* bg = sBattleGroundMgr.GetBattleGround(group->IsInvitedToBGInstanceGUID, group->BgTypeId);
        if (bg)
        {
            bg->DecreaseInvitedCount(group->GroupTeam);
        }
    }

    // remove player queue info
    m_QueuedPlayers.erase(itr);

    // remove group queue info if needed
    if (group->Players.empty())
    {
        m_QueuedGroups[bracket_id][index].erase(group_itr);
        delete group;
    }
}

// returns true when player pl_guid is in queue and is invited to bgInstanceGuid
bool BattleGroundQueue::IsPlayerInvited(ObjectGuid pl_guid, const uint32 bgInstanceGuid, const uint32 removeTime)
{
    // ACE_Guard<ACE_Recursive_Thread_Mutex> g(m_Lock);
    QueuedPlayersMap::const_iterator qItr = m_QueuedPlayers.find(pl_guid);
    return (qItr != m_QueuedPlayers.end()
            && qItr->second.GroupInfo->IsInvitedToBGInstanceGUID == bgInstanceGuid
            && qItr->second.GroupInfo->RemoveInviteTime == removeTime);
}

bool BattleGroundQueue::GetPlayerGroupInfoData(ObjectGuid guid, GroupQueueInfo* ginfo)
{
    // ACE_Guard<ACE_Recursive_Thread_Mutex> g(m_Lock);
    QueuedPlayersMap::const_iterator qItr = m_QueuedPlayers.find(guid);
    if (qItr == m_QueuedPlayers.end())
    {
        return false;
    }
    *ginfo = *(qItr->second.GroupInfo);
    return true;
}

bool BattleGroundQueue::InviteGroupToBG(GroupQueueInfo* ginfo, BattleGround* bg, Team side)
{
    // set side if needed
    if (side)
    {
        ginfo->GroupTeam = side;
    }

    if (!ginfo->IsInvitedToBGInstanceGUID)
    {
        // not yet invited
        // set invitation
        ginfo->IsInvitedToBGInstanceGUID = bg->GetInstanceID();
        BattleGroundTypeId bgTypeId = bg->GetTypeID();
        BattleGroundQueueTypeId bgQueueTypeId = BattleGroundMgr::BGQueueTypeId(bgTypeId);
        BattleGroundBracketId bracket_id = bg->GetBracketId();

        ginfo->RemoveInviteTime = GameTime::GetGameTimeMS() + INVITE_ACCEPT_WAIT_TIME;

        // loop through the players
        for (GroupQueueInfoPlayers::iterator itr = ginfo->Players.begin(); itr != ginfo->Players.end(); ++itr)
        {
            // get the player
            Player* plr = sObjectMgr.GetPlayer(itr->first);
            // if offline, skip him, this should not happen - player is removed from queue when he logs out
            if (!plr)
            {
                continue;
            }

            // invite the player
            PlayerInvitedToBGUpdateAverageWaitTime(ginfo, bracket_id);
            // sBattleGroundMgr.InvitePlayer(plr, bg, ginfo->Team);

            // set invited player counters
            bg->IncreaseInvitedCount(ginfo->GroupTeam);

            plr->SetInviteForBattleGroundQueueType(bgQueueTypeId, ginfo->IsInvitedToBGInstanceGUID);

            // create remind invite events
            BGQueueInviteEvent* inviteEvent = new BGQueueInviteEvent(plr->GetObjectGuid(), ginfo->IsInvitedToBGInstanceGUID, bgTypeId, ginfo->RemoveInviteTime);
            plr->m_Events.AddEvent(inviteEvent, plr->m_Events.CalculateTime(INVITATION_REMIND_TIME));
            // create automatic remove events
            BGQueueRemoveEvent* removeEvent = new BGQueueRemoveEvent(plr->GetObjectGuid(), ginfo->IsInvitedToBGInstanceGUID, bgTypeId, bgQueueTypeId, ginfo->RemoveInviteTime);
            plr->m_Events.AddEvent(removeEvent, plr->m_Events.CalculateTime(INVITE_ACCEPT_WAIT_TIME));

            WorldPacket data;

            uint32 queueSlot = plr->GetBattleGroundQueueIndex(bgQueueTypeId);

            DEBUG_LOG("Battleground: invited %s to BG instance %u queueindex %u bgtype %u, I can't help it if they don't press the enter battle button.",
                      plr->GetGuidStr().c_str(), bg->GetInstanceID(), queueSlot, bg->GetTypeID());

            // send status packet
            sBattleGroundMgr.BuildBattleGroundStatusPacket(&data, bg, queueSlot, STATUS_WAIT_JOIN, INVITE_ACCEPT_WAIT_TIME, 0);
            plr->GetSession()->SendPacket(&data);
        }
        return true;
    }

    return false;
}

/*
This function is inviting players to already running battlegrounds
Invitation type is based on config file
large groups are disadvantageous, because they will be kicked first if invitation type = 1
*/
void BattleGroundQueue::FillPlayersToBG(BattleGround* bg, BattleGroundBracketId bracket_id)
{
    int32 hordeFree = bg->GetFreeSlotsForTeam(HORDE);
    int32 aliFree   = bg->GetFreeSlotsForTeam(ALLIANCE);

    // iterator for iterating through bg queue
    GroupsQueueType::const_iterator Ali_itr = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE].begin();
    // count of groups in queue - used to stop cycles
    uint32 aliCount = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE].size();
    // index to queue which group is current
    uint32 aliIndex = 0;
    for (; aliIndex < aliCount && m_SelectionPools[TEAM_INDEX_ALLIANCE].AddGroup((*Ali_itr), aliFree); ++aliIndex)
    {
        ++Ali_itr;
    }
    // the same thing for horde
    GroupsQueueType::const_iterator Horde_itr = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_HORDE].begin();
    uint32 hordeCount = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_HORDE].size();
    uint32 hordeIndex = 0;
    for (; hordeIndex < hordeCount && m_SelectionPools[TEAM_INDEX_HORDE].AddGroup((*Horde_itr), hordeFree); ++hordeIndex)
    {
        ++Horde_itr;
    }

    // if ofc like BG queue invitation is set in config, then we are happy
    if (sWorld.getConfig(CONFIG_UINT32_BATTLEGROUND_INVITATION_TYPE) == 0)
    {
        return;
    }

    /*
    if we reached this code, then we have to solve NP - complete problem called Subset sum problem
    So one solution is to check all possible invitation subgroups, or we can use these conditions:
    1. Last time when BattleGroundQueue::Update was executed we invited all possible players - so there is only small possibility
        that we will invite now whole queue, because only 1 change has been made to queues from the last BattleGroundQueue::Update call
    2. Other thing we should consider is group order in queue
    */

    // At first we need to compare free space in bg and our selection pool
    int32 diffAli   = aliFree   - int32(m_SelectionPools[TEAM_INDEX_ALLIANCE].GetPlayerCount());
    int32 diffHorde = hordeFree - int32(m_SelectionPools[TEAM_INDEX_HORDE].GetPlayerCount());
    while (abs(diffAli - diffHorde) > 1 && (m_SelectionPools[TEAM_INDEX_HORDE].GetPlayerCount() > 0 || m_SelectionPools[TEAM_INDEX_ALLIANCE].GetPlayerCount() > 0))
    {
        // each cycle execution we need to kick at least 1 group
        if (diffAli < diffHorde)
        {
            // kick alliance group, add to pool new group if needed
            if (m_SelectionPools[TEAM_INDEX_ALLIANCE].KickGroup(diffHorde - diffAli))
            {
                for (; aliIndex < aliCount && m_SelectionPools[TEAM_INDEX_ALLIANCE].AddGroup((*Ali_itr), (aliFree >= diffHorde) ? aliFree - diffHorde : 0); ++aliIndex)
                {
                    ++Ali_itr;
                }
            }
            // if ali selection is already empty, then kick horde group, but if there are less horde than ali in bg - break;
            if (!m_SelectionPools[TEAM_INDEX_ALLIANCE].GetPlayerCount())
            {
                if (aliFree <= diffHorde + 1)
                {
                    break;
                }
                m_SelectionPools[TEAM_INDEX_HORDE].KickGroup(diffHorde - diffAli);
            }
        }
        else
        {
            // kick horde group, add to pool new group if needed
            if (m_SelectionPools[TEAM_INDEX_HORDE].KickGroup(diffAli - diffHorde))
            {
                for (; hordeIndex < hordeCount && m_SelectionPools[TEAM_INDEX_HORDE].AddGroup((*Horde_itr), (hordeFree >= diffAli) ? hordeFree - diffAli : 0); ++hordeIndex)
                {
                    ++Horde_itr;
                }
            }
            if (!m_SelectionPools[TEAM_INDEX_HORDE].GetPlayerCount())
            {
                if (hordeFree <= diffAli + 1)
                {
                    break;
                }
                m_SelectionPools[TEAM_INDEX_ALLIANCE].KickGroup(diffAli - diffHorde);
            }
        }
        // count diffs after small update
        diffAli   = aliFree   - int32(m_SelectionPools[TEAM_INDEX_ALLIANCE].GetPlayerCount());
        diffHorde = hordeFree - int32(m_SelectionPools[TEAM_INDEX_HORDE].GetPlayerCount());
    }
}

// this method checks if premade versus premade battleground is possible
// then after 30 mins (default) in queue it moves premade group to normal queue
// it tries to invite as much players as it can - to MaxPlayersPerTeam, because premade groups have more than MinPlayersPerTeam players
bool BattleGroundQueue::CheckPremadeMatch(BattleGroundBracketId bracket_id, uint32 MinPlayersPerTeam, uint32 MaxPlayersPerTeam)
{
    // check match
    if (!m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].empty() && !m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].empty())
    {
        // start premade match
        // if groups aren't invited
        GroupsQueueType::const_iterator ali_group, horde_group;
        for (ali_group = m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].begin(); ali_group != m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].end(); ++ali_group)
            if (!(*ali_group)->IsInvitedToBGInstanceGUID)
            {
                break;
            }
        for (horde_group = m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].begin(); horde_group != m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].end(); ++horde_group)
            if (!(*horde_group)->IsInvitedToBGInstanceGUID)
            {
                break;
            }

        if (ali_group != m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].end() && horde_group != m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].end())
        {
            m_SelectionPools[TEAM_INDEX_ALLIANCE].AddGroup((*ali_group), MaxPlayersPerTeam);
            m_SelectionPools[TEAM_INDEX_HORDE].AddGroup((*horde_group), MaxPlayersPerTeam);
            // add groups/players from normal queue to size of bigger group
            uint32 maxPlayers = std::max(m_SelectionPools[TEAM_INDEX_ALLIANCE].GetPlayerCount(), m_SelectionPools[TEAM_INDEX_HORDE].GetPlayerCount());
            GroupsQueueType::const_iterator itr;
            for (uint8 i = 0; i < PVP_TEAM_COUNT; ++i)
            {
                for (itr = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + i].begin(); itr != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + i].end(); ++itr)
                {
                    // if itr can join BG and player count is less that maxPlayers, then add group to selectionpool
                    if (!(*itr)->IsInvitedToBGInstanceGUID && !m_SelectionPools[i].AddGroup((*itr), maxPlayers))
                    {
                        break;
                    }
                }
            }
            // premade selection pools are set
            return true;
        }
    }
    // now check if we can move group from Premade queue to normal queue (timer has expired) or group size lowered!!
    // this could be 2 cycles but i'm checking only first team in queue - it can cause problem -
    // if first is invited to BG and seconds timer expired, but we can ignore it, because players have only 80 seconds to click to enter bg
    // and when they click or after 80 seconds the queue info is removed from queue
    uint32 time_before = GameTime::GetGameTimeMS() - sWorld.getConfig(CONFIG_UINT32_BATTLEGROUND_PREMADE_GROUP_WAIT_FOR_MATCH);
    for (uint8 i = 0; i < PVP_TEAM_COUNT; ++i)
    {
        if (!m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE + i].empty())
        {
            GroupsQueueType::iterator itr = m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE + i].begin();
            if (!(*itr)->IsInvitedToBGInstanceGUID && ((*itr)->JoinTime < time_before || (*itr)->Players.size() < MinPlayersPerTeam))
            {
                // we must insert group to normal queue and erase pointer from premade queue
                m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + i].push_front((*itr));
                m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE + i].erase(itr);
            }
        }
    }
    // selection pools are not set
    return false;
}

// this method tries to create battleground or arena with MinPlayersPerTeam against MinPlayersPerTeam
bool BattleGroundQueue::CheckNormalMatch(BattleGroundBracketId bracket_id, uint32 minPlayers, uint32 maxPlayers)
{
    GroupsQueueType::const_iterator itr_team[PVP_TEAM_COUNT];
    for (uint8 i = 0; i < PVP_TEAM_COUNT; ++i)
    {
        itr_team[i] = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + i].begin();
        for (; itr_team[i] != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + i].end(); ++(itr_team[i]))
        {
            if (!(*(itr_team[i]))->IsInvitedToBGInstanceGUID)
            {
                m_SelectionPools[i].AddGroup(*(itr_team[i]), maxPlayers);
                if (m_SelectionPools[i].GetPlayerCount() >= minPlayers)
                {
                    break;
                }
            }
        }
    }
    // try to invite same number of players - this cycle may cause longer wait time even if there are enough players in queue, but we want ballanced bg
    uint32 j = TEAM_INDEX_ALLIANCE;
    if (m_SelectionPools[TEAM_INDEX_HORDE].GetPlayerCount() < m_SelectionPools[TEAM_INDEX_ALLIANCE].GetPlayerCount())
    {
        j = TEAM_INDEX_HORDE;
    }
    if (sWorld.getConfig(CONFIG_UINT32_BATTLEGROUND_INVITATION_TYPE) != 0
        && m_SelectionPools[TEAM_INDEX_HORDE].GetPlayerCount() >= minPlayers && m_SelectionPools[TEAM_INDEX_ALLIANCE].GetPlayerCount() >= minPlayers)
    {
        // we will try to invite more groups to team with less players indexed by j
        ++(itr_team[j]);                                    // this will not cause a crash, because for cycle above reached break;
        for (; itr_team[j] != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + j].end(); ++(itr_team[j]))
        {
            if (!(*(itr_team[j]))->IsInvitedToBGInstanceGUID)
                if (!m_SelectionPools[j].AddGroup(*(itr_team[j]), m_SelectionPools[(j + 1) % PVP_TEAM_COUNT].GetPlayerCount()))
                {
                    break;
                }
        }
        // do not allow to start bg with more than 2 players more on 1 faction
        if (abs((int32)(m_SelectionPools[TEAM_INDEX_HORDE].GetPlayerCount() - m_SelectionPools[TEAM_INDEX_ALLIANCE].GetPlayerCount())) > 2)
        {
            return false;
        }
    }
    // allow 1v0 if debug bg
    if (sBattleGroundMgr.isTesting() && (m_SelectionPools[TEAM_INDEX_ALLIANCE].GetPlayerCount() || m_SelectionPools[TEAM_INDEX_HORDE].GetPlayerCount()))
    {
        return true;
    }
    // return true if there are enough players in selection pools - enable to work .debug bg command correctly
    return m_SelectionPools[TEAM_INDEX_ALLIANCE].GetPlayerCount() >= minPlayers && m_SelectionPools[TEAM_INDEX_HORDE].GetPlayerCount() >= minPlayers;
}

/*
this method is called when group is inserted, or player / group is removed from BG Queue - there is only one player's status changed, so we don't use while(true) cycles to invite whole queue
it must be called after fully adding the members of a group to ensure group joining
should be called from BattleGround::RemovePlayer function in some cases
*/
void BattleGroundQueue::Update(BattleGroundTypeId bgTypeId, BattleGroundBracketId bracket_id)
{
    // ACE_Guard<ACE_Recursive_Thread_Mutex> guard(m_Lock);
    // if no players in queue - do nothing
    if (m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].empty() &&
        m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].empty() &&
        m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE].empty() &&
        m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_HORDE].empty())
    {
        return;
    }

    // battleground with free slot for player should be always in the beggining of the queue
    // maybe it would be better to create bgfreeslotqueue for each bracket_id
    BGFreeSlotQueueType::iterator itr, next;
    for (itr = sBattleGroundMgr.BGFreeSlotQueue[bgTypeId].begin(); itr != sBattleGroundMgr.BGFreeSlotQueue[bgTypeId].end(); itr = next)
    {
        next = itr;
        ++next;
        // battleground is running, so if:
        if ((*itr)->GetTypeID() == bgTypeId && (*itr)->GetBracketId() == bracket_id &&
            (*itr)->GetStatus() > STATUS_WAIT_QUEUE && (*itr)->GetStatus() < STATUS_WAIT_LEAVE)
        {
            BattleGround* bg = *itr; // we have to store battleground pointer here, because when battleground is full, it is removed from free queue (not yet implemented!!)
            // and iterator is invalid

            // clear selection pools
            m_SelectionPools[TEAM_INDEX_ALLIANCE].Init();
            m_SelectionPools[TEAM_INDEX_HORDE].Init();

            // call a function that does the job for us
            FillPlayersToBG(bg, bracket_id);

            // now everything is set, invite players
            for (GroupsQueueType::const_iterator citr = m_SelectionPools[TEAM_INDEX_ALLIANCE].SelectedGroups.begin(); citr != m_SelectionPools[TEAM_INDEX_ALLIANCE].SelectedGroups.end(); ++citr)
            {
                InviteGroupToBG((*citr), bg, (*citr)->GroupTeam);
            }
            for (GroupsQueueType::const_iterator citr = m_SelectionPools[TEAM_INDEX_HORDE].SelectedGroups.begin(); citr != m_SelectionPools[TEAM_INDEX_HORDE].SelectedGroups.end(); ++citr)
            {
                InviteGroupToBG((*citr), bg, (*citr)->GroupTeam);
            }

            if (!bg->HasFreeSlots())
            {
                // remove BG from BGFreeSlotQueue
                bg->RemoveFromBGFreeSlotQueue();
            }
        }
    }

    // finished iterating through the bgs with free slots, maybe we need to create a new bg

    BattleGround* bg_template = sBattleGroundMgr.GetBattleGroundTemplate(bgTypeId);
    if (!bg_template)
    {
        sLog.outError("Battleground: Update: bg template not found for %u", bgTypeId);
        return;
    }

    // get the min. players per team, properly for larger arenas as well. (must have full teams for arena matches!)
    uint32 MinPlayersPerTeam = bg_template->GetMinPlayersPerTeam();
    uint32 MaxPlayersPerTeam = bg_template->GetMaxPlayersPerTeam();
    if (sBattleGroundMgr.isTesting())
    {
        MinPlayersPerTeam = 1;
    }

    m_SelectionPools[TEAM_INDEX_ALLIANCE].Init();
    m_SelectionPools[TEAM_INDEX_HORDE].Init();

    {
        // check if there is premade against premade match
        if (CheckPremadeMatch(bracket_id, MinPlayersPerTeam, MaxPlayersPerTeam))
        {
            // create new battleground
            BattleGround* bg2 = sBattleGroundMgr.CreateNewBattleGround(bgTypeId, bracket_id);
            if (!bg2)
            {
                sLog.outError("BattleGroundQueue::Update - Can not create battleground: %u", bgTypeId);
                return;
            }
            // invite those selection pools
            for (uint8 i = 0; i < PVP_TEAM_COUNT; ++i)
                for (GroupsQueueType::const_iterator citr = m_SelectionPools[TEAM_INDEX_ALLIANCE + i].SelectedGroups.begin(); citr != m_SelectionPools[TEAM_INDEX_ALLIANCE + i].SelectedGroups.end(); ++citr)
                {
                    InviteGroupToBG((*citr), bg2, (*citr)->GroupTeam);
                }
            // start bg
            bg2->StartBattleGround();
            // clear structures
            m_SelectionPools[TEAM_INDEX_ALLIANCE].Init();
            m_SelectionPools[TEAM_INDEX_HORDE].Init();
        }
    }

    // now check if there are in queues enough players to start new game of (normal battleground, or non-rated arena)
    {
        // if there are enough players in pools, start new battleground or non rated arena
        if (CheckNormalMatch(bracket_id, MinPlayersPerTeam, MaxPlayersPerTeam))
        {
            // we successfully created a pool
            BattleGround* bg2 = sBattleGroundMgr.CreateNewBattleGround(bgTypeId, bracket_id);
            if (!bg2)
            {
                sLog.outError("BattleGroundQueue::Update - Can not create battleground: %u", bgTypeId);
                return;
            }

            // invite those selection pools
            for (uint8 i = 0; i < PVP_TEAM_COUNT; ++i)
                for (GroupsQueueType::const_iterator citr = m_SelectionPools[TEAM_INDEX_ALLIANCE + i].SelectedGroups.begin(); citr != m_SelectionPools[TEAM_INDEX_ALLIANCE + i].SelectedGroups.end(); ++citr)
                {
                    InviteGroupToBG((*citr), bg2, (*citr)->GroupTeam);
                }
            // start bg
            bg2->StartBattleGround();
        }
    }
}

/*********************************************************/
/***            BATTLEGROUND QUEUE EVENTS              ***/
/*********************************************************/

bool BGQueueInviteEvent::Execute(uint64 /*e_time*/, uint32 /*p_time*/)
{
    Player* plr = sObjectMgr.GetPlayer(m_PlayerGuid);
    // player logged off (we should do nothing, he is correctly removed from queue in another procedure)
    if (!plr)
    {
        return true;
    }

    BattleGround* bg = sBattleGroundMgr.GetBattleGround(m_BgInstanceGUID, m_BgTypeId);
    // if battleground ended and its instance deleted - do nothing
    if (!bg)
    {
        return true;
    }

    BattleGroundQueueTypeId bgQueueTypeId = BattleGroundMgr::BGQueueTypeId(bg->GetTypeID());
    uint32 queueSlot = plr->GetBattleGroundQueueIndex(bgQueueTypeId);
    if (queueSlot < PLAYER_MAX_BATTLEGROUND_QUEUES)         // player is in queue or in battleground
    {
        // check if player is invited to this bg
        BattleGroundQueue& bgQueue = sBattleGroundMgr.m_BattleGroundQueues[bgQueueTypeId];
        if (bgQueue.IsPlayerInvited(m_PlayerGuid, m_BgInstanceGUID, m_RemoveTime))
        {
            WorldPacket data;
            // we must send remaining time in queue
            sBattleGroundMgr.BuildBattleGroundStatusPacket(&data, bg, queueSlot, STATUS_WAIT_JOIN, INVITE_ACCEPT_WAIT_TIME - INVITATION_REMIND_TIME, 0);
            plr->GetSession()->SendPacket(&data);
        }
    }
    return true;                                            // event will be deleted
}

void BGQueueInviteEvent::Abort(uint64 /*e_time*/)
{
    // do nothing
}

/*
    this event has many possibilities when it is executed:
    1. player is in battleground ( he clicked enter on invitation window )
    2. player left battleground queue and he isn't there any more
    3. player left battleground queue and he joined it again and IsInvitedToBGInstanceGUID = 0
    4. player left queue and he joined again and he has been invited to same battleground again -> we should not remove him from queue yet
    5. player is invited to bg and he didn't choose what to do and timer expired - only in this condition we should call queue::RemovePlayer
    we must remove player in the 5. case even if battleground object doesn't exist!
*/
bool BGQueueRemoveEvent::Execute(uint64 /*e_time*/, uint32 /*p_time*/)
{
    Player* plr = sObjectMgr.GetPlayer(m_PlayerGuid);
    if (!plr)
    {
        // player logged off (we should do nothing, he is correctly removed from queue in another procedure)
        return true;
    }

    BattleGround* bg = sBattleGroundMgr.GetBattleGround(m_BgInstanceGUID, m_BgTypeId);
    // battleground can be deleted already when we are removing queue info
    // bg pointer can be NULL! so use it carefully!

    uint32 queueSlot = plr->GetBattleGroundQueueIndex(m_BgQueueTypeId);
    if (queueSlot < PLAYER_MAX_BATTLEGROUND_QUEUES)         // player is in queue, or in Battleground
    {
        // check if player is in queue for this BG and if we are removing his invite event
        BattleGroundQueue& bgQueue = sBattleGroundMgr.m_BattleGroundQueues[m_BgQueueTypeId];
        if (bgQueue.IsPlayerInvited(m_PlayerGuid, m_BgInstanceGUID, m_RemoveTime))
        {
            DEBUG_LOG("Battleground: removing player %u from bg queue for instance %u because of not pressing enter battle in time.", plr->GetGUIDLow(), m_BgInstanceGUID);

            plr->RemoveBattleGroundQueueId(m_BgQueueTypeId);
            bgQueue.RemovePlayer(m_PlayerGuid, true);
            // update queues if battleground isn't ended
            if (bg && bg->GetStatus() != STATUS_WAIT_LEAVE)
            {
                sBattleGroundMgr.ScheduleQueueUpdate(m_BgQueueTypeId, m_BgTypeId, bg->GetBracketId());
            }

            WorldPacket data;
            sBattleGroundMgr.BuildBattleGroundStatusPacket(&data, bg, queueSlot, STATUS_NONE, 0, 0);
            plr->GetSession()->SendPacket(&data);
        }
    }

    // event will be deleted
    return true;
}

void BGQueueRemoveEvent::Abort(uint64 /*e_time*/)
{
    // do nothing
}

/*********************************************************/
/***            BATTLEGROUND MANAGER                   ***/
/*********************************************************/

BattleGroundMgr::BattleGroundMgr()
{
    for (uint8 i = BATTLEGROUND_TYPE_NONE; i < MAX_BATTLEGROUND_TYPE_ID; ++i)
    {
        m_BattleGrounds[i].clear();
    }
    m_Testing = false;
}

BattleGroundMgr::~BattleGroundMgr()
{
    DeleteAllBattleGrounds();
}

void BattleGroundMgr::DeleteAllBattleGrounds()
{
    // will also delete template bgs:
    for (uint8 i = BATTLEGROUND_TYPE_NONE; i < MAX_BATTLEGROUND_TYPE_ID; ++i)
    {
        for (BattleGroundSet::iterator itr = m_BattleGrounds[i].begin(); itr != m_BattleGrounds[i].end();)
        {
            BattleGround* bg = itr->second;
            ++itr;                                          // step from invalidate iterator pos in result element remove in ~BattleGround call
            delete bg;
        }
    }
}

// used to update running battlegrounds, and delete finished ones
void BattleGroundMgr::Update(uint32 /*diff*/)
{
    // update scheduled queues
    if (!m_QueueUpdateScheduler.empty())
    {
        std::vector<uint32> scheduled;
        {
            // create mutex
            // ACE_Guard<ACE_Thread_Mutex> guard(SchedulerLock);
            // copy vector and clear the other
            scheduled = std::vector<uint32>(m_QueueUpdateScheduler);
            m_QueueUpdateScheduler.clear();
            // release lock
        }

        for (uint8 i = 0; i < scheduled.size(); ++i)
        {
            BattleGroundQueueTypeId bgQueueTypeId = BattleGroundQueueTypeId(scheduled[i] >> 16 & 255);
            BattleGroundTypeId bgTypeId = BattleGroundTypeId((scheduled[i] >> 8) & 255);
            BattleGroundBracketId bracket_id = BattleGroundBracketId(scheduled[i] & 255);
            m_BattleGroundQueues[bgQueueTypeId].Update(bgTypeId, bracket_id);
        }
    }
}

void BattleGroundMgr::BuildBattleGroundStatusPacket(WorldPacket* data, BattleGround* bg, uint8 QueueSlot, uint8 StatusID, uint32 Time1, uint32 Time2)
{
    // we can be in 3 queues in same time...

    if (StatusID == 0 || !bg)
    {
        data->Initialize(SMSG_BATTLEFIELD_STATUS, 4 * 2);
        *data << uint32(QueueSlot);                         // queue id (0...2)
        *data << uint32(0);
        return;
    }

    data->Initialize(SMSG_BATTLEFIELD_STATUS, (4 + 8 + 4 + 1 + 4 + 4 + 4));
    *data << uint32(QueueSlot);                             // queue id (0...2) - player can be in 3 queues in time
    *data << uint32(bg->GetMapId());                        // MapID
    *data << uint8(0);                                      // Unknown
    *data << uint32(bg->GetClientInstanceID());
    *data << uint32(StatusID);                              // status
    switch (StatusID)
    {
        case STATUS_WAIT_QUEUE:                             // status_in_queue
            *data << uint32(Time1);                         // average wait time, milliseconds
            *data << uint32(Time2);                         // time in queue, updated every minute!, milliseconds
            break;
        case STATUS_WAIT_JOIN:                              // status_invite
            *data << uint32(Time1);                         // time to remove from queue, milliseconds
            break;
        case STATUS_IN_PROGRESS:                            // status_in_progress
            *data << uint32(Time1);                         // time to bg auto leave, 0 at bg start, 120000 after bg end, milliseconds
            *data << uint32(Time2);                         // time from bg start, milliseconds
            break;
        default:
            sLog.outError("Unknown BG status!");
            break;
    }
}

void BattleGroundMgr::BuildPvpLogDataPacket(WorldPacket* data, BattleGround* bg)
{
    data->Initialize(MSG_PVP_LOG_DATA, (1 + 4 + 40 * bg->GetPlayerScoresSize()));

    if (bg->GetStatus() != STATUS_WAIT_LEAVE)
    {
        *data << uint8(0);                                  // bg not ended
    }
    else
    {
        *data << uint8(1);                                  // bg ended
        *data << uint8(bg->GetWinner());                    // who win
    }

    *data << (uint32)(bg->GetPlayerScoresSize());

    for (BattleGround::BattleGroundScoreMap::const_iterator itr = bg->GetPlayerScoresBegin(); itr != bg->GetPlayerScoresEnd(); ++itr)
    {
        const BattleGroundScore* score = itr->second;

        *data << ObjectGuid(itr->first);

        Player* plr = sObjectMgr.GetPlayer(itr->first);

        *data << uint32(plr ? plr->GetHonorRankInfo().visualRank : 0);
        *data << uint32(itr->second->KillingBlows);
        *data << uint32(itr->second->HonorableKills);
        *data << uint32(itr->second->Deaths);
        *data << uint32(itr->second->BonusHonor);

        switch (bg->GetTypeID())                            // battleground specific things
        {
            case BATTLEGROUND_AV:
                *data << (uint32)0x00000007;                // count of next fields
                *data << (uint32)((BattleGroundAVScore*)score)->GraveyardsAssaulted;  // GraveyardsAssaulted
                *data << (uint32)((BattleGroundAVScore*)score)->GraveyardsDefended;   // GraveyardsDefended
                *data << (uint32)((BattleGroundAVScore*)score)->TowersAssaulted;      // TowersAssaulted
                *data << (uint32)((BattleGroundAVScore*)score)->TowersDefended;       // TowersDefended
                *data << (uint32)((BattleGroundAVScore*)score)->SecondaryObjectives;  // Mines Taken
                *data << (uint32)((BattleGroundAVScore*)score)->LieutnantCount;       // Lieutnant kills
                *data << (uint32)((BattleGroundAVScore*)score)->SecondaryNPC;         // Secondary unit summons
                break;
            case BATTLEGROUND_WS:
                *data << (uint32)0x00000002;                // count of next fields
                *data << (uint32)((BattleGroundWGScore*)score)->FlagCaptures;         // flag captures
                *data << (uint32)((BattleGroundWGScore*)score)->FlagReturns;          // flag returns
                break;
            case BATTLEGROUND_AB:
                *data << (uint32)0x00000002;                // count of next fields
                *data << (uint32)((BattleGroundABScore*)score)->BasesAssaulted;       // bases asssulted
                *data << (uint32)((BattleGroundABScore*)score)->BasesDefended;        // bases defended
                break;
            default:
                DEBUG_LOG("Unhandled MSG_PVP_LOG_DATA for BG id %u", bg->GetTypeID());
                *data << (uint32)0;
                break;
        }
    }
}

void BattleGroundMgr::BuildGroupJoinedBattlegroundPacket(WorldPacket* data, int32 status)
{
    data->Initialize(SMSG_GROUP_JOINED_BATTLEGROUND, 4);
    // for status, see enum BattleGroundGroupJoinStatus
    *data << int32(status);
}

void BattleGroundMgr::BuildUpdateWorldStatePacket(WorldPacket* data, uint32 field, uint32 value)
{
    data->Initialize(SMSG_UPDATE_WORLD_STATE, 4 + 4);
    *data << uint32(field);
    *data << uint32(value);
}

void BattleGroundMgr::BuildPlaySoundPacket(WorldPacket* data, uint32 soundid)
{
    data->Initialize(SMSG_PLAY_SOUND, 4);
    *data << uint32(soundid);
}

void BattleGroundMgr::BuildPlayerLeftBattleGroundPacket(WorldPacket* data, ObjectGuid guid)
{
    data->Initialize(SMSG_BATTLEGROUND_PLAYER_LEFT, 8);
    *data << ObjectGuid(guid);
}

void BattleGroundMgr::BuildPlayerJoinedBattleGroundPacket(WorldPacket* data, Player* plr)
{
    data->Initialize(SMSG_BATTLEGROUND_PLAYER_JOINED, 8);
    *data << plr->GetObjectGuid();
}

BattleGround* BattleGroundMgr::GetBattleGroundThroughClientInstance(uint32 instanceId, BattleGroundTypeId bgTypeId)
{
    // cause at HandleBattleGroundJoinOpcode the clients sends the instanceid he gets from
    // SMSG_BATTLEFIELD_LIST we need to find the battleground with this clientinstance-id
    BattleGround* bg = GetBattleGroundTemplate(bgTypeId);
    if (!bg)
    {
        return NULL;
    }

    for (BattleGroundSet::iterator itr = m_BattleGrounds[bgTypeId].begin(); itr != m_BattleGrounds[bgTypeId].end(); ++itr)
    {
        if (itr->second->GetClientInstanceID() == instanceId)
        {
            return itr->second;
        }
    }
    return NULL;
}

BattleGround* BattleGroundMgr::GetBattleGround(uint32 InstanceID, BattleGroundTypeId bgTypeId)
{
    // search if needed
    BattleGroundSet::iterator itr;
    if (bgTypeId == BATTLEGROUND_TYPE_NONE)
    {
        for (uint8 i = BATTLEGROUND_AV; i < MAX_BATTLEGROUND_TYPE_ID; ++i)
        {
            itr = m_BattleGrounds[i].find(InstanceID);
            if (itr != m_BattleGrounds[i].end())
            {
                return itr->second;
            }
        }
        return NULL;
    }
    itr = m_BattleGrounds[bgTypeId].find(InstanceID);
    return ((itr != m_BattleGrounds[bgTypeId].end()) ? itr->second : NULL);
}

BattleGround* BattleGroundMgr::GetBattleGroundTemplate(BattleGroundTypeId bgTypeId)
{
    // map is sorted and we can be sure that lowest instance id has only BG template
    return m_BattleGrounds[bgTypeId].empty() ? NULL : m_BattleGrounds[bgTypeId].begin()->second;
}

uint32 BattleGroundMgr::CreateClientVisibleInstanceId(BattleGroundTypeId bgTypeId, BattleGroundBracketId bracket_id)
{
    // we create here an instanceid, which is just for
    // displaying this to the client and without any other use..
    // the client-instanceIds are unique for each battleground-type
    // the instance-id just needs to be as low as possible, beginning with 1
    // the following works, because std::set is default ordered with "<"
    // the optimalization would be to use as bitmask std::vector<uint32> - but that would only make code unreadable
    uint32 lastId = 0;
    ClientBattleGroundIdSet& ids = m_ClientBattleGroundIds[bgTypeId][bracket_id];
    for (ClientBattleGroundIdSet::const_iterator itr = ids.begin(); itr != ids.end();)
    {
        if ((++lastId) != *itr)                             // if there is a gap between the ids, we will break..
        {
            break;
        }
        lastId = *itr;
    }
    ids.insert(lastId + 1);
    return lastId + 1;
}

// create a new battleground that will really be used to play
BattleGround* BattleGroundMgr::CreateNewBattleGround(BattleGroundTypeId bgTypeId, BattleGroundBracketId bracket_id)
{
    // get the template BG
    BattleGround* bg_template = GetBattleGroundTemplate(bgTypeId);
    if (!bg_template)
    {
        sLog.outError("BattleGround: CreateNewBattleGround - bg template not found for %u", bgTypeId);
        return NULL;
    }

    BattleGround* bg = NULL;
    // create a copy of the BG template
    switch (bgTypeId)
    {
        case BATTLEGROUND_AV:
            bg = new BattleGroundAV(*(BattleGroundAV*)bg_template);
            break;
        case BATTLEGROUND_WS:
            bg = new BattleGroundWS(*(BattleGroundWS*)bg_template);
            break;
        case BATTLEGROUND_AB:
            bg = new BattleGroundAB(*(BattleGroundAB*)bg_template);
            break;
        default:
            // error, but it is handled few lines above
            return 0;
    }

    // will also set m_bgMap, instanceid
    sMapMgr.CreateBgMap(bg->GetMapId(), bg);

    bg->SetClientInstanceID(CreateClientVisibleInstanceId(bgTypeId, bracket_id));

    // reset the new bg (set status to status_wait_queue from status_none)
    bg->Reset();

    // start the joining of the bg
    bg->SetStatus(STATUS_WAIT_JOIN);
    bg->SetBracketId(bracket_id);

    return bg;
}

// used to create the BG templates
uint32 BattleGroundMgr::CreateBattleGround(BattleGroundTypeId bgTypeId, uint32 MinPlayersPerTeam, uint32 MaxPlayersPerTeam, uint32 LevelMin, uint32 LevelMax, char const* BattleGroundName, uint32 MapID, float Team1StartLocX, float Team1StartLocY, float Team1StartLocZ, float Team1StartLocO, float Team2StartLocX, float Team2StartLocY, float Team2StartLocZ, float Team2StartLocO, float StartMaxDist)
{
    // Create the BG
    BattleGround* bg;
    switch (bgTypeId)
    {
        case BATTLEGROUND_AV: bg = new BattleGroundAV; break;
        case BATTLEGROUND_WS: bg = new BattleGroundWS; break;
        case BATTLEGROUND_AB: bg = new BattleGroundAB; break;
        default:              bg = new BattleGround;   break;                           // placeholder for non implemented BG
    }

    bg->SetMapId(MapID);
    bg->SetTypeID(bgTypeId);
    bg->SetMinPlayersPerTeam(MinPlayersPerTeam);
    bg->SetMaxPlayersPerTeam(MaxPlayersPerTeam);
    bg->SetMinPlayers(MinPlayersPerTeam * 2);
    bg->SetMaxPlayers(MaxPlayersPerTeam * 2);
    bg->SetName(BattleGroundName);
    bg->SetTeamStartLoc(ALLIANCE, Team1StartLocX, Team1StartLocY, Team1StartLocZ, Team1StartLocO);
    bg->SetTeamStartLoc(HORDE,    Team2StartLocX, Team2StartLocY, Team2StartLocZ, Team2StartLocO);
    bg->SetStartMaxDist(StartMaxDist);
    bg->SetLevelRange(LevelMin, LevelMax);

    // add bg to update list
    AddBattleGround(bg->GetInstanceID(), bg->GetTypeID(), bg);

    // return some not-null value, bgTypeId is good enough for me
    return bgTypeId;
}

void BattleGroundMgr::CreateInitialBattleGrounds()
{
    uint32 count = 0;

    //                                                0   1                 2                 3      4      5                6              7             8            9
    QueryResult* result = WorldDatabase.Query("SELECT `id`, `MinPlayersPerTeam`,`MaxPlayersPerTeam`,`MinLvl`,`MaxLvl`,`AllianceStartLoc`,`AllianceStartO`,`HordeStartLoc`,`HordeStartO`, `StartMaxDist` FROM `battleground_template`");

    if (!result)
    {
        BarGoLink bar(1);
        bar.step();
        sLog.outErrorDb(">> Loaded 0 battlegrounds. DB table `battleground_template` is empty.");
        sLog.outString();
        return;
    }

    BarGoLink bar(result->GetRowCount());

    do
    {
        Field* fields = result->Fetch();
        bar.step();

        uint32 bgTypeID_ = fields[0].GetUInt32();

        if (DisableMgr::IsDisabledFor(DISABLE_TYPE_BATTLEGROUND, bgTypeID_))
        {
            continue;
        }

        BattleGroundTypeId bgTypeID = BattleGroundTypeId(bgTypeID_);

        uint32 MinPlayersPerTeam = fields[1].GetUInt32();
        uint32 MaxPlayersPerTeam = fields[2].GetUInt32();
        uint32 MinLvl = fields[3].GetUInt32();
        uint32 MaxLvl = fields[4].GetUInt32();

        float AStartLoc[4];
        float HStartLoc[4];

        uint32 start1 = fields[5].GetUInt32();

        WorldSafeLocsEntry const* start = sWorldSafeLocsStore.LookupEntry(start1);
        if (start)
        {
            AStartLoc[0] = start->x;
            AStartLoc[1] = start->y;
            AStartLoc[2] = start->z;
            AStartLoc[3] = fields[6].GetFloat();
        }
        else
        {
            sLog.outErrorDb("Table `battleground_template` for id %u have nonexistent WorldSafeLocs.dbc id %u in field `AllianceStartLoc`. BG not created.", bgTypeID, start1);
            continue;
        }

        uint32 start2 = fields[7].GetUInt32();

        start = sWorldSafeLocsStore.LookupEntry(start2);
        if (start)
        {
            HStartLoc[0] = start->x;
            HStartLoc[1] = start->y;
            HStartLoc[2] = start->z;
            HStartLoc[3] = fields[8].GetFloat();
        }
        else
        {
            sLog.outErrorDb("Table `battleground_template` for id %u refers to a non-existing WorldSafeLocs.dbc id %u in field `HordeStartLoc`. BG not created.", bgTypeID, start2);
            continue;
        }

        uint32 mapId = GetBattleGrounMapIdByTypeId(bgTypeID);
        char const* name;

        if (MapEntry const* mapEntry = sMapStore.LookupEntry(mapId))
        {
            name = mapEntry->name[sWorld.GetDefaultDbcLocale()];
        }
        else
        {
            sLog.outErrorDb("Table `battleground_template` for id %u associated with nonexistent map id %u.", bgTypeID, mapId);
            continue;
        }

        float startMaxDist = fields[9].GetFloat();
        // sLog.outDetail("Creating battleground %s, %u-%u", bl->name[sWorld.GetDBClang()], MinLvl, MaxLvl);
        if (!CreateBattleGround(bgTypeID, MinPlayersPerTeam, MaxPlayersPerTeam, MinLvl, MaxLvl, name, mapId, AStartLoc[0], AStartLoc[1], AStartLoc[2], AStartLoc[3], HStartLoc[0], HStartLoc[1], HStartLoc[2], HStartLoc[3], startMaxDist))
        {
            continue;
        }

        ++count;
    }
    while (result->NextRow());

    delete result;

    sLog.outString(">> Loaded %u battlegrounds", count);
    sLog.outString();
}

void BattleGroundMgr::BuildBattleGroundListPacket(WorldPacket* data, ObjectGuid guid, Player* plr, BattleGroundTypeId bgTypeId)
{
    if (!plr)
    {
        return;
    }

    uint32 mapId = GetBattleGrounMapIdByTypeId(bgTypeId);

    data->Initialize(SMSG_BATTLEFIELD_LIST);
    *data << guid;                                          // battlemaster guid
    *data << uint32(mapId);                                 // battleground id
    *data << uint8(0x00);                                   // unk

    // battleground
    {
        //*data << uint8(0x00);                               // [-ZERO] unk

        //size_t count_pos = data->wpos();
        //uint32 count = 0;

        uint32 bracket_id = plr->GetBattleGroundBracketIdFromLevel(bgTypeId);
        ClientBattleGroundIdSet const& ids = m_ClientBattleGroundIds[bgTypeId][bracket_id];
        *data << uint32(ids.size());                        // number of bg instances
        for (std::set<uint32>::const_iterator itr = ids.begin(); itr != ids.end(); ++itr)
        {
            *data << uint32(*itr);
            //++count;
        }
        //data->put<uint32>(count_pos , count);
    }
}

void BattleGroundMgr::SendToBattleGround(Player* pl, uint32 instanceId, BattleGroundTypeId bgTypeId)
{
    BattleGround* bg = GetBattleGround(instanceId, bgTypeId);
    if (bg)
    {
        uint32 mapid = bg->GetMapId();
        float x, y, z, O;
        Team team = pl->GetBGTeam();
        if (team == 0)
        {
            team = pl->GetTeam();
        }
        bg->GetTeamStartLoc(team, x, y, z, O);

        DETAIL_LOG("BATTLEGROUND: Sending %s to map %u, X %f, Y %f, Z %f, O %f", pl->GetName(), mapid, x, y, z, O);
        pl->TeleportTo(mapid, x, y, z, O);
    }
    else
    {
        sLog.outError("player %u trying to port to nonexistent bg instance %u", pl->GetGUIDLow(), instanceId);
    }
}

BattleGroundQueueTypeId BattleGroundMgr::BGQueueTypeId(BattleGroundTypeId bgTypeId)
{
    switch (bgTypeId)
    {
        case BATTLEGROUND_WS:
            return BATTLEGROUND_QUEUE_WS;
        case BATTLEGROUND_AB:
            return BATTLEGROUND_QUEUE_AB;
        case BATTLEGROUND_AV:
            return BATTLEGROUND_QUEUE_AV;
        default:
            return BATTLEGROUND_QUEUE_NONE;
    }
}

BattleGroundTypeId BattleGroundMgr::BGTemplateId(BattleGroundQueueTypeId bgQueueTypeId)
{
    switch (bgQueueTypeId)
    {
        case BATTLEGROUND_QUEUE_WS:
            return BATTLEGROUND_WS;
        case BATTLEGROUND_QUEUE_AB:
            return BATTLEGROUND_AB;
        case BATTLEGROUND_QUEUE_AV:
            return BATTLEGROUND_AV;
        default:
            return BattleGroundTypeId(0);                   // used for unknown template (it exist and do nothing)
    }
}

void BattleGroundMgr::ToggleTesting()
{
    m_Testing = !m_Testing;
    if (m_Testing)
    {
        sWorld.SendWorldText(LANG_DEBUG_BG_ON);
    }
    else
    {
        sWorld.SendWorldText(LANG_DEBUG_BG_OFF);
    }
}

void BattleGroundMgr::ScheduleQueueUpdate(BattleGroundQueueTypeId bgQueueTypeId, BattleGroundTypeId bgTypeId, BattleGroundBracketId bracket_id)
{
    // ACE_Guard<ACE_Thread_Mutex> guard(SchedulerLock);
    // we will use only 1 number created of bgTypeId and bracket_id
    uint32 schedule_id = (bgQueueTypeId << 16) | (bgTypeId << 8) | bracket_id;
    bool found = false;
    for (uint8 i = 0; i < m_QueueUpdateScheduler.size(); ++i)
    {
        if (m_QueueUpdateScheduler[i] == schedule_id)
        {
            found = true;
            break;
        }
    }
    if (!found)
    {
        m_QueueUpdateScheduler.push_back(schedule_id);
    }
}

uint32 BattleGroundMgr::GetPrematureFinishTime() const
{
    return sWorld.getConfig(CONFIG_UINT32_BATTLEGROUND_PREMATURE_FINISH_TIMER);
}

void BattleGroundMgr::LoadBattleMastersEntry()
{
    mBattleMastersMap.clear();                              // need for reload case

    QueryResult* result = WorldDatabase.Query("SELECT `entry`,`bg_template` FROM `battlemaster_entry`");

    uint32 count = 0;

    if (!result)
    {
        BarGoLink bar(1);
        bar.step();
        sLog.outString(">> Loaded 0 battlemaster entries - table is empty!");
        sLog.outString();
        return;
    }

    BarGoLink bar(result->GetRowCount());

    do
    {
        ++count;
        bar.step();

        Field* fields = result->Fetch();

        uint32 entry = fields[0].GetUInt32();
        uint32 bgTypeId  = fields[1].GetUInt32();
        if (bgTypeId >= MAX_BATTLEGROUND_TYPE_ID)
        {
            sLog.outErrorDb("Table `battlemaster_entry` contain entry %u for nonexistent battleground type %u, ignored.", entry, bgTypeId);
            continue;
        }

        mBattleMastersMap[entry] = BattleGroundTypeId(bgTypeId);
    }
    while (result->NextRow());

    delete result;

    sLog.outString(">> Loaded %u battlemaster entries", count);
    sLog.outString();
}

HolidayIds BattleGroundMgr::BGTypeToWeekendHolidayId(BattleGroundTypeId bgTypeId)
{
    switch (bgTypeId)
    {
        case BATTLEGROUND_AV: return HOLIDAY_CALL_TO_ARMS_AV;
        case BATTLEGROUND_WS: return HOLIDAY_CALL_TO_ARMS_WS;
        case BATTLEGROUND_AB: return HOLIDAY_CALL_TO_ARMS_AB;
        default: return HOLIDAY_NONE;
    }
}

BattleGroundTypeId BattleGroundMgr::WeekendHolidayIdToBGType(HolidayIds holiday)
{
    switch (holiday)
    {
        case HOLIDAY_CALL_TO_ARMS_AV: return BATTLEGROUND_AV;
        case HOLIDAY_CALL_TO_ARMS_WS: return BATTLEGROUND_WS;
        case HOLIDAY_CALL_TO_ARMS_AB: return BATTLEGROUND_AB;
        default: return BATTLEGROUND_TYPE_NONE;
    }
}

bool BattleGroundMgr::IsBGWeekend(BattleGroundTypeId bgTypeId)
{
    return sGameEventMgr.IsActiveHoliday(BGTypeToWeekendHolidayId(bgTypeId));
}

void BattleGroundMgr::LoadBattleEventIndexes()
{
    BattleGroundEventIdx events;
    events.event1 = BG_EVENT_NONE;
    events.event2 = BG_EVENT_NONE;
    m_GameObjectBattleEventIndexMap.clear();             // need for reload case
    m_GameObjectBattleEventIndexMap[-1] = events;
    m_CreatureBattleEventIndexMap.clear();               // need for reload case
    m_CreatureBattleEventIndexMap[-1] = events;

    uint32 count = 0;

    QueryResult* result =
        //                           0         1           2                3                4              5           6
        WorldDatabase.Query("SELECT `data`.`typ`, `data`.`guid1`, `data`.`ev1` AS `ev1`, `data`.`ev2` AS ev2, `data`.`map` AS m, `data`.`guid2`, `description`.`map`, "
                            //                              7                  8                   9
                            "`description`.`event1`, `description`.`event2`, `description`.`description` "
                            "FROM "
                            "(SELECT '1' AS typ, `a`.`guid` AS `guid1`, `a`.`event1` AS ev1, `a`.`event2` AS ev2, `b`.`map` AS map, `b`.`guid` AS guid2 "
                            "FROM `gameobject_battleground` AS a "
                            "LEFT OUTER JOIN `gameobject` AS b ON `a`.`guid` = `b`.`guid` "
                            "UNION "
                            "SELECT '2' AS typ, `a`.`guid` AS guid1, `a`.`event1` AS ev1, `a`.`event2` AS ev2, `b`.`map` AS map, `b`.`guid` AS guid2 "
                            "FROM `creature_battleground` AS a "
                            "LEFT OUTER JOIN `creature` AS b ON `a`.`guid` = `b`.`guid` "
                            ") data "
                            "RIGHT OUTER JOIN `battleground_events` AS `description` ON `data`.`map` = `description`.`map` "
                            "AND `data`.`ev1` = `description`.`event1` AND `data`.`ev2` = `description`.`event2` "
                            // full outer join doesn't work in mysql :-/ so just UNION-select the same again and add a left outer join
                            "UNION "
                            "SELECT `data`.`typ`, `data`.`guid1`, `data`.`ev1`, `data`.`ev2`, `data`.`map`, `data`.`guid2`, `description`.`map`, "
                            "`description`.`event1`, `description`.`event2`, `description`.`description` "
                            "FROM "
                            "(SELECT '1' AS typ, `a`.`guid` AS guid1, `a`.`event1` AS ev1, `a`.`event2` AS ev2, `b`.`map` AS map, `b`.`guid` AS guid2 "
                            "FROM `gameobject_battleground` AS a "
                            "LEFT OUTER JOIN `gameobject` AS b ON `a`.`guid` = `b`.`guid` "
                            "UNION "
                            "SELECT '2' AS typ, `a`.`guid` AS guid1, `a`.`event1` AS ev1, `a`.`event2` AS ev2, `b`.`map` AS map, `b`.`guid` AS guid2 "
                            "FROM `creature_battleground` AS a "
                            "LEFT OUTER JOIN `creature` AS b ON `a`.`guid` = `b`.`guid` "
                            ") data "
                            "LEFT OUTER JOIN `battleground_events` AS `description` ON `data`.`map` = `description`.`map` "
                            "AND `data`.`ev1` = `description`.`event1` AND `data`.`ev2` = `description`.`event2` "
                            "ORDER BY `m`, `ev1`, `ev2`");
    if (!result)
    {
        BarGoLink bar(1);
        bar.step();
        sLog.outErrorDb(">> Loaded 0 battleground eventindexes.");
        sLog.outString();
        return;
    }

    BarGoLink bar(result->GetRowCount());

    do
    {
        bar.step();
        Field* fields = result->Fetch();
        if (fields[2].GetUInt8() == BG_EVENT_NONE || fields[3].GetUInt8() == BG_EVENT_NONE)
        {
            continue; // we don't need to add those to the eventmap
        }

        bool gameobject         = (fields[0].GetUInt8() == 1);
        uint32 dbTableGuidLow   = fields[1].GetUInt32();
        events.event1           = fields[2].GetUInt8();
        events.event2           = fields[3].GetUInt8();
        uint32 map              = fields[4].GetUInt32();

        uint32 desc_map = fields[6].GetUInt32();
        uint8 desc_event1 = fields[7].GetUInt8();
        uint8 desc_event2 = fields[8].GetUInt8();
        const char* description = fields[9].GetString();

        // checking for NULL - through right outer join this will mean following:
        if (fields[5].GetUInt32() != dbTableGuidLow)
        {
            sLog.outErrorDb("BattleGroundEvent: %s with nonexistent guid %u for event: map:%u, event1:%u, event2:%u (\"%s\")",
                            (gameobject) ? "gameobject" : "creature", dbTableGuidLow, map, events.event1, events.event2, description);
            continue;
        }

        // checking for NULL - through full outer join this can mean 2 things:
        if (desc_map != map)
        {
            // there is an event missing
            if (dbTableGuidLow == 0)
            {
                sLog.outErrorDb("BattleGroundEvent: missing db-data for map:%u, event1:%u, event2:%u (\"%s\")", desc_map, desc_event1, desc_event2, description);
                continue;
            }
            // we have an event which shouldn't exist
            else
            {
                sLog.outErrorDb("BattleGroundEvent: %s with guid %u is registered, for a nonexistent event: map:%u, event1:%u, event2:%u",
                                (gameobject) ? "gameobject" : "creature", dbTableGuidLow, map, events.event1, events.event2);
                continue;
            }
        }

        if (gameobject)
        {
            m_GameObjectBattleEventIndexMap[dbTableGuidLow] = events;
        }
        else
        {
            m_CreatureBattleEventIndexMap[dbTableGuidLow] = events;
        }

        ++count;
    }
    while (result->NextRow());

    sLog.outString(">> Loaded %u battleground eventindexes", count);
    sLog.outString();
    delete result;
}
