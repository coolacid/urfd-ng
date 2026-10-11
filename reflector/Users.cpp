//  Copyright © 2015 Jean-Luc Deltombe (LX3JL). All rights reserved.

// urfd -- The universal reflector
// Copyright © 2021 Thomas A. Early N7TAE
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.


#include <cmath>
#include "Users.h"
#include "Global.h"

////////////////////////////////////////////////////////////////////////////////////////
// constructor

CUsers::CUsers() {}

////////////////////////////////////////////////////////////////////////////////////////
// users management

#define LASTHEARD_USERS_MAX_SIZE 20
void CUsers::AddUser(const CUser &user)
{
	// add
	m_Users.push_front(user);

	// if list size too big, remove oldest
	if ( m_Users.size() >= (LASTHEARD_USERS_MAX_SIZE-1) )
	{
		m_Users.resize(m_Users.size()-1);
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// operation

void CUsers::Hearing(const CCallsign &my, const CCallsign &rpt1, const CCallsign &rpt2, const std::string &proto)
{
	Hearing(my, rpt1, rpt2, g_Reflector.GetCallsign(), proto);
}

void CUsers::Hearing(const CCallsign &my, const CCallsign &rpt1, const CCallsign &rpt2, const CCallsign &xlx, const std::string &proto)
{
	CUser heard(my, rpt1, rpt2, xlx, proto);

	// a known position stays with the callsign until it reports a new one
	for ( auto &user : m_Users )
	{
		if ( user.IsCallsign(my) )
		{
			heard.SetLocation(user);
			break;
		}
	}

	// first check if this user is already listed. If so, erase him.
	for ( auto it=begin(); it!=end(); it++ )
	{
		if (*it == heard)
		{
			m_Users.erase(it);
			break;
		}
	}

	AddUser(heard);
}

void CUsers::Location(const CCallsign &my, double lat, double lon)
{
	if ( (lat < -90.0) || (lat >= 90.0) || (lon < -180.0) || (lon >= 180.0) )
		return;
	const auto loc = CUser::Locator(lat, lon);
	// the dashboard, XML and JSON are public: publish the middle of the
	// locator's field (about 4.6 x 2.3 km), not where the station is
	lat = (std::floor((lat + 90.0) * 24.0) + 0.5) / 24.0 - 90.0;
	lon = (std::floor((lon + 180.0) * 12.0) + 0.5) / 12.0 - 180.0;
	// the most recent entry of this callsign
	for ( auto &user : m_Users )
	{
		if ( user.IsCallsign(my) )
		{
			user.SetLocation(loc, lat, lon);
			break;
		}
	}
}
