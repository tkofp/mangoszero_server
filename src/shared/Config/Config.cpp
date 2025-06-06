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

#include "Config.h"
#include <ace/Configuration_Import_Export.h>

#include "Policies/Singleton.h"

INSTANTIATE_SINGLETON_1(Config);

static bool GetValueHelper(ACE_Configuration_Heap* mConf, const char* name, ACE_TString& result)
{
    if (!mConf)
    {
        return false;
    }

    ACE_TString section_name;
    ACE_Configuration_Section_Key section_key;
    ACE_Configuration_Section_Key root_key = mConf->root_section();

    int i = 0;
    while (mConf->enumerate_sections(root_key, i, section_name) == 0)
    {
        mConf->open_section(root_key, section_name.c_str(), 0, section_key);
        if (mConf->get_string_value(section_key, name, result) == 0)
        {
            return true;
        }
        ++i;
    }

    return false;
}

Config::Config()
    : mConf(NULL)
{
}

Config::~Config()
{
    delete mConf;
}

bool Config::SetSource(const char* file)
{
    mFilename = file;

    return Reload();
}

bool Config::Reload()
{
    delete mConf;
    mConf = new ACE_Configuration_Heap;

    if (mConf->open() == 0)
    {
        ACE_Ini_ImpExp config_importer(*mConf);
        if (config_importer.import_config(mFilename.c_str()) == 0)
        {
            return true;
        }
    }

    delete mConf;
    mConf = NULL;
    return false;
}

std::string Config::GetStringDefault(const char* name, const char* def)
{
    ACE_TString val;
    return GetValueHelper(mConf, name, val) ? val.c_str() : def;
}

bool Config::GetBoolDefault(const char* name, bool def)
{
    ACE_TString val;
    if (!GetValueHelper(mConf, name, val))
    {
        return def;
    }

    const char* str = val.c_str();
    if (strcmp(str, "true") == 0 || strcmp(str, "TRUE") == 0 ||
        strcmp(str, "yes") == 0 || strcmp(str, "YES") == 0 ||
        strcmp(str, "1") == 0)
    {
        return true;
    }
    else
    {
        return false;
    }
}

int32 Config::GetIntDefault(const char* name, int32 def)
{
    ACE_TString val;
    return GetValueHelper(mConf, name, val) ? atoi(val.c_str()) : def;
}

float Config::GetFloatDefault(const char* name, float def)
{
    ACE_TString val;
    return GetValueHelper(mConf, name, val) ? (float)atof(val.c_str()) : def;
}
