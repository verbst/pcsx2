// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

namespace GroovyMiSTer
{
	/// Decides WHEN a keepalive datagram goes on the wire. It does no I/O - the caller
	/// does the sending - so the timing contract below is unit-testable, which the socket
	/// path itself is not.
	///
	/// WHY THIS EXISTS
	///
	/// The Groovy core arms an idle timer at CMD_INIT and ends the session if *nothing*
	/// arrives on the video socket within it (OSD: Server -> Idle timeout, 5s default,
	/// or 10/15/Off). That is the safety net for a client that crashed - but PCSX2 is also
	/// silent whenever it is alive and not blitting: paused, loading a savestate, swapping
	/// discs, or holding off because the current video mode produced no acceptable
	/// modeline. Without a keepalive, pausing drops the MiSTer back to its
	/// connection-search screen and the CRT goes with it.
	class KeepAliveScheduler
	{
	public:
		/// Send once our last outbound datagram is this old.
		///
		/// The core's shortest timeout is 5s, and the rule is to send at <= timeout/2:
		/// UDP has no retransmit, so halving is what lets us lose one keepalive and still
		/// hold the session.
		static constexpr u64 IDLE_THRESHOLD_MS = 2000;

		/// How often the caller is expected to ask. This is NOT the send interval, and the
		/// difference is the trap:
		///
		///     worst-case silence = idle threshold + poll period
		///
		/// because a poll landing just under the threshold defers the send by a whole
		/// period. Polling at the same rate you send turns a 2s threshold into a ~4s worst
		/// case - inside a 5s timeout only by luck, and gone the moment a datagram drops.
		/// So we poll well under the threshold; the poll itself is a comparison, and it
		/// sends nothing while we are blitting.
		static constexpr u64 POLL_PERIOD_MS = 250;

		static_assert(IDLE_THRESHOLD_MS + POLL_PERIOD_MS <= 2500,
			"worst-case silence must stay within half of the core's 5s idle timeout");
		static_assert(2 * (IDLE_THRESHOLD_MS + POLL_PERIOD_MS) < 5000,
			"one lost keepalive must still leave us inside the core's 5s idle timeout");

		/// Call after EVERY outbound datagram - blit and audio alike, not just keepalives.
		///
		/// Gating on real wire activity rather than on a free-running heartbeat is what
		/// makes sending during normal play structurally impossible instead of merely
		/// unlikely: at 60fps this is refreshed every ~16ms, so the threshold is never
		/// reached and the keepalive costs exactly nothing on the hot path.
		void NotifyWireActivity(u64 now_ms) { m_last_wire_ms = now_ms; }

		/// True when the session has been silent long enough to need a keepalive.
		bool ShouldSend(u64 now_ms) const
		{
			// Defensive against a non-monotonic or rewound clock: treat "the future" as
			// fresh activity rather than underflowing the subtraction into a huge age.
			return now_ms >= m_last_wire_ms && (now_ms - m_last_wire_ms) >= IDLE_THRESHOLD_MS;
		}

		/// Milliseconds since the last outbound datagram (0 if the clock went backwards).
		u64 SilenceMs(u64 now_ms) const
		{
			return (now_ms >= m_last_wire_ms) ? (now_ms - m_last_wire_ms) : 0;
		}

		/// Re-arm from a known-quiet baseline - call when a session starts, so a fresh
		/// connection does not immediately look stale.
		void Reset(u64 now_ms) { m_last_wire_ms = now_ms; }

	private:
		u64 m_last_wire_ms = 0;
	};
} // namespace GroovyMiSTer
