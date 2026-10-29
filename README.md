# dvv-components

Component libraries of the dvv stack used by rawreel: the FFmpeg component (source providers
`ffmpeg` and `lavfi`). C++23; depends on
[dvv-base](https://github.com/BarabashkaD/dvv-base),
[dvv-registry](https://github.com/BarabashkaD/dvv-registry),
[dvv-platform](https://github.com/BarabashkaD/dvv-platform),
[dvv-gpu](https://github.com/BarabashkaD/dvv-gpu) and
[dvv-media](https://github.com/BarabashkaD/dvv-media).

## Build

Builds and tests on Windows (Visual Studio 2026) and Linux (Clang 19 with libstdc++ 14) with
[dvv-build](https://github.com/BarabashkaD/dvv-build), which provides the presets, the shared CMake
modules, the third-party dependencies (including FFmpeg's headers and shared libraries) and the CI
workflow; its README lists the prerequisites. dvv-build is cloned next to dvv-components at the
version of the `dvv-build` line in `dvv-deps.txt`; `fetch-deps.cmake` clones dvv-base,
dvv-registry, dvv-platform, dvv-gpu and dvv-media next to them at their minimum versions from the
same file:

```powershell
git clone https://github.com/BarabashkaD/dvv-components
git clone --branch v0.1.1 https://github.com/BarabashkaD/dvv-build
python -m pip install -r dvv-build/tools/requirements.txt
cmake -DREPO=dvv-components -P dvv-build/tools/fetch-deps.cmake
./dvv-build/bootstrap.ps1
cd dvv-components
cmake --workflow --preset vs2026
```

On Linux: `./dvv-build/bootstrap.sh`, then `cmake --workflow --preset ninja-clang`.
Applications use dvv-components as a git submodule in their `libs/` folder, next to dvv-build.

## Linking

An application links the umbrella target `dvv::components` and calls `dvv_components_link()`,
which links every enabled component library as a whole archive:

```cmake
find_package(dvv_components 0.1 REQUIRED)   # or dvv_components in-tree
target_link_libraries(my_app PRIVATE dvv::components)
dvv_components_link(my_app)
```

## Adding a component

Declare the library in the top-level `CMakeLists.txt` at the marked place, before the install rules
and the tests, with `dvv_add_component()` from `cmake/DvvComponent.cmake`
(`${DVV_COMPONENTS_CMAKE_DIR}/DvvComponent.cmake` for higher repositories):

```cmake
dvv_add_component(<id> KIND <Kind> SOURCES <file>... [LINK <target>...]
                  [REGISTERS <component id>...])
```

- It adds the option `DVV_COMPONENT_<ID>` (default `ON`). When on, it builds the static library
  `dvv_component_<id>` (alias `dvv::component_<id>`), linking `dvv::media` publicly and the `LINK`
  targets privately.
- `<id>` matches `[a-z][a-z0-9_]*`; `KIND` is a `dvv::Kind` name (`Source`, `FormatDetector`,
  `Exporter`, `Tool`, `Metric`, `Analyser`, `Processor`).
- `REGISTERS` (default `<id>`) lists the component ids the library registers, each matching
  `[a-z][a-z0-9_.-]*` and registered once across all libraries. The conformance test
  (`tests/test_conformance.cpp`) checks that the registry holds exactly these ids with their kinds,
  plus the source providers of `dvv::media_sources` (`raw`, `synthetic`, `y4m`).

Every component class is `final`, derives `dvv::Managed<T>`, has the constructor
`(dvv::CreationKey<dvv::ComponentRegistry>, const dvv::ParameterValues&)` and static `info()`,
`schema()` and `probe()`. Its constructor neither calls FFmpeg nor does I/O. Add any new capability
a component probes for to `ALL_CAPABILITIES` in the conformance test.

## FFmpeg

FFmpeg is loaded at run time and never linked. `dvv/components/ffmpeg.h` (target `dvv::ffmpeg`,
part of `dvv::components`) reports the status without FFmpeg types; `loadFfmpeg()` loads once per
process and never unloads.

- It opens the six libraries of an LGPL shared build (`avutil`, `swresample`, `swscale`,
  `avcodec`, `avformat`, `avfilter`), each with its dependencies taken only from the library's own
  folder, the application folder and System32. FFmpeg's headers are used only inside
  dvv-components.
- Each library's major version must equal the one rawreel was built with and its minor version
  must not be older; the licence must be LGPL, without `--enable-gpl` or `--enable-nonfree`. A
  failure leaves FFmpeg off with the reason in `FfmpegStatus::reason`.
- Search order: `DVV_FFMPEG_DIR` (an absolute path; final; an empty value counts as unset); else
  `<exe dir>/ffmpeg`, then `<exe dir>/../lib/ffmpeg`. The first default folder that contains the
  avutil library is final too.
- Capabilities (`addFfmpegCapabilities()`): `ffmpeg` (avutil, avcodec, avformat, swscale,
  swresample) and `ffmpeg.lavfi` (also avfilter).
- FFmpeg's log goes to the dvv log as warnings; nothing goes to stderr. AVERROR codes map to dvv
  errors (`Io`, `Format`, `Unsupported`, `OutOfMemory`, else `Internal`).
- `dvv_check_no_ffmpeg_imports(<test name> <target>...)` adds a test that fails when a binary
  imports an FFmpeg library.

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
  standard; a note "signalled but non-standard (P/T/M = x/y/z)" names the codes. rawreel writes
  the notes to its Log.
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
| `dvv_components.oracle` | `ffmpeg;oracle` | unpacked frames and the display colour against FFmpeg's output |
| `dvv_components.ffmpeg_fixtures`, `.lavfi_fixtures`, `.oracle_fixtures` | `ffmpeg` (`ffmpeg;oracle` for the oracle fixtures) | make the fixtures in the build tree (never committed) |
| `dvv_components.no_ffmpeg_imports` (+ `_selftest`), `.cmake_functions`, `.compile_fail.*` | | no binary imports FFmpeg; the CMake functions; compile-time rules |
| `dvv_components.package` | `package` | the installed package from a consumer project |

Without FFmpeg the `ffmpeg` tests are skipped (exit code 77) unless `DVV_REQUIRE_FFMPEG=1`. The
`oracle` tests run only without sanitizers.

Licence: MIT.
