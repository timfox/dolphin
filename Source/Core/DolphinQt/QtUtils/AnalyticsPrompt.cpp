// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/QtUtils/AnalyticsPrompt.h"

#include <QWidget>

#include "Core/Config/MainSettings.h"
#include "Core/DolphinAnalytics.h"
#include "DolphinQt/Settings.h"

void ShowAnalyticsPrompt(QWidget* parent)
{
  (void)parent;

  Config::SetBase(Config::MAIN_ANALYTICS_PERMISSION_ASKED, true);
  Settings::Instance().SetAnalyticsEnabled(false);

  DolphinAnalytics::Instance().ReloadConfig();
}
