// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <atomic>
#include <mutex>
#include <vector>

namespace GroovyMiSTer
{
	/// Bridges SPU2's mixed output to the Groovy sender thread.
	///
	/// Why a bridge at all: the Groovy video/audio socket may only be driven by the sender
	/// thread (the RIO send path is not thread-safe, and cross-thread sends get silently
	/// dropped), but audio is mixed on the SPU2 thread. So SPU2 deposits samples here and
	/// the sender drains them.
	///
	/// Producer: SPU2 thread (Write). Consumer: sender thread (Read). Exactly one of each.
	///
	/// Overflow policy is DROP-OLDEST. If the sender stalls, we would rather throw away
	/// stale audio than let latency grow without bound - audio that is seconds behind the
	/// picture is worse than a dropout, and dropping old samples lets the stream
	/// self-correct instead of drifting forever.
	class AudioTap
	{
	public:
		/// PS2 mixes at 48kHz stereo; PS1 mode is 44.1kHz. Either way, 2 channels of s16.
		static constexpr u32 CHANNELS = 2;
		static constexpr u32 BYTES_PER_SAMPLE = sizeof(s16) * CHANNELS;

		/// ~256KB is about 1.3 seconds at 48kHz stereo - far more headroom than we ever
		/// want to use, but it means a brief sender hiccup costs nothing.
		static constexpr size_t CAPACITY = 256 * 1024;

		void Reset();

		void SetActive(bool active) { m_active.store(active, std::memory_order_release); }
		bool IsActive() const { return m_active.load(std::memory_order_acquire); }

		/// SPU2 thread. `samples` is interleaved stereo float (SPU2's own chunk buffer),
		/// `frames` the number of stereo frames. Converts to interleaved s16.
		void Write(const float* samples, u32 frames);

		/// Sender thread. Copies up to `max_bytes` into `dst`; returns bytes written.
		/// Always returns a whole number of stereo frames.
		u32 Read(u8* dst, u32 max_bytes);

		/// Diagnostics.
		u64 GetDroppedBytes() const { return m_dropped_bytes.load(std::memory_order_relaxed); }

	private:
		std::mutex m_lock;
		std::vector<u8> m_buffer;
		size_t m_read_pos = 0;
		size_t m_size = 0; // bytes currently queued

		std::atomic_bool m_active{false};
		std::atomic<u64> m_dropped_bytes{0};
	};
} // namespace GroovyMiSTer
