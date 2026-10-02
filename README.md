# iso2rvz

A standalone Linux command-line tool that converts GameCube and Wii disc images to
Dolphin's RVZ format (or WIA, GCZ, plain ISO). It is built from Dolphin's own `DiscIO`
code, so its output is what Dolphin's "Convert File..." dialog produces, but it doesn't need
the emulator, Qt or a Dolphin user directory.

```
iso2rvz game.iso                      # -> game.rvz (zstd level 5, 128 KiB blocks)
iso2rvz -c lzma -l 9 -s 2M game.iso   # smaller, much slower to create and to read
iso2rvz -o out/ *.iso *.wbfs          # batch, results go to out/
iso2rvz -b iso game.rvz               # back to a plain ISO
```

Input can be ISO/GCM, CISO, GCZ, WBFS, WIA, RVZ, TGC or NFS. Run `iso2rvz --help` for every
option. Existing outputs are skipped unless you pass `--force`. Each file is written to
`<output>.part` and renamed when it finishes, so an interrupted or failed run never leaves a
truncated image behind. Ctrl-C cancels cleanly; press it twice to quit at once.

The defaults (RVZ, zstd level 5, 128 KiB blocks) match the settings Dolphin recommends.
Larger blocks and LZMA give somewhat smaller files, but Dolphin decompresses them more slowly
while a game is running.

## Building

Requires CMake 3.16+, a C++20/23 compiler (GCC 11+ or Clang 15+) and zlib headers
(`zlib1g-dev`). All other libraries (fmt, zstd, bzip2, liblzma, mbedtls AES/SHA-1) are bundled
in `externals/` and linked statically.

```
cmake -S . -B build -G Ninja
ninja -C build
sudo cmake --install build        # optional; installs to /usr/local/bin
```

The resulting binary depends only on glibc and libz.

## Layout

| Path | Contents |
|------|----------|
| `src/main.cpp` | The command-line tool. It started from the `DolphinConvert` patch. |
| `dolphin/Source/Core/` | The Dolphin `Common` and `DiscIO` sources in use, copied verbatim plus `patches/dolphin-iso2rvz.patch`. `dolphin/DOLPHIN_REVISION` records the upstream commit. |
| `compat/` | Replacements for the Dolphin parts that are left out: a cut-down `Core/IOS/ES/Formats` (ticket/TMD parsing and title-key decryption without the emulated IOS), a stderr logger, a stub Wii save-banner reader, and a `<expected>` polyfill for libstdc++ 11. |
| `externals/` | Bundled third-party libraries, from Dolphin's `Externals/`. |

The patch is guarded by `#ifndef ISO2RVZ`. It removes:

* extracted-directory input (`DirectoryBlob`) and WAD files, which need much more of Dolphin's
  core;
* a `resize_and_overwrite` call, so the code builds with libstdc++ 11 (Ubuntu 22.04).

## Updating from a newer Dolphin

```
scripts/update-dolphin.sh /path/to/dolphin
ninja -C build
```

The script re-copies every vendored file from the checkout and re-applies the patch. Also check
Dolphin's `Core/IOS/ES/Formats.*` for changes that affect `compat/Core/IOS/ES/Formats.*`.

## Differences from the DolphinConvert patch

* It no longer links Dolphin's whole emulator core and headless platform. It goes from a
  full Dolphin build to roughly 50 source files.
* The GameCube/Wii platform is detected from the disc. `-p/--platform` is gone.
* The options are validated the way `dolphin-tool convert` does it: block sizes per format,
  compression levels per method, and no zstd for WIA or purge for RVZ.
* `-s/--block-size` takes a size like `128K` or `2M`. Before, it read a misspelled option
  and was always ignored.
* The default compression is zstd level 5 with 128 KiB blocks. It used to be LZMA level 9 with
  2 MiB blocks; use `-c lzma -l 9 -s 2M` to get that back.
* New: progress display, multiple inputs, an output directory, `--scrub`, `--force`, ISO
  output, a proper exit status, and clean cancellation.

## License

GPL-2.0-or-later, like Dolphin. See `COPYING` and the licenses of the bundled libraries in
`externals/`.
