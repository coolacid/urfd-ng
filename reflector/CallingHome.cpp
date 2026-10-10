// CallingHome -- registers the reflector in the XLX directory
//
// Copyright (C) 2026 Jens-Christian Merg (DL4JC)
// Licensed under the GNU General Public License v3 or later

#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <random>
#include <regex>
#include <sstream>
#include <sys/stat.h>

#include "CallingHome.h"
#include "CurlGet.h"
#include "Global.h"

// the dashboard's file, so both use the same identity in the directory
#define CALLINGHOME_HASHFILE "/var/lib/xlxd-ch/callinghome.php"
#define CALLINGHOME_SERVER   "http://xlxapi.rlx.lu/api.php"

static std::string XmlEscape(const std::string &in)
{
	std::string out;
	for (const char c : in)
	{
		switch (c)
		{
			case '&': out += "&amp;"; break;
			case '<': out += "&lt;"; break;
			case '>': out += "&gt;"; break;
			default:  out += c; break;
		}
	}
	return out;
}

void CCallingHome::Start(void)
{
	if (!g_Configure.GetBoolean(g_Keys.callinghome.enable))
		return;

	// the directory knows URF reflectors by their XLX name, as the dashboard reports it
	m_Name = "XLX" + g_Configure.GetString(g_Keys.names.callsign).substr(3, 3);
	m_Url = g_Configure.GetString(g_Keys.names.url);
	m_Country = g_Configure.GetString(g_Keys.callinghome.country);
	m_Comment = g_Configure.GetString(g_Keys.callinghome.comment);
	m_OverrideIP = g_Configure.GetString(g_Keys.callinghome.overrideip);
	m_Server = g_Configure.GetString(g_Keys.callinghome.server);
	if (m_Server.empty())
		m_Server = CALLINGHOME_SERVER;
	if (g_Configure.Contains(g_Keys.callinghome.pushdelay))
		m_PushDelay = g_Configure.GetUnsigned(g_Keys.callinghome.pushdelay);
	if (g_Configure.Contains(g_Keys.callinghome.timeout))
		m_Timeout = g_Configure.GetUnsigned(g_Keys.callinghome.timeout);
	m_HashFile = g_Configure.GetString(g_Keys.callinghome.hashfile);
	if (m_HashFile.empty())
		m_HashFile = CALLINGHOME_HASHFILE;
	m_Hash.clear();
	m_Reached = false;
	if (LoadOrCreateHash())
		return;

	m_StartTime = std::chrono::steady_clock::now();
	m_Run = true;
	try
	{
		m_Future = std::async(std::launch::async, &CCallingHome::Thread, this);
	}
	catch (const std::exception &e)
	{
		m_Run = false;
		std::cerr << "CallingHome: cannot start its thread: " << e.what() << std::endl;
	}
}

void CCallingHome::Stop(void)
{
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Run = false;
	}
	m_Wake.notify_all();
	if (m_Future.valid())
		m_Future.get();
}

// The hash identifies the reflector in the directory, which ties the name to it.
// Reuse the dashboard's, create one only if there is no file at all, and never
// replace a file that exists but can't be read.
bool CCallingHome::LoadOrCreateHash(void)
{
	std::ifstream in(m_HashFile);
	if (in)
	{
		std::stringstream content;
		content << in.rdbuf();
		std::smatch m;
		const std::string s(content.str());
		if (std::regex_search(s, m, std::regex("\\$Hash\\s*=\\s*\"([A-Za-z0-9]+)\"")))
		{
			m_Hash = m[1];
			std::cout << "CallingHome: using the hash from " << m_HashFile << std::endl;
			return false;
		}
		std::cerr << "CallingHome: no $Hash in " << m_HashFile << ", not registering (fix or remove the file)" << std::endl;
		return true;
	}

	struct stat st;
	if (0 == stat(m_HashFile.c_str(), &st))
	{
		std::cerr << "CallingHome: cannot read " << m_HashFile << ", not registering" << std::endl;
		return true;
	}

	// same alphabet and length as the dashboard's CreateCode(16)
	static const char chars[] = "1234567890abcdefghijklmnopqrstuvwyxzABCDEFGHIJKLMNAOPQRSTUVWYXZ";
	std::random_device rd;
	std::uniform_int_distribution<size_t> pick(0, sizeof(chars) - 2);
	for (int i = 0; i < 16; i++)
		m_Hash += chars[pick(rd)];

	// The directory ties the name to the first hash it sees, so only register
	// with a hash that will still be there after a restart.
	if (WriteHashFile())
	{
		std::cerr << "CallingHome: cannot save a new hash to " << m_HashFile << ", not registering" << std::endl;
		return true;
	}
	std::cout << "CallingHome: created a new hash in " << m_HashFile << std::endl;
	return false;
}

// The dashboard's format. Written once, to a temporary file renamed over the
// target, so a crash can never leave the reflector's identity half written.
bool CCallingHome::WriteHashFile(void) const
{
	const std::string tmp = m_HashFile + ".tmp";
	{
		std::ofstream out(tmp, std::ios::trunc);
		out << "<?php\n\n$LastSync = " << std::time(nullptr) << ";\n$Hash     = \"" << m_Hash << "\";\n\n?>";
		out.flush();
		if (!out)
		{
			std::remove(tmp.c_str());
			return true;
		}
	}
	chmod(tmp.c_str(), 0644);
	if (0 != std::rename(tmp.c_str(), m_HashFile.c_str()))
	{
		std::remove(tmp.c_str());
		return true;
	}
	return false;
}

std::string CCallingHome::BuildXml(void) const
{
	const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - m_StartTime).count();
	std::stringstream version;
	version << g_Version;

	std::stringstream xml;
	xml << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<query>CallingHome</query>\n<reflector>\n"
	    << "   <name>" << m_Name << "</name>\n"
	    << "   <uptime>" << uptime << "</uptime>\n"
	    << "   <hash>" << m_Hash << "</hash>\n"
	    << "   <url>" << XmlEscape(m_Url) << "</url>\n"
	    << "   <country>" << XmlEscape(m_Country) << "</country>\n"
	    << "   <comment>" << XmlEscape(m_Comment) << "</comment>\n"
	    << "   <ip>" << XmlEscape(m_OverrideIP) << "</ip>\n"
	    << "   <reflectorversion>" << version.str() << "</reflectorversion>\n"
	    << "</reflector>\n<interlinks>";

	auto ilmap = g_GateKeeper.GetInterlinkMap();
	for (auto it = ilmap->begin(); it != ilmap->end(); it++)
	{
		std::string address;
#ifndef NO_DHT
		if (it->second.UsesDHT())
			address = it->second.GetIPv4();
		else
#endif
			address = it->second.GetIp().GetAddress();
		xml << "\n   <interlink>\n"
		    << "      <name>" << XmlEscape(it->first) << "</name>\n"
		    << "      <address>" << address << "</address>\n"
		    << "      <modules>" << it->second.GetModules() << "</modules>\n"
		    << "   </interlink>";
	}
	g_GateKeeper.ReleaseInterlinkMap();
	xml << "\n</interlinks>";
	return xml.str();
}

// true when the directory took it
bool CCallingHome::Push(void)
{
	CCurlGet curl;
	std::stringstream answer;
	long http = 0;
	const CURLcode code = curl.PostForm(m_Server, "xml", BuildXml(), answer, http, m_Timeout);
	if (CURLE_OK != code || http < 200 || http >= 300)
	{
		std::cerr << "CallingHome: " << m_Server << " did not take the update ("
		          << ((CURLE_OK != code) ? curl_easy_strerror(code) : "HTTP " + std::to_string(http)) << ")" << std::endl;
		return false;
	}
	return true;
}

void CCallingHome::Thread(void)
{
	while (m_Run)
	{
		const bool reached = Push();
		if (reached && !m_Reached)
			std::cout << "CallingHome: registered as " << m_Name << " in the XLX directory, updating every " << m_PushDelay << " s" << std::endl;
		m_Reached = reached;

		std::unique_lock<std::mutex> lock(m_Mutex);
		m_Wake.wait_for(lock, std::chrono::seconds(m_PushDelay), [this] { return !m_Run; });
	}
}
