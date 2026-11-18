// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace GxBake
{
// Runs a program (looked up on PATH) without a console window, sends its stdout and stderr to
// log_path, and waits for it. Returns the exit code, or nothing if it could not be started.
std::optional<int> RunProcess(const std::vector<std::string>& args, const std::string& log_path);

// Splits a command line on spaces, honouring double quotes.
std::vector<std::string> SplitCommandLine(const std::string& command);
}  // namespace GxBake
