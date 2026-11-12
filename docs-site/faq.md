---
title: Frequently asked questions
description: Answers to common questions about exact vs. similar matches, Not compared file counts, supported media formats, first-run warnings, and bug reports.
nav_order: 7
---

# Frequently asked questions

## Why did two files match in Similar videos or Similar images when their size, audio, or quality differs?

**Similar videos** and **Similar images** compare sampled visual content only. Re-encoded, resized, watermarked, letterboxed, mirrored, or rotated copies of the same scene or photo are meant to group together so you can compare them.

Similarity says nothing about audio tracks, embedded subtitles, metadata, or visual quality. Always inspect a pair—and use **Details…** or **Tools** > **Compare sampled audio** when needed—before removing a copy. If you only want byte-for-byte identical files, scan in **Exact copies** mode instead.

## Why does Scan report list files as "Not compared"?

Compareel avoids decoding files when no match is possible under your current scan settings:

- In **Exact copies**, a file is only hashed when at least one other file has the exact same byte size. Files with a unique size are counted as not compared.
- In **Similar videos**, a video is only sampled when another video's length is within **Duration tolerance (seconds)** (0.20 s by default in **Tools** > **Scan rules…**).
- Files outside **Minimum size** or **Maximum size (0 = unlimited)** in **Scan rules…** are skipped.
- Videos whose sampled positions are mostly black or single-color frames with too little visual detail cannot be matched reliably by picture and are left ungrouped.

## Which formats are unsupported or have limits?

Files that cannot be decoded never stop a scan; Compareel skips them and lists each reason and `0x...` code in **Tools** > **Scan report**. Notable rules and safety limits:

- **Videos:** Opened with the system decoder (Media Foundation on Windows) or the bundled FFmpeg 8.1 decoders. Encrypted (DRM) or damaged streams are skipped and reported. Embedded subtitles are listed in **File details**, not played or compared.
- **JPEG XL (`jxl`):** Decoded via `libjxl` in FFmpeg (included in official Windows, macOS, and Flatpak builds).
- **JPEG XR (`jxr`, `wdp`, `hdp`):** Supported on Windows only (via WIC); macOS and Linux have no built-in decoder.
- **Camera RAW:** Compares either embedded previews or system-developed RAW images (Windows and macOS); embedded previews and developed RAW data never match each other.
- **Animations and multi-page images:** Static images never match animations. Animations are capped at 600 frames (or 512 megapixels total); multi-page files are capped at 256 pages.
- **Single-image size limits:** Up to 64 megapixels, 16,384 pixels per side (large JPEGs up to 131,072 pixels per side are decoded at reduced size), and 256 MiB file size (`svg` / `svgz`: 16 MiB; scripts and external references in SVG are rejected).

## Why does the Exact copies filter show nothing after a Similar videos or Similar images scan?

Similarity and **Advanced criteria** scans do not calculate SHA-256 file hashes while scanning. Choose **Tools** > **Verify exact matches in results** first so Compareel hashes same-size files in your result groups, then filter by **Exact copies** or **Includes exact copies** (or use **Tools** > **Exact-copy groups first**).

## Why did a short trimmed clip not match the full video?

**Similar videos** compares videos of nearly equal total duration (within **Duration tolerance (seconds)**). To search for a short clip inside longer recordings across your folders, use **Tools** > **Find partial clips in folders…**. If you already have both videos open side by side, click **Auto-align partial clip** (`Ctrl+Shift+A`, `Cmd+Shift+A` on macOS) next to **Offset**.

## Why is the Recycle Bin (Trash) button or cleanup mark disabled for a file?

Compareel blocks removal whenever a file is protected or when removing it would leave no verified copy behind:

- The file is inside a folder marked **Protected** in **File** > **Scan sources…**.
- The per-file lock (**Protect this file**) is turned on—including pairs opened from **Find partial clips in folders…**, which lock both files by default.
- The pair has a **Keep both files** (`Ctrl+2`) or **Review later** (`Ctrl+3`) decision.
- Every other copy in the group was already removed, changed on disk, or sits on a drive that is currently disconnected.

## How do I undo removing or moving a file?

- If you used **Copy to folder…**, **Move to folder…**, or **Quarantine…**, choose **File** > **Undo last copy or move…** (`Ctrl+Z`, `Cmd+Z` on macOS). This works even after restarting Compareel, provided **File operation history** was not cleared and neither path changed.
- If you sent files directly to the Recycle Bin (Trash), open the Windows Recycle Bin (Trash in Finder on macOS, or Trash in your Linux file manager) and restore them there.

## Why does Windows SmartScreen or macOS warn when opening Compareel the first time?

Compareel is an unfunded open-source project and its release binaries are not signed with paid commercial code-signing certificates:

- **Windows:** In the SmartScreen window, click **More info**, then click **Run anyway**.
- **macOS:** Control-click `Compareel` in Finder (or **Applications**) and choose **Open**, then confirm **Open** once. If no **Open** button is shown, open **System Settings** > **Privacy & Security** and click **Open Anyway**.

## How do I compare two specific files without scanning a folder?

Choose **File** > **Open two files…** (`Ctrl+Shift+O`, `Cmd+Shift+O` on macOS), drag and drop two videos or two images onto the Compareel window, or (on Windows, after enabling **Show Compareel in Explorer's Send To menu** in **Tools** > **Settings…**) select two files in Explorer and choose **Send to** > **Compareel**.

## How do I report a bug?

1. Make sure you can reproduce the problem on the [latest release](https://github.com/turxlab/compareel/releases/latest) and check whether the file hits one of the format limits above.
2. Open the [GitHub Bug report form](https://github.com/turxlab/compareel/issues/new?template=bug_report.yml).
3. Fill in your operating system, package type, Compareel version (shown in **Help** > **About Compareel**, or **Compareel** > **About Compareel** on macOS), scan mode, numbered steps to reproduce, expected behavior, and actual behavior.
4. If the issue involves scanning or unreadable files, open **Tools** > **Scan report** and paste the summary lines and any `0x...` error codes into the form. For slow operations, starting Compareel with `COMPAREEL_TRACE=1` prints step timing to stderr (on Windows, also set `QT_FORCE_STDERR_LOGGING=1`).
5. **Remove private file names and folder paths** from anything you paste or attach before submitting. (For security vulnerabilities, use **Security** > **Report a vulnerability** on GitHub rather than a public issue.)
