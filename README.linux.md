# Linux Wine runtime

The Linux target uses the same maintained Wine source tree as the macOS
runtime. It builds an x86_64 Unix core with i386 and x86_64 Windows modules
using new WoW64. The initial distribution target is Ubuntu 24.04 amd64, with
X11, Wayland, ALSA, PulseAudio, Vulkan, FFmpeg, GnuTLS, and FreeType enabled.

## Build

Run on x86_64 Linux after installing the packages in
`scripts/linux/install-deps.sh`:

```sh
python3 scripts/linux/build.py all --work-dir /tmp/ge-wine-linux
python3 scripts/linux/build-probes.py --work-dir /tmp/ge-wine-linux
xvfb-run -a python3 scripts/linux/verify.py --work-dir /tmp/ge-wine-linux
python3 scripts/linux/package.py --work-dir /tmp/ge-wine-linux --output /tmp/ge-linux-out --validation-mode runtime
```

The build uses the repository checkout directly and records
`source_commit`/dirty-state provenance instead of applying patches.

## Platform compatibility policy

The unified source tree includes the macOS D3DMetal routing and Steam default
argument support. Those features remain environment/platform gated at runtime.
During Linux packaging, `scripts/linux/package.py` removes the two macOS Steam
`CommandLineAppend` entries from the installed `share/wine/wine.inf`, matching
the old Linux behavior while avoiding a separate Linux source branch.

This is transitional. The preferred long-term design is for Maltoria to own
Steam command-line defaults through prefix/application configuration, after
which Wine can drop those product-specific registry defaults entirely.

## Release contract

Linux packaging preserves these assets:

- `wine-linux-amd64.tar.gz`
- `wine-linux-amd64-sources.tar.gz`
- `linux-release-manifest.json`
- `SHA256SUMS-linux-amd64`
- privacy, static-check, and FFmpeg capability reports

`patches/` is frozen migration evidence used by license/provenance audit
tests. It is not part of the build pipeline.
