/* 
 * Copyright (C) 2005,2006,2007 MaNGOS <http://www.mangosproject.org/>
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
#include "Player.h"
#include "BattleGroundMgr.h"
#include "BattleGroundAV.h"
#include "BattleGroundAB.h"
#include "BattleGroundEY.h"
#include "BattleGroundWS.h"
#include "BattleGroundNA.h"
#include "BattleGroundBE.h"
#include "BattleGroundAA.h"
#include "BattleGroundRL.h"
#include "SharedDefines.h"
#include "Policies/SingletonImp.h"
#include "MapManager.h"
#include "ObjectMgr.h"
#include "ProgressBar.h"
#include "World.h"

INSTANTIATE_SINGLETON_1( BattleGroundMgr );

/*********************************************************/
/***            BATTLEGROUND QUEUE SYSTEM              ***/
/*********************************************************/

BattleGroundQueue::BattleGroundQueue()
{
    //queues are empty, we don't have to call clear()
    for (int i = 0; i < MAX_BATTLEGROUND_QUEUES; i++)
    {
        m_QueuedPlayers[i].Horde = 0;
        m_QueuedPlayers[i].Alliance = 0;
        //m_QueuedPlayers[i].AverageTime = 0;
    }
}

BattleGroundQueue::~BattleGroundQueue()
{
    for (int i = 0; i < MAX_BATTLEGROUND_QUEUES; i++)
    {
        m_QueuedPlayers[i].clear();
    }
}

void BattleGroundQueue::AddPlayer(Player *plr, uint32 bgTypeId)
{
    uint32 queue_id = plr->GetBattleGroundQueueIdFromLevel();

    //if player isn't in queue, he is added, if already is, then values are overwritten, no memory leak
    PlayerQueueInfo& info = m_QueuedPlayers[queue_id][plr->GetGUID()];
    info.InviteTime                 = 0;
    info.IsInvitedToBGInstanceGUID  = 0;
    info.LastInviteTime             = 0;
    info.LastOnlineTime             = getMSTime();
    info.Team                       = plr->GetTeam();

    //add player to waiting order queue
    m_PlayersSortedByWaitTime[queue_id].push_back(plr->GetGUID());

    if(plr->GetTeam() == ALLIANCE)
        m_QueuedPlayers[queue_id].Alliance++;
    else
        m_QueuedPlayers[queue_id].Horde++;

    /* temporary BG queue status message 
    std::ostringstream str;
    str << "Battleground queue invited status: horde: " << m_QueuedPlayers[queue_id].Horde << ", aliance: " << m_QueuedPlayers[queue_id].Alliance;
    plr->Say(str.str(), LANG_UNIVERSAL);*/

    this->Update(bgTypeId, queue_id);
}

void BattleGroundQueue::RemovePlayer(uint64 guid, bool decreaseInvitedCount)
{
    Player *plr = objmgr.GetPlayer(guid);

    uint32 queue_id = 0;
    QueuedPlayersMap::iterator itr;
    bool IsSet = false;
    if(!plr)
    {                                                       //player is offline, we need to find him somewhere in queues
        /// there is something wrong if this code is run, because we have in queue only online players!
        sLog.outError("Battleground: removing offline player from BG queue - this might not happen, but it should not cause crash");
        for (uint32 i = 0; i < MAX_BATTLEGROUND_QUEUES; i++)
        {
            itr = m_QueuedPlayers[i].find(guid);
            if(itr != m_QueuedPlayers[i].end())
            {
                queue_id = i;
                IsSet = true;
                break;
            }
        }
    }
    else
    {                                                       //player is online, we have his level, so we can find exact queue from his level
        queue_id = plr->GetBattleGroundQueueIdFromLevel();
        itr = m_QueuedPlayers[queue_id].find(guid);
        IsSet = true;
    }

    //all variables are set, so remove player
    //remove player from time queue
    m_PlayersSortedByWaitTime[queue_id].remove(guid);

    if (IsSet && itr != m_QueuedPlayers[queue_id].end())
    {
        if (!itr->second.IsInvitedToBGInstanceGUID)
        {
            if(itr->second.Team == ALLIANCE)
                m_QueuedPlayers[queue_id].Alliance--;
            else
                m_QueuedPlayers[queue_id].Horde--;
        }
        else
        {
            if (decreaseInvitedCount)
            {
                BattleGround* bg = sBattleGroundMgr.GetBattleGround(itr->second.IsInvitedToBGInstanceGUID);
                if (bg)
                    bg->DecreaseInvitedCount(itr->second.Team);
            }
        }
        m_QueuedPlayers[queue_id].erase(itr);
    }
}

/*
this method is called when player is inserted, or removed from BG Queue - there is only one player's status changed, so we don't use while(true) cycles to invite whole queue
add method calls this by itself, the remove method could works in other way, so you have to call this method from other code after calling remove method
*/
void BattleGroundQueue::Update(uint32 bgTypeId, uint32 queue_id)
{
    if (queue_id >= MAX_BATTLEGROUND_QUEUES)
    {
        //this is error, that caused crashes (not in , but now it shouldn't)
        sLog.outError("BattleGroundQueue::Update() called for non existing queue type - this can cause crash, pls report problem, if this is the last line of error log before crash");
        return;
    }

    //if no players in queue ... do nothing
    if (this->m_QueuedPlayers[queue_id].Alliance == 0 && this->m_QueuedPlayers[queue_id].Horde == 0)
        return;

    //battleground with free slot for player should be always the last in this queue
    BGFreeSlotQueueType::iterator itr;
    for (itr = sBattleGroundMgr.BGFreeSlotQueue[bgTypeId].begin(); itr != sBattleGroundMgr.BGFreeSlotQueue[bgTypeId].end(); ++itr)
    {
        // battleground is running, so if:
        if ((*itr)->GetQueueType() == queue_id && (*itr)->GetStatus() > STATUS_WAIT_QUEUE && (*itr)->GetStatus() < STATUS_WAIT_LEAVE)
        {
            //we must check both teams
            BattleGround* bg = *itr; //we have to store battleground pointer here, because when battleground is full, it is removed from free queue (not yet implemented!!)
            // and iterator is invalid

            //check if there are some players in queue
            if (m_QueuedPlayers[queue_id].Alliance > 0 || m_QueuedPlayers[queue_id].Horde > 0)
            {
                for (PlayerGuidsSortedByTimeQueue::iterator itr2 = m_PlayersSortedByWaitTime[queue_id].begin(); itr2 != m_PlayersSortedByWaitTime[queue_id].end();)
                {
                    Player* plr = objmgr.GetPlayer(*itr2);
                    if (!plr)
                    {
                        //something is wrong!, kick player from queue
                        sLog.outError("BATTLEGROUND: problem with inviting offline player to Battleground queue .... pls report bug");
                        uint64 oldval = *itr2;
                        itr2 = m_PlayersSortedByWaitTime[queue_id].erase(itr2);
                        this->RemovePlayer(oldval, true);
                        continue;
                    }

                    // player will be invited, if in bg there is a free slot for him
                    if (bg->HasFreeSlotsForTeam(plr->GetTeam()))
                    {
                        // iterator to player's queue status
                        QueuedPlayersMap::iterator itrPlayerStatus = m_QueuedPlayers[queue_id].find(*itr2);

                        // remove him from time queue
                        itr2 = m_PlayersSortedByWaitTime[queue_id].erase(itr2);

                        // only check to be sure ... but this condition shouldn't be true (if it is true, then there is a bug somewhere and pls report it)
                        if (itrPlayerStatus == m_QueuedPlayers[queue_id].end())
                            continue;

                        // check if player is not already invited
                        if (!itrPlayerStatus->second.IsInvitedToBGInstanceGUID)
                        {
                            itrPlayerStatus->second.IsInvitedToBGInstanceGUID = bg->GetInstanceID();
                            itrPlayerStatus->second.InviteTime = getMSTime();
                            itrPlayerStatus->second.LastInviteTime = getMSTime();
                            if(itrPlayerStatus->second.Team == ALLIANCE)
                                m_QueuedPlayers[queue_id].Alliance--;
                            else
                                m_QueuedPlayers[queue_id].Horde--;
                            sBattleGroundMgr.InvitePlayer(plr, bg->GetInstanceID());

                            WorldPacket data;
                            uint32 queueSlot = plr->GetBattleGroundQueueIndex(bgTypeId);
                            sBattleGroundMgr.BuildBattleGroundStatusPacket(&data, bg, plr->GetTeam(), queueSlot, STATUS_WAIT_JOIN, INVITE_ACCEPT_WAIT_TIME, 0);
                            plr->GetSession()->SendPacket(&data);
                        }
                    }
                    else
                        ++itr2;

                    //if battleground is FULL, then it is removed from free slot queue - not yet implemented!
                    if (!bg->HasFreeSlots())
                    {
                        //if bg is full, there is no need to invite other players, so break
                        break;
                        //remove BG from BGFreeSlotQueue - not used now, in this system we don't remove BGs from free queue
                        //bg->RemoveFromBGFreeSlotQueue() --- do not uncomment this - not yet implemented
                    }
                }
            }
        }
    }

    /* THIS IS A CASE THAT IN QUEUE THERE IS ENOUGHT PLAYERS TO START NEW BG */
    //itr->end is the last BG - template, which is not already started!

    /* here will be a most of change, when we create battlegrounds instanciated */
    /* if (there is enought players to start new BG)
        Battleground* newbg = sBattleGroundMgr.CreateNewBattleGround(bgTypeId)
        - that function will use the COPY constructor on BattleGround class ( in bg manager we should have one battleground as a template 
            (battleground template will be used only to create new BGs, it will be an instance of BG class, but it won't ever start) */

    /* folowing code is working with current Battleground system and it should be removed, when BGs will work like instances */
    BattleGround* bg2 = sBattleGroundMgr.GetBattleGround(bgTypeId);
    if (bg2->GetQueueType() != MAX_BATTLEGROUND_QUEUES || bg2->GetStatus() != STATUS_WAIT_QUEUE)
        return;
    if (m_QueuedPlayers[queue_id].Alliance >= bg2->GetMinPlayersPerTeam() && m_QueuedPlayers[queue_id].Horde >= bg2->GetMinPlayersPerTeam())
    {
        bg2->SetStatus(STATUS_WAIT_JOIN);
        bg2->SetQueueType(queue_id);

        for (PlayerGuidsSortedByTimeQueue::iterator itr2 = m_PlayersSortedByWaitTime[queue_id].begin(); itr2 != m_PlayersSortedByWaitTime[queue_id].end();)
        {
            Player* plr = objmgr.GetPlayer(*itr2);
            if (!plr)
            {
                //something is wrong!, kick player from queue
                sLog.outError("BATTLEGROUND: problem with inviting offline player to Battleground queue .... pls report bug");
                uint64 oldval = *itr2;
                itr2 = m_PlayersSortedByWaitTime[queue_id].erase(itr2);
                this->RemovePlayer(oldval, true);
                continue;
            }

            // player will be invited, if in bg there is a free slot for him
            if (bg2->HasFreeSlotsForTeam(plr->GetTeam()))
            {
                // iterator to player's queue status
                QueuedPlayersMap::iterator itrPlayerStatus = m_QueuedPlayers[queue_id].find(*itr2);

                // remove him from time queue
                itr2 = m_PlayersSortedByWaitTime[queue_id].erase(itr2);

                // only check to be sure ... but this condition shouldn't be true (if it is true, then there is a bug somewhere and report it)
                if (itrPlayerStatus == m_QueuedPlayers[queue_id].end())
                    continue;

                //check if player is not already invited
                if (!itrPlayerStatus->second.IsInvitedToBGInstanceGUID)
                {
                    itrPlayerStatus->second.IsInvitedToBGInstanceGUID = bg2->GetInstanceID();
                    itrPlayerStatus->second.InviteTime = getMSTime();
                    itrPlayerStatus->second.LastInviteTime = getMSTime();

                    if(itrPlayerStatus->second.Team == ALLIANCE)
                        m_QueuedPlayers[queue_id].Alliance--;
                    else
                        m_QueuedPlayers[queue_id].Horde--;

                    sBattleGroundMgr.InvitePlayer(plr, bg2->GetInstanceID());

                    WorldPacket data;
                    uint32 queueSlot = plr->GetBattleGroundQueueIndex(bgTypeId);
                    sBattleGroundMgr.BuildBattleGroundStatusPacket(&data, bg2, plr->GetTeam(), queueSlot, STATUS_WAIT_JOIN, INVITE_ACCEPT_WAIT_TIME, 0);
                    plr->GetSession()->SendPacket(&data);
                }
            }
            else
                ++itr2;
        }
        bg2->StartBattleGround(0);
    }
}

/*********************************************************/
/***            BATTLEGROUND QUEUE EVENTS              ***/
/*********************************************************/

bool BGQueueInviteEvent::Execute(uint64 /*e_time*/, uint32 p_time)
{
    Player* plr = objmgr.GetPlayer( m_PlayerGuid );

    // player logged off (we should do nothing, he is correctly removed from queue in another procedure)
    if (!plr)
        return true;

    // player is already in battleground ... do nothing (battleground queue status is deleted when player is teleported to BG)
    if (plr->GetBattleGroundId() > 0)
        return true;

    BattleGround* bg = sBattleGroundMgr.GetBattleGround(m_BgInstanceGUID);
    if (bg)
    {
        uint32 queueSlot = plr->GetBattleGroundQueueIndex(bg->GetTypeID());
        if (queueSlot < PLAYER_MAX_BATTLEGROUND_QUEUES) // player is in queue
        {
            // check if player is invited to this bg ... this check must be here, because when player leaves queue and joins another, it would cause a problems
            BattleGroundQueue::QueuedPlayersMap const& qpMap = sBattleGroundMgr.m_BattleGroundQueues[bg->GetTypeID()].m_QueuedPlayers[plr->GetBattleGroundQueueIdFromLevel()];
            BattleGroundQueue::QueuedPlayersMap::const_iterator qItr = qpMap.find(m_PlayerGuid);
            if (qItr != qpMap.end() && qItr->second.IsInvitedToBGInstanceGUID == m_BgInstanceGUID)
            {
                WorldPacket data;
                sBattleGroundMgr.BuildBattleGroundStatusPacket(&data, bg, plr->GetTeam(), queueSlot, STATUS_WAIT_JOIN, BG_REMIND_INVITE_TIME, 0);
                plr->GetSession()->SendPacket(&data);
            }
        }
    }
    return true;                                        //event will be deleted
}

void BGQueueInviteEvent::Abort(uint64 /*e_time*/)
{
    //this should not be called
    sLog.outError("Battleground invite event ABORTED!");
}

bool BGQueueRemoveEvent::Execute(uint64 /*e_time*/, uint32 /*p_time*/)
{
    Player* plr = objmgr.GetPlayer( m_PlayerGuid );
    if (!plr)
        // player logged off (we should do nothing, he is correctly removed from queue in another procedure)
        return true;
    if (plr->InBattleGround())
        // player is already in battleground ... do nothing (battleground queue status is deleted when player is teleported to BG)
        return true;

    BattleGround* bg = sBattleGroundMgr.GetBattleGround(m_BgInstanceGUID);
    if (bg)
    {
        uint32 queueSlot = plr->GetBattleGroundQueueIndex(bg->GetTypeID());
        if (queueSlot < PLAYER_MAX_BATTLEGROUND_QUEUES) // player is in queue
        {
            // check if player is invited to this bg ... this check must be here, because when player leaves queue and joins another, it would cause a problems
            if (sBattleGroundMgr.m_BattleGroundQueues[bg->GetTypeID()].m_QueuedPlayers[plr->GetBattleGroundQueueIdFromLevel()].find(m_PlayerGuid)->second.IsInvitedToBGInstanceGUID == m_BgInstanceGUID)
            {
                plr->RemoveBattleGroundQueueId(bg->GetTypeID());
                sBattleGroundMgr.m_BattleGroundQueues[bg->GetTypeID()].RemovePlayer(m_PlayerGuid, true);
                sBattleGroundMgr.m_BattleGroundQueues[bg->GetTypeID()].Update(bg->GetTypeID(), bg->GetQueueType());
                WorldPacket data;
                sBattleGroundMgr.BuildBattleGroundStatusPacket(&data, bg, plr->GetTeam(), queueSlot, STATUS_NONE, 0, 0);
                plr->GetSession()->SendPacket(&data);
            }
        }
    }
    //event will be deleted
    return true;
}

void BGQueueRemoveEvent::Abort(uint64 /*e_time*/)
{
    //this should not be called
    sLog.outError("Battleground remove event ABORTED!");
}

/*********************************************************/
/***            BATTLEGROUND MANAGER                   ***/
/*********************************************************/

BattleGroundMgr::BattleGroundMgr()
{
    m_BattleGrounds.clear();
}

BattleGroundMgr::~BattleGroundMgr()
{
    for(std::map<uint32, BattleGround*>::iterator itr = m_BattleGrounds.begin(); itr != m_BattleGrounds.end(); ++itr)
        delete itr->second;
    m_BattleGrounds.clear();
}

void BattleGroundMgr::Update(time_t diff)
{
    for(BattleGroundSet::iterator itr = m_BattleGrounds.begin(); itr != m_BattleGrounds.end(); ++itr)
        itr->second->Update(diff);
}

void BattleGroundMgr::BuildBattleGroundStatusPacket(WorldPacket *data, BattleGround *bg, uint32 team, uint8 QueueSlot, uint8 StatusID, uint32 Time1, uint32 Time2)
{
    // we can be in 3 queues in same time...
    if(StatusID == 0)
    {
        data->Initialize(SMSG_BATTLEFIELD_STATUS, 4*3);
        *data << uint32(QueueSlot);                         // queue id (0...2)
        *data << uint32(1);
        *data << uint32(2);
        return;
    }

    data->Initialize(SMSG_BATTLEFIELD_STATUS, (4+1+1+4+2+4+1+4+4+4));
    *data << uint32(QueueSlot);                             // queue id (0...2) - player can be in 3 queues in time
    *data << uint8(bg->GetArenaType());                     // team type (0=BG, 2=2x2, 3=3x3, 5=5x5), for arenas
    switch(bg->GetTypeID())                                 // value depends on bg id
    {
        case BATTLEGROUND_AV:
            *data << uint8(1);
            break;
        case BATTLEGROUND_WS:
            *data << uint8(2);
            break;
        case BATTLEGROUND_AB:
            *data << uint8(3);
            break;
        case BATTLEGROUND_NA:
            *data << uint8(4);
            break;
        case BATTLEGROUND_BE:
            *data << uint8(5);
            break;
        case BATTLEGROUND_AA:
            *data << uint8(6);
            break;
        case BATTLEGROUND_EY:
            *data << uint8(7);
            break;
        case BATTLEGROUND_RL:
            *data << uint8(8);
            break;
        default:                                            // unknown
            *data << uint8(0);
            break;
    }

    if(bg->isArena() && (StatusID == STATUS_WAIT_QUEUE))
        *data << uint32(BATTLEGROUND_AA);                   // all arenas
    else
        *data << uint32(bg->GetTypeID());                   // BG id from DBC

    *data << uint16(0x1F90);                                // unk value 8080
    *data << uint32(bg->GetInstanceID());                   // instance id

    if(bg->isBattleGround())
        *data << uint8(bg->GetTeamIndexByTeamId(team));     // team
    else
        *data << uint8(bg->isRated());                      // is rated battle

    *data << uint32(StatusID);                              // status
    switch(StatusID)
    {
        case STATUS_WAIT_QUEUE:                             // status_in_queue
            *data << uint32(Time1);                         // wait time, milliseconds
            *data << uint32(Time2);                         // time in queue, updated every minute?
            break;
        case STATUS_WAIT_JOIN:                              // status_invite
            *data << uint32(bg->GetMapId());                // map id
            *data << uint32(Time1);                         // time to remove from queue, milliseconds
            break;
        case STATUS_IN_PROGRESS:                            // status_in_progress
            *data << uint32(bg->GetMapId());                // map id
            *data << uint32(Time1);                         // 0 at bg start, 120000 after bg end, time to bg auto leave, milliseconds
            *data << uint32(Time2);                         // time from bg start, milliseconds
            *data << uint8(0x1);                            // unk sometimes 0x0!
            break;
        default:
            sLog.outError("Unknown BG status!");
            break;
    }
}

void BattleGroundMgr::BuildPvpLogDataPacket(WorldPacket *data, BattleGround *bg)
{
    uint8 type = (bg->isArena() ? 1 : 0);
                                                            // checked on 2.1.3
    data->Initialize(MSG_PVP_LOG_DATA, (1+1+4+40*bg->GetPlayerScoresSize()));
    *data << uint8(type);                                   // seems to be type (battleground=0/arena=1)
    if(type == 1)                                           // arena
    {
        for(uint8 i = 0; i < 2; i++)
        {
            *data << uint32(3000+1+i);                      // rating change: showed value - 3000
            *data << uint8(0);                              // string
        }
    }

    if(bg->GetWinner() < 2)                                 // we have winner
    {
        *data << uint8(1);                                  // bg ended
        *data << uint8(bg->GetWinner());                    // who win
    }
    else                                                    // no winner yet
    {
        *data << uint8(0);                                  // bg in progress
    }

    *data << (int32)(bg->GetPlayerScoresSize());

    for(std::map<uint64, BattleGroundScore*>::const_iterator itr = bg->GetPlayerScoresBegin(); itr != bg->GetPlayerScoresEnd(); ++itr)
    {
        *data << (uint64)itr->first;
        *data << (int32)itr->second->KillingBlows;
        if(type == 0)
        {
            *data << (int32)itr->second->HonorableKills;
            *data << (int32)itr->second->Deaths;
            *data << (int32)(itr->second->BonusHonor*10);
        }
        else
        {
            // that part probably wrong
            Player *plr = objmgr.GetPlayer(itr->first);
            if(plr)
            {
                if(plr->GetTeam() == HORDE)
                    *data << uint8(0);
                else if(plr->GetTeam() == ALLIANCE)
                    *data << uint8(1);
                else
                    *data << uint8(0);
            }
            else
                *data << uint8(0);
        }
        *data << (int32)itr->second->DamageDone;             // damage done
        *data << (int32)itr->second->HealingDone;            // healing done
        switch(bg->GetTypeID())                              // battleground specific things
        {
            case BATTLEGROUND_AV:
                *data << (uint32)0x00000005;                 // count of next fields
                *data << (uint32)((BattleGroundAVScore*)itr->second)->GraveyardsAssaulted;  // GraveyardsAssaulted
                *data << (uint32)((BattleGroundAVScore*)itr->second)->GraveyardsDefended;   // GraveyardsDefended
                *data << (uint32)((BattleGroundAVScore*)itr->second)->TowersAssaulted;      // TowersAssaulted
                *data << (uint32)((BattleGroundAVScore*)itr->second)->TowersDefended;       // TowersDefended
                *data << (uint32)((BattleGroundAVScore*)itr->second)->MinesCaptured;        // MinesCaptured
                break;
            case BATTLEGROUND_WS:
                *data << (uint32)0x00000002;                 // count of next fields
                *data << (uint32)((BattleGroundWGScore*)itr->second)->FlagCaptures;         // flag captures
                *data << (uint32)((BattleGroundWGScore*)itr->second)->FlagReturns;          // flag returns
                break;
            case BATTLEGROUND_AB:
                *data << (uint32)0x00000002;                 // count of next fields
                *data << (uint32)((BattleGroundABScore*)itr->second)->BasesAssaulted;       // bases asssulted
                *data << (uint32)((BattleGroundABScore*)itr->second)->BasesDefended;        // bases defended
                break;
            case BATTLEGROUND_NA:
            case BATTLEGROUND_BE:
            case BATTLEGROUND_AA:
            case BATTLEGROUND_RL:
                *data << (int32)0;                         // 0
                break;
            default:
                sLog.outDebug("Unhandled MSG_PVP_LOG_DATA for BG id %u", bg->GetTypeID());
                *data << (int32)0;
                break;
        }
    }
}

void BattleGroundMgr::BuildGroupJoinedBattlegroundPacket(WorldPacket *data, uint32 bgTypeId)
{
    /*bgTypeId is:
    0 - Your group has joined a battleground queue, but you are not iligible
    1 - Your group has joined the queue for AV
    2 - Your group has joined the queue for WS
    3 - Your group has joined the queue for AB
    4 - Your group has joined the queue for NA
    5 - Your group has joined the queue for BE Arena
    6 - Your group has joined the queue for All Arenas
    7 - Your group has joined the queue for EotS*/
    data->Initialize(SMSG_GROUP_JOINED_BATTLEGROUND, 4);
    *data << uint32(bgTypeId);
}

void BattleGroundMgr::BuildUpdateWorldStatePacket(WorldPacket *data, uint32 field, uint32 value)
{
    data->Initialize(SMSG_UPDATE_WORLD_STATE, 4+4);
    *data << uint32(field);
    *data << uint32(value);
}

void BattleGroundMgr::BuildPlaySoundPacket(WorldPacket *data, uint32 soundid)
{
    data->Initialize(SMSG_PLAY_SOUND, 4);
    *data << uint32(soundid);
}

void BattleGroundMgr::BuildPlayerLeftBattleGroundPacket(WorldPacket *data, Player *plr)
{
    data->Initialize(SMSG_BATTLEGROUND_PLAYER_LEFT, 8);
    *data << uint64(plr->GetGUID());
}

void BattleGroundMgr::BuildPlayerJoinedBattleGroundPacket(WorldPacket *data, Player *plr)
{
    data->Initialize(SMSG_BATTLEGROUND_PLAYER_JOINED, 8);
    *data << uint64(plr->GetGUID());
}

void BattleGroundMgr::InvitePlayer(Player* plr, uint32 bgInstanceGUID)
{
    // set invited player counters:
    BattleGround* bg = this->GetBattleGround(bgInstanceGUID);
    bg->IncreaseInvitedCount(plr->GetTeam());

    // create invite events:
    //add events to player's counters ---- this is not good way - there should be something like global event processor, where we should add those events
    BGQueueInviteEvent* inviteEvent = new BGQueueInviteEvent(plr->GetGUID(), bgInstanceGUID);
    plr->m_Events.AddEvent(inviteEvent, plr->m_Events.CalculateTime(BG_REMIND_INVITE_TIME));
    BGQueueRemoveEvent* removeEvent = new BGQueueRemoveEvent(plr->GetGUID(), bgInstanceGUID, plr->GetTeam());
    plr->m_Events.AddEvent(removeEvent, plr->m_Events.CalculateTime(2*BG_REMIND_INVITE_TIME));
}

uint32 BattleGroundMgr::CreateBattleGround(uint32 bgTypeId, uint32 MinPlayersPerTeam, uint32 MaxPlayersPerTeam, uint32 LevelMin, uint32 LevelMax, char* BattleGroundName, uint32 MapID, float Team1StartLocX, float Team1StartLocY, float Team1StartLocZ, float Team1StartLocO, float Team2StartLocX, float Team2StartLocY, float Team2StartLocZ, float Team2StartLocO)
{
    // Create the BG
    BattleGround *bg = NULL;

    switch(bgTypeId)
    {
        case BATTLEGROUND_AV: bg = new BattleGroundAV; break;
        case BATTLEGROUND_WS: bg = new BattleGroundWS; break;
        case BATTLEGROUND_AB: bg = new BattleGroundAB; break;
        case BATTLEGROUND_NA: bg = new BattleGroundNA; break;
        case BATTLEGROUND_BE: bg = new BattleGroundBE; break;
        case BATTLEGROUND_AA: bg = new BattleGroundAA; break;
        case BATTLEGROUND_EY: bg = new BattleGroundEY; break;
        case BATTLEGROUND_RL: bg = new BattleGroundRL; break;
        default:bg = new BattleGround;   break;             // placeholder for non implemented BG
    }

    bg->SetMapId(MapID);
    if(!bg->SetupBattleGround())
    {
        delete bg;
        return 0;
    }

    BattlemasterListEntry const *bl = sBattlemasterListStore.LookupEntry(bgTypeId);
    //in previous method is checked if exists entry in sBattlemasterListStore, so no check needed
    if (bl)
    {
        bg->SetArenaorBGType(bl->type == TYPE_ARENA);
    }

    if(bg->isArena())
        bg->SetArenaType(ARENA_TYPE_2v2);                   // 2x2
    else
        bg->SetArenaType(0);                                // battleground

    bg->SetTypeID(bgTypeId);
    bg->SetInstanceID(bgTypeId);                               // temporary
    bg->SetMinPlayersPerTeam(MinPlayersPerTeam);
    bg->SetMaxPlayersPerTeam(MaxPlayersPerTeam);
    bg->SetMinPlayers(MinPlayersPerTeam*2);
    bg->SetMaxPlayers(MaxPlayersPerTeam*2);
    bg->SetName(BattleGroundName);
    bg->SetTeamStartLoc(ALLIANCE, Team1StartLocX, Team1StartLocY, Team1StartLocZ, Team1StartLocO);
    bg->SetTeamStartLoc(HORDE,    Team2StartLocX, Team2StartLocY, Team2StartLocZ, Team2StartLocO);
    bg->SetLevelRange(LevelMin, LevelMax);
    //add BaggleGround instance to FreeSlotQueue
    bg->AddToBGFreeSlotQueue();

    AddBattleGround(bg->GetInstanceID(), bg);
    //sLog.outDetail("BattleGroundMgr: Created new battleground: %u %s (Map %u, %u players per team, Levels %u-%u)", bg_TypeID, bg->m_Name, bg->m_MapId, bg->m_MaxPlayersPerTeam, bg->m_LevelMin, bg->m_LevelMax);
    return bg->GetInstanceID();
}

void BattleGroundMgr::CreateInitialBattleGrounds()
{
    float AStartLoc[4];
    float HStartLoc[4];
    uint32 MaxPlayersPerTeam, MinPlayersPerTeam, MinLvl, MaxLvl, start1, start2;
    BattlemasterListEntry const *bl;
    WorldSafeLocsEntry const *start;

    uint32 count = 0;

    //                                            0     1                   2                   3        4        5                  6                7               8
    QueryResult *result = WorldDatabase.Query("SELECT `id`, `MinPlayersPerTeam`,`MaxPlayersPerTeam`,`MinLvl`,`MaxLvl`,`AllianceStartLoc`,`AllianceStartO`,`HordeStartLoc`,`HordeStartO` FROM `battleground_template`");

    if(!result)
    {
        barGoLink bar(1);

        bar.step();

        sLog.outString();
        sLog.outErrorDb(">> Loaded 0 battlegrounds. DB table `battleground_template` is empty.");
        return;
    }

    barGoLink bar(result->GetRowCount());

    do
    {
        Field *fields = result->Fetch();
        bar.step();

        uint32 bgTypeID = fields[0].GetUInt32();

        // can be overwrited by values from DB
        bl = sBattlemasterListStore.LookupEntry(bgTypeID);
        if(!bl)
        {
            sLog.outError("Battleground ID %u not found in BattlemasterList.dbc. Battleground not created.",bgTypeID);
            continue;
        }

        MaxPlayersPerTeam = bl->maxplayersperteam;
        MinPlayersPerTeam = bl->maxplayersperteam/2;
        MinLvl = bl->minlvl;
        MaxLvl = bl->maxlvl;

        if(fields[1].GetUInt32())
            MinPlayersPerTeam = fields[1].GetUInt32();

        if(fields[2].GetUInt32())
            MaxPlayersPerTeam = fields[2].GetUInt32();

        if(fields[3].GetUInt32())
            MinLvl = fields[3].GetUInt32();

        if(fields[4].GetUInt32())
            MaxLvl = fields[4].GetUInt32();

        start1 = fields[5].GetUInt32();

        start = sWorldSafeLocsStore.LookupEntry(start1);
        if(start)
        {
            AStartLoc[0] = start->x;
            AStartLoc[1] = start->y;
            AStartLoc[2] = start->z;
            AStartLoc[3] = fields[6].GetFloat();
        }
        else if(bgTypeID == BATTLEGROUND_AA)
        {
            AStartLoc[0] = 0;
            AStartLoc[1] = 0;
            AStartLoc[2] = 0;
            AStartLoc[3] = fields[6].GetFloat();
        }
        else
        {
            sLog.outErrorDb("Table `battleground_template` for id %u have non-existed WorldSafeLocs.dbc id %u in field `AllianceStartLoc`. BG not created.",bgTypeID,start1);
            continue;
        }

        start2 = fields[7].GetUInt32();

        start = sWorldSafeLocsStore.LookupEntry(start2);
        if(start)
        {
            HStartLoc[0] = start->x;
            HStartLoc[1] = start->y;
            HStartLoc[2] = start->z;
            HStartLoc[3] = fields[8].GetFloat();
        }
        else if(bgTypeID == BATTLEGROUND_AA)
        {
            HStartLoc[0] = 0;
            HStartLoc[1] = 0;
            HStartLoc[2] = 0;
            HStartLoc[3] = fields[8].GetFloat();
        }
        else
        {
            sLog.outErrorDb("Table `battleground_template` for id %u have non-existed WorldSafeLocs.dbc id %u in field `HordeStartLoc`. BG not created.",bgTypeID,start2);
            continue;
        }

        //sLog.outDetail("Creating battleground %s, %u-%u", bl->name[sWorld.GetDBClang()], MinLvl, MaxLvl);
        if(!CreateBattleGround(bgTypeID, MinPlayersPerTeam, MaxPlayersPerTeam, MinLvl, MaxLvl, bl->name[sWorld.GetDBClang()], bl->mapid[0], AStartLoc[0], AStartLoc[1], AStartLoc[2], AStartLoc[3], HStartLoc[0], HStartLoc[1], HStartLoc[2], HStartLoc[3]))
            continue;

        count++;
    } while (result->NextRow());

    delete result;

    sLog.outString();
    sLog.outString( ">> Loaded %u battlegrounds", count );
}

void BattleGroundMgr::BuildBattleGroundListPacket(WorldPacket *data, uint64 guid, Player* plr, uint32 bgTypeId)
{
    uint32 PlayerLevel = 10;

    if(plr)
        PlayerLevel = plr->getLevel();

    data->Initialize(SMSG_BATTLEFIELD_LIST);
    *data << uint64(guid);                                  // battlemaster guid
    *data << uint32(bgTypeId);                              // battleground id
    if(bgTypeId == BATTLEGROUND_AA)                         // arena
    {
        *data << uint8(5);                                  // unk
        *data << uint32(0);                                 // unk
    }
    else                                                    // battleground
    {
        *data << uint8(0x00);                               // unk

        size_t count_pos = data->wpos();
        uint32 count = 0;
        *data << uint32(0x00);                              // number of bg instances

        for(std::map<uint32, BattleGround*>::iterator itr = m_BattleGrounds.begin(); itr != m_BattleGrounds.end(); ++itr)
        {
            if(itr->second->GetTypeID() == bgTypeId && (PlayerLevel >= itr->second->GetMinLevel()) && (PlayerLevel <= itr->second->GetMaxLevel()))
            {
                *data << uint32(itr->second->GetInstanceID());
                count++;
            }
        }
        data->put<uint32>( count_pos , count);
    }
}

void BattleGroundMgr::SendToBattleGround(Player *pl, uint32 bgTypeId)
{
    BattleGround *bg = GetBattleGround(bgTypeId);
    if(bg)
    {
        uint32 mapid = bg->GetMapId();
        float x, y, z, O;
        bg->GetTeamStartLoc(pl->GetTeam(), x, y, z, O);

        sLog.outDetail("BATTLEGROUND: Sending %s to %f, %f, %f, %f", pl->GetName(), x, y, z, O);
        pl->TeleportTo(mapid, x, y, z, O);
    }
}

void BattleGroundMgr::SendAreaSpiritHealerQueryOpcode(Player *pl, BattleGround *bg, uint64 guid)
{
    WorldPacket data(SMSG_AREA_SPIRIT_HEALER_TIME, 12);
    uint32 time_ = 30000 - bg->GetLastResurrectTime(); // resurrect every 30 seconds
    if(time_ == uint32(-1))
        time_ = 0;
    data << guid << time_;
    pl->GetSession()->SendPacket(&data);
}
