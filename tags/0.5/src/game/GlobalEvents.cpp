/*
 * Copyright (C) 2005,2006 MaNGOS <http://www.mangosproject.org/>
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

#include "Log.h"
#include "Database/DatabaseEnv.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "EventSystem.h"
#include "GlobalEvents.h"
#include "ObjectDefines.h"
#include "Common.h"

static void CorpsesErase(CorpseType type,uint32 delay)
{
    QueryResult *result = sDatabase.PQuery("SELECT `guid`,`position_x`,`position_y`,`map` FROM `corpse` WHERE UNIX_TIMESTAMP()-UNIX_TIMESTAMP(`time`) > '%u' AND `bones_flag` = '%u'",delay,type );

    if(result)
    {
        do
        {
            Field *fields = result->Fetch();
            uint32 guidlow = fields[0].GetUInt32();
            float positionX = fields[1].GetFloat();
            float positionY = fields[2].GetFloat();
            //float positionZ = fields[4].GetFloat();
            //float ort       = fields[5].GetFloat();
            uint32 mapid    = fields[3].GetUInt32();

            uint64 guid = MAKE_GUID(guidlow,HIGHGUID_CORPSE);

            sLog.outDebug("[Global event] Removing %s %u (X:%f Y:%f Map:%u).",(type==CORPSE_BONES?"bones":"corpse"),guidlow,positionX,positionY,mapid);

            // not load grid if grid not loaded for corpse/bones removing
            if(!MapManager::Instance().GetMap(mapid)->IsRemovalGrid(positionX,positionY))
            {
                // convert corpse to bones
                if(type==CORPSE_RESURRECTABLE)
                {
                    Corpse *corpse = ObjectAccessor::Instance().GetCorpse(positionX,positionY,mapid,guid);
                    if(corpse)
                        corpse->ConvertCorpseToBones();
                    else
                    {
                        sLog.outDebug("Corpse %u not found in world. Delete from DB.",guidlow);
                        sDatabase.PExecute("DELETE FROM `corpse` WHERE `guid` = '%u'",guidlow);
                        sDatabase.PExecute("DELETE FROM `corpse_grid` WHERE `guid` = '%u'",guidlow);
                    }
                }
                // delete  bones
                else
                {
                    // not load grid if grid not loaded for bones removing
                    Corpse *corpse = ObjectAccessor::Instance().GetCorpse(positionX,positionY,mapid,guid);
                    if(corpse)
                        corpse->DeleteFromWorld(true);
                    else
                        sLog.outDebug("Bones %u not found in world. Delete from DB also.",guidlow);

                    sDatabase.PExecute("DELETE FROM `corpse` WHERE `guid` = '%u'",guidlow);
                    sDatabase.PExecute("DELETE FROM `corpse_grid` WHERE `guid` = '%u'",guidlow);
                }
            }
            else
            {
                // remove corpse/bones from DB in any case
                sDatabase.PExecute("DELETE FROM `corpse` WHERE `guid` = '%u'",guidlow);
                sDatabase.PExecute("DELETE FROM `corpse_grid` WHERE `guid` = '%u'",guidlow);
            }
        } while (result->NextRow());

        delete result;
    }
}

void CorpsesErase()
{
    //sLog.outBasic("Global Event (corpses/bones removal)");
    CorpsesErase(CORPSE_BONES,             20*60);          // 20 mins
    CorpsesErase(CORPSE_RESURRECTABLE,3*24*60*60);          // 3 days
}

void HandleCorpsesErase(void*)
{
    sDatabase.ThreadStart();                                // let thread do safe mySQL requests

    CorpsesErase();

    sDatabase.ThreadEnd();                                  // free mySQL thread resources
}
