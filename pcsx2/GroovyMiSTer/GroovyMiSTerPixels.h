// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Config.h"
#include "common/Pcsx2Defs.h"

#include <vector>

namespace GroovyMiSTer
{
	/// Bytes per pixel on the wire.
	constexpr u32 BytesPerPixel(GroovyMiSTerRgbMode mode)
	{
		switch (mode)
		{
			case GroovyMiSTerRgbMode::RGB565: return 2;
			case GroovyMiSTerRgbMode::RGBA8888: return 4;
			default: return 3;
		}
	}

	/// Convert a PCSX2 readback (RGBA8: byte 0 = R, 1 = G, 2 = B, 3 = A) into the MiSTer's
	/// wire layout, which is NOT what its format names suggest:
	///
	///     RGB888   -> B, G, R
	///     RGBA8888 -> B, G, R, A   (4th byte ignored by the core)
	///     RGB565   -> little-endian u16, (r << 11) | (g << 5) | b
	///
	/// Derived from the FPGA RTL (Groovy.sv, decode_pixel), which unpacks a pixel as
	/// `{r,g,b} <= word64[0 +: 24]`. That is a Verilog concatenation, so `b` occupies the
	/// least-significant byte - and since DDR is little-endian, stream byte 0 becomes blue.
	/// This matches what RPCS3 sends from its BGRA source, which is hardware-validated.
	///
	/// PCSX2's source is RGBA, so EVERY mode needs a channel swap; none of them is a memcpy.
	/// Get this wrong and the picture comes out red/blue swapped - which still looks like a
	/// working stream, so it is exactly the kind of bug that survives a casual eyeball test.
	/// Covered by tests/ctest/core/groovy_mister_tests.cpp.
	///
	/// `src_pitch` is in bytes and may exceed width * 4 (download textures are often padded).
	void PackFrame(GroovyMiSTerRgbMode mode, const u8* src, u32 src_pitch, u32 width, u32 height,
		std::vector<u8>& dst);
} // namespace GroovyMiSTer
