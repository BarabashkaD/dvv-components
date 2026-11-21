# Changelog

All notable changes to dvv-components. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow semantic versioning with
the dvv rules: compatible = the same major and at least the minimum version, also for 0.x; breaking
changes only in a new major, after a deprecation. This file is the release notes (there are no
GitHub Releases).

## [0.3.0] - 2026-11-21

### Added
- The FFmpeg module `ffmpeg`: `ComponentRegistry::load()` loads FFmpeg and adds its capabilities.
  `registry.module(FFMPEG_MODULE)` reports what it provides and withholds, the reason, and the
  properties `version`, `licence` and `directory` (when the core capability is provided) and,
  when `DVV_FFMPEG_DIR` chose the folder, `variable`. Option `directory`
  (`FFMPEG_DIRECTORY_OPTION`): an absolute folder, final.
- `FFMPEG_MODULE`, `FFMPEG_CAPABILITY`, `FFMPEG_LAVFI_CAPABILITY` and `FFMPEG_DIRECTORY_OPTION`
  in `dvv/components/ffmpeg.h`.
- `dvv_link_enabled_components(<target>)`.
- The global CMake property `DVV_COMPONENTS_IDS`: the component ids of the enabled component
  libraries.
- `detail::LibraryInfo`, the name of `detail::LibraryFacts` from now on.

### Changed
- Requires dvv-build 0.5.1, dvv-base 0.5.0, dvv-registry 0.2.0, dvv-platform 0.3.0,
  dvv-gpu 0.3.0 and dvv-media 0.2.0.
- FFmpeg and lavfi sources honour the frame store's stop token and the open's token: detaching an
  origin ends its queued and running requests with `Errc::Cancelled`, and a stopped open ends
  during the index scan or the lavfi graph's first frame with `Errc::Cancelled`. A cancelled
  request leaves a source shared by several origins in step. An open whose token is stopped
  before it starts opens no file.
- A `stopFfmpegSources()` that overlaps a lavfi open fails the open with "source closed"; it used
  to return a source that failed at its first read.
- Container and lavfi sources report `SourceInfo::fixed` (format, size and rate).
- The providers take no parameters, declare no schema and set no kind; the registry lists them by
  interface.
- `dvv_add_component` no longer checks `KIND`; a call without it records the kind `Source` in
  `DVV_COMPONENTS_REGISTERS`.
- The size and rate grammar of `ffmpeg` URIs and the library file names come from dvv-media and
  dvv-platform (same grammar, messages and names).
- Path text comes from dvv-base: on Linux (macOS file systems hold UTF-8 names only) a library
  folder or a file whose name is not UTF-8 is opened with its own bytes; messages show U+FFFD for
  such bytes, also the `DVV_FFMPEG_DIR` refusal.
- The refusal reasons name no application: "only LGPL builds are accepted", "dvv-components was
  built for <version>".

### Deprecated (removed in 1.0.0)
- `loadFfmpeg`, `addFfmpegCapabilities`, `FfmpegStatus` and `FfmpegLoadOptions`: use
  `ComponentRegistry::load()` and `registry.module(FFMPEG_MODULE)`. `FfmpegStatus::libraryVersions`
  has no replacement in the module.
- `stopFfmpegSources()`: detach the origin, or stop the open's token
  (`SourceFactory::open(request, stop)`).
- `dvv_components_link()`: use `dvv_link_enabled_components()`.
- `detail::LibraryFacts`: use `detail::LibraryInfo`.
- The `KIND` argument of `dvv_add_component` (not checked; it only feeds the property below).
- The global CMake property `DVV_COMPONENTS_REGISTERS` (`<component id>=<Kind>`): use
  `DVV_COMPONENTS_IDS`.

### Fixed
- README: the build clones dvv-build v0.5.1 and lists macOS; the FFmpeg search order names the
  macOS app bundle folder.

## [0.2.0] - 2026-11-07

### Added
- macOS (Apple Silicon): the FFmpeg libraries are found as `lib<name>.<major>.dylib`, the FFmpeg
  search also looks in `<exe>/../Frameworks/ffmpeg` (an app bundle), and the import check uses
  `otool -L` on macOS.

### Changed
- Requires dvv-build 0.2.0, dvv-base 0.2.0, dvv-registry 0.1.1, dvv-platform 0.2.0 and
  dvv-gpu 0.2.0.

## [0.1.2] - 2026-10-29

### Added
- A single-thread FFmpeg decode test (`dvv_components.ffmpeg_single_thread`, label `ffmpeg`): the
  decoders start no FFmpeg threads and the thread-sanitizer preset runs it without suppressions.

### Changed
- LICENSE: the copyright line names BarabashkaD.

### Fixed
- The ThreadSanitizer suppressions for FFmpeg (`tests/tsan.supp`) use `called_from_lib:` for the
  loaded FFmpeg libraries instead of `race:`, so races in dvv code that FFmpeg's threads run (the
  log callback) and races between dvv threads on memory FFmpeg allocated are reported again.
  FFmpeg's own locking is not visible to ThreadSanitizer (see `tests/tsan.supp`).

## [0.1.1] - 2026-10-28

### Fixed
- ThreadSanitizer runs suppress races inside the uninstrumented FFmpeg libraries
  (`tests/tsan.supp`, used by the FFmpeg tests of the thread-sanitizer preset only).

## [0.1.0] - 2026-10-28

First versioned release.

### Added
- `dvv_add_component()` and `dvv_components_link()` (`cmake/DvvComponent.cmake`): component
  libraries with their `DVV_COMPONENT_<ID>` options, linked as whole archives, and the umbrella
  target `dvv::components`.
- The FFmpeg run-time binding `dvv::ffmpeg`: FFmpeg is loaded at run time and never linked, with
  version and LGPL licence checks, the `ffmpeg` and `ffmpeg.lavfi` capabilities and log routing.
- The FFmpeg component `dvv_component_ffmpeg` with the source providers `ffmpeg` (lossless video
  codecs in any container FFmpeg demuxes, with colour keys) and `lavfi` (one allowlisted chain).
- `dvv_check_no_ffmpeg_imports()`: a test that fails when a binary imports an FFmpeg library.
- The CMake package `dvv_components` (`find_package(dvv_components 0.1)`, `SameMajorVersion`); it
  requires dvv-base, dvv-registry, dvv-platform, dvv-gpu and dvv-media 0.1.0 or later in the 0.x
  series.
