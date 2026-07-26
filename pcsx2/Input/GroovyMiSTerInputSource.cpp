// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Input/GroovyMiSTerInputSource.h"
#include "Input/InputManager.h"

#include "Config.h"

#include "common/Console.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include "groovymister_wrapper.h"

#include <cmath>

// Display names, in the order the Groovy protocol packs them into its button bitmap.
// Groovy's button space is generic positions (Button 1..12 on bits 4..15; D-pad on bits
// 0-3) - the MiSTer OSD shows per-controller-type labels (DualShock/Xbox names) only as a
// mapping aid, but positions are what travel and the physical pad may not be a DualShock.
// So the position number is the truthful, controller-agnostic label; we show it as the same
// B# used in the setting identifier. See Groovy_MiSTer build_output/INPUTS_V2_HANDOFF.md.
static const char* s_button_names[GroovyMiSTerInputSource::NUM_BUTTONS] = {
	"D-Pad Right", "D-Pad Left", "D-Pad Down", "D-Pad Up",
	"B1", "B2", "B3", "B4", "B5", "B6", "B7", "B8", "B9", "B10", "B11", "B12"};

// Stable identifiers written into the INI. Never reorder these: doing so would silently
// remap every existing user binding.
static const char* s_button_setting_names[GroovyMiSTerInputSource::NUM_BUTTONS] = {
	"DPadRight", "DPadLeft", "DPadDown", "DPadUp",
	"B1", "B2", "B3", "B4", "B5", "B6", "B7", "B8", "B9", "B10", "B11", "B12"};

static const char* s_axis_names[GroovyMiSTerInputSource::NUM_AXES] = {
	"Left X", "Left Y", "Right X", "Right Y", "Left Trigger", "Right Trigger"};

static const char* s_axis_setting_names[GroovyMiSTerInputSource::NUM_AXES] = {
	"LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger"};

// Default generic mapping, following the canonical Groovy PS layout (inputs v2; see
// GMW_JOY_CROSS..GMW_JOY_R3). NOTE: cores older than inputs v2 used RetroArch's
// mister_joypad order for B5..B8 (Select,Start,L1,R1) - on those, Automatic Mapping
// will cross those four buttons and the user must bind manually.
static const GenericInputBinding s_generic_binding_button_mapping[GroovyMiSTerInputSource::NUM_BUTTONS] = {
	GenericInputBinding::DPadRight, // GMW_JOY_RIGHT
	GenericInputBinding::DPadLeft, // GMW_JOY_LEFT
	GenericInputBinding::DPadDown, // GMW_JOY_DOWN
	GenericInputBinding::DPadUp, // GMW_JOY_UP
	GenericInputBinding::Cross, // B1
	GenericInputBinding::Circle, // B2
	GenericInputBinding::Square, // B3
	GenericInputBinding::Triangle, // B4
	GenericInputBinding::L1, // B5
	GenericInputBinding::R1, // B6
	GenericInputBinding::Select, // B7
	GenericInputBinding::Start, // B8
	GenericInputBinding::L2, // B9
	GenericInputBinding::R2, // B10
	GenericInputBinding::L3, // B11
	GenericInputBinding::R3, // B12
};

// The trigger axes deliberately map to Unknown: L2/R2 default to the digital bits above,
// which work in every MiSTer OSD mode (analog triggers only stream with Joysticks=Analog
// on a v2 session). Users wanting analog L2/R2 pressure can bind +LeftTrigger/+RightTrigger
// manually.
static const GenericInputBinding s_generic_binding_axis_mapping[GroovyMiSTerInputSource::NUM_AXES][2] = {
	{GenericInputBinding::LeftStickLeft, GenericInputBinding::LeftStickRight},
	{GenericInputBinding::LeftStickUp, GenericInputBinding::LeftStickDown},
	{GenericInputBinding::RightStickLeft, GenericInputBinding::RightStickRight},
	{GenericInputBinding::RightStickUp, GenericInputBinding::RightStickDown},
	{GenericInputBinding::Unknown, GenericInputBinding::Unknown},
	{GenericInputBinding::Unknown, GenericInputBinding::Unknown},
};

GroovyMiSTerInputSource::GroovyMiSTerInputSource() = default;

GroovyMiSTerInputSource::~GroovyMiSTerInputSource() = default;

bool GroovyMiSTerInputSource::Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
	// Nothing to open. The Groovy connection - including the input subscription - is owned
	// by the video path, because the core only accepts an input subscribe inside its
	// CMD_INIT handshake. We just read whatever that connection has cached.
	//
	// The two MiSTer pads are advertised statically via EnumerateDevices() for the whole time
	// the source is enabled, so the Controller Settings window (which enumerates on open) shows
	// them and they can be Automatic-Mapped with the game stopped. We deliberately do not fire
	// per-device connect/disconnect signals: they are not hotplug hardware, and Initialize is
	// also called for a source that is about to be disabled, which would spam signals for every
	// non-MiSTer user.
	m_initialized = true;
	m_streaming = false;
	for (u32 i = 0; i < NUM_CONTROLLERS; i++)
	{
		m_controllers[i].last_buttons = 0;
		m_controllers[i].last_axes.fill(0);
		m_controllers[i].last_rumble.fill(0);
	}
	return true;
}

void GroovyMiSTerInputSource::UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
}

bool GroovyMiSTerInputSource::ReloadDevices()
{
	return false;
}

void GroovyMiSTerInputSource::Shutdown()
{
	// Held-button release happens in PollEvents on stream-drop (no settings lock held there);
	// we avoid calling InvokeEvents from here since Shutdown runs under the settings lock.
	m_initialized = false;
	m_streaming = false;
}

bool GroovyMiSTerInputSource::IsInitialized()
{
	return m_initialized;
}

void GroovyMiSTerInputSource::ReleaseHeldInputs()
{
	// Emit a release for anything currently held, so the emulated pad does not latch a button
	// or a deflected stick when the MiSTer stream drops (or the source is disabled).
	for (u32 i = 0; i < NUM_CONTROLLERS; i++)
	{
		ControllerData& cd = m_controllers[i];

		for (u32 b = 0; b < NUM_BUTTONS; b++)
		{
			if (cd.last_buttons & (1u << b))
				InputManager::InvokeEvents(
					MakeGenericControllerButtonKey(InputSourceType::GroovyMiSTer, i, static_cast<s32>(b)),
					0.0f, s_generic_binding_button_mapping[b]);
		}

		for (u32 a = 0; a < NUM_AXES; a++)
		{
			if (cd.last_axes[a] != 0)
				InputManager::InvokeEvents(
					MakeGenericControllerAxisKey(InputSourceType::GroovyMiSTer, i, static_cast<s32>(a)),
					0.0f, GenericInputBinding::Unknown);
		}

		cd.last_buttons = 0;
		cd.last_axes.fill(0);
		// No 0/0 rumble datagram needed: the core force-stops motors on session close, and
		// with the stream down there is nobody listening anyway. Clearing the cache makes a
		// future session start from a known-idle state.
		cd.last_rumble.fill(0);
	}
}

void GroovyMiSTerInputSource::PollEvents()
{
	// Pad state only exists while the Groovy video connection is live (the shared client is
	// owned by the video path, and the input socket rides that connection). Device *presence*
	// is separate - the pads stay advertised for binding (EnumerateDevices) either way.
	if (!gmw_is_connected())
	{
		if (m_streaming)
		{
			// Stream just dropped - release anything held so the emulated pad does not stick.
			ReleaseHeldInputs();
			m_streaming = false;
		}
		return;
	}

	if (!m_streaming)
	{
		// Stream just (re)started - clear the baseline so the current state is re-emitted.
		for (u32 i = 0; i < NUM_CONTROLLERS; i++)
		{
			m_controllers[i].last_buttons = 0;
			m_controllers[i].last_axes.fill(0);
			m_controllers[i].last_rumble.fill(0);
		}
		m_streaming = true;
	}

	// Non-blocking; drains whatever the MiSTer has sent on UDP 32101.
	gmw_pollInputs();

	gmw_fpgaJoyInputs joy{};
	gmw_getJoyInputs(&joy);

	// Bits 16-31 of a v2 mask are reserved; drop them so a future core cannot invoke
	// buttons we never advertised.
	constexpr u32 button_mask = (1u << NUM_BUTTONS) - 1;
	const u32 buttons[NUM_CONTROLLERS] = {joy.joy1 & button_mask, joy.joy2 & button_mask};

	// Diagnostic (LogVerbosity >= 1): the MiSTer side has to actually forward pad data, and a
	// session log showed the input socket registered but no packets. Log on change so we can
	// confirm whether PCSX2 is receiving anything at all.
	if (EmuConfig.GroovyMiSTer.LogVerbosity >= 1 &&
		(buttons[0] != m_controllers[0].last_buttons || buttons[1] != m_controllers[1].last_buttons))
	{
		INFO_LOG("MiSTer inputs: frame={} joy1=0x{:08X} joy2=0x{:08X}", joy.joyFrame, joy.joy1, joy.joy2);
	}
	const std::array<s16, NUM_AXES> axes[NUM_CONTROLLERS] = {
		{joy.joy1LXAnalog, joy.joy1LYAnalog, joy.joy1RXAnalog, joy.joy1RYAnalog,
			joy.joy1LTAnalog, joy.joy1RTAnalog},
		{joy.joy2LXAnalog, joy.joy2LYAnalog, joy.joy2RXAnalog, joy.joy2RYAnalog,
			joy.joy2LTAnalog, joy.joy2RTAnalog},
	};

	for (u32 i = 0; i < NUM_CONTROLLERS; i++)
		CheckForStateChanges(i, buttons[i], axes[i]);
}

float GroovyMiSTerInputSource::NormalizeAxis(u32 axis, s16 raw)
{
	// Triggers carry an unsigned byte (0..255) -> 0..1.
	if (axis == AXIS_LEFTTRIGGER || axis == AXIS_RIGHTTRIGGER)
		return static_cast<float>(raw) / 255.0f;

	// Sticks carry a signed char (-128..127). Normalise to -1..1, taking the asymmetry
	// of two's complement into account so full deflection really reaches 1.0.
	return static_cast<float>(raw) / (raw < 0 ? 128.0f : 127.0f);
}

void GroovyMiSTerInputSource::CheckForStateChanges(u32 index, u32 buttons, const std::array<s16, NUM_AXES>& axes)
{
	ControllerData& cd = m_controllers[index];

	if (buttons != cd.last_buttons)
	{
		const u32 changed = buttons ^ cd.last_buttons;
		for (u32 b = 0; b < NUM_BUTTONS; b++)
		{
			const u32 mask = 1u << b;
			if (!(changed & mask))
				continue;

			const float value = (buttons & mask) ? 1.0f : 0.0f;
			InputManager::InvokeEvents(
				MakeGenericControllerButtonKey(InputSourceType::GroovyMiSTer, index, static_cast<s32>(b)),
				value, s_generic_binding_button_mapping[b]);
		}

		cd.last_buttons = buttons;
	}

	for (u32 a = 0; a < NUM_AXES; a++)
	{
		if (axes[a] == cd.last_axes[a])
			continue;

		InputManager::InvokeEvents(
			MakeGenericControllerAxisKey(InputSourceType::GroovyMiSTer, index, static_cast<s32>(a)),
			NormalizeAxis(a, axes[a]), GenericInputBinding::Unknown);

		cd.last_axes[a] = axes[a];
	}
}

std::vector<std::pair<std::string, std::string>> GroovyMiSTerInputSource::EnumerateDevices()
{
	// Always advertise both pads while the source is enabled - even when not streaming. They
	// are network devices with no hotplug, so this is what makes them appear in the Automatic
	// Mapping menu / Detected Devices list with the game stopped. Events only actually flow
	// while streaming (PollEvents), which is fine: bindings are just INI strings.
	std::vector<std::pair<std::string, std::string>> ret;
	for (u32 i = 0; i < NUM_CONTROLLERS; i++)
		ret.emplace_back(fmt::format("MiSTer-{}", i), fmt::format("MiSTer Joystick {}", i + 1));
	return ret;
}

std::vector<InputBindingKey> GroovyMiSTerInputSource::EnumerateMotors()
{
	// Inputs v2 added a rumble channel (client -> core on the inputs socket). Advertise the
	// motors unconditionally, like the pads themselves: whether they do anything depends on
	// runtime state (v2 session, MiSTer OSD Rumble=On, pad with motors), but the bindings
	// are just INI strings. gmw_send_rumble() no-ops when it cannot be delivered.
	std::vector<InputBindingKey> ret;

	InputBindingKey key = {};
	key.source_type = InputSourceType::GroovyMiSTer;
	key.source_subtype = InputSubclass::ControllerMotor;

	for (u32 i = 0; i < NUM_CONTROLLERS; i++)
	{
		key.source_index = i;
		for (u32 m = 0; m < NUM_MOTORS; m++)
		{
			key.data = m;
			ret.push_back(key);
		}
	}

	return ret;
}

void GroovyMiSTerInputSource::SendRumble(u32 index)
{
	const ControllerData& cd = m_controllers[index];
	const u8 large = cd.last_rumble[0];
	const u8 small = cd.last_rumble[1];
	const bool caps_rumble = (gmw_get_input_caps() & GMW_CAP_RUMBLE) != 0;

	// Rumble diagnostic (LogVerbosity >= 1): logged on every would-be send, BEFORE the caps
	// gate, so the trace distinguishes the three failure modes - motors not driving us at all
	// (no lines), driving us but caps not negotiated (caps=0, v1 fallback), or emitting fine.
	// It also exposes the value/rate stream to correlate with the MiSTer's [RUMBLE] log
	// (groovy.cpp, LOG level 2). Keep it: it is a cheap, permanent parallel to the input log.
	if (EmuConfig.GroovyMiSTer.LogVerbosity >= 1)
		INFO_LOG("MiSTer rumble: player={} large={} small={} caps_rumble={}", index, large, small, caps_rumble ? 1 : 0);

	// The Groovy core repeats the last value until replaced, so only a change may hit the
	// wire; the core force-stops motors on session close, and drops rumble entirely unless
	// GMW_CAP_RUMBLE was negotiated - don't bother it (or an old core) otherwise.
	if (!caps_rumble)
		return;

	// UDP has no delivery guarantee, and the core holds the last value it saw (its FF effect
	// runs for ~32s), so a single dropped "stop" datagram would leave a motor buzzing long
	// after the game silenced it. A stop is a one-shot transition - UpdateMotorState only
	// reaches SendRumble on a change, so a (0,0) here means we just went from buzzing to idle
	// - which makes re-sending it a few times cheap insurance: the core de-dupes identical
	// values, so the extra datagrams are harmless no-ops. Non-zero values are sent once (the
	// game re-drives them continuously anyway; spamming them would only restart the effect).
	constexpr int STOP_RESENDS = 3;
	const int sends = (large == 0 && small == 0) ? STOP_RESENDS : 1;
	for (int i = 0; i < sends; i++)
		gmw_send_rumble(static_cast<u8>(index), large, small);
}

void GroovyMiSTerInputSource::UpdateMotorState(InputBindingKey key, float intensity)
{
	if (key.source_subtype != InputSubclass::ControllerMotor || key.source_index >= NUM_CONTROLLERS ||
		key.data >= NUM_MOTORS)
		return;

	ControllerData& cd = m_controllers[key.source_index];
	const u8 value = static_cast<u8>(std::lround(intensity * 255.0f));
	if (cd.last_rumble[key.data] == value)
		return;

	cd.last_rumble[key.data] = value;
	SendRumble(key.source_index);
}

void GroovyMiSTerInputSource::UpdateMotorState(
	InputBindingKey large_key, InputBindingKey small_key, float large_intensity, float small_intensity)
{
	if (large_key.source_index != small_key.source_index ||
		large_key.source_subtype != InputSubclass::ControllerMotor ||
		small_key.source_subtype != InputSubclass::ControllerMotor)
	{
		// Mapped to different pads; fall back to two independent updates.
		UpdateMotorState(large_key, large_intensity);
		UpdateMotorState(small_key, small_intensity);
		return;
	}

	if (large_key.source_index >= NUM_CONTROLLERS || large_key.data >= NUM_MOTORS ||
		small_key.data >= NUM_MOTORS)
		return;

	ControllerData& cd = m_controllers[large_key.source_index];
	const u8 large_value = static_cast<u8>(std::lround(large_intensity * 255.0f));
	const u8 small_value = static_cast<u8>(std::lround(small_intensity * 255.0f));
	if (cd.last_rumble[large_key.data] == large_value && cd.last_rumble[small_key.data] == small_value)
		return;

	cd.last_rumble[large_key.data] = large_value;
	cd.last_rumble[small_key.data] = small_value;
	SendRumble(large_key.source_index);
}

InputLayout GroovyMiSTerInputSource::GetControllerLayout(u32 index)
{
	// Since inputs v2 the core's button space is semantically PlayStation by contract.
	return InputLayout::Playstation;
}

std::optional<InputBindingKey> GroovyMiSTerInputSource::ParseKeyString(
	const std::string_view device, const std::string_view binding)
{
	if (!device.starts_with("MiSTer-") || binding.empty())
		return std::nullopt;

	const std::optional<s32> player_id = StringUtil::FromChars<s32>(device.substr(7));
	if (!player_id.has_value() || player_id.value() < 0 || player_id.value() >= static_cast<s32>(NUM_CONTROLLERS))
		return std::nullopt;

	InputBindingKey key = {};
	key.source_type = InputSourceType::GroovyMiSTer;
	key.source_index = static_cast<u32>(player_id.value());

	// Axes are prefixed with a sign, e.g. "MiSTer-0/+LeftX".
	if (binding[0] == '+' || binding[0] == '-')
	{
		const std::string_view axis_name = binding.substr(1);
		for (u32 i = 0; i < NUM_AXES; i++)
		{
			if (axis_name == s_axis_setting_names[i])
			{
				key.source_subtype = InputSubclass::ControllerAxis;
				key.data = i;
				key.modifier = (binding[0] == '-') ? InputModifier::Negate : InputModifier::None;
				return key;
			}
		}

		return std::nullopt;
	}

	if (binding == "LargeMotor")
	{
		key.source_subtype = InputSubclass::ControllerMotor;
		key.data = 0;
		return key;
	}
	if (binding == "SmallMotor")
	{
		key.source_subtype = InputSubclass::ControllerMotor;
		key.data = 1;
		return key;
	}

	for (u32 i = 0; i < NUM_BUTTONS; i++)
	{
		if (binding == s_button_setting_names[i])
		{
			key.source_subtype = InputSubclass::ControllerButton;
			key.data = i;
			return key;
		}
	}

	return std::nullopt;
}

TinyString GroovyMiSTerInputSource::ConvertKeyToString(InputBindingKey key, bool display, bool migration)
{
	TinyString ret;

	if (key.source_type != InputSourceType::GroovyMiSTer)
		return ret;

	if (key.source_subtype == InputSubclass::ControllerAxis && key.data < std::size(s_axis_setting_names))
	{
		const char modifier = (key.modifier == InputModifier::Negate) ? '-' : '+';
		if (display)
			ret.format("MiSTer-{} {}{}", static_cast<u32>(key.source_index), modifier, s_axis_names[key.data]);
		else
			ret.format("MiSTer-{}/{}{}", static_cast<u32>(key.source_index), modifier, s_axis_setting_names[key.data]);
	}
	else if (key.source_subtype == InputSubclass::ControllerButton && key.data < std::size(s_button_setting_names))
	{
		if (display)
			ret.format("MiSTer-{} {}", static_cast<u32>(key.source_index), s_button_names[key.data]);
		else
			ret.format("MiSTer-{}/{}", static_cast<u32>(key.source_index), s_button_setting_names[key.data]);
	}
	else if (key.source_subtype == InputSubclass::ControllerMotor && key.data < NUM_MOTORS)
	{
		if (display)
			ret.format("MiSTer-{} {} Motor", static_cast<u32>(key.source_index), key.data ? "Small" : "Large");
		else
			ret.format("MiSTer-{}/{}Motor", static_cast<u32>(key.source_index), key.data ? "Small" : "Large");
	}

	return ret;
}

TinyString GroovyMiSTerInputSource::ConvertKeyToIcon(InputBindingKey key)
{
	// No icon font for MiSTer pads; the caller falls back to the text name.
	return {};
}

bool GroovyMiSTerInputSource::GetGenericBindingMapping(
	const std::string_view device, InputManager::GenericInputBindingMapping* mapping)
{
	if (!device.starts_with("MiSTer-"))
		return false;

	const std::optional<s32> player_id = StringUtil::FromChars<s32>(device.substr(7));
	if (!player_id.has_value() || player_id.value() < 0 || player_id.value() >= static_cast<s32>(NUM_CONTROLLERS))
		return false;

	const s32 pid = player_id.value();

	for (u32 i = 0; i < NUM_AXES; i++)
	{
		const GenericInputBinding negative = s_generic_binding_axis_mapping[i][0];
		const GenericInputBinding positive = s_generic_binding_axis_mapping[i][1];
		if (negative != GenericInputBinding::Unknown)
			mapping->emplace_back(negative, fmt::format("MiSTer-{}/-{}", pid, s_axis_setting_names[i]));
		if (positive != GenericInputBinding::Unknown)
			mapping->emplace_back(positive, fmt::format("MiSTer-{}/+{}", pid, s_axis_setting_names[i]));
	}

	for (u32 i = 0; i < NUM_BUTTONS; i++)
	{
		const GenericInputBinding binding = s_generic_binding_button_mapping[i];
		if (binding != GenericInputBinding::Unknown)
			mapping->emplace_back(binding, fmt::format("MiSTer-{}/{}", pid, s_button_setting_names[i]));
	}

	mapping->emplace_back(GenericInputBinding::LargeMotor, fmt::format("MiSTer-{}/LargeMotor", pid));
	mapping->emplace_back(GenericInputBinding::SmallMotor, fmt::format("MiSTer-{}/SmallMotor", pid));

	return true;
}
