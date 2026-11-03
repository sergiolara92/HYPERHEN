# Building HyperHEN 0.9.5

This snapshot includes the dashboard/tool additions and firmware, mount,
localization and lifecycle changes made after the public 0.9.2 tag. The
[release notes](RELEASE_NOTES.md) describe that complete publication
comparison. The recipe below is a release-compatible source build for
0.9.5. R71 identifies the integration base; individual components retain their
own source provenance. Its inputs include the dedicated
corrected R48 HyperCore for 12.70, standard/lite Automount beta11, the R52 Hyper
worker and the isolated CRT with the retained 13.60 initializer. The product
version is 0.9.5; model weights remain separate console downloads.

Use Linux or WSL2. Extract the source archive into the Linux filesystem and keep
`source/`, `scripts/`, `assets/`, `third_party/`, `release-components/`,
`release-inputs/`, and `VERSION` together. The helper resolves every project
path relative to its location; no original developer workspace is required.

For a Git checkout, download
[HyperHEN-v0.9.5-build-inputs.zip](https://github.com/Michele-M-Media/HYPERHEN/releases/download/HYPERHEN_V0_9_5/HyperHEN-v0.9.5-build-inputs.zip)
from the same release and extract it at the checkout root. It supplies `release-components/`
and `release-inputs/`; the Git tree retains their checksum manifests. The full
source ZIP already includes these directories. The helper also stages five
verified runtime tools into their untracked `third_party/debug-tools/`
filenames when a thin checkout does not contain them. A different existing
local tool is rejected rather than overwritten.

## Requirements

- CMake 3.20 or newer, Ninja, Python 3, GNU coreutils and xz/lzma.
- Host LLVM tools selected by the SDK. The observed build used LLVM 18;
  `LLVM_CONFIG=/usr/lib/llvm-18/bin/llvm-config` selects that installation
  when the SDK's wrappers need an explicit host LLVM path.
- SDK target libcurl with its CA bundle under `target/user/homebrew`, including
  the static dependencies `psl`, `ssl`, `crypto`, `zstd` and `z`, for the Hyper model installer.
- A complete community PS5 Payload SDK with its Prospero compiler/toolchain,
  target headers, system-library stubs and C++ runtime: `libc++.a`,
  `libc++abi.a`, `libunwind.a`, and `target/include/c++/v1`.

The full SDK is not bundled. The build-input ZIP contains the isolated, checksum-pinned CRT object used by the release. The helper links it explicitly after application objects/libraries with `-nostartfiles`; it never falls back to the installed default CRT or edits the SDK. `scripts/prepare_isolated_crt.py` contains the narrowly scoped adaptation source and requires an exact original-object checksum.

The SDK is not bundled. The observed release environment used Clang 18.1.8,
CMake 4.2.3 and Ninja 1.13.2. These are environment observations, not a claim
that an independently identified SDK release has been pinned.

```bash
export PS5_PAYLOAD_SDK=/absolute/path/to/ps5-payload-sdk
bash scripts/build_release.sh --jobs 6
```

Output: `build-release/bin/HyperHEN.elf`.

Use `--build-dir '/path/with spaces/build'` to select another build tree, or
`--configure-only` to validate the inputs and configure without compiling.
The helper does not delete build trees, download dependencies or edit the SDK.
The supplied checksums must pass before configuration.

## Hyper worker and source dependencies

The default release build embeds the verified R52 worker unchanged. An optional
`python3 scripts/build_hyper_worker.py --jobs 4` rebuilds its first-party sources
against seven checksum-pinned static AI libraries. See [the worker notes](source/hyper/README.md)
for the relocatable recipe, exact upstream revisions, changed translation units
and the corresponding-source archive included in the build inputs. No model
weights are bundled; they are downloaded separately from the console menu.

## CI scope

Pushes and pull requests run source metadata, catalog JSON and host-side build
helper checks without downloading the retained ELF inputs or a console SDK.
The former full-component nightly build is now manual-only and remains a
separate pipeline from this release recipe.

A published release can run a compatible native rebuild when repository
variables `PS5_PAYLOAD_SDK_ARCHIVE_URL` and `PS5_PAYLOAD_SDK_ARCHIVE_SHA256`
identify a complete, checksum-pinned SDK ZIP. The workflow obtains the retained
inputs from that release's build-inputs asset using its read-only GitHub token
and compares their manifests with the checked-out release source. Without
those SDK variables, it records a notice and skips the native job; source/host
checks are not presented as a successful native rebuild. Manual dispatch can
also provide a release tag, SDK ZIP URL and SHA256 explicitly.

Publishing with an Actions `GITHUB_TOKEN` does not trigger a second workflow
from the resulting release event. For that publication path, use manual
`workflow_dispatch` to request the optional native rebuild; the source checks
on the source commit are separate.

## Published component provenance

The packaged ShellUI preserves the **R53** implementation, and HyperDeck
preserves its **R45** implementation. For 0.9.5, the version-bearing
`prx.cpp` and `toolbox_xml.cpp` units in ShellUI and `backend_ps5.cpp` in
HyperDeck were recompiled with the new version header, then linked with
their remaining parent objects and archives. The parent links were first
reproduced byte-for-byte. This preserves each component's functional base;
the product version does not imply that every embedded component was fully
rebuilt from one R71 source tree.

The default release helper reuses the checksum-pinned, versioned ShellUI and
HyperDeck components. To relink those components separately, run
`python3 scripts/build_canonical_components.py`; add
`--recompile-version-units` to compile the three version-bearing units first.
The canonical source snapshots, generated inputs and retained objects are
included. Independent builds on other SDKs or hosts are not promised identical
bytes.

## What this build rebuilds

The release-compatible recipe rebuilds the first-party daemon, bootstrapper
and unpacker, using the versioned canonical ShellUI and HyperDeck inputs. It retains the established
firmware engines, corrected dedicated 12.70 HyperCore, Automount beta11,
utilities and private loaders selected by the R71 development base. The
integrated R52 Hyper worker, voice-data archive and previously restored 13.xx
compatibility core are explicit, checksum-pinned inputs. The packaged ShellUI
and HyperDeck report `v0.9.5`, while their embedded native tools remain the
retained inputs. The plugin runtime is compiled from
the included first-party adapters and credited native helper sources.

`hyperhen_elfldr_inner.elf` is the private recovery loader inside the daemon;
it is intentionally distinct from the external `hyperhen_elfldr.elf` input.
`hyperdeck-pkg-helper.elf` is the retained package-install helper. The other four
HyperDeck engines and the HTTP library/header are staged from `release-inputs/`
into the selected build tree by the helper.

Keep prebuilt inputs and their provenance explicit. This package includes
vendored source plus retained binary dependencies; it is not a claim that
every dependency can be rebuilt exclusively from the included source.
Read the component inventory and upstream license notices before redistributing.

The historical `scripts/build.sh` is a broader component-source build and is
not this release recipe. It may rebuild additional engines or fetch missing
SDK runtime files. Optional retained-component rebuild helpers are separate
from the ordinary release build:

```bash
python3 scripts/build_core_1270.py --check-only
python3 scripts/build_core_1270.py --jobs 4
python3 scripts/build_automount_beta11.py --check-only
python3 scripts/build_automount_beta11.py --variant both --jobs 4
```

The dedicated core helper uses `third_party/kstuff-1270-r48/` and a fresh
`build-core-1270/` directory. The Automount helper uses the adapted beta11
source and pinned private json-c input, creating separate standard/lite
outputs under a fresh `build-automount-beta11/` directory. Both retain the
explicit isolated CRT and leave the release inputs unchanged. They do not
download dependencies or establish console compatibility. The Automount
link-stub compatibility declarations link to the real `libkernel_sys.sprx`;
they are not executable replacement syscall implementations.

Toolchain, build paths, version metadata and compression tools can affect
binary output. No bit-for-bit reproducible rebuild claim is made.
