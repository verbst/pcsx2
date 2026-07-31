// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GroovyMiSTer/GroovyMiSTerOutput.h"
#include "GroovyMiSTer/GroovyMiSTer.h"

#include "GS/GS.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "Host.h"
#include "SPU2/spu2.h"

#include "common/Console.h"
#include "common/Threading.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include "groovymister_wrapper.h"
#include "switchres_wrapper.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>

// =====================================================================================
//  Protocol constants, pinned.
// =====================================================================================
// These values go straight onto the wire in CMD_INIT. The vendored client is tracking an
// actively-developed branch (Groovy_MiSTer proto/nlc-B), and the codec model has already
// been reshaped once mid-development. If a re-sync renumbers any of these, we want a
// compile error here rather than a garbled picture on someone's CRT.
static_assert(static_cast<int>(GroovyMiSTerCodec::Raw) == LZ4_OFF);
static_assert(static_cast<int>(GroovyMiSTerCodec::LZ4) == LZ4);
static_assert(static_cast<int>(GroovyMiSTerCodec::LZ4HC) == LZ4_HC);
static_assert(static_cast<int>(GroovyMiSTerRgbMode::RGB888) == RGB_888);
static_assert(static_cast<int>(GroovyMiSTerRgbMode::RGBA8888) == RGB_A888);
static_assert(static_cast<int>(GroovyMiSTerRgbMode::RGB565) == RGB_565);
static_assert(static_cast<int>(GroovyMiSTerNlcPack::Tiled) == 1, "NLC_PACK_TILED");
static_assert(static_cast<int>(GroovyMiSTerNlcPack::Rice) == 2, "NLC_PACK_RICE");
// Codec id 7 = NLC. It is deliberately NOT in the vendored Lz4FramesCode enum any more
// (pack became a separate knob), so it cannot be cross-checked against a symbol.
static_assert(static_cast<int>(GroovyMiSTerCodec::NLC) == 7, "Groovy NLC codec id");

namespace GroovyMiSTer
{
	namespace
	{
		std::unique_ptr<Output> s_output;

		void GmwLogSink(const char* msg)
		{
			if (msg && *msg)
				Console.WriteLn(fmt::format("[MiSTer] {}", msg));
		}

		void SrLogSink(const char* fmt_str, ...)
		{
			// switchres logs printf-style. Keep it short; this is only modeline maths.
			char buf[512];
			va_list ap;
			va_start(ap, fmt_str);
			std::vsnprintf(buf, sizeof(buf), fmt_str, ap);
			va_end(ap);
			Console.WriteLn(fmt::format("[MiSTer/switchres] {}", buf));
		}

		u32 SoundRateCodeFor(u32 hz)
		{
			switch (hz)
			{
				case 22050: return RATE_22050;
				case 44100: return RATE_44100;
				case 48000: return RATE_48000;
				default: return RATE_OFF;
			}
		}

		// The tri-sync arcade monitor (15/25/31 kHz) - the safe default and the correct
		// match for the reference hardware. A compiled-in switchres preset, no ini needed.
		constexpr const char* DEFAULT_MONITOR = "arcade_15_25_31";

		// Every compiled-in switchres monitor preset (3rdparty/switchres/monitor.cpp,
		// monitor_set_preset). An unknown name silently falls back to generic_15 (15 kHz only)
		// inside switchres, which would refuse every 31 kHz mode, so we validate against this
		// list rather than trust the string. "custom" is handled separately (it takes its
		// timings from the Switchres INI). MUST stay in sync with the dropdown in
		// pcsx2-qt/Settings/GroovyMiSTerSettingsWidget.cpp (s_monitor_values).
		bool IsKnownMonitorPreset(const std::string& p)
		{
			static const char* const kKnown[] = {
				// Arcade multi-sync / single-sync
				"arcade_15_25_31", "arcade_15_31", "arcade_15_25",
				"arcade_15", "arcade_15ex", "arcade_25", "arcade_31",
				// Generic / broadcast
				"generic_15", "ntsc", "pal",
				// Specific monitor models. "d9400" and "polo" are switchres aliases of "d9800"
				// and "h9110"; they are accepted here (a hand-edited INI may use them) even
				// though the dropdown only lists the primary name.
				"d9800", "d9400", "d9200", "k7000", "k7131", "m3129", "m2929",
				"h9110", "polo", "pstar", "ms2930", "ms929", "r666b",
				// PC CRT / VESA GTF
				"pc_31_120", "pc_70_120", "vesa_480", "vesa_600", "vesa_768", "vesa_1024",
				// Timings supplied by the user's Switchres INI
				"custom"};
			for (const char* k : kKnown)
			{
				if (p == k)
					return true;
			}
			return false;
		}

		std::string ToLowerAscii(std::string s)
		{
			for (char& c : s)
			{
				if (c >= 'A' && c <= 'Z')
					c = static_cast<char>(c - 'A' + 'a');
			}
			return s;
		}
	} // namespace

	// =================================================================================
	//  Lifecycle
	// =================================================================================

	Output::Output() = default;

	Output::~Output()
	{
		Close();
	}

	bool Output::Open()
	{
		if (m_active.load(std::memory_order_relaxed))
			return true;

		m_cfg = EmuConfig.GroovyMiSTer;
		if (!m_cfg.Enabled)
			return false;

		m_quit.store(false, std::memory_order_relaxed);
		m_reconnect_requested.store(false, std::memory_order_relaxed);
		m_blit_frame = 0;
		m_have_mode = false;
		m_mode_refused = false;
		m_src_w = m_src_h = 0;
		m_src_hz = 0.0f;
		m_logged_first_frame = false;
		m_logged_oversized_frame = false;

		m_codec = static_cast<u8>(m_cfg.Codec);
		m_rgb_mode = static_cast<u8>(m_cfg.RgbMode);
		m_bpp = BytesPerPixel(m_cfg.RgbMode);

		// The sample rate is baked into CMD_INIT, so we must know it up front. PS2 mixes at
		// 48kHz; PS1 mode is 44.1kHz. If it changes later, SPU2 calls OnSampleRateChanged()
		// and we reconnect.
		if (m_cfg.TapAudio)
		{
			m_sound_rate = SoundRateCodeFor(SPU2::GetConsoleSampleRate());
			m_sound_chan = CHAN_STEREO;
			if (m_sound_rate == RATE_OFF)
			{
				Console.Warning(fmt::format("[MiSTer] Unsupported console sample rate {}Hz; audio disabled.",
					SPU2::GetConsoleSampleRate()));
				m_sound_chan = CHAN_OFF;
			}
		}
		else
		{
			m_sound_rate = RATE_OFF;
			m_sound_chan = CHAN_OFF;
		}

		if (!InitSwitchres())
			return false;

		if (!TryConnect())
		{
			// Non-fatal on purpose: a MiSTer that is off/asleep must never stop the user
			// from booting a game. The sender thread keeps retrying in the background.
			Console.Warning(fmt::format("[MiSTer] Could not reach {} yet; will keep retrying.", m_cfg.Host));
		}

		m_audio_tap.Reset();
		m_audio_tap.SetActive(m_cfg.TapAudio && m_sound_chan != CHAN_OFF);

		m_active.store(true, std::memory_order_release);
		m_sender = std::thread([this]() { SenderLoop(); });

		Console.WriteLn(fmt::format("[MiSTer] Output enabled (host={}, codec={}, {} bpp).",
			m_cfg.Host, m_codec == 7 ? "NLC" : (m_codec == 0 ? "raw" : "LZ4"), m_bpp));
		return true;
	}

	void Output::Close()
	{
		if (!m_active.exchange(false, std::memory_order_acq_rel))
		{
			// Never connected (or already closed) - still tear down switchres if Open()
			// got that far before bailing.
			ShutdownSwitchres();
			return;
		}

		m_audio_tap.SetActive(false);

		{
			std::lock_guard<std::mutex> guard(m_queue_lock);
			m_quit.store(true, std::memory_order_release);
			m_queue.clear();
		}
		m_queue_cv.notify_all();
		m_space_cv.notify_all();

		// The sender owns the socket, so it must also be the one to close it (see the
		// RIO note in the header). DoGroovyClose() runs at the end of SenderLoop().
		if (m_sender.joinable())
			m_sender.join();

		ReleaseGpuResources();
		ShutdownSwitchres();

		Console.WriteLn("[MiSTer] Output closed.");
	}

	// =================================================================================
	//  Connection
	// =================================================================================

	bool Output::TryConnect()
	{
		gmw_set_log_callback(&GmwLogSink, m_cfg.LogVerbosity);

		if (m_cfg.Codec == GroovyMiSTerCodec::NLC)
		{
			if (m_cfg.NlcPack == GroovyMiSTerNlcPack::Rice)
			{
				// There is no negotiation for this. A core without the Rice decoder ignores
				// the bit and parses our Rice bytes as Tiled, which looks like a garbage
				// picture rather than an error - so say it out loud.
				Console.WriteLn(fmt::format(
					"[MiSTer] NLC codec: pack=Rice NEAR={} - REQUIRES a core with the Rice decoder "
					"(rbf_rice_r3 kit or newer). On an older core the picture will be garbage; "
					"switch NLC Pack to Tiled if so.",
					m_cfg.NlcNearLevel));
			}
			else
			{
				Console.WriteLn(fmt::format("[MiSTer] NLC codec: pack=Tiled NEAR={}.", m_cfg.NlcNearLevel));
			}
		}

		// Drop anything half-alive from a previous attempt.
		gmw_close();

		// ORDER MATTERS, three times over:
		//
		// 1. The input subscription must be sent BEFORE CMD_INIT. The MiSTer core reads the
		//    subscribe datagram synchronously inside its CMD_INIT handler; gmw_bindInputs
		//    already sends it, the extra resubscribe is UDP-loss insurance.
		//
		// 2. The NLC knobs ride CMD_INIT byte[1], so they must be set before gmw_init() too.
		//    Calling them afterwards is a silent no-op.
		//
		// 3. The input caps ride CMD_INIT byte[5], same rule again. The client probes the
		//    core version (CMD_GET_VERSION) inside gmw_init and drops to a caps-less v1
		//    handshake on a pre-v2 core by itself - no app-side fallback loop needed.
		//    A v1 session still delivers all 16 digital buttons; only analog triggers and
		//    rumble need the caps.
		gmw_bindInputs(m_cfg.Host.c_str());
		gmw_resubscribe_inputs();

		if (m_cfg.Codec == GroovyMiSTerCodec::NLC)
		{
			gmw_set_nlc_pack(static_cast<u8>(m_cfg.NlcPack));
			gmw_set_near_level(m_cfg.NlcNearLevel);
		}
		gmw_set_input_caps(GMW_CAP_INPUTS_V2 | GMW_CAP_RUMBLE);

		// The reconnect watchdog is opt-in since the convergence sync; arm it to keep the
		// old always-on behavior. It now re-subscribes inputs across the internal reconnect.
		gmw_set_auto_reconnect(1);

		if (gmw_init(m_cfg.Host.c_str(), m_codec, m_sound_rate, m_sound_chan, m_rgb_mode, m_cfg.Mtu) < 0)
			return false;

		if (gmw_get_input_caps() != 0)
			Console.WriteLn("[MiSTer] Inputs v2 negotiated (PS-semantic buttons, analog triggers, rumble).");
		else
			Console.WriteLn("[MiSTer] Inputs v1 session (16 buttons + sticks; no analog triggers/rumble).");

		{
			std::lock_guard<std::mutex> guard(m_status_lock);
			m_status.connected = true;
		}

		// A fresh CMD_INIT restarts the core's own frame counter at zero, so ours has to
		// restart with it - otherwise we blit numbers thousands ahead of a core that just
		// began counting, which desyncs the raster servo (the client clamps the resulting
		// sleep rather than hanging, but stays unaligned until we realign).
		//
		// Re-seed the epoch from the client rather than assuming a value: the gmw_close()
		// above DESTROYS the wrapper's singleton, so the epoch it counts is per-object and
		// restarts at 0 here, while the client's internal watchdog reconnects increment it
		// within an object's life. It is therefore not monotonic across a session - which
		// is why the sender loop compares it for inequality, not for growth.
		m_blit_frame = 0;
		m_reconnect_epoch = gmw_reconnect_epoch();
		m_keepalive.Reset(static_cast<u64>(
			Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue())));

		// Force the modeline to be re-sent on the new connection.
		m_have_mode = false;
		m_src_w = m_src_h = 0;

		Console.WriteLn(fmt::format("[MiSTer] Connected to {} (client v{}).", m_cfg.Host, gmw_get_version()));
		return true;
	}

	void Output::DoGroovyClose()
	{
		// Tell the MiSTer we are leaving, so it drops back to its "searching for a
		// connection" screen instead of sitting on our last frame forever. Sent a few times
		// because a single lost UDP datagram would strand it.
		for (int i = 0; i < 3; i++)
			gmw_send_close();

		gmw_close();

		std::lock_guard<std::mutex> guard(m_status_lock);
		m_status.connected = false;
	}

	// =================================================================================
	//  switchres
	// =================================================================================

	bool Output::InitSwitchres()
	{
		if (m_sr_inited)
			return true;

		sr_init();
		sr_set_log_callback_error(reinterpret_cast<void*>(&SrLogSink));
		sr_set_log_callback_info(reinterpret_cast<void*>(&SrLogSink));
		sr_set_log_callback_debug(reinterpret_cast<void*>(&SrLogSink));

		// Decide the monitor to use. switchres's OWN default is generic_15 - a 15 kHz-only
		// band that refuses every 31 kHz mode (i.e. all PS2 480p) - so we must never leave it
		// unset. switchres also matches preset names with a raw strcmp and silently falls back
		// to generic_15 on an unknown name, so lowercase and validate before trusting it.
		std::string preset = ToLowerAscii(m_cfg.MonitorPreset);
		if (preset.empty() || !IsKnownMonitorPreset(preset))
		{
			if (!preset.empty())
				Console.Warning(fmt::format("[MiSTer] Unknown monitor preset '{}'; using '{}'.", preset, DEFAULT_MONITOR));
			preset = DEFAULT_MONITOR;
		}

		// "custom" means the timings come from a Switchres INI (monitor custom + crt_range*).
		// Without an INI there is nothing to define the ranges, so fall back rather than run
		// on an empty custom monitor.
		if (preset == "custom" && m_cfg.SwitchresIni.empty())
		{
			Console.Warning(fmt::format("[MiSTer] Monitor preset is 'custom' but no Switchres INI is set; using '{}'.",
				DEFAULT_MONITOR));
			preset = DEFAULT_MONITOR;
		}

		// Set the built-in monitor explicitly. Doing this AFTER sr_init() (which parses any
		// stray switchres.ini in the working directory) also means our monitor overrides a
		// stray ini's `monitor` line. For "custom" we deliberately do not set a monitor - the
		// INI's own `monitor custom` line supplies it.
		if (preset != "custom")
			sr_set_monitor(preset.c_str());

		// "dummy" == calculate only. switchres must never touch the *host's* display; we
		// only want the modeline maths, which we then ship to the FPGA. (The library is
		// also compiled with SR_CALC_ONLY, so the real backends are not even linked in.)
		sr_init_disp("dummy", nullptr);

		if (!m_cfg.SwitchresIni.empty())
			sr_load_ini(const_cast<char*>(m_cfg.SwitchresIni.c_str()));

		m_sr_inited = true;

		// Log what switchres actually settled on. If this does not match `preset`, a stray
		// switchres.ini or an unexpected fallback is in play - which is exactly the kind of
		// thing that makes "no valid signal" hard to diagnose.
		sr_state st{};
		sr_get_state(&st);
		Console.WriteLn(fmt::format("[MiSTer] switchres ready (requested monitor '{}', active '{}').",
			preset, st.monitor));
		return true;
	}

	void Output::ShutdownSwitchres()
	{
		if (!m_sr_inited)
			return;

		sr_deinit();
		m_sr_inited = false;
	}

	bool Output::EnsureMode(int src_w, int src_h, float refresh_hz, bool interlaced)
	{
		// Reject an obviously-not-ready source triple WITHOUT caching it, so it is retried on
		// the next vsync rather than latched as a failed mode. The GS video mode can be briefly
		// unstable right after a connection or a mode change (e.g. refresh reported as 0), and
		// we do not want that transient to wedge the stream until the game happens to switch
		// modes again. The window is wide on purpose - it only screens out garbage; switchres
		// applies its own per-monitor refresh tolerance.
		if (src_w <= 0 || src_h <= 0 || refresh_hz < 40.0f || refresh_hz > 130.0f)
			return false;

		const bool unchanged = (src_w == m_src_w && src_h == m_src_h &&
		                        std::abs(refresh_hz - m_src_hz) < 0.01f &&
		                        interlaced == m_src_interlaced);
		if (unchanged)
			return m_have_mode;

		m_src_w = src_w;
		m_src_h = src_h;
		m_src_hz = refresh_hz;
		m_src_interlaced = interlaced;
		m_have_mode = false;
		m_mode_refused = false;

		// Whether we ask switchres for an interlaced modeline. "Force progressive" (the user
		// picking Interlace == Progressive) never does, so a 480i source is turned into a 480p
		// (31 kHz) modeline. Otherwise interlace tracks the source: an interlaced frame gets a
		// 15 kHz interlaced modeline, a progressive one stays progressive. This flag matters
		// because switchres tries the *progressive* band first for a generated mode, so a 480i
		// source that also fits progressively (e.g. 448 lines on a 31 kHz band) comes back as
		// 480p unless we tell it the source is interlaced (see modeline.cpp scan_penalty).
		const bool prefer_progressive = (m_cfg.Interlace == GroovyMiSTerInterlace::Progressive);
		const int sr_flags = (interlaced && !prefer_progressive) ? SR_MODE_INTERLACED : 0;

		sr_mode srm{};
		const int ok = sr_add_mode(src_w, src_h, static_cast<double>(refresh_hz), sr_flags, &srm);
		if (!ok || srm.width <= 0 || srm.height <= 0)
		{
			Console.Error(fmt::format("[MiSTer] switchres could not produce a modeline for {}x{}@{:.2f}Hz.",
				src_w, src_h, refresh_hz));
			m_mode_refused = true;
			return false;
		}

		Modeline ml{};
		ml.pclock = static_cast<double>(srm.pclock) / 1000000.0; // switchres reports Hz
		ml.h_active = static_cast<u16>(srm.width);
		ml.h_begin = static_cast<u16>(srm.hbegin);
		ml.h_end = static_cast<u16>(srm.hend);
		ml.h_total = static_cast<u16>(srm.htotal);
		ml.v_active = static_cast<u16>(srm.height);
		ml.v_begin = static_cast<u16>(srm.vbegin);
		ml.v_end = static_cast<u16>(srm.vend);
		ml.v_total = static_cast<u16>(srm.vtotal);

		// The interlace value we put on the wire is the user's choice, not switchres's:
		// a progressive modeline is always 0, but an interlaced one can be sent either as
		// true fields (1) or as a progressive framebuffer over an interlaced signal (2).
		ml.interlace = srm.interlace ? static_cast<u8>(m_cfg.Interlace) : 0;
		if (srm.interlace && ml.interlace == 0)
			ml.interlace = static_cast<u8>(GroovyMiSTerInterlace::ProgressiveFB);

		if (!IsModelineAcceptable(ml, m_bpp, m_cfg.CrtSafetyCap))
		{
			const bool malformed = !IsWellFormed(ml);
			const bool too_big = !malformed && !FitsBlitBuffer(ml, m_bpp);

			// The byte-budget case is the only one the user can actually act on, so it names
			// the remedy - and works out whether there is one, because past a certain size no
			// pixel format fits. BytesPerPixel() values: RGB565 = 2, RGB888 = 3, RGBA8888 = 4.
			std::string reason;
			std::string osd_reason;
			if (malformed)
			{
				reason = "the modeline is malformed";
				osd_reason = "malformed modeline";
			}
			else if (too_big)
			{
				const char* remedy = "and no RGB Mode is small enough for a mode this large";
				if (m_bpp > 3 && FitsBlitBuffer(ml, 3))
					remedy = "- switch RGB Mode to RGB888 or RGB565";
				else if (m_bpp > 2 && FitsBlitBuffer(ml, 2))
					remedy = "- switch RGB Mode to RGB565";

				reason = fmt::format("at {} bytes/pixel it needs {} bytes per blit, over the "
									 "{}-byte buffer {}",
					m_bpp, BlitBytes(ml, m_bpp), Pcsx2Config::GroovyMiSTerOptions::MAX_BLIT_BYTES,
					remedy);
				osd_reason = "too many bytes per frame - see the log for which RGB Mode fits";
			}
			else
			{
				reason = fmt::format("it exceeds the CRT safety cap ({}x{} max)",
					Pcsx2Config::GroovyMiSTerOptions::MAX_SAFE_H_ACTIVE,
					Pcsx2Config::GroovyMiSTerOptions::MAX_SAFE_V_ACTIVE);
				osd_reason = "outside the CRT-safe range";
			}

			Console.Error(fmt::format("[MiSTer] Refusing modeline {}x{} @ {:.2f}Hz: {}.",
				ml.h_active, ml.v_active, refresh_hz, reason));

			Host::AddKeyedOSDMessage("GroovyMiSTerMode",
				fmt::format("MiSTer: refusing {}x{} @ {:.0f}Hz - {}. Streaming is paused until the "
							"game changes video mode.",
					ml.h_active, ml.v_active, refresh_hz, osd_reason),
				15.0f);

			m_mode_refused = true;
			return false;
		}

		m_modeline = ml;
		m_have_mode = true;
		m_dst_w = ml.h_active;
		m_dst_h = ml.v_active;

		// A true-field stream sends one field per blit, so the buffer is half height.
		if (ml.interlace == static_cast<u8>(GroovyMiSTerInterlace::Field))
			m_dst_h = ml.v_active / 2;

		ReleaseGpuResources(); // geometry changed; rebuild lazily on the next capture

		{
			std::lock_guard<std::mutex> guard(m_sr_lock);
			m_pending_modeline = ml;
			m_switchres_pending = true;
		}

		{
			std::lock_guard<std::mutex> guard(m_status_lock);
			m_status.native_width = src_w;
			m_status.native_height = src_h;
			m_status.refresh_hz = refresh_hz;
			m_status.interlaced = interlaced;
			m_status.modeline = ml;
			m_status.monitor_preset = m_cfg.MonitorPreset.empty() ? "generic_15" : m_cfg.MonitorPreset;
		}

		// Tell the user what the game is actually running at, and what we turned it into.
		// This is the answer to "how do I know the game's native resolution?" - it is the
		// PCRTC output, i.e. the real thing, not a guess.
		const std::string msg = fmt::format("MiSTer: PS2 {}x{}{} @ {:.2f}Hz -> {}x{} {:.2f}kHz ({})",
			src_w, src_h, interlaced ? "i" : "p", refresh_hz,
			ml.h_active, ml.v_active, srm.hfreq / 1000.0,
			m_cfg.MonitorPreset.empty() ? "generic_15" : m_cfg.MonitorPreset);

		Console.WriteLn(fmt::format("[MiSTer] {}", msg));
		Console.WriteLn(fmt::format("[MiSTer]   modeline: pclock={:.4f}MHz h({} {} {}) v({} {} {}) interlace={}",
			ml.pclock, ml.h_begin, ml.h_end, ml.h_total, ml.v_begin, ml.v_end, ml.v_total, ml.interlace));
		Host::AddKeyedOSDMessage("GroovyMiSTerMode", msg, 5.0f);

		return true;
	}

	// =================================================================================
	//  Capture (GS thread)
	// =================================================================================

	bool Output::EnsureGpuResources()
	{
		if (m_dst_w <= 0 || m_dst_h <= 0)
			return false;

		for (Readback& rb : m_readback)
		{
			if (!rb.tex || static_cast<int>(rb.tex->GetWidth()) != m_dst_w ||
				static_cast<int>(rb.tex->GetHeight()) != m_dst_h)
			{
				rb.tex = g_gs_device->CreateDownloadTexture(
					static_cast<u32>(m_dst_w), static_cast<u32>(m_dst_h), GSTexture::Format::Color);
				rb.pending = false;
				if (!rb.tex)
				{
					Console.Error(fmt::format("[MiSTer] Failed to create {}x{} download texture.", m_dst_w, m_dst_h));
					return false;
				}
			}

			// Only Sync mode needs a single slot; Deferred1 needs both.
			if (m_cfg.Readback == GroovyMiSTerReadback::Sync)
				break;
		}

		return true;
	}

	void Output::ReleaseGpuResources()
	{
		if (m_scratch_rt)
		{
			g_gs_device->Recycle(m_scratch_rt);
			m_scratch_rt = nullptr;
		}

		for (Readback& rb : m_readback)
		{
			rb.tex.reset();
			rb.pending = false;
		}
		m_readback_idx = 0;
	}

	void Output::PushFrame(std::vector<u8>&& pixels, u8 field)
	{
		std::unique_lock<std::mutex> lock(m_queue_lock);

		if (m_cfg.Pacing == GroovyMiSTerPacing::MisterMaster)
		{
			// The CRT is the clock. Block until the sender has room; that backpressure
			// travels GS thread -> MTGS ring -> EE thread, which is precisely how the
			// emulator ends up paced by the raster rather than by its own frame limiter.
			m_space_cv.wait(lock, [this]() {
				return m_queue.size() < MAX_QUEUED_FRAMES || m_quit.load(std::memory_order_acquire);
			});
			if (m_quit.load(std::memory_order_acquire))
				return;
		}
		else
		{
			// PCSX2 is the clock. Newest-wins: a frame the CRT has not shown yet is worth
			// less than the one we are holding, so drop it rather than grow a backlog.
			while (m_queue.size() >= MAX_QUEUED_FRAMES)
			{
				m_queue.pop_front();
				std::lock_guard<std::mutex> sguard(m_status_lock);
				m_status.frames_dropped++;
			}
		}

		m_queue.push_back(OutFrame{std::move(pixels), field});
		lock.unlock();
		m_queue_cv.notify_one();
	}

	void Output::Capture(GSTexture* current, const GSVector4i& src_rect, u32 field)
	{
		if (!current || !g_gs_device || !g_gs_renderer)
			return;

		if (m_reconnect_requested.exchange(false, std::memory_order_relaxed))
		{
			// Sample rate changed; the rate is part of CMD_INIT so we have to redo it.
			// Cheapest correct thing: tear down and let Open() rebuild.
			Console.WriteLn("[MiSTer] Console sample rate changed; reconnecting.");
			Close();
			Open();
			return;
		}

		// The PS2's real display resolution - the PCRTC output, not our upscaled internal
		// buffer. This is exactly the (w, h, hz) triple switchres wants.
		const GSVector2i native = g_gs_renderer->GetInternalResolution();
		const float refresh = g_gs_renderer->GetTvRefreshRate();
		// Whether this frame is an interlaced scanout. Use isReallyInterlaced() (SMODE1.CMOD +
		// the SYNCV field toggle), NOT GetVideoMode(): a game can render 240p while the video mode
		// is still NTSC/PAL, and mislabelling that as interlaced would ask switchres for a
		// nonsensical interlaced 240-line modeline. This is the same predicate the merge circuit
		// uses to decide it must deinterlace, i.e. exactly when GetCurrent() is a full-height
		// interlaced-derived frame.
		const bool interlaced = g_gs_renderer->isReallyInterlaced();

		if (!EnsureMode(native.x, native.y, refresh, interlaced))
			return; // no usable modeline (refused, or switchres failed) - stay quiet

		if (!EnsureGpuResources())
			return;

		// The FPGA derives the interlaced field cadence from the modeline itself, so a progressive
		// framebuffer over an interlaced signal (ProgressiveFB) must be blitted as field 0. Sending
		// the PS2's alternating field there would make the core read consecutive frames from its two
		// field buffers (DDR_FB/DDR_FD) and comb on horizontal motion. Only a true-field stream
		// carries real alternating fields.
		const u8 blit_field = (m_modeline.interlace == static_cast<u8>(GroovyMiSTerInterlace::Field))
								  ? static_cast<u8>(field) : 0;

		// The valid sub-rect matters. `current` can be larger than the video mode when a
		// game flips an over-allocated render-target surface; the host window only blits
		// this rect. Capturing the whole texture streams the stale region and shows up as a
		// duplicated, offset image. Clamp to what is actually displayed.
		const GSVector4i valid = src_rect.rintersect(GSVector4i::loadh(current->GetSize()));
		if (valid.rempty())
			return;

		if (!m_logged_first_frame)
		{
			m_logged_first_frame = true;
			Console.WriteLn(fmt::format("[MiSTer] first frame: surface={}x{} valid={}x{} -> dst={}x{}",
				current->GetWidth(), current->GetHeight(), valid.width(), valid.height(), m_dst_w, m_dst_h));
		}

		// Scale to the modeline's active area. If the user runs at native 1x this is a
		// straight copy of the same size, but we still go through StretchRect so the
		// download texture always has a known, tightly-packed geometry.
		GSTexture* readback_src = current;
		GSVector4i readback_rect = valid;

		if (valid.width() != m_dst_w || valid.height() != m_dst_h)
		{
			if (!m_scratch_rt || m_scratch_rt->GetWidth() != m_dst_w || m_scratch_rt->GetHeight() != m_dst_h)
			{
				if (m_scratch_rt)
					g_gs_device->Recycle(m_scratch_rt);
				m_scratch_rt = g_gs_device->CreateRenderTarget(m_dst_w, m_dst_h, GSTexture::Format::Color, true);
				if (!m_scratch_rt)
					return;
			}

			const GSVector4 s_rect = GSVector4(valid) / GSVector4(current->GetSize()).xyxy();
			const GSVector4 d_rect = GSVector4(0, 0, m_dst_w, m_dst_h);
			g_gs_device->StretchRect(current, s_rect, m_scratch_rt, d_rect, ShaderConvert::COPY, Biln);

			readback_src = m_scratch_rt;
			readback_rect = GSVector4i(0, 0, m_dst_w, m_dst_h);
		}

		Readback& slot = m_readback[m_readback_idx];
		const GSVector4i dst_rect(0, 0, m_dst_w, m_dst_h);

		if (m_cfg.Readback == GroovyMiSTerReadback::Deferred1)
		{
			// Drain the frame we started last vsync. By now the GPU has certainly finished
			// it, so Flush() is effectively free - we trade exactly one frame of latency for
			// never blocking the GS thread.
			if (slot.pending)
			{
				if (slot.tex->NeedsFlush())
					slot.tex->Flush();

				if (slot.tex->Map(dst_rect))
				{
					std::vector<u8> packed;
					PackFrame(m_cfg.RgbMode, slot.tex->GetMapPointer(), slot.tex->GetMapPitch(),
						static_cast<u32>(m_dst_w), static_cast<u32>(m_dst_h), packed);
					slot.tex->Unmap();
					PushFrame(std::move(packed), slot.field);
				}
				slot.pending = false;
			}

			slot.tex->CopyFromTexture(dst_rect, readback_src, readback_rect, 0);
			slot.pending = true;
			slot.field = blit_field;
			m_readback_idx ^= 1u;
			return;
		}

		// Sync: flush and map in-frame. Zero added frames. This blocks the GS thread on the
		// GPU for a fraction of a millisecond - which is affordable here precisely because
		// PCSX2 puts the MTGS ring between the GS thread and the EE, so the stall is
		// absorbed rather than passed straight to emulation. (RPCS3 cannot do this: the
		// equivalent readback stalls its RSX thread directly, which is why it defers.)
		slot.tex->CopyFromTexture(dst_rect, readback_src, readback_rect, 0);
		if (slot.tex->NeedsFlush())
			slot.tex->Flush();

		if (!slot.tex->Map(dst_rect))
			return;

		std::vector<u8> packed;
		PackFrame(m_cfg.RgbMode, slot.tex->GetMapPointer(), slot.tex->GetMapPitch(),
			static_cast<u32>(m_dst_w), static_cast<u32>(m_dst_h), packed);
		slot.tex->Unmap();

		PushFrame(std::move(packed), blit_field);
	}

	// =================================================================================
	//  Sender thread - sole owner of the Groovy video/audio socket
	// =================================================================================

	void Output::SenderLoop()
	{
		Threading::SetNameOfCurrentThread("GroovyMiSTer Sender");

		// CMD_AUDIO carries its payload size in a u16, so one send can never exceed 65535
		// bytes - and the cast to u16 must never be able to wrap. It previously could: the
		// scratch buffer was exactly 65536 bytes, Read() could fill it completely, and
		// static_cast<u16>(65536) == 0, which put an empty CMD_AUDIO on the wire that the core
		// rejected with UDP_ERROR. That fired whenever a large backlog had built up (e.g. the
		// several seconds between the output coming up and the first video frame, which fills
		// the tap's ring). Cap the drain instead: comfortably inside u16, a whole number of
		// 4-byte stereo frames, and small enough that we never dump a huge stale burst in one
		// packet (~85ms @ 48kHz stereo; steady state is only ~3.2KB per frame).
		static constexpr u32 MAX_AUDIO_SEND_BYTES = 16 * 1024;
		static_assert(MAX_AUDIO_SEND_BYTES <= 65535, "CMD_AUDIO size must fit in a u16");
		static_assert((MAX_AUDIO_SEND_BYTES % AudioTap::BYTES_PER_SAMPLE) == 0, "whole stereo frames only");

		std::vector<u8> audio_scratch(MAX_AUDIO_SEND_BYTES);
		Common::Timer reconnect_timer;

		// Monotonic milliseconds for the keepalive scheduler. Taken once per iteration.
		const auto now_ms = []() -> u64 {
			return static_cast<u64>(
				Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue()));
		};
		m_keepalive.Reset(now_ms());
		m_reconnect_epoch = gmw_reconnect_epoch();

		while (!m_quit.load(std::memory_order_acquire))
		{
			// Wake on a frame OR on a keepalive poll tick. The timed wait is what makes an
			// idle session survivable: every way PCSX2 stops producing frames (pause,
			// savestate load, disc swap, a modeline the safety gate refused) used to park
			// this thread here indefinitely, and a session that sends nothing for the
			// core's idle timeout - 5s by default - is closed core-side and the CRT is
			// freed. Poll well under the send threshold; see KeepAliveScheduler.
			OutFrame frame;
			bool have_frame = false;
			{
				std::unique_lock<std::mutex> lock(m_queue_lock);
				m_queue_cv.wait_for(lock, std::chrono::milliseconds(KeepAliveScheduler::POLL_PERIOD_MS),
					[this]() {
						return !m_queue.empty() || m_quit.load(std::memory_order_acquire);
					});
				if (m_quit.load(std::memory_order_acquire))
					break;

				if (!m_queue.empty())
				{
					frame = std::move(m_queue.front());
					m_queue.pop_front();
					have_frame = true;
				}
			}
			if (have_frame)
				m_space_cv.notify_one();

			if (!gmw_is_connected())
			{
				// Retry at a human pace, not a spin. This also runs on idle ticks, so a
				// session lost while the emulator is paused comes back on its own instead
				// of waiting for frames to resume.
				if (reconnect_timer.GetTimeSeconds() >= 2.0)
				{
					reconnect_timer.Reset();
					TryConnect();
				}
				if (!gmw_is_connected())
					continue;
			}

			// The client's internal watchdog may have reconnected underneath us. The core
			// restarts its own frame counter on the fresh session, so ours has to restart
			// too - see the note on m_blit_frame below for why only this direction needs
			// handling.
			const u32 epoch = gmw_reconnect_epoch();
			if (epoch != m_reconnect_epoch)
			{
				m_reconnect_epoch = epoch;
				m_blit_frame = 0;
				Console.WriteLn(fmt::format("[MiSTer] Client reconnected (epoch {}); realigning frame counter.", epoch));
			}

			// Receive any pending ACKs from the FPGA. This is not optional: the vendored
			// client only updates fpga.frameEcho inside getACK(), and its CmdBlit watchdog
			// force-reconnects when frameEcho stops advancing for 10 blits. getStatus() below
			// merely copies the cache, and gmw_blit() never receives - so if nothing polls,
			// frameEcho is stuck at 0 and every blit drives an endless reconnect loop (each
			// reconnect sends CMD_CLOSE, which drops the FPGA's video output, so the CRT can
			// never lock). In MisterMaster gmw_waitSync() does this poll for us as a side
			// effect of raster pacing; in Pcsx2Master nothing did, which is the bug. A
			// non-blocking poll (0 ms) is enough - the previous frame's ACK has long since
			// arrived by the time we loop back here at 60 Hz.
			//
			// Polling on IDLE ticks as well is not cosmetic. fpga.frame is the core's own
			// GPU counter and it free-runs at the CRT's refresh rate whether or not we are
			// blitting, so without this it would freeze at its pre-pause value: the first
			// frame after a long pause would be numbered thousands behind where the core
			// actually is, and discarded as stale.
			gmw_getACK(0);

			// Hold the session open if nothing has gone out lately. Deliberately evaluated
			// on EVERY iteration rather than only on idle ticks: an iteration can carry a
			// frame and still send nothing (the oversized-frame drop below, or a null blit
			// buffer), and gating this on !have_frame would let that case starve the core's
			// idle timer while looking busy. During normal play ShouldSend() is false
			// because every blit refreshes the timestamp, so this costs one comparison.
			const u64 tick_ms = now_ms();
			if (m_keepalive.ShouldSend(tick_ms))
			{
				gmw_send_keepalive();
				m_keepalive.NotifyWireActivity(tick_ms);
			}

			// Idle tick: holding the session is all there is to do. No switchres, no audio,
			// no frame counter movement.
			if (!have_frame)
				continue;

			// Any modeline change must land before the frame that depends on it.
			{
				std::lock_guard<std::mutex> guard(m_sr_lock);
				if (m_switchres_pending)
				{
					const Modeline& m = m_pending_modeline;
					gmw_switchres(m.pclock, m.h_active, m.h_begin, m.h_end, m.h_total,
						m.v_active, m.v_begin, m.v_end, m.v_total, m.interlace);
					m_switchres_pending = false;
				}
			}

			// Audio first - the core wants it ahead of the frame it belongs to.
			gmw_fpgaStatus status{};
			gmw_getStatus(&status);
			if (status.audio && m_audio_tap.IsActive())
			{
				const u32 n = m_audio_tap.Read(audio_scratch.data(), MAX_AUDIO_SEND_BYTES);
				if (n > 0)
				{
					std::memcpy(gmw_get_pBufferAudio(), audio_scratch.data(), n);
					gmw_audio(static_cast<u16>(n));
				}
			}

			// The FPGA displays frames in counter order, so ours has to stay ahead of what
			// it is currently showing or the frame is discarded as stale.
			//
			// This forward jump is deliberately UNBOUNDED, which is a considered departure
			// from the Groovy integration handoff (§11.5, "bound that resync"). That advice
			// predates the keepalive: now that we hold a paused session open, status.frame
			// free-runs while we are quiet, so after a 60s pause the core is legitimately
			// ~3600 frames ahead and clamping the jump would leave us permanently numbering
			// behind the display position - every frame stale, forever. The failure the
			// bound guarded against was jumping onto a DEAD session's counter, and that is
			// no longer reachable: the vendored client's resetSessionState() zeroes fpga.*
			// at the top of every CmdInit, so status.frame can only ever describe the
			// session we are actually talking to.
			//
			// The direction that does still need handling is backwards - a reconnect
			// restarts the core at zero while we would otherwise keep counting - and that
			// is what the reconnectEpoch check above and TryConnect() take care of.
			m_blit_frame = std::max(m_blit_frame + 1, status.frame + 1);

			// Belt and braces. EnsureMode() already refuses any mode whose blit would not fit
			// (GroovyMiSTerModeline.h, FitsBlitBuffer), so reaching this means the queued
			// geometry and the negotiated pixel format have disagreed somewhere. Dropping the
			// frame is survivable; the memcpy below - into a fixed-size, RIO-registered
			// allocation - is not.
			if (frame.pixels.size() > Pcsx2Config::GroovyMiSTerOptions::MAX_BLIT_BYTES)
			{
				if (!m_logged_oversized_frame)
				{
					m_logged_oversized_frame = true;
					Console.Error(fmt::format(
						"[MiSTer] Dropping oversized frame: {} bytes exceeds the {}-byte blit buffer. "
						"This is a bug - the mode gate should have refused it.",
						frame.pixels.size(), Pcsx2Config::GroovyMiSTerOptions::MAX_BLIT_BYTES));
				}

				std::lock_guard<std::mutex> guard(m_status_lock);
				m_status.frames_dropped++;
				continue;
			}

			char* blit_buf = gmw_get_pBufferBlit(frame.field);
			if (blit_buf)
			{
				std::memcpy(blit_buf, frame.pixels.data(), frame.pixels.size());

				// vCountSync = 1: raster-chase at line 1. (The client's "auto frame delay"
				// mode assumes a synchronous per-frame caller, which we are not.)
				gmw_blit(m_blit_frame, frame.field, 1, 0, 0);

				// Gate the keepalive on real wire activity rather than a free-running
				// heartbeat: at 60fps this lands every ~16ms, so the idle threshold is
				// never reached and a keepalive during normal play is structurally
				// impossible, not merely unlikely. (Any CmdAudio above rode the same
				// socket a few lines earlier, so one update here covers both.)
				m_keepalive.NotifyWireActivity(now_ms());

				std::lock_guard<std::mutex> guard(m_status_lock);
				m_status.frames_sent++;
				m_status.last_encoded_bytes = static_cast<u32>(frame.pixels.size());
				m_status.connected = true;
				// frameEcho/vCountEcho say where the raster was when the FPGA got our frame;
				// frame/vCount say where it is now. The gap is the real end-to-end latency.
				if (m_modeline.v_total > 0 && m_src_hz > 0.0f)
				{
					const float lines = static_cast<float>(m_modeline.v_total);
					const float frame_ms = 1000.0f / m_src_hz;
					const int dl = static_cast<int>(status.vCount) - static_cast<int>(status.vCountEcho);
					m_status.latency_ms = (static_cast<float>(dl) / lines) * frame_ms;
				}
			}

			if (m_cfg.Pacing == GroovyMiSTerPacing::MisterMaster)
			{
				// Sleep until the CRT is ready for the next frame. This is what makes the
				// raster - not PCSX2's frame limiter - the master clock.
				gmw_waitSync();
			}
		}

		DoGroovyClose();
	}

	Status Output::GetStatus() const
	{
		std::lock_guard<std::mutex> guard(m_status_lock);
		return m_status;
	}

	// =================================================================================
	//  Facade (GroovyMiSTer.h) - the only surface upstream PCSX2 touches
	// =================================================================================

	void Open()
	{
		if (!EmuConfig.GroovyMiSTer.Enabled)
			return;

		if (!s_output)
			s_output = std::make_unique<Output>();

		if (!s_output->Open())
			s_output.reset();
	}

	void Close()
	{
		if (s_output)
		{
			s_output->Close();
			s_output.reset();
		}
	}

	bool IsActive()
	{
		return s_output && s_output->IsActive();
	}

	void OnVSync(GSTexture* current, const GSVector4i& src_rect, u32 field)
	{
		if (s_output)
			s_output->Capture(current, src_rect, field);
	}

	bool IsAudioActive()
	{
		return s_output && s_output->IsAudioActive();
	}

	void OnAudioChunk(const float* samples, u32 frames)
	{
		if (s_output)
			s_output->WriteAudio(samples, frames);
	}

	void OnSampleRateChanged()
	{
		if (s_output)
			s_output->RequestReconnect();
	}
} // namespace GroovyMiSTer
