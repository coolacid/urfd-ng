// DMREmbeddedLC -- the link control carried in the embedded signalling of DMR
// voice bursts B to E, one LC per superframe, and the talker alias LCs
//
// The encoding follows CDMREmbeddedData of MMDVMHost by Jonathan Naylor G4KLX
// (GPL v2 or later).
//
// Copyright (C) 2026 Jens-Christian Merg (DL4JC)
// Licensed under the GNU General Public License v3 or later

#include <cstring>

#include "DMREmbeddedLC.h"
#include "Hamming.h"
#include "QR1676.h"

// full link control opcodes of the talker alias (ETSI TS 102 361-2 7.1.1)
#define FLCO_TALKER_ALIAS_HEADER 0x04U
#define TA_FORMAT_ISO8           1U

void CDmrEmbeddedLC::Encode(const uint8_t *lc)
{
	bool bits[72];
	for (unsigned i = 0; i < 72U; i++)
		bits[i] = (lc[i / 8U] >> (7U - i % 8U)) & 0x01U;

	// 5 bit checksum: the sum of the 9 bytes modulo 31
	unsigned crc = 0;
	for (unsigned i = 0; i < 9U; i++)
		crc += lc[i];
	crc %= 31U;

	bool data[128];
	memset(data, 0, sizeof(data));
	data[106] = crc & 0x01U;
	data[90]  = crc & 0x02U;
	data[74]  = crc & 0x04U;
	data[58]  = crc & 0x08U;
	data[42]  = crc & 0x10U;

	// 11 bits in the first two rows, 10 in the others
	unsigned b = 0;
	for (unsigned a = 0; a < 11U; a++)
		data[a] = bits[b++];
	for (unsigned a = 16; a < 27U; a++)
		data[a] = bits[b++];
	for (unsigned row = 32; row < 112U; row += 16U)
		for (unsigned a = row; a < row + 10U; a++)
			data[a] = bits[b++];

	// Hamming (16,11,4) on every row but the last, which is column parity
	for (unsigned a = 0; a < 112U; a += 16U)
		CHamming::encode16114(data + a);
	for (unsigned a = 0; a < 16U; a++)
		data[a + 112U] = data[a] ^ data[a + 16U] ^ data[a + 32U] ^ data[a + 48U] ^ data[a + 64U] ^ data[a + 80U] ^ data[a + 96U];

	// sent column by column
	b = 0;
	for (unsigned a = 0; a < 128U; a++)
	{
		m_Raw[a] = data[b];
		b += 16U;
		if (b > 127U)
			b -= 127U;
	}
}

uint8_t CDmrEmbeddedLC::Fragment(unsigned n, uint8_t *frame) const
{
	if (n < 1U || n > 4U)
		return 0U;
	n--;

	// 32 bits after 4 bits of EMB, ending 4 bits before the rest of it
	bool bits[40];
	memset(bits, 0, sizeof(bits));
	memcpy(bits + 4, m_Raw + n * 32U, 32U * sizeof(bool));
	uint8_t bytes[5];
	for (unsigned i = 0; i < 5U; i++)
	{
		bytes[i] = 0;
		for (unsigned j = 0; j < 8U; j++)
			bytes[i] = (bytes[i] << 1) | (bits[i * 8U + j] ? 1U : 0U);
	}
	frame[14] = (frame[14] & 0xF0U) | (bytes[0] & 0x0FU);
	frame[15] = bytes[1];
	frame[16] = bytes[2];
	frame[17] = bytes[3];
	frame[18] = (frame[18] & 0x0FU) | (bytes[4] & 0xF0U);

	// LCSS: first fragment, continuation, last fragment
	return (0U == n) ? 1U : ((3U == n) ? 2U : 3U);
}

bool CDmrEmbeddedLC::Collect(const uint8_t *sync, uint8_t *lc)
{
	// sync holds frame bytes 13-19: EMB in the outer nibbles, embedded bits between
	const uint8_t emb[2] = { uint8_t((sync[0] << 4) | (sync[1] >> 4)), uint8_t((sync[5] << 4) | (sync[6] >> 4)) };
	const uint8_t lcss = (CQR1676::decode(emb) >> 1) & 0x03U;
	bool bits[32];
	for (unsigned i = 0; i < 32U; i++)
	{
		const unsigned bit = 12U + i;	// after the 4 bits of sync[0] and 8 of EMB
		bits[i] = (sync[bit / 8U] >> (7U - bit % 8U)) & 0x01U;
	}

	switch (lcss)
	{
		case 1U:	// first fragment
			memcpy(m_In, bits, sizeof(bits));
			m_InCount = 1;
			return false;
		case 3U:	// continuation
			if (1U != m_InCount && 2U != m_InCount)
			{
				m_InCount = 0;
				return false;
			}
			memcpy(m_In + 32U * m_InCount++, bits, sizeof(bits));
			return false;
		case 2U:	// last fragment
			if (3U != m_InCount)
			{
				m_InCount = 0;
				return false;
			}
			memcpy(m_In + 96U, bits, sizeof(bits));
			m_InCount = 0;
			break;
		default:	// single fragment burst, no LC in it
			m_InCount = 0;
			return false;
	}

	// back into rows, check Hamming and column parity, then the 5 bit checksum
	bool data[128];
	unsigned b = 0;
	for (unsigned a = 0; a < 128U; a++)
	{
		data[b] = m_In[a];
		b += 16U;
		if (b > 127U)
			b -= 127U;
	}
	for (unsigned a = 0; a < 112U; a += 16U)
		if (!CHamming::decode16114(data + a))
			return false;
	for (unsigned a = 0; a < 16U; a++)
		if (data[a] ^ data[a + 16U] ^ data[a + 32U] ^ data[a + 48U] ^ data[a + 64U] ^ data[a + 80U] ^ data[a + 96U] ^ data[a + 112U])
			return false;

	bool out[72];
	b = 0;
	for (unsigned a = 0; a < 11U; a++)
		out[b++] = data[a];
	for (unsigned a = 16; a < 27U; a++)
		out[b++] = data[a];
	for (unsigned row = 32; row < 112U; row += 16U)
		for (unsigned a = row; a < row + 10U; a++)
			out[b++] = data[a];
	const unsigned crc = (data[42] ? 16U : 0U) + (data[58] ? 8U : 0U) + (data[74] ? 4U : 0U) + (data[90] ? 2U : 0U) + (data[106] ? 1U : 0U);

	memset(lc, 0, 9);
	unsigned sum = 0;
	for (unsigned i = 0; i < 72U; i++)
		if (out[i])
			lc[i / 8U] |= 0x80U >> (i % 8U);
	for (unsigned i = 0; i < 9U; i++)
		sum += lc[i];
	return (sum % 31U) == crc;
}

std::vector<std::array<uint8_t, 9>> CDmrEmbeddedLC::TalkerAlias(const std::string &utf8)
{
	// to ISO 8859-1, what the radios show; anything beyond it becomes '?'
	std::string text;
	for (size_t i = 0; i < utf8.size() && text.size() < 27U; i++)
	{
		const uint8_t c = utf8[i];
		if (c < 0x80U)
		{
			if (c >= 0x20U)
				text += char(c);
		}
		else if ((0xC2U == c || 0xC3U == c) && i + 1 < utf8.size() && 0x80U == (0xC0U & uint8_t(utf8[i + 1])))
			text += char(((c & 0x1FU) << 6) | (uint8_t(utf8[++i]) & 0x3FU));
		else if (0xC0U == (0xC0U & c))
		{
			text += '?';
			while (i + 1 < utf8.size() && 0x80U == (0xC0U & uint8_t(utf8[i + 1])))
				i++;
		}
	}

	std::vector<std::array<uint8_t, 9>> lcs;
	if (text.empty())
		return lcs;

	// header: format, length, then 6 characters; blocks 1-3: 7 characters each
	std::array<uint8_t, 9> lc {};
	lc[0] = FLCO_TALKER_ALIAS_HEADER;
	lc[2] = uint8_t((TA_FORMAT_ISO8 << 6) | (text.size() << 1));
	for (size_t i = 0; i < 6U && i < text.size(); i++)
		lc[3 + i] = uint8_t(text[i]);
	lcs.push_back(lc);
	for (size_t pos = 6; pos < text.size(); pos += 7U)
	{
		lc.fill(0);
		lc[0] = uint8_t(FLCO_TALKER_ALIAS_HEADER + lcs.size());
		for (size_t i = 0; i < 7U && pos + i < text.size(); i++)
			lc[2 + i] = uint8_t(text[pos + i]);
		lcs.push_back(lc);
	}
	return lcs;
}
