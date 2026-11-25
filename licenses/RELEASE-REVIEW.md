# Pre-release license and attribution review

Date: 2026-11-25

Scope: the full-source migration for `ge-proton-macos-wine`, its macOS
`Libraries` runtime contract, and its Linux `wine-linux-amd64` runtime
contract. This is an engineering compliance review, not legal advice.

## Verdict

No fundamental license incompatibility is known that prevents publishing the
runtimes, provided the release gates below remain enforced. The full-source
repository improves Wine/LGPL source correspondence compared with the old
patch-only repository because the exact modified Wine tree is now the primary
artifact.

Do not publish a runtime until items marked **release gate** are satisfied in
CI for the exact release commit.

## Wine, CrossOver, Valve Wine, and GE-Proton

- Wine and CrossOver Wine are LGPL-2.1-or-later projects. The tree retains
  `LICENSE`, `COPYING.LIB`, `AUTHORS`, upstream copyright notices, and file
  headers.
- CrossOver 26.3.0 is imported byte-identically on `vendor/crossover`; its URL
  and SHA-256 are locked in `configs/crossover-source-lock.json`.
- Valve/GE-Proton-derived Wine changes are distributed as LGPL-2.1-or-later
  Wine derivatives. Proton's top-level BSD-3-Clause text is retained as
  upstream context in `licenses/Proton-LICENSE.BSD-3-Clause`, but it does not
  replace the LGPL terms of Wine files.
- `configs/source-license-inventory.json` currently records 275
  LGPL-2.1-or-later file mappings, one permissive Autoconf-generated file, and
  two GPL-2.0-only/Linux-syscall-note header mappings across the frozen patch
  corpus.
- Dated modification notices are present in every file recorded by
  `MODIFICATIONS.json` and `LINUX_MODIFICATIONS.json`; `scripts/release-check.py`
  now verifies existence, SHA-256 correspondence, and notice presence.

## Generated build system

`configure` is an Autoconf-generated file. The inventory classifies it under
the permissive Autoconf output exception rather than treating generated output
as a separate copyleft work. `configure.ac` remains the maintained input.

## FFmpeg

Both macOS and Linux builds configure FFmpeg with `--enable-shared`,
`--disable-static`, `--disable-gpl`, `--disable-nonfree`, and
`--disable-version3`. The intended distributed license is therefore
LGPL-2.1-or-later, not GPL. Dynamic linking also preserves the LGPL relinking
path. `licenses/FFmpeg-LICENSE.md` and `licenses/FFmpeg-COPYING.LGPLv2.1` are
carried in runtime notices, and source packages include the pinned FFmpeg
archive.

Release gate: the release pipeline must continue to reject GPL/nonfree/version3
FFmpeg configuration changes and must ship the exact corresponding FFmpeg
source archive in each source package.

## GnuTLS cryptographic stack

GnuTLS, libtasn1, Nettle/Hogweed, GMP, and libunistring are built as shared
libraries with static archives disabled. Their LGPL/GPL-dual or LGPL notices
are retained in `licenses/` and `UPSTREAMS.md`; pinned source archives are part
of the source package.

Release gate: do not switch these dependencies to static linking unless a new
LGPL object/relinking review is performed.

## Graphics and upscaler components

- DXVK is distributed under the zlib/libpng license; attribution and license
  text are in `licenses/DXVK-LICENSE.txt`.
- MoltenVK is Apache-2.0. It is a separate dynamic Vulkan-to-Metal library.
  The shadow-import patch and corresponding modified source are included in the
  source package. Do not copy Apple D3DMetal/GPTK proprietary binaries into
  this repository or releases.
- Vulkan-Headers carry Apache-2.0/MIT notices; ncnn is BSD-3-Clause;
  Real-CUGAN components are MIT; Anime4K is MIT. Their license texts are in
  `licenses/` and vendored upstream notices remain under
  `native/gdi-upscalers/vendor/`.

## Linux UAPI headers

`dlls/ntdll/unix/ntsync_tmp.h` and `server/ntsync_tmp.h` appear only in the
inactive frozen `patches/wine-ge/macos-platform.patch` audit corpus; they are
not present in the maintained compiled tree. Their terms are GPL-2.0-only WITH
Linux-syscall-note.

Release gate: do not reactivate or copy those headers into the maintained
source tree without a dedicated review. Prefer using the host distribution's
UAPI headers or a clean LGPL-compatible compatibility shim.

## Corresponding source and build recipes

macOS source packages contain a `git archive` of the exact release commit,
pinned third-party source archives, the MoltenVK patch/source, licenses, and
build configuration. Linux source packages contain a `git archive` extraction
of the exact release commit under `prepared/wine` plus pinned dependency
archives. Runtime provenance records the source commit and rejects dirty
checkouts for packaging.

Release gates:

1. `python3 scripts/release-check.py --public-history` passes on the release
   commit.
2. macOS and Linux packaging complete from a clean checkout and include the
   exact source archive for the release commit.
3. Runtime notices include `COPYING.LIB`, `UPSTREAMS.md`, `licenses/`,
   Wine's `LICENSE`, and component-specific texts.
4. `MODIFICATIONS.json` and `LINUX_MODIFICATIONS.json` hashes match the shipped
   source tree.
5. No proprietary Apple D3DMetal/GPTK payload is distributed.

## Remaining migration debt

The license inventory is still patch-centered because `patches/` is retained
as frozen migration evidence. This is acceptable for the initial migration but
should be converted to commit/path provenance before routine full-source
development. After that conversion, remove the frozen patch corpus and the
legacy `apply-patch-series.py` audit helper.

The pinned DXVK bootstrap archive is currently fetched from the old
`ge-proton-macos-wine-poc` release. This is not a license blocker (the payload
is digest-pinned and its license is distributed), but operationally the old
release must remain available or the payload must be re-hosted/rebuilt before
the old repository is deleted.
