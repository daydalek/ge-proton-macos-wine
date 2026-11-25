#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
project="$(cd "$(dirname "$0")/../.." && pwd -P)"
cd "$project"
test "$(uname -s)" = Darwin && test "$(uname -m)" = arm64 || { echo 'Requires an arm64 macOS runner.' >&2; exit 1; }
major="$(sw_vers -productVersion | cut -d. -f1)"
test "$major" -ge 26 || { echo 'Requires a macOS 26+ runner.' >&2; exit 1; }
/usr/bin/arch -x86_64 /usr/bin/true || { echo 'Install Rosetta 2 before running this workflow.' >&2; exit 1; }
export POC_MACOSX_DEPLOYMENT_TARGET=15.0
: "${POC_WORK_DIR:?Set a fresh isolated POC_WORK_DIR}"
test ! -e "$POC_WORK_DIR" || { echo 'Build work directory must be fresh.' >&2; exit 1; }
command -v cmake >/dev/null
command -v pkg-config >/dev/null || command -v pkgconf >/dev/null
bash scripts/build-deps.sh
bash scripts/crossover/build.sh full
python3 scripts/ci/make-release.py --output "${POC_RELEASE_DIR:-$project/outputs/release}"

# Continue the published 11.0.x runtime sequence from the previous repository.
# The last published runtime was 11.0.68; new-repository run numbers restart low.
: "${GITHUB_RUN_NUMBER:?Set GITHUB_RUN_NUMBER for monotonically increasing runtime versions}"
RUNTIME_VERSION_BASE="${POC_RUNTIME_VERSION_BASE:-68}"
case "$RUNTIME_VERSION_BASE" in ''|*[!0-9]*) echo 'POC_RUNTIME_VERSION_BASE must be numeric' >&2; exit 2 ;; esac
RUNTIME_VERSION="11.0.$((RUNTIME_VERSION_BASE + GITHUB_RUN_NUMBER))"
python3 scripts/ci/build-libraries.py --version "$RUNTIME_VERSION" --output "${POC_RELEASE_DIR:-$project/outputs/release}"
