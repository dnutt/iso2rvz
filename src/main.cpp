// Copyright 2008 Dolphin Emulator Project
// Copyright 2026 iso2rvz contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// iso2rvz: convert GameCube/Wii disc images to RVZ (or WIA/GCZ/ISO) using Dolphin's DiscIO.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <getopt.h>
#include <signal.h>
#include <unistd.h>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"
#include "DiscIO/Blob.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/ScrubbedBlob.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/WIABlob.h"
#include "compat/Log.h"

#ifndef ISO2RVZ_VERSION
#define ISO2RVZ_VERSION "unknown"
#endif

namespace fs = std::filesystem;

namespace
{
std::atomic<bool> s_cancel_requested{false};

struct Options
{
  DiscIO::BlobType format = DiscIO::BlobType::RVZ;
  std::optional<DiscIO::WIARVZCompressionType> compression;
  std::optional<int> compression_level;
  std::optional<int> block_size;
  std::optional<std::string> output;
  bool scrub = false;
  bool force = false;
  bool quiet = false;
  int verbosity = 0;
};

enum LongOnlyOption
{
  OPT_SCRUB = 0x100,
};

void PrintUsage(FILE* out)
{
  fmt::print(out,
             "Usage: iso2rvz [OPTION]... INPUT...\n"
             "Convert GameCube and Wii disc images to RVZ (or WIA, GCZ, ISO).\n"
             "\n"
             "Input can be ISO, GCM, CISO, GCZ, WBFS, WIA, RVZ, TGC or NFS.\n"
             "By default each INPUT is written next to itself with its extension replaced.\n"
             "\n"
             "Options:\n"
             "  -o, --output PATH        output file (one INPUT only) or existing directory\n"
             "  -b, --blob FORMAT        output format: rvz, wia, gcz, iso  [rvz]\n"
             "  -c, --compress METHOD    WIA/RVZ compression: none, zstd, bzip2, lzma, lzma2,\n"
             "                           purge (WIA only)  [rvz: zstd, wia: lzma]\n"
             "  -l, --level N            compression level  [zstd: 5, others: 9]\n"
             "                           zstd: {}..{} (negative: faster), bzip2/lzma/lzma2: 1..9\n"
             "  -s, --block-size SIZE    block size in bytes, K and M suffixes accepted\n"
             "                           [rvz/gcz: 128K, wia: 2M]\n"
             "      --scrub              remove unused (junk) data while converting\n"
             "  -f, --force              overwrite existing output files\n"
             "  -q, --quiet              no progress output\n"
             "  -v, --verbose            show Dolphin log messages (twice for more)\n"
             "  -h, --help               show this help and exit\n"
             "  -V, --version            show version and exit\n",
             DiscIO::GetAllowedCompressionLevels(DiscIO::WIARVZCompressionType::Zstd, true).first,
             DiscIO::GetAllowedCompressionLevels(DiscIO::WIARVZCompressionType::Zstd, true).second);
}

std::string ToLower(std::string_view s)
{
  std::string result(s);
  for (char& c : result)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return result;
}

std::optional<DiscIO::BlobType> ParseFormat(std::string_view str)
{
  const std::string s = ToLower(str);
  if (s == "iso")
    return DiscIO::BlobType::PLAIN;
  if (s == "gcz")
    return DiscIO::BlobType::GCZ;
  if (s == "wia")
    return DiscIO::BlobType::WIA;
  if (s == "rvz")
    return DiscIO::BlobType::RVZ;
  return std::nullopt;
}

std::optional<DiscIO::WIARVZCompressionType> ParseCompression(std::string_view str)
{
  const std::string s = ToLower(str);
  if (s == "none")
    return DiscIO::WIARVZCompressionType::None;
  if (s == "purge")
    return DiscIO::WIARVZCompressionType::Purge;
  if (s == "bzip2")
    return DiscIO::WIARVZCompressionType::Bzip2;
  if (s == "lzma")
    return DiscIO::WIARVZCompressionType::LZMA;
  if (s == "lzma2")
    return DiscIO::WIARVZCompressionType::LZMA2;
  if (s == "zstd")
    return DiscIO::WIARVZCompressionType::Zstd;
  return std::nullopt;
}

std::optional<int> ParseInt(std::string_view str)
{
  if (str.empty())
    return std::nullopt;
  char* end = nullptr;
  const std::string s(str);
  errno = 0;
  const long value = std::strtol(s.c_str(), &end, 10);
  if (errno != 0 || *end != '\0' || value < std::numeric_limits<int>::min() ||
      value > std::numeric_limits<int>::max())
  {
    return std::nullopt;
  }
  return static_cast<int>(value);
}

// Accepts "131072", "128K", "128k", "2M", "2MiB" etc.
std::optional<int> ParseSize(std::string_view str)
{
  std::string s = ToLower(str);
  if (s.ends_with("ib"))
    s.resize(s.size() - 2);
  else if (s.ends_with("b"))
    s.resize(s.size() - 1);

  long long multiplier = 1;
  if (!s.empty() && (s.back() == 'k' || s.back() == 'm'))
  {
    multiplier = s.back() == 'k' ? 1024 : 1024 * 1024;
    s.pop_back();
  }

  const std::optional<int> base = ParseInt(s);
  if (!base || *base <= 0)
    return std::nullopt;
  const long long value = static_cast<long long>(*base) * multiplier;
  if (value > std::numeric_limits<int>::max())
    return std::nullopt;
  return static_cast<int>(value);
}

std::string_view FormatExtension(DiscIO::BlobType format)
{
  switch (format)
  {
  case DiscIO::BlobType::PLAIN:
    return ".iso";
  case DiscIO::BlobType::GCZ:
    return ".gcz";
  case DiscIO::BlobType::WIA:
    return ".wia";
  case DiscIO::BlobType::RVZ:
  default:
    return ".rvz";
  }
}

std::string FormatBytes(u64 bytes)
{
  if (bytes >= 1024ull * 1024 * 1024)
    return fmt::format("{:.2f} GiB", bytes / (1024.0 * 1024 * 1024));
  if (bytes >= 1024ull * 1024)
    return fmt::format("{:.1f} MiB", bytes / (1024.0 * 1024));
  if (bytes >= 1024)
    return fmt::format("{:.1f} KiB", bytes / 1024.0);
  return fmt::format("{} B", bytes);
}

bool MsgAlertHandler(const char* caption, const char* text, bool yes_no, Common::MsgType style)
{
  // Make sure an alert doesn't end up glued to the end of the progress line.
  fmt::print(stderr, "\n{}: {}\n", caption, text);
  // There is nobody to answer questions; answering "no" is the safe choice.
  return !yes_no && style != Common::MsgType::Question;
}

void SignalHandler(int)
{
  if (!s_cancel_requested.exchange(true))
  {
    static constexpr char message[] =
        "\nCancelling (a second signal will terminate immediately)...\n";
    [[maybe_unused]] const ssize_t ret = write(STDERR_FILENO, message, sizeof(message) - 1);
  }
  else
  {
    _exit(130);
  }
}

void InstallSignalHandlers()
{
  struct sigaction sa = {};
  sa.sa_handler = SignalHandler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
}

class ProgressPrinter
{
public:
  ProgressPrinter(bool enabled, std::string label)
      : m_enabled(enabled), m_tty(isatty(STDERR_FILENO)), m_label(std::move(label)),
        m_start(std::chrono::steady_clock::now())
  {
  }

  bool operator()(const std::string& text, float fraction)
  {
    if (!m_enabled)
      return !s_cancel_requested;

    const auto now = std::chrono::steady_clock::now();
    // Without a terminal, only print every 10% so logs stay readable.
    const int percent = static_cast<int>(fraction * 100.0f);
    if (m_tty)
    {
      if (now - m_last_print < std::chrono::milliseconds(200) && fraction < 1.0f)
        return !s_cancel_requested;
    }
    else if (percent / 10 == m_last_decile)
    {
      return !s_cancel_requested;
    }

    m_last_print = now;
    m_last_decile = percent / 10;

    const double elapsed = std::chrono::duration<double>(now - m_start).count();
    std::string eta;
    if (fraction > 0.01f && fraction < 1.0f)
    {
      const int remaining = static_cast<int>(elapsed / fraction * (1.0f - fraction));
      eta = fmt::format(", ETA {}:{:02}", remaining / 60, remaining % 60);
    }

    if (m_tty)
    {
      constexpr int BAR_WIDTH = 30;
      const int filled = static_cast<int>(fraction * BAR_WIDTH);
      fmt::print(stderr, "\r\x1b[K[{}{}] {:3}%  {}{}", std::string(filled, '#'),
                 std::string(BAR_WIDTH - filled, '-'), percent, text, eta);
    }
    else
    {
      fmt::print(stderr, "{}: {:3}%  {}{}\n", m_label, percent, text, eta);
    }
    m_printed = true;

    return !s_cancel_requested;
  }

  void Finish()
  {
    if (m_enabled && m_tty && m_printed)
      fmt::print(stderr, "\n");
  }

  double ElapsedSeconds() const
  {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
  }

private:
  bool m_enabled;
  bool m_tty;
  bool m_printed = false;
  std::string m_label;
  std::chrono::steady_clock::time_point m_start;
  std::chrono::steady_clock::time_point m_last_print{};
  int m_last_decile = -1;
};

// Validates the format-dependent options and fills in defaults. Returns false on error.
bool ResolveOptions(Options& options)
{
  const DiscIO::BlobType format = options.format;

  if (format == DiscIO::BlobType::PLAIN)
  {
    if (options.compression || options.compression_level || options.block_size)
      fmt::print(stderr, "Warning: compression options are ignored when writing ISO.\n");
    return true;
  }

  if (!options.block_size)
  {
    options.block_size =
        format == DiscIO::BlobType::WIA ? DiscIO::WIA_MIN_BLOCK_SIZE :
                                          DiscIO::GCZ_RVZ_PREFERRED_BLOCK_SIZE;
  }
  if (!DiscIO::IsDiscImageBlockSizeValid(*options.block_size, format))
  {
    fmt::print(stderr,
               "Error: block size {} is not valid for this format.\n"
               "  GCZ: a power of 2. WIA: a multiple of 2 MiB.\n"
               "  RVZ: a power of 2 of at least 32 KiB, or a multiple of 2 MiB.\n",
               *options.block_size);
    return false;
  }
  if (*options.block_size < DiscIO::PREFERRED_MIN_BLOCK_SIZE ||
      *options.block_size > DiscIO::PREFERRED_MAX_BLOCK_SIZE)
  {
    fmt::print(stderr, "Warning: block size {} is outside the range Dolphin considers good for "
                       "performance (32 KiB to 2 MiB).\n",
               *options.block_size);
  }

  if (format == DiscIO::BlobType::GCZ)
  {
    if (options.compression || options.compression_level)
      fmt::print(stderr, "Warning: --compress and --level are ignored for GCZ (always zlib).\n");
    return true;
  }

  // WIA / RVZ
  if (!options.compression)
  {
    options.compression = format == DiscIO::BlobType::RVZ ? DiscIO::WIARVZCompressionType::Zstd :
                                                            DiscIO::WIARVZCompressionType::LZMA;
  }
  const DiscIO::WIARVZCompressionType compression = *options.compression;

  if (format == DiscIO::BlobType::WIA && compression == DiscIO::WIARVZCompressionType::Zstd)
  {
    fmt::print(stderr, "Error: WIA does not support zstd compression.\n");
    return false;
  }
  if (format == DiscIO::BlobType::RVZ && compression == DiscIO::WIARVZCompressionType::Purge)
  {
    fmt::print(stderr, "Error: RVZ does not support purge compression.\n");
    return false;
  }

  if (compression == DiscIO::WIARVZCompressionType::None ||
      compression == DiscIO::WIARVZCompressionType::Purge)
  {
    options.compression_level = 0;
    return true;
  }

  if (!options.compression_level)
    options.compression_level = compression == DiscIO::WIARVZCompressionType::Zstd ? 5 : 9;

  const auto [min_level, max_level] = DiscIO::GetAllowedCompressionLevels(compression, false);
  if (*options.compression_level < min_level || *options.compression_level > max_level)
  {
    fmt::print(stderr, "Error: compression level {} is out of range ({}..{}).\n",
               *options.compression_level, min_level, max_level);
    return false;
  }

  return true;
}

std::string OutputPathFor(const Options& options, const std::string& input, bool output_is_dir)
{
  const std::string_view extension = FormatExtension(options.format);
  fs::path out;
  if (options.output && !output_is_dir)
    return *options.output;

  fs::path in_path(input);
  if (options.output)
    out = fs::path(*options.output) / in_path.filename();
  else
    out = in_path;
  out.replace_extension(extension);
  return out.string();
}

bool SameFile(const std::string& a, const std::string& b)
{
  std::error_code ec;
  return fs::exists(b, ec) && fs::equivalent(a, b, ec);
}

enum class Result
{
  Success,
  Failed,
  Skipped,
  Cancelled,
};

Result ConvertOne(const Options& options, const std::string& input_path,
                  const std::string& output_path)
{
  if (SameFile(input_path, output_path))
  {
    fmt::print(stderr, "Error: {}: output would overwrite the input.\n", input_path);
    return Result::Failed;
  }

  std::error_code ec;
  if (fs::exists(output_path, ec) && !options.force)
  {
    fmt::print(stderr, "Skipping {}: {} already exists (use --force to overwrite).\n", input_path,
               output_path);
    return Result::Skipped;
  }

  std::unique_ptr<DiscIO::BlobReader> blob_reader = DiscIO::CreateBlobReader(input_path);
  if (!blob_reader)
  {
    fmt::print(stderr, "Error: {}: could not open the file.\n", input_path);
    return Result::Failed;
  }

  const std::unique_ptr<DiscIO::VolumeDisc> volume = DiscIO::CreateDisc(input_path);
  if (!volume)
  {
    if (options.scrub)
    {
      fmt::print(stderr, "Error: {}: scrubbing is only supported for GC/Wii disc images.\n",
                 input_path);
      return Result::Failed;
    }
    fmt::print(stderr, "Warning: {}: not a GC/Wii disc image. Continuing anyway.\n", input_path);
  }

  if (options.scrub)
  {
    if (volume->IsDatelDisc())
    {
      fmt::print(stderr, "Error: {}: scrubbing a Datel disc is not supported.\n", input_path);
      return Result::Failed;
    }
    blob_reader = DiscIO::ScrubbedBlob::Create(input_path);
    if (!blob_reader)
    {
      fmt::print(stderr, "Error: {}: unable to scrub. Try again without --scrub.\n", input_path);
      return Result::Failed;
    }
  }

  if (volume && volume->IsNKit())
    fmt::print(stderr, "Warning: {}: this is an NKit image; the output will still be NKit.\n",
               input_path);

  if (!options.scrub && options.format == DiscIO::BlobType::GCZ && volume &&
      volume->GetVolumeType() == DiscIO::Platform::WiiDisc && !volume->IsDatelDisc())
  {
    fmt::print(stderr, "Warning: Wii images converted to GCZ without --scrub are usually not "
                       "smaller than ISO.\n");
  }

  if (!options.quiet)
  {
    std::string description;
    if (volume)
    {
      const std::string game_id = volume->GetGameID();
      const std::string name = volume->GetInternalName();
      const char* platform =
          volume->GetVolumeType() == DiscIO::Platform::WiiDisc ? "Wii" : "GameCube";
      description = fmt::format(" [{} {}{}{}]", platform, game_id, name.empty() ? "" : " ", name);
    }
    fmt::print(stderr, "{}{}\n  -> {}\n", input_path, description, output_path);
  }

  // Write to a temporary file so an existing output (with --force) survives a failed
  // conversion, and so an interrupted run never leaves a truncated file at the final path.
  const std::string temp_path = output_path + ".part";

  ProgressPrinter progress(!options.quiet, fs::path(input_path).filename().string());
  const auto callback = [&progress](const std::string& text, float percent) {
    return progress(text, percent);
  };

  bool success = false;
  switch (options.format)
  {
  case DiscIO::BlobType::PLAIN:
    success = DiscIO::ConvertToPlain(blob_reader.get(), input_path, temp_path, callback);
    break;

  case DiscIO::BlobType::GCZ:
  {
    u32 sub_type = std::numeric_limits<u32>::max();
    if (volume)
    {
      if (volume->GetVolumeType() == DiscIO::Platform::GameCubeDisc)
        sub_type = 0;
      else if (volume->GetVolumeType() == DiscIO::Platform::WiiDisc)
        sub_type = 1;
    }
    success = DiscIO::ConvertToGCZ(blob_reader.get(), input_path, temp_path, sub_type,
                                   *options.block_size, callback);
    break;
  }

  case DiscIO::BlobType::WIA:
  case DiscIO::BlobType::RVZ:
    success = DiscIO::ConvertToWIAOrRVZ(blob_reader.get(), input_path, temp_path,
                                        options.format == DiscIO::BlobType::RVZ,
                                        *options.compression, *options.compression_level,
                                        *options.block_size, callback);
    break;

  default:
    break;
  }
  progress.Finish();

  if (!success)
  {
    fs::remove(temp_path, ec);
    if (s_cancel_requested)
      return Result::Cancelled;
    fmt::print(stderr, "Error: {}: conversion failed.\n", input_path);
    return Result::Failed;
  }

  fs::rename(temp_path, output_path, ec);
  if (ec)
  {
    fmt::print(stderr, "Error: could not rename {} to {}: {}\n", temp_path, output_path,
               ec.message());
    fs::remove(temp_path, ec);
    return Result::Failed;
  }

  if (!options.quiet)
  {
    const u64 in_size = blob_reader->GetRawSize();
    const u64 out_size = fs::file_size(output_path, ec);
    const double seconds = progress.ElapsedSeconds();
    fmt::print(stderr, "  done: {} -> {} ({:.1f}%) in {}:{:02}\n", FormatBytes(in_size),
               FormatBytes(out_size), in_size ? 100.0 * out_size / in_size : 0.0,
               static_cast<int>(seconds) / 60, static_cast<int>(seconds) % 60);
  }

  return Result::Success;
}
}  // namespace

int main(int argc, char* argv[])
{
  Options options;

  static const option long_options[] = {
      {"output", required_argument, nullptr, 'o'},
      {"blob", required_argument, nullptr, 'b'},
      {"format", required_argument, nullptr, 'b'},
      {"compress", required_argument, nullptr, 'c'},
      {"level", required_argument, nullptr, 'l'},
      {"block-size", required_argument, nullptr, 's'},
      {"blocksize", required_argument, nullptr, 's'},
      {"scrub", no_argument, nullptr, OPT_SCRUB},
      {"force", no_argument, nullptr, 'f'},
      {"quiet", no_argument, nullptr, 'q'},
      {"verbose", no_argument, nullptr, 'v'},
      {"help", no_argument, nullptr, 'h'},
      {"version", no_argument, nullptr, 'V'},
      {nullptr, 0, nullptr, 0},
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "o:b:c:l:s:fqvhV", long_options, nullptr)) != -1)
  {
    switch (opt)
    {
    case 'o':
      options.output = optarg;
      break;
    case 'b':
    {
      const auto format = ParseFormat(optarg);
      if (!format)
      {
        fmt::print(stderr, "Error: unknown format '{}' (expected rvz, wia, gcz or iso).\n", optarg);
        return EXIT_FAILURE;
      }
      options.format = *format;
      break;
    }
    case 'c':
    {
      const auto compression = ParseCompression(optarg);
      if (!compression)
      {
        fmt::print(stderr, "Error: unknown compression method '{}'.\n", optarg);
        return EXIT_FAILURE;
      }
      options.compression = *compression;
      break;
    }
    case 'l':
      options.compression_level = ParseInt(optarg);
      if (!options.compression_level)
      {
        fmt::print(stderr, "Error: invalid compression level '{}'.\n", optarg);
        return EXIT_FAILURE;
      }
      break;
    case 's':
      options.block_size = ParseSize(optarg);
      if (!options.block_size)
      {
        fmt::print(stderr, "Error: invalid block size '{}'.\n", optarg);
        return EXIT_FAILURE;
      }
      break;
    case OPT_SCRUB:
      options.scrub = true;
      break;
    case 'f':
      options.force = true;
      break;
    case 'q':
      options.quiet = true;
      break;
    case 'v':
      ++options.verbosity;
      break;
    case 'h':
      PrintUsage(stdout);
      return EXIT_SUCCESS;
    case 'V':
      fmt::print("iso2rvz {}\n", ISO2RVZ_VERSION);
      return EXIT_SUCCESS;
    default:
      fmt::print(stderr, "Try 'iso2rvz --help' for more information.\n");
      return EXIT_FAILURE;
    }
  }

  const std::vector<std::string> inputs(argv + optind, argv + argc);
  if (inputs.empty())
  {
    PrintUsage(stderr);
    return EXIT_FAILURE;
  }

  if (!ResolveOptions(options))
    return EXIT_FAILURE;

  bool output_is_dir = false;
  if (options.output)
  {
    std::error_code ec;
    output_is_dir = fs::is_directory(*options.output, ec) || options.output->ends_with('/');
    if (output_is_dir && !fs::is_directory(*options.output, ec))
    {
      fmt::print(stderr, "Error: output directory {} does not exist.\n", *options.output);
      return EXIT_FAILURE;
    }
    if (!output_is_dir && inputs.size() > 1)
    {
      fmt::print(stderr, "Error: with several inputs, --output must be a directory.\n");
      return EXIT_FAILURE;
    }
  }

  Common::RegisterMsgAlertHandler(MsgAlertHandler);
  Common::Log::SetMaxLogLevel(options.verbosity >= 2 ? Common::Log::LogLevel::LINFO :
                              options.verbosity == 1 ? Common::Log::LogLevel::LWARNING :
                                                       Common::Log::LogLevel::LNOTICE);
  InstallSignalHandlers();

  int failed = 0;
  int skipped = 0;
  int succeeded = 0;
  for (const std::string& input : inputs)
  {
    if (s_cancel_requested)
      break;

    const Result result = ConvertOne(options, input, OutputPathFor(options, input, output_is_dir));
    if (result == Result::Success)
      ++succeeded;
    else if (result == Result::Skipped)
      ++skipped;
    else if (result == Result::Failed)
      ++failed;
  }

  if (s_cancel_requested)
  {
    fmt::print(stderr, "Cancelled.\n");
    return 130;
  }

  if (inputs.size() > 1 && !options.quiet)
  {
    fmt::print(stderr, "\n{} converted, {} skipped, {} failed.\n", succeeded, skipped, failed);
  }

  return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
