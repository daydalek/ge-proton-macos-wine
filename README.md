# ge-proton-macos-wine

`ge-proton-macos-wine` is the maintained full-source Wine tree used to build the
Maltoria macOS and Linux Wine runtimes. It succeeds the patch-only maintenance
layout in `daydalek/ge-proton-macos-wine-poc`.

## Repository layout

The repository root is the Wine source tree. It is based on the exact
CrossOver 26.3.0 `sources/wine` tree recorded in
`configs/crossover-source-lock.json`; downstream compatibility, media, macOS,
and Linux changes are normal commits in this repository rather than patches
applied at build time.

Project-owned build and release infrastructure is kept in these top-level
directories:

- `scripts/` — macOS/Linux build, packaging, validation, and release tooling.
- `configs/` — pinned source/dependency inputs and migration audit metadata.
- `native/` — native GDI upscaler integration.
- `tools/probes/` and `tools/tests/` — runtime probes and source tests.
- `licenses/` — third-party license texts used by release packaging.

The `patches/` directory is frozen migration evidence from the old patch-based
repository. Builds must not apply it. It is retained temporarily because the
source-license inventory and migration-audit tests use it to establish the
provenance of commits already present in this tree.

## Branch model

- `vendor/crossover` records pristine CrossOver source imports.
- `main` is the downstream maintained source tree.
- `runtime-channel` is the static release-channel branch consumed by Maltoria.

## Builds

macOS runtime builds require an Apple Silicon macOS 26+ runner with Rosetta 2
and use the repository itself as the Wine source:

```sh
export POC_WORK_DIR="$(mktemp -d)"
export GITHUB_RUN_NUMBER=69
bash scripts/ci/build-release.sh
```

Linux x86_64 runtime builds use the same maintained source tree:

```sh
python3 scripts/linux/build.py all --work-dir /tmp/ge-wine-linux
python3 scripts/linux/package.py --work-dir /tmp/ge-wine-linux --output /tmp/ge-linux-out
```

Linux packaging intentionally removes the two macOS Steam `CommandLineAppend`
entries from the installed `wine.inf`, preserving the old Linux behavior while
keeping one unified source tree. This is transitional; the preferred long-term
home for those defaults is Maltoria prefix/application configuration rather
than platform-specific Wine source divergence.

## Migration rule

Do not resurrect patch-driven source preparation (`prepare-source.sh` or
build-time `git apply`). Fix and evolve the Wine tree directly, with test
coverage under `tools/tests/`. Remove the frozen patch corpus only after the
license inventory has been converted from patch-file provenance to commit/path
provenance.

---

# Upstream Wine README

## INTRODUCTION

Wine is a program which allows running Microsoft Windows programs
(including DOS, Windows 3.x, Win32, and Win64 executables) on Unix.
It consists of a program loader which loads and executes a Microsoft
Windows binary, and a library (called Winelib) that implements Windows
API calls using their Unix, X11 or Mac equivalents.  The library may also
be used for porting Windows code into native Unix executables.

Wine is free software, released under the GNU LGPL; see the file
LICENSE for the details.


## QUICK START

From the top-level directory of the Wine source (which contains this file),
run:

```
./configure
make
```

Then either install Wine:

```
make install
```

Or run Wine directly from the build directory:

```
./wine notepad
```

Run programs as `wine program`. For more information and problem
resolution, read the rest of this file, the Wine man page, and
especially the wealth of information found at https://www.winehq.org.


## REQUIREMENTS

To compile and run Wine, you must have one of the following:

- Linux version 2.6.22 or later
- FreeBSD 12.4 or later
- Solaris x86 9 or later
- NetBSD-current
- macOS 10.12 or later

As Wine requires kernel-level thread support to run, only the operating
systems mentioned above are supported.  Other operating systems which
support kernel threads may be supported in the future.

**FreeBSD info**:
  See https://wiki.freebsd.org/Wine for more information.

**Solaris info**:
  You will most likely need to build Wine with the GNU toolchain
  (gcc, gas, etc.). Warning : installing gas does *not* ensure that it
  will be used by gcc. Recompiling gcc after installing gas or
  symlinking cc, as and ld to the gnu tools is said to be necessary.

**NetBSD info**:
  Make sure you have the USER_LDT, SYSVSHM, SYSVSEM, and SYSVMSG options
  turned on in your kernel.

**macOS info**:
  You need Xcode/Xcode Command Line Tools or Apple cctools.  The
  minimum requirements for compiling Wine are clang 3.8 with the
  MacOSX10.13.sdk and mingw-w64 v12 for 32-bit wine.  The
  MacOSX10.14.sdk and later can build 64-bit wine.

**Supported file systems**:
  Wine should run on most file systems. A few compatibility problems
  have also been reported using files accessed through Samba. Also,
  NTFS does not provide all the file system features needed by some
  applications.  Using a native Unix file system is recommended.

**Basic requirements**:
  You need to have the X11 development include files installed
  (called xorg-dev in Debian and libX11-devel in Red Hat).
  Of course you also need make (most likely GNU make).
  You also need flex version 2.5.33 or later and bison.

**Optional support libraries**:
  Configure will display notices when optional libraries are not found
  on your system. See https://gitlab.winehq.org/wine/wine/-/wikis/Building-Wine
  for hints about the packages you should install. On 64-bit
  platforms, you have to make sure to install the 32-bit versions of
  these libraries.


## COMPILATION

To build Wine, do:

```
./configure
make
```

This will build the program "wine" and numerous support libraries/binaries.
The program "wine" will load and run Windows executables.
The library "libwine" ("Winelib") can be used to compile and link
Windows source code under Unix.

To see compile configuration options, do `./configure --help`.

For more information, see https://gitlab.winehq.org/wine/wine/-/wikis/Building-Wine


## SETUP

Once Wine has been built correctly, you can do `make install`; this
will install the wine executable and libraries, the Wine man page, and
other needed files.

Don't forget to uninstall any conflicting previous Wine installation
first.  Try either `dpkg -r wine` or `rpm -e wine` or `make uninstall`
before installing.

Once installed, you can run the `winecfg` configuration tool. See the
Support area at https://www.winehq.org/ for configuration hints.


## RUNNING PROGRAMS

When invoking Wine, you may specify the entire path to the executable,
or a filename only.

For example, to run Notepad:

```
wine notepad            (using the search Path as specified in
wine notepad.exe         the registry to locate the file)

wine c:\\windows\\notepad.exe      (using DOS filename syntax)

wine ~/.wine/drive_c/windows/notepad.exe  (using Unix filename syntax)

wine notepad.exe readme.txt          (calling program with parameters)
```

Wine is not perfect, so some programs may crash. If that happens you
will get a crash log that you should attach to your report when filing
a bug.


## GETTING MORE INFORMATION

- **WWW**: A great deal of information about Wine is available from WineHQ at
	https://www.winehq.org/ : various Wine Guides, application database,
	bug tracking. This is probably the best starting point.

- **FAQ**: The Wine FAQ is located at https://gitlab.winehq.org/wine/wine/-/wikis/FAQ

- **Wiki**: The Wine Wiki is located at https://gitlab.winehq.org/wine/wine/-/wikis/

- **Gitlab**: Wine development is hosted at https://gitlab.winehq.org

- **Mailing lists**:
	There are several mailing lists for Wine users and developers; see
	https://gitlab.winehq.org/wine/wine/-/wikis/Forums for more
	information.

- **Bugs**: Report bugs to Wine Bugzilla at https://bugs.winehq.org
	Please search the bugzilla database to check whether your
	problem is already known or fixed before posting a bug report.

- **IRC**: Online help is available at channel `#WineHQ` on irc.libera.chat.
