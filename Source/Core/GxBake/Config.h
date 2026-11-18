// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/Config/Config.h"

namespace Config
{
// [GxBake] in Dolphin.ini.
// Bake a game folder's assets/ tree when the game is loaded.
extern const Info<bool> GXBAKE_ENABLED;
// Re-bake while the game runs, replacing outputs whose size did not change.
extern const Info<bool> GXBAKE_LIVE;
// Hand formats the native baker does not read to the GxBake Python tool.
extern const Info<bool> GXBAKE_PYTHON_FALLBACK;
// How to run the Python tool, e.g. "gxbake" or "python3 -m gxbake". Empty: find it on PATH.
extern const Info<std::string> GXBAKE_PYTHON_COMMAND;
}  // namespace Config
