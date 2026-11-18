// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinTool/BakeCommand.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <OptionParser.h>
#include <fmt/ostream.h>

#include "Common/FileUtil.h"
#include "GxBake/AssetBaker.h"
#include "UICommon/UICommon.h"

namespace DolphinTool
{
int BakeCommand(const std::vector<std::string>& args)
{
  optparse::OptionParser parser;
  parser.usage("usage: bake [options]...\n\n"
               "Converts a GxBake assets folder (a game folder's assets/) into GameCube-native\n"
               "files, the same way Dolphin does when it boots the folder.");

  parser.add_option("-i", "--input")
      .type("string")
      .action("store")
      .help("Path to the assets FOLDER (or the game folder that holds assets/).")
      .metavar("FOLDER");
  parser.add_option("-o", "--output")
      .type("string")
      .action("store")
      .help("Optional. Copy the baked tree (outputs plus files that pass through) to FOLDER.")
      .metavar("FOLDER");

  parser.add_option("-u", "--user")
      .type("string")
      .action("store")
      .help("Optional. User folder path, which holds the GxBake cache (Cache/GxBake).")
      .metavar("PATH");

  const optparse::Values& options = parser.parse_args(args);

  UICommon::SetUserDirectory(options["user"]);
  UICommon::Init();

  std::string input = options["input"];
  if (input.empty())
  {
    fmt::print(std::cerr, "Error: No input set\n");
    return EXIT_FAILURE;
  }
  if (input.back() != '/' && input.back() != '\\')
    input += '/';
  if (File::IsDirectory(input + "assets/"))
    input += "assets/";
  if (!File::IsDirectory(input))
  {
    fmt::print(std::cerr, "Error: {} is not a folder\n", input);
    return EXIT_FAILURE;
  }

  GxBake::AssetBaker baker(input);
  const std::vector<GxBake::BakedFile> files = baker.Bake();

  std::string output = options["output"];
  if (!output.empty() && output.back() != '/' && output.back() != '\\')
    output += '/';
  for (const GxBake::BakedFile& file : files)
  {
    fmt::print(std::cout, "{}\t{}\n", file.disc_path, File::GetSize(file.host_path));
    if (output.empty())
      continue;
    const std::string dest = output + file.disc_path;
    File::CreateFullPath(dest);
    if (!File::Copy(file.host_path, dest, true))
    {
      fmt::print(std::cerr, "Error: could not write {}\n", dest);
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}
}  // namespace DolphinTool
