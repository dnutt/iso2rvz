// Copyright 2026 iso2rvz contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// iso2rvz: there is no emulated NAND to read save banners from, so every banner is invalid.

#include "DiscIO/WiiSaveBanner.h"

namespace DiscIO
{
WiiSaveBanner::WiiSaveBanner(u64 title_id) : m_header{}, m_valid(false)
{
}

std::string WiiSaveBanner::GetName() const
{
  return {};
}

std::string WiiSaveBanner::GetDescription() const
{
  return {};
}

std::vector<u32> WiiSaveBanner::GetBanner(u32* width, u32* height) const
{
  *width = 0;
  *height = 0;
  return {};
}
}  // namespace DiscIO
