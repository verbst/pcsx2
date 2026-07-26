// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

class GSTexture;
class GSVector4i;

// =====================================================================================
//  GroovyMiSTer - low-latency streaming output to a MiSTer FPGA
// =====================================================================================
//
// Streams the finished PS2 frame and the mixed SPU2 audio to a MiSTer over UDP, and
// reads MiSTer-side controllers back in. The point of the whole feature is latency: the
// MiSTer "raster-chases" the CRT, so a frame arrives microseconds before it is scanned
// out. A single saved frame is worth real effort here.
//
// THIS HEADER IS THE ENTIRE SEAM INTO UPSTREAM PCSX2.
//
// Everything else lives under pcsx2/GroovyMiSTer/ and pcsx2/Input/. Upstream files call
// only the functions below, each in a one- or two-line hunk, so that rebasing onto a new
// PCSX2 release stays cheap. Please keep it that way: if you find yourself wanting to
// reach into upstream code from the module, add a function here instead.
//
// Current call sites:
//   GS/GS.cpp                          -> Open() / Close()
//   GS/Renderers/Common/GSRenderer.cpp -> IsActive() / OnVSync()
//   SPU2/spu2.cpp                      -> IsAudioActive() / OnAudioChunk()
//   Input/InputManager.cpp             -> registers GroovyMiSTerInputSource
//
namespace GroovyMiSTer
{
	/// Bring the output up if EmuCore/GroovyMiSTer/Enabled is set. Safe to call when
	/// disabled (does nothing) and safe to call twice. Called on the GS thread from
	/// GSopen(); connection failures are non-fatal and retried in the background, so a
	/// missing/asleep MiSTer never blocks the emulator from starting.
	void Open();

	/// Tear down: stop the sender, tell the MiSTer we are going away (so it returns to its
	/// "waiting for connection" screen instead of freezing on our last frame), and release
	/// GPU resources. Called on the GS thread from GSclose().
	void Close();

	/// True when we are streaming. This is the guard on every hot-path hook, so it must
	/// stay cheap - a single relaxed atomic load.
	bool IsActive();

	/// Hand over the finished frame. Called on the GS thread from GSRenderer::VSync(),
	/// straight after Merge() and BEFORE presentation, so our GPU work and network send
	/// never queue up behind the host window's present.
	///
	/// `current`  the merged frame (g_gs_device->GetCurrent()).
	/// `src_rect` the VALID sub-rectangle of `current`, from CalculateDrawSrcRect().
	///            `current` can be larger than the video mode - a game may flip a
	///            render-target surface bigger than what it actually displays, and the
	///            host window only blits this sub-rect. Capturing the whole texture
	///            streams the stale/aux region and shows up as a duplicated, offset
	///            image (RPCS3 hit exactly this in Gran Turismo). Always pass the rect.
	/// `field`    the interlace field for this vsync (0/1).
	void OnVSync(GSTexture* current, const GSVector4i& src_rect, u32 field);

	/// True when audio should be mirrored to the MiSTer. Guard for OnAudioChunk().
	bool IsAudioActive();

	/// Hand over one chunk of mixed audio. Called on the SPU2 thread from spu2Output(),
	/// with the same buffer SPU2 gives its own host audio backend: `frames` interleaved
	/// stereo float samples. Cheap - it converts to s16 and drops them in a ring; the
	/// sender thread owns the socket and does the sending.
	void OnAudioChunk(const float* samples, u32 frames);

	/// The console sample rate changed (PS2 48kHz <-> PS1 44.1kHz). The rate is baked into
	/// the Groovy CMD_INIT handshake, so this forces a reconnect. Called from
	/// SPU2::UpdateSampleRate().
	void OnSampleRateChanged();
} // namespace GroovyMiSTer
