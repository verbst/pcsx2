// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "ui_GroovyMiSTerSettingsWidget.h"

#include "SettingsWidget.h"

/// Settings for streaming the emulated frame/audio to a MiSTer FPGA over the network.
///
/// Global-only: registered in SettingsWindow only when !isPerGameSettings(). This is
/// machine-wide hardware setup (which MiSTer, which CRT, which network), so `sif` is always
/// null here and none of the per-game "Use Global Setting" plumbing applies.
class GroovyMiSTerSettingsWidget : public SettingsWidget
{
	Q_OBJECT

public:
	GroovyMiSTerSettingsWidget(SettingsWindow* settings_dialog, QWidget* parent);
	~GroovyMiSTerSettingsWidget();

private Q_SLOTS:
	void onEnabledChanged();
	void onCodecChanged();
	void onNlcPackChanged();
	void onCrtSafetyCapToggled(bool checked);
	void onBrowseSwitchresIni();

private:
	void setupCodecBinding();
	void setupInterlaceBinding();
	void addTooltips();

	Ui::GroovyMiSTerSettingsWidget m_ui;
};
