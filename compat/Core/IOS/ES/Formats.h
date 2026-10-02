// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// iso2rvz: cut-down replacement for Dolphin's Core/IOS/ES/Formats.h.
// Only the TMD/ticket parsing used by DiscIO is kept; there is no dependency on the
// emulated IOS (IOSC, filesystem, savestates). The common keys live in Formats.cpp.

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "DiscIO/Enums.h"

namespace IOS::ES
{
enum class SignatureType : u32
{
  RSA4096 = 0x00010000,
  RSA2048 = 0x00010001,
  ECC = 0x00010002,
};

#pragma pack(push, 4)
struct SignatureRSA4096
{
  SignatureType type;
  u8 sig[0x200];
  u8 fill[0x3c];
  char issuer[0x40];
};
static_assert(sizeof(SignatureRSA4096) == 0x280, "Wrong size for SignatureRSA4096");

struct SignatureRSA2048
{
  SignatureType type;
  u8 sig[0x100];
  u8 fill[0x3c];
  char issuer[0x40];
};
static_assert(sizeof(SignatureRSA2048) == 0x180, "Wrong size for SignatureRSA2048");

struct SignatureECC
{
  SignatureType type;
  std::array<u8, 60> sig;
  u8 fill[0x40];
  char issuer[0x40];
};
static_assert(sizeof(SignatureECC) == 0xc0, "Wrong size for SignatureECC");
#pragma pack(pop)

enum class TitleType : u32
{
  System = 0x00000001,
  Game = 0x00010000,
  Channel = 0x00010001,
  SystemChannel = 0x00010002,
  GameWithChannel = 0x00010004,
  DLC = 0x00010005,
  HiddenChannel = 0x00010008,
};

bool IsTitleType(u64 title_id, TitleType title_type);
bool IsDiscTitle(u64 title_id);
bool IsChannel(u64 title_id);

enum TitleFlags : u32
{
  // All official titles have this flag set.
  TITLE_TYPE_DEFAULT = 0x1,
  // Unknown.
  TITLE_TYPE_0x4 = 0x4,
  // Used for DLC titles.
  TITLE_TYPE_DATA = 0x8,
  // Unknown.
  TITLE_TYPE_0x10 = 0x10,
  // Appears to be used for WFS titles.
  TITLE_TYPE_WFS_MAYBE = 0x20,
  // Unknown.
  TITLE_TYPE_CT = 0x40,
};

#pragma pack(push, 4)
struct TMDHeader
{
  SignatureRSA2048 signature;
  u8 tmd_version;
  u8 ca_crl_version;
  u8 signer_crl_version;
  // This is usually an always 0 padding byte, which is set to 1 on vWii TMDs
  u8 is_vwii;
  u64 ios_id;
  u64 title_id;
  u32 title_flags;
  u16 group_id;
  u16 zero;
  u16 region;
  u8 ratings[16];
  u8 reserved[12];
  u8 ipc_mask[12];
  u8 reserved2[18];
  u32 access_rights;
  u16 title_version;
  u16 num_contents;
  u16 boot_index;
  u16 fill2;
};
static_assert(sizeof(TMDHeader) == 0x1e4, "TMDHeader has the wrong size");
static_assert(offsetof(TMDHeader, ios_id) == 0x184);

struct Content
{
  bool IsShared() const;
  bool IsOptional() const;
  u32 id;
  u16 index;
  u16 type;
  u64 size;
  std::array<u8, 20> sha1;
};
static_assert(sizeof(Content) == 36, "Content has the wrong size");
bool operator==(const Content&, const Content&);

struct TimeLimit
{
  u32 enabled;
  u32 seconds;
};

struct TicketView
{
  u8 version;
  u64 ticket_id;
  u32 device_id;
  u64 title_id;
  u16 access_mask;
  u32 permitted_title_id;
  u32 permitted_title_mask;
  u8 title_export_allowed;
  u8 common_key_index;
  u8 unknown2[0x30];
  u8 content_access_permissions[0x40];
  TimeLimit time_limits[8];
};
static_assert(sizeof(TicketView) == 0xd8, "TicketView has the wrong size");

// This structure is used for (signed) tickets. Technically, there are other types of tickets
// (RSA4096, ECDSA, ...). However, only RSA2048 tickets have ever been seen and these are also
// the only ticket type that is supported by the Wii's IOS.
struct Ticket
{
  SignatureRSA2048 signature;
  u8 server_public_key[0x3c];
  u8 version;
  u8 ca_crl_version;
  u8 signer_crl_version;
  u8 title_key[0x10];
  u64 ticket_id;
  u32 device_id;
  u64 title_id;
  u16 access_mask;
  u16 ticket_version;
  u32 permitted_title_id;
  u32 permitted_title_mask;
  u8 title_export_allowed;
  u8 common_key_index;
  u8 unknown2[0x30];
  u8 content_access_permissions[0x40];
  TimeLimit time_limits[8];
};
static_assert(sizeof(Ticket) == 0x2A4, "Ticket has the wrong size");

struct V1TicketHeader
{
  u16 version;
  u16 header_size;
  u32 v1_ticket_size;
  u32 section_header_table_offset;
  u16 number_of_section_headers;
  u16 section_header_size;
  u32 flags;
};
static_assert(sizeof(V1TicketHeader) == 0x14, "V1TicketHeader has the wrong size");
#pragma pack(pop)

constexpr u32 MAX_TMD_SIZE = 0x49e4;

class SignedBlobReader
{
public:
  SignedBlobReader() = default;
  explicit SignedBlobReader(std::vector<u8> bytes);

  const std::vector<u8>& GetBytes() const;
  void SetBytes(std::vector<u8> bytes);

  std::array<u8, 20> GetSha1() const;
  bool IsSignatureValid() const;
  SignatureType GetSignatureType() const;
  std::vector<u8> GetSignatureData() const;
  size_t GetSignatureSize() const;
  std::string GetIssuer() const;

protected:
  std::vector<u8> m_bytes;
};

bool IsValidTMDSize(size_t size);

class TMDReader final : public SignedBlobReader
{
public:
  TMDReader() = default;
  explicit TMDReader(std::vector<u8> bytes);

  bool IsValid() const;

  u16 GetBootIndex() const;
  u64 GetIOSId() const;
  u64 GetTitleId() const;
  u32 GetTitleFlags() const;
  u16 GetTitleVersion() const;
  u16 GetGroupId() const;
  DiscIO::Region GetRegion() const;
  bool IsvWii() const;

  u16 GetNumContents() const;
  bool GetContent(u16 index, Content* content) const;
  std::vector<Content> GetContents() const;
  bool FindContentById(u32 id, Content* content) const;
};

class TicketReader final : public SignedBlobReader
{
public:
  TicketReader() = default;
  explicit TicketReader(std::vector<u8> bytes);

  bool IsValid() const;
  bool IsV1Ticket() const;
  size_t GetNumberOfTickets() const;

  u8 GetVersion() const;
  u32 GetDeviceId() const;
  u32 GetTicketSize() const;
  u64 GetTitleId() const;
  u8 GetCommonKeyIndex() const;
  // Get the decrypted title key, using the retail or devkit common keys depending on the issuer.
  std::array<u8, 16> GetTitleKey() const;
};
}  // namespace IOS::ES
