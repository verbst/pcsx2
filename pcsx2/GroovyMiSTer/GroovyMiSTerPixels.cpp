// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GroovyMiSTer/GroovyMiSTerPixels.h"

#include <cstring>

namespace GroovyMiSTer
{
	void PackFrame(GroovyMiSTerRgbMode mode, const u8* src, u32 src_pitch, u32 width, u32 height,
		std::vector<u8>& dst)
	{
		dst.resize(static_cast<size_t>(width) * height * BytesPerPixel(mode));
		u8* out = dst.data();

		switch (mode)
		{
			case GroovyMiSTerRgbMode::RGB565:
			{
				for (u32 y = 0; y < height; y++)
				{
					const u8* in = src + static_cast<size_t>(y) * src_pitch;
					for (u32 x = 0; x < width; x++, in += 4, out += 2)
					{
						const u16 px = static_cast<u16>(
							((in[0] & 0xF8) << 8) | ((in[1] & 0xFC) << 3) | (in[2] >> 3));
						std::memcpy(out, &px, sizeof(px));
					}
				}
				break;
			}

			case GroovyMiSTerRgbMode::RGBA8888:
			{
				for (u32 y = 0; y < height; y++)
				{
					const u8* in = src + static_cast<size_t>(y) * src_pitch;
					for (u32 x = 0; x < width; x++, in += 4, out += 4)
					{
						out[0] = in[2]; // B
						out[1] = in[1]; // G
						out[2] = in[0]; // R
						out[3] = in[3]; // A (ignored by the core, but the slot exists)
					}
				}
				break;
			}

			default: // RGB888
			{
				for (u32 y = 0; y < height; y++)
				{
					const u8* in = src + static_cast<size_t>(y) * src_pitch;
					for (u32 x = 0; x < width; x++, in += 4, out += 3)
					{
						out[0] = in[2]; // B
						out[1] = in[1]; // G
						out[2] = in[0]; // R
					}
				}
				break;
			}
		}
	}
} // namespace GroovyMiSTer
