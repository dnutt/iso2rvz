// Copyright 2026 iso2rvz contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/Logging/Log.h"

namespace Common::Log
{
// Messages more verbose than this level are discarded. Defaults to LERROR.
void SetMaxLogLevel(LogLevel level);
}  // namespace Common::Log
