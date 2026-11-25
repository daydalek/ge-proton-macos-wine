# ge-proton-macos-wine

English | [简体中文](README.md)

This repository is the maintained full-source Wine tree for Maltoria macOS and
Linux runtimes. It replaces the patch-only `ge-proton-macos-wine-poc` workflow.

The root is a Wine source tree based on the exact CrossOver 26.3.0
`sources/wine` input recorded in `configs/crossover-source-lock.json`.
GE-Proton multimedia work, CrossOver compatibility fixes, macOS presentation
and GDI-upscaler integration, and Linux runtime repairs are commits in the
tree. Project build and release infrastructure lives under `scripts/`,
`configs/`, `native/`, `tools/probes/`, and `tools/tests/`.

`patches/` is frozen migration evidence retained for license/provenance audit
tests. It is not applied during builds. New changes must edit the source tree
directly.

## macOS build

macOS runtime builds require Apple Silicon, macOS 26+, Rosetta 2, CMake, and
pkg-config/pkgconf:

```sh
export POC_WORK_DIR="$(mktemp -d)"
export GITHUB_RUN_NUMBER=69
bash scripts/ci/build-release.sh
```

The release contract remains `Libraries.tar.gz`, `Libraries-sources.tar.gz`,
`WhiskyWineVersion.plist`, `SHA256SUMS`, and the validation JSON reports.

## Linux build

See [README.linux.md](README.linux.md).

## Upstreams and licenses

See [UPSTREAMS.md](UPSTREAMS.md), `COPYING.LIB`, `LICENSE`, and `licenses/`.
Do not publish runtime binaries until the license/attribution migration review
in the initial migration PR is resolved.
