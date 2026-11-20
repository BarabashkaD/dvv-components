# dvv-components

Component libraries of the dvv stack: the FFmpeg component (source providers
`ffmpeg` and `lavfi`). C++23; depends on
[dvv-base](https://github.com/BarabashkaD/dvv-base),
[dvv-registry](https://github.com/BarabashkaD/dvv-registry),
[dvv-platform](https://github.com/BarabashkaD/dvv-platform),
[dvv-gpu](https://github.com/BarabashkaD/dvv-gpu) and
[dvv-media](https://github.com/BarabashkaD/dvv-media).

## Build

Builds and tests on Windows (Visual Studio 2026), Linux (Clang 19 with libstdc++ 14) and macOS
(Xcode on Apple Silicon) with [dvv-build](https://github.com/BarabashkaD/dvv-build), which provides
the presets, the shared CMake modules, the third-party dependencies (including FFmpeg's headers and
shared libraries) and the CI workflow; its README lists the prerequisites. dvv-build is cloned next
to dvv-components at the version of the `dvv-build` line in `dvv-deps.txt`; `fetch-deps.cmake`
clones dvv-base, dvv-registry, dvv-platform, dvv-gpu and dvv-media next to them at their minimum
versions from the same file:

```powershell
git clone https://github.com/BarabashkaD/dvv-components
git clone --branch v0.5.1 https://github.com/BarabashkaD/dvv-build
python -m pip install -r dvv-build/tools/requirements.txt
cmake -DREPO=dvv-components -P dvv-build/tools/fetch-deps.cmake
./dvv-build/bootstrap.ps1
cd dvv-components
cmake --workflow --preset vs2026
```

On Linux: `./dvv-build/bootstrap.sh`, then `cmake --workflow --preset ninja-clang`.
Applications use dvv-components as a git submodule in their `libs/` folder, next to dvv-build.

## Linking

An application links the umbrella target `dvv::components` and calls
`dvv_link_enabled_components()`, which links every enabled component library as a whole archive,
so its components and modules register:

    find_package(dvv_components 0.3 REQUIRED)   # or dvv_components in-tree
    target_link_libraries(my_app PRIVATE dvv::components)
    dvv_link_enabled_components(my_app)

`dvv_components_link()` is the 0.2 name; it is deprecated and removed in 1.0.0.

## Adding a component

    dvv_add_component(<id> SOURCES <file>... [LINK <target>...] [REGISTERS <component id>...])

- It adds the option `DVV_COMPONENT_<ID>` (default `ON`). When on, it builds the static library
  `dvv_component_<id>` (alias `dvv::component_<id>`), linking `dvv::media` publicly and the `LINK`
  targets privately.
- `<id>` matches `[a-z][a-z0-9_]*`. `REGISTERS` (default `<id>`) lists the component ids the
  library registers, each matching `[a-z][a-z0-9_.-]*` and registered once across all libraries.
  The conformance test checks that the registry holds exactly these ids, plus the source
  providers of `dvv::media_sources` (`raw`, `synthetic`, `y4m`).
- Each component id is recorded in the global property `DVV_COMPONENTS_IDS`.
- A component's category is the interface it registers for. `KIND` and the global property
  `DVV_COMPONENTS_REGISTERS` (`<component id>=<Kind>`, `Source` when `KIND` is not given) are
  deprecated and removed in 1.0.0.

Every component class is `final`, derives `dvv::Managed<T>`, has the constructor
`(dvv::CreationKey<dvv::ComponentRegistry>)` and static `info()` and `probe()`. Its constructor
neither calls FFmpeg nor does I/O. A component probes for capability constants (such as
`FFMPEG_CAPABILITY`), never for literals; add a new one to `ALL_CAPABILITIES` in the conformance
test.

## FFmpeg

FFmpeg is loaded at run time and never linked. The `ffmpeg` component library registers the module
`ffmpeg` (`FFMPEG_MODULE`): `dvv::ComponentRegistry::load()` loads FFmpeg once per process, never
unloads it, and adds the capabilities `ffmpeg` (`FFMPEG_CAPABILITY`: avutil, avcodec, avformat,
swscale, swresample) and `ffmpeg.lavfi` (`FFMPEG_LAVFI_CAPABILITY`: also avfilter) as far as they
are available. `registry.module(FFMPEG_MODULE)` is the status: what is withheld and why, and the
properties `version`, `licence`, `directory` and `variable`. The reason and the `directory` property may
hold a folder of the user's machine: the reason shows it as `dvv::pathDisplayText`, the property is
`dvv::pathText` (`dvv::pathFromText` gives the folder back).

- It opens the six libraries of an LGPL shared build (`avutil`, `swresample`, `swscale`,
  `avcodec`, `avformat`, `avfilter`), each with its dependencies taken only from the library's own
  folder, the application folder and System32 (Windows). FFmpeg's headers are used only inside
  dvv-components.
- Each library's major version must equal the one dvv-components was built with and its minor
  version must not be older; the licence must be LGPL, without `--enable-gpl` or
  `--enable-nonfree`. A failure withholds the capabilities, with the reason.
- Search order: the module option `directory` (`FFMPEG_DIRECTORY_OPTION`, an absolute folder;
  final); else `DVV_FFMPEG_DIR` (an absolute path; final; an empty value counts as unset); else
  `<exe dir>/ffmpeg`, then `<exe dir>/../lib/ffmpeg`, and on macOS
  `<exe dir>/../Frameworks/ffmpeg` (an app bundle). The first default folder that contains the
  avutil library is final too. The first load of the process decides; later loads report the same
  result.
- The sources stop through dvv-media: detaching an origin, or stopping the open's token, ends
  their work with `Errc::Cancelled`.
- FFmpeg's log goes to the dvv log as warnings; nothing goes to stderr. AVERROR codes map to dvv
  errors (`Io`, `Format`, `Unsupported`, `OutOfMemory`, else `Internal`).
- `dvv_check_no_ffmpeg_imports(<test name> <target>...)` adds a test that fails when a binary
  imports an FFmpeg library.
- Deprecated (removed in 1.0.0): `loadFfmpeg()`, `addFfmpegCapabilities()`, `FfmpegStatus`,
  `FfmpegLoadOptions` and `stopFfmpegSources()`.

## The `ffmpeg` component

The component library `dvv_component_ffmpeg` registers two source providers:

- `ffmpeg` (capability `ffmpeg`): `file:` URIs of any container FFmpeg demuxes (AVI, MKV, MOV, MP4,
  NUT, …); raw and Y4M files stay with dvv-media's providers. It opens only lossless video codecs:
  `rawvideo`, `v210`, `v410`, `r210`, `ffv1`, `huffyuv`, `ffvhuff`, `utvideo`, `magicyuv`, `png`
  and `qtrle`. Any other codec is `Unsupported` "codec <name> is not supported (lossless codecs
  only)". The URI takes two colour keys: `colour=<standard>` (`bt601-ntsc`, `bt601-pal`,
  `bt709`, `bt2020`, `bt2100-pq`, `bt2100-hlg`) replaces the container's matrix, curve and
  primaries (a stream tagged with the GBR matrix keeps it: its planes hold G, B and R), and
  `range=full|limited` replaces its range (allowed with every standard). The removed
  keys `matrix`, `kr`, `kb`, `transfer` and `primaries` are refused with "unknown parameter '<key>'
  (set the colour with colour=<standard>: ...)". Without `colour=`, the container's colour tags
  (the decoded frame's first, then the stream's; a note in the source info names both when they
  differ) are mapped as they are; dvv-media picks the standard they point to. The BT.601 primaries
  tag (or, without primaries, the matrix tag) tells PAL from NTSC. A tag that is signalled but
  fits no standard (e.g. SMPTE 240M) leaves its field unspecified (a missing field is shown with
  BT.709's value) and sets the source's colour hint, so the stream is labelled as having no
  standard; a note "signalled but non-standard (P/T/M = x/y/z)" names the codes. The notes are in
  `SourceInfo::notes`.
- `lavfi` (capability `ffmpeg.lavfi`): `lavfi:<graph>` URIs, one chain with one output. Every filter
  must be on `LAVFI_ALLOWED_FILTERS` (`src/sources/ffmpeg_rules.h`: generators and one-in, one-out
  video filters); any other is `Unsupported` "lavfi: filter <name> is not supported", with
  "; open files with file: instead" added for a filter that reads files (such as `movie`,
  `subtitles` or `drawtext`).

Decoded frames are copied into catalogue formats; a decoder output format outside the catalogue is
`Unsupported`.

## Tests

| Test | Labels | What it checks |
| --- | --- | --- |
| `dvv_components.unit` | `sanity` | the rules without FFmpeg |
| `dvv_components.ffmpeg`, `.ffmpeg_absent`, `.ffmpeg_relative` | `ffmpeg` | the loader and both providers against FFmpeg's own output; a missing FFmpeg; a relative `DVV_FFMPEG_DIR` |
| `dvv_components.ffmpeg_single_thread` | `ffmpeg` | a decode whose decoders start no FFmpeg threads, so the thread sanitizer checks our own threading without suppressions |
| `dvv_components.oracle` | `ffmpeg;oracle` | unpacked frames and the display colour against FFmpeg's output |
| `dvv_components.ffmpeg_fixtures`, `.lavfi_fixtures`, `.oracle_fixtures` | `ffmpeg` (`ffmpeg;oracle` for the oracle fixtures) | make the fixtures in the build tree (never committed) |
| `dvv_components.no_ffmpeg_imports` (+ `_selftest`), `.cmake_functions`, `.compile_fail.*` | | no binary imports FFmpeg; the CMake functions; compile-time rules |
| `dvv_components.package` | `package` | the installed package from a consumer project |

Without FFmpeg the `ffmpeg` tests are skipped (exit code 77) unless `DVV_REQUIRE_FFMPEG=1`. The
`oracle` tests run only without sanitizers.

Licence: MIT.
