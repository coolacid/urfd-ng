#ifndef SW_MODES_ONLY
/*
 *   Copyright (C) 2014 by Jonathan Naylor G4KLX and John Hays K7VE
 *   Copyright 2016 by Jeremy McDermond (NH6Z)
 *   Copyright 2021 by Thomas Early N7TAE
 *
 *   Adapted by K7VE from G4KLX dv3000d
 */

// tcd - a hybid transcoder using DVSI hardware and Codec2 software
// Copyright © 2022-2023 Thomas A. Early N7TAE

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <sys/select.h>
#include <csignal>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <cerrno>
#include <thread>

#include "DVSIDevice.h"
#include "Configure.h"

#include "TcdStats.h"

extern CConfigure g_Conf;

using DWORD = unsigned long;
using ULONG = unsigned long;
CDVDevice::CDVDevice(Encoding t) : type(t), buffer_depth(0), keep_running(true)
{
}

CDVDevice::~CDVDevice()
{
	CloseDevice();
}

void CDVDevice::CloseDevice()
{
	input_queue.Shutdown();
	keep_running = false;
	if (device)
	{
		device->Close();
	}

	if (feedFuture.valid())
		feedFuture.get();
	if (readFuture.valid())
		readFuture.get();
}

void CDVDevice::FTDI_Error(const char *where, int status) const
{
	std::cerr << "Device ERROR: " << where << ": " << status << std::endl;
}

bool CDVDevice::checkResponse(SDV_Packet &p, uint8_t response) const
{
	if(p.start_byte != PKT_HEADER || p.header.packet_type != PKT_CONTROL || p.field_id != response)
		return true;

	return false;
}

uint8_t CDVDevice::MapPacketToChannel(const std::shared_ptr<CTranscoderPacket> &packet) const
{
	// Default mapping: no specific channel mapping provided by base class
	return 0xFF;
}

bool CDVDevice::OpenDevice(const std::string &serialno, const std::string &desc, Edvtype dvtype, int8_t in_gain, int8_t out_gain)
{
	// create POSIX device instance
	device.reset(CreatePosixDevice());
	if (!device)
	{
		std::cerr << "Failed to create POSIX device" << std::endl;
		return true;
	}

	std::string outDesc;
	if (!device->Open(serialno, outDesc))
	{
		FTDI_Error("Open", errno);
		return true;
	}

	// purge buffers
	device->Purge();

	// set 8N1
	if (!device->SetDataCharacteristics(8, 1, 0))
	{
		FTDI_Error("SetDataCharacteristics", errno);
		return true;
	}

	// request RTS
	if (!device->SetRTS(true))
	{
		FTDI_Error("SetRTS", errno);
		return true;
	}

	if (std::string::npos == description.find("DF2ET"))
	{
		// for usb-3012 pull DTR high to take AMBE3003 out of reset
		if (!device->SetDTR(false))
		{
			FTDI_Error("ClrDTR", errno);
			return true;
		}
	}
	else
	{
		if (!device->SetDTR(true))
		{
			FTDI_Error("SetDTR", errno);
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	ULONG baudrate = (Edvtype::dv3000 == dvtype) ? 460800 : 921600;
	if (!device->SetBaudRate(baudrate))
	{
		FTDI_Error("SetBaudRate", errno);
		return false;
	}

	description.assign(desc);
	description.append(" ");
	description.append(serialno);

	std::cout << "Opened " << description << " at " << baudrate << " baud" << std::endl;

	if (InitDevice())
		return true;

	const uint8_t limit = (Edvtype::dv3000 == dvtype) ? PKT_CHANNEL0 : PKT_CHANNEL2;
	for (uint8_t ch=PKT_CHANNEL0; ch<=limit; ch++)
	{
		if (ConfigureVocoder(ch, type, in_gain, out_gain))
			return true;
	}
	return false;
}

bool CDVDevice::InitDevice()
{
	SDV_Packet responsePacket, ctrlPacket;

	// ********** soft reset *************
	ctrlPacket.start_byte = PKT_HEADER;
	ctrlPacket.header.payload_length = htons(3);
	ctrlPacket.header.packet_type = PKT_CONTROL;
	ctrlPacket.field_id = PKT_RESET;
	ctrlPacket.payload.ctrl.data.paritymode[0] = PKT_PARITYBYTE;
	ctrlPacket.payload.ctrl.data.paritymode[1] = 0x3U ^ PKT_RESET ^ PKT_PARITYBYTE;
	ssize_t written = device->Write(&ctrlPacket, 7);
	if (written < 0)
	{
		FTDI_Error("Error writing soft reset packet", errno);
		return true;
	}
	else if (7 != written)
	{
		std::cerr << "Incomplete soft reset packet write" << std::endl;
		return true;
	}

	if (GetResponse(responsePacket))
	{
		std::cerr << "Error receiving response to reset" << std::endl;
		return true;
	}

	if (checkResponse(responsePacket, PKT_READY))
	{
	   std::cerr << "Invalid response to soft reset" << std::endl;
	   dump("Soft Reset Response Packet:", &responsePacket, packet_size(responsePacket));
	   return true;
	}
	std::cout << "Successfully did a soft reset on " << description << std::endl;

	// ********** turn off parity *********
	ctrlPacket.start_byte = PKT_HEADER;
	ctrlPacket.header.payload_length = htons(4);
	ctrlPacket.header.packet_type = PKT_CONTROL;
	ctrlPacket.field_id = PKT_PARITYMODE;
	ctrlPacket.payload.ctrl.data.paritymode[0] = 0;
	ctrlPacket.payload.ctrl.data.paritymode[1] = PKT_PARITYBYTE;
	ctrlPacket.payload.ctrl.data.paritymode[2] = 0x4U ^ PKT_PARITYMODE ^ PKT_PARITYBYTE;
	written = device->Write(&ctrlPacket, 8);
	if (written < 0)
	{
		FTDI_Error("Error writing parity control packet", errno);
		return true;
	}
	else if (8 != written)
	{
		std::cerr << "Incomplete disable parity packet write" << std::endl;
		return true;
	}

	if (GetResponse(responsePacket))
	{
		std::cerr << "Error receiving response to parity set" << std::endl;
		return true;
	}

	if (checkResponse(responsePacket, PKT_PARITYMODE))
	{
		std::cerr << "Invalid response to parity control" << std::endl;
		dump("Parity Ctrl Response Packet:", &responsePacket, packet_size(responsePacket));
		return true;
	}

	std::cout << "Successfully disabled parity on " << description << std::endl;

	// ********* Product ID and Version *************
	ctrlPacket.start_byte = PKT_HEADER;
	ctrlPacket.header.payload_length = htons(1);
	ctrlPacket.header.packet_type = PKT_CONTROL;
	ctrlPacket.field_id = PKT_PRODID;

	written = device->Write(&ctrlPacket, 5);
	if (written < 0)
	{
		FTDI_Error("Error writing Product ID packet", errno);
		return true;
	}
	else if (5 != written)
	{
		std::cerr << "Incomplete Product ID Packet write" << std::endl;
		return true;
	}

	if (GetResponse(responsePacket))
	{
		std::cerr << "Error receiving response to Product ID request" << std::endl;
		return true;
	}

	if (checkResponse(responsePacket, PKT_PRODID))
	{
	   std::cerr << "Invalid response to Product ID query" << std::endl;
	   dump("Product ID Response Packet", &responsePacket, packet_size(responsePacket));
	   return true;
	}
	productid.assign(responsePacket.payload.ctrl.data.prodid);

	ctrlPacket.field_id = PKT_VERSTRING;
	written = device->Write(&ctrlPacket, 5);
	if (written < 0)
	{
		FTDI_Error("Error writing Version packet", errno);
		return true;
	}
	else if (5 != written)
	{
		std::cerr << "Incomplete Version packet write" << std::endl;
		return true;
	}

	if (GetResponse(responsePacket))
	{
		std::cerr << "Error receiving response to Version request" << std::endl;
		return true;
	}

	if (checkResponse(responsePacket, PKT_VERSTRING))
	{
	   std::cerr << "Invalid response to Version query" << std::endl;
	   dump("Product Version Response Packet:", &responsePacket, packet_size(responsePacket));
	   return true;
	}
	const std::string version(responsePacket.payload.ctrl.data.version);
	std::cout << description << ": ID=" <<  productid << " Version=" << version << std::endl;

	return false;
}

void CDVDevice::Start()
{
	feedFuture = std::async(std::launch::async, &CDVDevice::FeedDevice, this);
	readFuture = std::async(std::launch::async, &CDVDevice::ReadDevice, this);
}

bool CDVDevice::ConfigureVocoder(uint8_t pkt_ch, Encoding type, int8_t in_gain, int8_t out_gain)
{
	SDV_Packet controlPacket, responsePacket;
	const uint8_t ecmode[] { PKT_ECMODE, 0x0, 0x0 };
	const uint8_t dcmode[] { PKT_DCMODE, 0x0, 0x0 };
	const uint8_t  dstar[] { PKT_RATEP, 0x01U, 0x30U, 0x07U, 0x63U, 0x40U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x48U };
	const uint8_t    dmr[] { PKT_RATEP, 0x04U, 0x31U, 0x07U, 0x54U, 0x24U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x6FU, 0x48U };
	const uint8_t  chfmt[] { PKT_CHANFMT, 0x0, 0x0 };
	const uint8_t  spfmt[] { PKT_SPCHFMT, 0x0, 0x0 };
	const uint8_t   gain[] { PKT_GAIN, uint8_t(in_gain), uint8_t(out_gain) };
	const uint8_t   init[] { PKT_INIT, 0x3U };
	const uint8_t   resp[] { 0x0, PKT_ECMODE, 0x0, PKT_DCMODE, 0x0, PKT_RATEP, 0x0, PKT_CHANFMT, 0x0, PKT_SPCHFMT, 0x0, PKT_GAIN, 0x0, PKT_INIT, 0x0 };


	controlPacket.start_byte = PKT_HEADER;
	controlPacket.header.payload_length = htons(1 + sizeof(SDV_Packet::payload.codec));
	controlPacket.header.packet_type = PKT_CONTROL;
	controlPacket.field_id = pkt_ch;
	memcpy(controlPacket.payload.codec.ecmode, ecmode, 3);
	memcpy(controlPacket.payload.codec.dcmode, dcmode, 3);
	if (type == Encoding::dstar)
	{
		memcpy(controlPacket.payload.codec.ratep, dstar, 13);
	}
	else
	{
		memcpy(controlPacket.payload.codec.ratep, dmr, 13);
	}
	memcpy(controlPacket.payload.codec.chfmt, chfmt, 3);
	memcpy(controlPacket.payload.codec.spfmt, spfmt, 3);
	memcpy(controlPacket.payload.codec.gain, gain, 3);
	memcpy(controlPacket.payload.codec.init, init, 2);

	// write packet
	const DWORD size = packet_size(controlPacket);
	ssize_t written = device->Write(&controlPacket, size);
	if (written < 0)
	{
		FTDI_Error("error writing codec config packet", errno);
		return true;
	}
	else if ((DWORD)size != (DWORD)written)
	{
		std::cerr << "Incomplete Configuration packet write" << std::endl;
		return true;
	}

	if (GetResponse(responsePacket))
	{
		std::cerr << "Error reading Configuration response packet" << std::endl;
		return true;
	}

	if ((ntohs(responsePacket.header.payload_length) != 16) || (responsePacket.field_id != pkt_ch) || (0 != memcmp(responsePacket.payload.ctrl.data.resp, resp, sizeof(resp))))
	{
		std::cerr << "Config response packet failed" << std::endl;
		dump("Configuration Response Packet:", &responsePacket, packet_size(responsePacket));
		return true;
	};
	;

	std::cout << description << " channel " << (unsigned int)(pkt_ch - PKT_CHANNEL0) << " is now configured for " << ((Encoding::dstar == type) ? "D-Star" : "DMR/YSF") << std::endl;

	return false;
}

bool CDVDevice::GetResponse(SDV_Packet &packet)
{
	ssize_t r;

	// get the start byte (block until found)
	for (unsigned i = 0U; i < USB3XXX_MAXPACKETSIZE+2; ++i) {
		r = device->Read(&packet.start_byte, 1);
		if (r < 0)
		{
			FTDI_Error("Reading packet start byte", errno);
			return true;
		}
		if (r == 0)
		{
			// timeout, no data yet - continue waiting
			continue;
		}

		if (packet.start_byte == PKT_HEADER)
			break;
	}

	if (packet.start_byte != PKT_HEADER) {
		std::cerr << "Couldn't find start byte!" << std::endl;
		return true;
	}

	// get the packet size and type (three bytes)
	size_t bytesLeft = sizeof(packet.header);
	char *hptr = reinterpret_cast<char*>(&packet.header);
	while (bytesLeft > 0) {
		r = device->Read(hptr, bytesLeft);
		if (r < 0)
		{
			FTDI_Error("Error reading response packet header", errno);
			return true;
		}
		if (r == 0)
		{
			// timeout waiting for header bytes; retry
			continue;
		}
		bytesLeft -= (size_t)r;
		hptr += r;
	}

	bytesLeft = ntohs(packet.header.payload_length);
	if (bytesLeft > 1 + int(sizeof(packet.payload))) {
		std::cout << "AMBEserver: Serial payload exceeds buffer size: " << int(bytesLeft) << std::endl;
		return true;
	}

	char *pptr = reinterpret_cast<char*>(&packet.field_id);
	while (bytesLeft > 0) {
		r = device->Read(pptr, bytesLeft);
		if (r < 0)
		{
			FTDI_Error("Error reading packet payload", errno);
			return true;
		}
		if (r == 0)
		{
			// timeout waiting for payload bytes; retry
			continue;
		}

		bytesLeft -= (size_t)r;
		pptr += r;
	}

	return false;
}

void CDVDevice::AddPacket(const std::shared_ptr<CTranscoderPacket> packet)
{
	auto size = input_queue.push(packet);
	if (size > 200)
	{
		std::cerr << ((type==Encoding::dstar) ? "DStar" : "DMR/YSF") << " inQ size is overflowing! Shutting down..." << std::endl;
		raise(SIGINT);
	}
}

void CDVDevice::dump(const char *title, const void *pointer, int length) const
{
	const uint8_t *data = (const uint8_t *)pointer;

	std::cout << title << std::endl;

	unsigned int offset = 0U;

	while (length > 0) {

		unsigned int bytes = (length > 16) ? 16U : length;

		for (unsigned i = 0U; i < bytes; i++) {
			if (i)
				std::cout << " ";
			std::cout << std::hex << std::setw(2) << std::right << std::setfill('0') << int(data[offset + i]);
		}

		for (unsigned int i = bytes; i < 16U; i++)
			std::cout << "   ";

		std::cout << "   *";

		for (unsigned i = 0U; i < bytes; i++) {
			uint8_t c = data[offset + i];

			if (::isprint(c))
				std::cout << c;
			else
				std::cout << '.';
		}

		std::cout << '*' << std::endl;

		offset += 16U;

		if (length >= 16)
			length -= 16;
		else
			length = 0;
	}
}

void CDVDevice::FeedDevice()
{
	const std::string modules(g_Conf.GetTCMods());
	const auto n = modules.size();
	while (keep_running)
	{
		auto packet = input_queue.pop();	// blocks until there is something to pop, unless shutting down

		if (packet)
		{
			while (keep_running)	// wait until there is room
			{
				if (buffer_depth < 2)
					break;

				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}

			if (keep_running)
			{
				auto index = modules.find(packet->GetModule());
				// save the packet in the vocoder's queue while the vocoder does its magic
				if (std::string::npos == index)
				{
					std::cerr << "Module '" << packet->GetModule() << "' is not configured on " << description << std::endl;
				}
				else
				{
					PushWaitingPacket(index, packet);

					const bool needs_audio = (Encoding::dstar==type) ? packet->DStarIsSet() : packet->DMRIsSet();

					if (needs_audio)
					{
						SendData(index, (Encoding::dstar==type) ? packet->GetDStarData() : packet->GetDMRData());
					}
					else
					{
						SendAudio(index, packet->GetAudioSamples());
					}
					buffer_depth++;
				}
			}
		}
	}
}

void CDVDevice::ReadDevice()
{
	while (keep_running)
	{
		SDV_Packet p;
		if (! GetResponse(p))
		{
			ProcessPacket(p);
		}
	}
}
#endif
