// Copyright 2026 iso2rvz contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// iso2rvz: replacement for Dolphin's LogManager. Messages go to stderr, filtered by level.

#include "Common/Logging/Log.h"

#include <cstdio>

#include <fmt/format.h>

#include "compat/Log.h"

namespace Common::Log
{
static LogLevel s_max_level = LogLevel::LERROR;

void SetMaxLogLevel(LogLevel level)
{
  s_max_level = level;
}

void GenericLogFmtImpl(LogLevel level, LogType type, const char* file, int line,
                       fmt::string_view format, const fmt::format_args& args)
{
  if (static_cast<int>(level) > static_cast<int>(s_max_level))
    return;

  const char level_char = LOG_LEVEL_TO_CHAR[static_cast<int>(level)];
  fmt::print(stderr, "[{}] {}\n", level_char, fmt::vformat(format, args));
}
}  // namespace Common::Log
