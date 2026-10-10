//
//  Copyright © 2020 Thomas A. Early, N7TAE
//
// ----------------------------------------------------------------------------
//
//    m17ref is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    m17ref is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    with this software.  If not, see <http://www.gnu.org/licenses/>.
// ----------------------------------------------------------------------------

#include <arpa/inet.h>

#include "M17Packet.h"

CM17Packet::CM17Packet(const uint8_t *buf)
{
	memcpy(m17.magic, buf, sizeof(SM17Frame));

	destination.CodeIn(m17.lich.addr_dst);
	source.CodeIn(m17.lich.addr_src);
}

const CCallsign &CM17Packet::GetDestCallsign() const
{
	return destination;
}

const CCallsign &CM17Packet::GetSourceCallsign() const
{
	return source;
}

char CM17Packet::GetDestModule() const
{
	return destination.GetCSModule();
}

uint16_t CM17Packet::GetFrameNumber() const
{
	return ntohs(m17.framenumber);
}

uint16_t CM17Packet::GetFrameType() const
{
	return ntohs(m17.lich.frametype);
}

// The TYPE field comes in two layouts. The V#3 layout has the payload type in the
// top nibble (never zero), the legacy layout leaves that nibble empty.
//   legacy: bit 0 stream, bits 1-2 data type (2 = 3200, 3 = 1600), bits 3-4 encryption
//   V#3:    bits 12-15 payload (2 = 3200, 3 = 1600), bits 9-11 encryption

ECodecType CM17Packet::GetCodecType() const
{
	const uint16_t t = GetFrameType();
	const unsigned payload = (0xF000U & t) ? (t >> 12) : (0x3U & (t >> 1));
	switch (payload)
	{
		case 2U:
			return ECodecType::c2_3200;
		case 3U:
			return ECodecType::c2_1600;
		default:
			return ECodecType::none;
	}
}

bool CM17Packet::IsEncrypted() const
{
	const uint16_t t = GetFrameType();
	return (0xF000U & t) ? (0 != (0x7U & (t >> 9))) : (0 != (0x3U & (t >> 3)));
}

const uint8_t *CM17Packet::GetPayload() const
{
	return m17.payload;
}

const uint8_t *CM17Packet::GetNonce() const
{
	return m17.lich.nonce;
}

void CM17Packet::SetPayload(const uint8_t *newpayload)
{
	memcpy(m17.payload, newpayload, 16);
}

uint16_t CM17Packet::GetStreamId() const
{
	return ntohs(m17.streamid);
}

uint16_t CM17Packet::GetCRC() const
{
	return ntohs(m17.crc);
}

void CM17Packet::SetCRC(uint16_t crc)
{
	m17.crc = htons(crc);
}

bool CM17Packet::IsLastPacket() const
{
	return ((0x8000u & ntohs(m17.framenumber)) == 0x8000u);
}
