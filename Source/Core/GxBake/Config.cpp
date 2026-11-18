// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "GxBake/Config.h"

namespace Config
{
const Info<bool> GXBAKE_ENABLED{{System::Main, "GxBake", "Enabled"}, true};
const Info<bool> GXBAKE_LIVE{{System::Main, "GxBake", "Live"}, true};
const Info<bool> GXBAKE_PYTHON_FALLBACK{{System::Main, "GxBake", "PythonFallback"}, true};
const Info<std::string> GXBAKE_PYTHON_COMMAND{{System::Main, "GxBake", "PythonCommand"}, ""};
}  // namespace Config
