// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "GxBake/Process.h"

#ifdef _WIN32
#include <Windows.h>

#include "Common/StringUtil.h"
#else
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace GxBake
{
std::vector<std::string> SplitCommandLine(const std::string& command)
{
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false, have = false;
  for (char c : command)
  {
    if (c == '"')
    {
      quoted = !quoted;
      have = true;
    }
    else if (c == ' ' && !quoted)
    {
      if (have)
        out.push_back(cur);
      cur.clear();
      have = false;
    }
    else
    {
      cur += c;
      have = true;
    }
  }
  if (have)
    out.push_back(cur);
  return out;
}

#ifdef _WIN32
static std::wstring QuoteArg(const std::wstring& arg)
{
  if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos)
    return arg;
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (wchar_t c : arg)
  {
    if (c == L'\\')
    {
      backslashes++;
      continue;
    }
    if (c == L'"')
      out.append(backslashes * 2 + 1, L'\\');
    else
      out.append(backslashes, L'\\');
    backslashes = 0;
    out += c;
  }
  out.append(backslashes * 2, L'\\');
  out += L'"';
  return out;
}

std::optional<int> RunProcess(const std::vector<std::string>& args, const std::string& log_path)
{
  if (args.empty())
    return std::nullopt;
  std::wstring cmdline;
  for (const std::string& a : args)
  {
    if (!cmdline.empty())
      cmdline += L' ';
    cmdline += QuoteArg(UTF8ToWString(a));
  }
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE log = CreateFileW(UTF8ToWString(log_path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nullptr;
  si.hStdOutput = log;
  si.hStdError = log;
  PROCESS_INFORMATION pi{};
  const BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                 nullptr, nullptr, &si, &pi);
  if (log != INVALID_HANDLE_VALUE)
    CloseHandle(log);
  if (!ok)
    return std::nullopt;
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return static_cast<int>(code);
}
#else
std::optional<int> RunProcess(const std::vector<std::string>& args, const std::string& log_path)
{
  if (args.empty())
    return std::nullopt;
  std::vector<char*> argv;
  for (const std::string& a : args)
    argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log_path.c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
  pid_t pid;
  const int err = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (err != 0)
    return std::nullopt;
  int status = 0;
  while (waitpid(pid, &status, 0) < 0)
  {
    if (errno != EINTR)
      return std::nullopt;
  }
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  return 128;
}
#endif
}  // namespace GxBake
