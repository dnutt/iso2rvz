// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// iso2rvz: cut-down replacement for Dolphin's Core/IOS/ES/Formats.cpp. The functions below are
// copied verbatim from Dolphin, except for TicketReader::GetTitleKey, which decrypts the title
// key directly instead of going through the emulated IOSC.

#include "Core/IOS/ES/Formats.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "Common/Crypto/AES.h"
#include "Common/Crypto/SHA1.h"
#include "Common/MsgHandler.h"
#include "Common/Swap.h"

namespace IOS::ES
{
namespace Titles
{
constexpr u64 SYSTEM_MENU = 0x0000000100000002;
}

// Common keys, as in Dolphin's Core/IOS/IOSC.cpp.
constexpr std::array<u8, 16> RETAIL_COMMON_KEY = {0xeb, 0xe4, 0x2a, 0x22, 0x5e, 0x85, 0x93, 0xe4,
                                                  0x48, 0xd9, 0xc5, 0x45, 0x73, 0x81, 0xaa, 0xf7};
constexpr std::array<u8, 16> RVT_COMMON_KEY = {0xa1, 0x60, 0x4a, 0x6a, 0x71, 0x23, 0xb5, 0x29,
                                               0xae, 0x8b, 0xec, 0x32, 0xc8, 0x16, 0xfc, 0xaa};
constexpr std::array<u8, 16> KOREAN_COMMON_KEY = {0x63, 0xb8, 0x2b, 0xb4, 0xf4, 0x61, 0x4e, 0x2e,
                                                  0x13, 0xf2, 0xfe, 0xfb, 0xba, 0x4c, 0x9b, 0x7e};

bool IsTitleType(u64 title_id, TitleType title_type)
{
  return static_cast<u32>(title_id >> 32) == static_cast<u32>(title_type);
}

bool IsDiscTitle(u64 title_id)
{
  return IsTitleType(title_id, TitleType::Game) ||
         IsTitleType(title_id, TitleType::GameWithChannel);
}

bool IsChannel(u64 title_id)
{
  if (title_id == Titles::SYSTEM_MENU)
    return true;

  return IsTitleType(title_id, TitleType::Channel) ||
         IsTitleType(title_id, TitleType::SystemChannel) ||
         IsTitleType(title_id, TitleType::GameWithChannel) ||
         IsTitleType(title_id, TitleType::HiddenChannel);
}

bool Content::IsShared() const
{
  return (type & 0x8000) != 0;
}

bool Content::IsOptional() const
{
  return (type & 0x4000) != 0;
}

bool operator==(const Content& lhs, const Content& rhs)
{
  auto fields = [](const Content& c) { return std::tie(c.id, c.index, c.type, c.size, c.sha1); };
  return fields(lhs) == fields(rhs);
}

SignedBlobReader::SignedBlobReader(std::vector<u8> bytes) : m_bytes(std::move(bytes))
{
}

const std::vector<u8>& SignedBlobReader::GetBytes() const
{
  return m_bytes;
}

void SignedBlobReader::SetBytes(std::vector<u8> bytes)
{
  m_bytes = std::move(bytes);
}

static size_t GetIssuerOffset(SignatureType signature_type)
{
  switch (signature_type)
  {
  case SignatureType::RSA2048:
    return offsetof(SignatureRSA2048, issuer);
  case SignatureType::RSA4096:
    return offsetof(SignatureRSA4096, issuer);
  case SignatureType::ECC:
    return offsetof(SignatureECC, issuer);
  default:
    return 0;
  }
}

std::array<u8, 20> SignedBlobReader::GetSha1() const
{
  const size_t skip = GetIssuerOffset(GetSignatureType());
  return Common::SHA1::CalculateDigest(m_bytes.data() + skip, m_bytes.size() - skip);
}

bool SignedBlobReader::IsSignatureValid() const
{
  // Too small for the certificate type.
  if (m_bytes.size() < sizeof(SignatureType))
    return false;

  // Too small to contain the whole signature data.
  const size_t signature_size = GetSignatureSize();
  if (signature_size == 0 || m_bytes.size() < signature_size)
    return false;

  return true;
}

SignatureType SignedBlobReader::GetSignatureType() const
{
  return static_cast<SignatureType>(Common::swap32(m_bytes.data()));
}

template <typename T, typename It>
static std::vector<u8> DetailGetSignatureData(It begin)
{
  const auto signature_begin = begin + offsetof(T, sig);
  return std::vector<u8>(signature_begin, signature_begin + sizeof(T::sig));
}

std::vector<u8> SignedBlobReader::GetSignatureData() const
{
  switch (GetSignatureType())
  {
  case SignatureType::RSA4096:
    return DetailGetSignatureData<SignatureRSA4096>(m_bytes.cbegin());
  case SignatureType::RSA2048:
    return DetailGetSignatureData<SignatureRSA2048>(m_bytes.cbegin());
  case SignatureType::ECC:
    return DetailGetSignatureData<SignatureECC>(m_bytes.cbegin());
  default:
    return {};
  }
}

size_t SignedBlobReader::GetSignatureSize() const
{
  switch (GetSignatureType())
  {
  case SignatureType::RSA4096:
    return sizeof(SignatureRSA4096);
  case SignatureType::RSA2048:
    return sizeof(SignatureRSA2048);
  case SignatureType::ECC:
    return sizeof(SignatureECC);
  default:
    return 0;
  }
}

template <typename T>
static std::string DetailGetIssuer(const u8* bytes)
{
  const char* issuer = reinterpret_cast<const char*>(bytes + offsetof(T, issuer));
  return {issuer, strnlen(issuer, sizeof(T::issuer))};
}

std::string SignedBlobReader::GetIssuer() const
{
  switch (GetSignatureType())
  {
  case SignatureType::RSA4096:
    return DetailGetIssuer<SignatureRSA4096>(m_bytes.data());
  case SignatureType::RSA2048:
    return DetailGetIssuer<SignatureRSA2048>(m_bytes.data());
  case SignatureType::ECC:
    return DetailGetIssuer<SignatureECC>(m_bytes.data());
  default:
    return "";
  }
}

bool IsValidTMDSize(size_t size)
{
  return size >= sizeof(TMDHeader) && size <= 0x49e4;
}

TMDReader::TMDReader(std::vector<u8> bytes) : SignedBlobReader(std::move(bytes))
{
}

bool TMDReader::IsValid() const
{
  if (!IsSignatureValid())
    return false;

  if (m_bytes.size() < sizeof(TMDHeader))
  {
    // TMD is too small to contain its base fields.
    return false;
  }

  if (m_bytes.size() < sizeof(TMDHeader) + GetNumContents() * sizeof(Content))
  {
    // TMD is too small to contain all its expected content entries.
    return false;
  }

  return true;
}

u16 TMDReader::GetBootIndex() const
{
  return Common::swap16(m_bytes.data() + offsetof(TMDHeader, boot_index));
}

u64 TMDReader::GetIOSId() const
{
  return Common::swap64(m_bytes.data() + offsetof(TMDHeader, ios_id));
}

u64 TMDReader::GetTitleId() const
{
  return Common::swap64(m_bytes.data() + offsetof(TMDHeader, title_id));
}

u32 TMDReader::GetTitleFlags() const
{
  return Common::swap32(m_bytes.data() + offsetof(TMDHeader, title_flags));
}

u16 TMDReader::GetTitleVersion() const
{
  return Common::swap16(m_bytes.data() + offsetof(TMDHeader, title_version));
}

u16 TMDReader::GetGroupId() const
{
  return Common::swap16(m_bytes.data() + offsetof(TMDHeader, group_id));
}

DiscIO::Region TMDReader::GetRegion() const
{
  if (!IsChannel(GetTitleId()))
    return DiscIO::Region::Unknown;

  if (GetTitleId() == Titles::SYSTEM_MENU)
    return DiscIO::GetSysMenuRegion(GetTitleVersion());

  const DiscIO::Region region =
      static_cast<DiscIO::Region>(Common::swap16(m_bytes.data() + offsetof(TMDHeader, region)));

  return region <= DiscIO::Region::NTSC_K ? region : DiscIO::Region::Unknown;
}

bool TMDReader::IsvWii() const
{
  return *(m_bytes.data() + offsetof(TMDHeader, is_vwii));
}

u16 TMDReader::GetNumContents() const
{
  return Common::swap16(m_bytes.data() + offsetof(TMDHeader, num_contents));
}

bool TMDReader::GetContent(u16 index, Content* content) const
{
  if (!IsValid() || index >= GetNumContents())
  {
    return false;
  }

  const u8* content_base = m_bytes.data() + sizeof(TMDHeader) + index * sizeof(Content);
  content->id = Common::swap32(content_base + offsetof(Content, id));
  content->index = Common::swap16(content_base + offsetof(Content, index));
  content->type = Common::swap16(content_base + offsetof(Content, type));
  content->size = Common::swap64(content_base + offsetof(Content, size));
  std::copy_n(content_base + offsetof(Content, sha1), content->sha1.size(), content->sha1.begin());

  return true;
}

std::vector<Content> TMDReader::GetContents() const
{
  std::vector<Content> contents(IsValid() ? GetNumContents() : 0);
  for (size_t i = 0; i < contents.size(); ++i)
    GetContent(static_cast<u16>(i), &contents[i]);
  return contents;
}

bool TMDReader::FindContentById(u32 id, Content* content) const
{
  for (u16 index = 0; index < GetNumContents(); ++index)
  {
    if (!GetContent(index, content))
    {
      return false;
    }
    if (content->id == id)
    {
      return true;
    }
  }
  return false;
}

TicketReader::TicketReader(std::vector<u8> bytes) : SignedBlobReader(std::move(bytes))
{
}

bool TicketReader::IsValid() const
{
  if (!IsSignatureValid() || m_bytes.empty())
    return false;

  if (IsV1Ticket())
    return m_bytes.size() == GetTicketSize();

  return m_bytes.size() % sizeof(Ticket) == 0;
}

bool TicketReader::IsV1Ticket() const
{
  // Version can only be 0 or 1.
  return GetVersion() == 1;
}

size_t TicketReader::GetNumberOfTickets() const
{
  if (IsV1Ticket())
    return 1;

  return m_bytes.size() / sizeof(Ticket);
}

u32 TicketReader::GetTicketSize() const
{
  if (IsV1Ticket())
  {
    return Common::swap32(m_bytes.data() + sizeof(Ticket) +
                          offsetof(V1TicketHeader, v1_ticket_size)) +
           sizeof(Ticket);
  }

  return sizeof(Ticket);
}

u8 TicketReader::GetVersion() const
{
  return m_bytes[offsetof(Ticket, version)];
}

u32 TicketReader::GetDeviceId() const
{
  return Common::swap32(m_bytes.data() + offsetof(Ticket, device_id));
}

u64 TicketReader::GetTitleId() const
{
  return Common::swap64(m_bytes.data() + offsetof(Ticket, title_id));
}

u8 TicketReader::GetCommonKeyIndex() const
{
  return m_bytes[offsetof(Ticket, common_key_index)];
}

std::array<u8, 16> TicketReader::GetTitleKey() const
{
  u8 iv[16] = {};
  std::copy_n(&m_bytes[offsetof(Ticket, title_id)], sizeof(Ticket::title_id), iv);

  u8 index = m_bytes.at(offsetof(Ticket, common_key_index));
  if (index > 1)
  {
    PanicAlertFmt("Bad common key index for title {:016x}: {} -- using common key 0", GetTitleId(),
                  index);
    index = 0;
  }

  const bool is_rvt = GetIssuer() == "Root-CA00000002-XS00000006";
  const std::array<u8, 16>& common_key =
      index == 1 ? KOREAN_COMMON_KEY : (is_rvt ? RVT_COMMON_KEY : RETAIL_COMMON_KEY);

  std::array<u8, 16> key;
  Common::AES::CreateContextDecrypt(common_key.data())
      ->Crypt(iv, &m_bytes[offsetof(Ticket, title_key)], key.data(), key.size());
  return key;
}
}  // namespace IOS::ES
