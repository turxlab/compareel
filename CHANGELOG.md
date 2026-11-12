# Changelog

All notable changes to Compareel are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.1.0] - 2026-11-12

### Added

- **Sort groups** by **Highest similarity** or **Lowest similarity**; group headers show the measured similarity of each group.
- **Oldest creation date** and, for images, **Oldest date taken (EXIF)** as **Keep in each group** and cleanup keep rules.
- **File details** show the creation and modification dates; image comparisons show the EXIF date taken.
- Minimum and maximum file size with selectable units in **Tools > Scan rules…**; the rules shown follow the selected mode.
- Page counter for multi-page images in the player.
- Tone mapping for half-float OpenEXR and float grayscale-with-alpha images.
- `.ilbm` files are found by scans and offered in **Open two files…**.

### Changed

- Faster visual grouping for large image and video collections.
- Folder scans are no longer limited to 100,000 files. Above that size, pausing and automatic review saving are turned off, and the app says so.
- Camera RAW files report their full sensor resolution, so **Highest resolution** keeps the RAW file over a smaller JPEG copy. Embedded preview selection for RAW, DNG and CR3 files is more reliable.
- **View > Privacy mode** is now **View > Hide all previews**; the toolbar button is labelled **Hide**.
- A failed preview is reported separately from SHA-256 verification.
- The Windows `licenses.txt` now includes the licenses of every statically linked library.

### Fixed

- Pairs ignored in an earlier session could reappear when a group was regrouped.
- Sorting by similarity could change which file became the group reference.
- **Pause scan** could become enabled again for scans that cannot be paused.
- The alignment **Offset** was limited to one hour; partial clips up to 24 hours can now be aligned.
- **Previous group** and **Next group** now skip groups hidden by the filter.
- The cleanup mark preview could propose files that the cleanup plan then left out.
- "No files to compare" could appear after a scan that found files but no matches.
- File paths in message boxes are always shown as plain text.
- Lower memory use for long animations; fixes for some DDS files.
- Opening previews no longer rewrites unchanged cache entries.
- The Linux Flatpak shows the correct version in **About**.
- The libheif rebuild scripts under `third_party/` point to the shipped FFmpeg build.

## [1.0.0] - 2026-11-04

First public release.

### Added

- **Scanning and matching**
  - **Exact copies** mode verifying full file contents with SHA-256 for videos and images.
  - **Similar videos** mode comparing sampled frames, with optional tolerance for letterboxing, mirroring, 90-degree rotations and light cropping (**Recognize black borders, mirrored and rotated copies**).
  - **Similar images** mode for static, animated, multi-page, icon, HDR, PSD composite, SVG and camera RAW files after normalizing EXIF orientation, ICC color profiles and transparency.
  - **Advanced criteria** mode and **Tools > Scan rules…** filters for file size, extensions, similarity threshold, duration tolerance and parallel read count.
  - **Scan sources…** with included, excluded and protected folders, per-folder subfolder toggles and **Compare only between selected folders**.
  - Storage-aware parallel scanning, SQLite scan cache (**Store media cache on disk**, **Clear scan cache**), resumable scans (**Pause scan**, **Resume paused scan**) and **Tools > Scan report**.
- **Side-by-side review**
  - Linked dual playback (**Link video controls**), frame stepping, manual time **Offset**, **Playback > Auto-align partial clip** and optional hardware decoding (**Prefer GPU video decoding (experimental)**).
  - **Side by side**, **Wipe**, **Overlay** and **Difference (display aid)** views with shared zoom and pan, plus holding `B` to flip between files.
  - **Tools > Compare sampled audio**, **Tools > Find partial clips in folders…** and **Tools > Verify exact matches in results**.
  - Per-pair review decisions (**Reviewed**, **Keep both files**, **Review later**), **Ignore this pair**, per-file protection (**Protect this file**), **Keep in each group** reference rules, result filtering and **File details** with embedded subtitle track listing.
- **Cleanup, transfers and sessions**
  - Removal to the system Recycle Bin / Trash only, per file, for marked files (**Mark right file for cleanup**, **Move marked files to Recycle Bin…**) or across all groups (**Move other copies to Recycle Bin…**) with keep rules, preferred folder, live preview and CSV plan export.
  - SHA-256-verified **Copy to folder…**, **Move to folder…**, **Quarantine…** and batch transfers (**File > All result files**) without overwriting existing files, backed by persistent **File > Undo last copy or move…** and **Tools > Operation history**.
  - `.cvsession` session files (**Save review session…**, **Open review session…**), **Remember last review** / **Resume last review**, **Recover last cleanup review…** and **File > Export results…** (CSV).
- **Interface, privacy and packages**
  - **View > Privacy mode** to hide previews and thumbnails on screen, **Help > Data and privacy** and **File > Clear history…**.
  - Four themes (**Dark**, **Graphite**, **Light**, **Sand**) and 41 interface languages that follow the system language, each a JSON file in `languages/`.
  - Optional Windows Explorer **Send To** shortcut and optional GitHub release check (**Help > Check for updates…**).
  - Packages for Windows 10/11 x64 (installer and portable zip), macOS 12+ (universal `.dmg`) and Linux x86_64 (Flatpak `io.github.turxlab.Compareel`), with corresponding source archive.

[1.1.0]: https://github.com/turxlab/compareel/releases/tag/v1.1.0
[1.0.0]: https://github.com/turxlab/compareel/releases/tag/v1.0.0
