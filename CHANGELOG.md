# Changelog

All notable changes to Compareel are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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

[1.0.0]: https://github.com/turxlab/compareel/releases/tag/v1.0.0
