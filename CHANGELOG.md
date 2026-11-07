# Changelog

All notable changes to dvv-components. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow semantic versioning with
the dvv rules: compatible = the same major and at least the minimum version, also for 0.x; breaking
changes only in a new major, after a deprecation. This file is the release notes (there are no
GitHub Releases).

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
