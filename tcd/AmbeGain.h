#pragma once

// AMBE2+ bitstream gain adjustment via b2 (delta-gamma) parameter manipulation.
// Modifies the gain field directly in the 72-bit (9-byte) AMBE2+ frame without
// requiring vocoder decode/re-encode. Uses Golay(24,12) FEC for the A partition
// and Golay(23,12) plus the PRNG scrambling for the B partition.
//
// b2 is a 5-bit field (0-31) at voice bit positions [8,9,10,11,36].
// Bits 8-11 are in the Golay-protected A partition, bit 36 is in the C partition.
// b2 indexes the AmbeDg[] delta-gamma table; reducing b2 lowers frame loudness.
//
// References:
//   - mbelib ambe3600x2450.c (b2 extraction and AmbeDg table)
//   - ETSI TS 102 361 (DMR air interface)
//   - xlxd/ambed (CODECGAIN_AMBE2PLUS = +10 dB)

#include <cstdint>
#include <cmath>
#include "Golay24128.h"
#include "YSFDefines.h"	// DMR_A/B/C_TABLE (72-bit AMBE2+ partitions) and PRNG_TABLE

#define AMBE_READ_BIT(p,i)     (((p)[(i)>>3] & BIT_MASK_TABLE[(i)&7]) != 0)
#define AMBE_WRITE_BIT(p,i,b)  (p)[(i)>>3] = (b) ? ((p)[(i)>>3] | BIT_MASK_TABLE[(i)&7]) : ((p)[(i)>>3] & ~BIT_MASK_TABLE[(i)&7])

// Adjust the b2 (delta-gamma) gain parameter in a 9-byte AMBE2+ frame.
// step_offset > 0 reduces gain (quieter), < 0 increases gain (louder).
// The A partition is Golay(24,12) protected; FEC is recomputed after modification.
// The B partition is Golay(23,12) protected and scrambled with PRNG_TABLE[u0],
// u0 being the A partition's data; as b2 lives in u0, B is unscrambled with the
// old u0 and scrambled again with the new one, else the decoder loses B.
static inline void AmbeAdjustGain(uint8_t *ambe, int step_offset)
{
	if (step_offset == 0) return;

	// 1. De-interleave A (24 bits) and B (23 bits) partitions from the 72-bit frame
	unsigned int a_bits = 0;
	for (int i = 0; i < 24; i++)
	{
		a_bits <<= 1;
		if (AMBE_READ_BIT(ambe, DMR_A_TABLE[i]))
			a_bits |= 1;
	}
	unsigned int b_bits = 0;
	for (int i = 0; i < 23; i++)
	{
		b_bits <<= 1;
		if (AMBE_READ_BIT(ambe, DMR_B_TABLE[i]))
			b_bits |= 1;
	}

	// 2. Golay decode: extract 12 data bits (with error correction)
	unsigned int data12 = CGolay24128::decode24128(a_bits);

	// 3. Unscramble and decode B with the original u0
	unsigned int b_data = CGolay24128::decode23127(b_bits ^ (PRNG_TABLE[data12] >> 1));

	// 4. Extract b2 upper 4 bits from data bits 8-11
	//    data12 bit numbering: bit 11 = MSB, bit 0 = LSB
	//    Voice bits 0-11 map to data12 bits 11..0
	//    Voice bit 8 = data12 bit 3, voice bit 9 = data12 bit 2,
	//    voice bit 10 = data12 bit 1, voice bit 11 = data12 bit 0
	unsigned int b2_upper = (data12 & 0x0F);  // bits 3..0 = voice bits 8..11

	// 5. Read b2 LSB from C partition bit 12 (voice bit 36 = 24+12)
	unsigned int b2_lsb = AMBE_READ_BIT(ambe, DMR_C_TABLE[12]) ? 1 : 0;

	// 6. Reconstruct b2: upper4 << 1 | lsb
	int b2 = (int)((b2_upper << 1) | b2_lsb);

	// 7. Apply offset and clamp
	b2 -= step_offset;
	if (b2 < 0) b2 = 0;
	if (b2 > 31) b2 = 31;

	// 8. Write back b2 upper 4 bits into data12
	data12 = (data12 & 0xFF0) | ((unsigned int)b2 >> 1);

	// 9. Golay re-encode A, re-encode B and scramble it with the new u0
	unsigned int a_new = CGolay24128::encode24128(data12);
	unsigned int b_new = (CGolay24128::encode23127(b_data) >> 1) ^ (PRNG_TABLE[data12] >> 1);

	// 10. Re-interleave A and B partitions back into the 72-bit frame
	for (int i = 0; i < 24; i++)
	{
		AMBE_WRITE_BIT(ambe, DMR_A_TABLE[i], (a_new >> (23 - i)) & 1);
	}
	for (int i = 0; i < 23; i++)
	{
		AMBE_WRITE_BIT(ambe, DMR_B_TABLE[i], (b_new >> (22 - i)) & 1);
	}

	// 11. Write b2 LSB back to C partition
	AMBE_WRITE_BIT(ambe, DMR_C_TABLE[12], b2 & 1);
}

#undef AMBE_READ_BIT
#undef AMBE_WRITE_BIT
