---
title: Privacy and local data
description: Learn how Compareel processes all videos and images locally on your computer with zero telemetry, how hiding previews works, and how to clear local history.
nav_order: 6
---

# Privacy and local data

Compareel analyzes and compares all media locally on your computer. There are no user accounts, analytics, telemetry, cloud services, or background daemons.

## Everything stays local

- Your videos, images, thumbnails, folder paths, scan reports, and review sessions never leave your computer.
- The bundled FFmpeg decoders are compiled for local decoding only, with network protocols disabled.
- SVG images are rendered locally without executing scripts or loading external files or URLs.

You can review a summary of local storage inside the app at any time from **Help** > **Data and privacy** (or the **Data and privacy** button in **Tools** > **Settings…**).

## Hide all previews

Press `Ctrl+Shift+H` (`Cmd+Shift+H` on macOS), click the **Hide** eye button in the toolbar, or choose **View** > **Hide all previews** to hide both player previews and result thumbnails on screen at once and pause playback.

You can also hide either part on its own from the **View** menu:

- **Hide both video previews** (shown as **Hide both image previews** in **Images** mode)
- **Hide thumbnails**

While **Hide all previews** is turned on, automatic background saving of `last-review.cvsession` (**Remember last review**) is paused.

**Important limitation:** **Hide all previews** only hides images and videos on screen. Compareel does not encrypt or securely erase your media files, local cache database, saved `.cvsession` files, operation history, or **Quarantine…** copies on disk.

## Clear history

Choose **File** > **Clear history…** (or click **Clear history…** in **Tools** > **Settings…**) to remove Compareel's local records without touching your media files. All five categories are ticked by default, and you can untick any item you want to keep:

- **Scan results** — Clears the results currently on screen and deletes `last-scan-report.txt`.
- **Reviews and decisions** — Removes `last-review.cvsession`, `last-cleanup.cvsession`, saved review decisions (**Reviewed**, **Keep both files**, **Review later**), and ignored pairs.
- **File operation history** — Clears recorded copies, moves, quarantines, and removals from the local journal. Completed transfers can no longer be undone with **Undo last copy or move…** afterwards. (Interrupted operations that still need manual review remain listed until checked.)
- **Scan cache** — Clears cached visual signatures, hashes, and thumbnails from `scan-cache-v1.sqlite`, compacts the database, and deletes any paused scan file list (`.scan-job`).
- **Recent folders** — Clears saved included, excluded, and protected source folders.

Click **Clear history** to apply your selection. If you only want to free disk space used by cached media signatures while keeping your folders, decisions, and undo history, use **Clear scan cache** in **Tools** > **Settings…** instead.

## Local data folder locations

Compareel stores its configuration and local records in your per-user application configuration directory (Qt `AppConfigLocation`), unless you start Compareel with `--data-dir <folder>`:

- **Windows (installer and portable `.zip`):** `%LOCALAPPDATA%\Compareel` (for example `C:\Users\<username>\AppData\Local\Compareel`)
- **macOS:** `~/Library/Preferences/Compareel`
- **Linux (Flatpak `io.github.turxlab.Compareel`):** `~/.var/app/io.github.turxlab.Compareel/config/Compareel`
- **Linux (native / built from source):** `~/.config/Compareel` (or `$XDG_CONFIG_HOME/Compareel`)

To open the exact folder Compareel is using on your system, open **Tools** > **Settings…** (`Ctrl+,`, `Cmd+,` on macOS) and click **Open folder** next to **Local data folder**.

Files stored in this folder:

- `settings.ini` — Preferences, window state, and saved scan source lists.
- `scan-cache-v1.sqlite` — Cached file identities, SHA-256 hashes, metadata, compressed visual signatures (capped at 256 MiB / 100,000 current media entries), review decisions, ignored pairs, and operation history.
- `scan-cache-v1.sqlite.scan-job` — Remaining file list for a paused scan (up to 100,000 files / 32 MiB), removed when the scan finishes or when the scan cache is cleared.
- `last-review.cvsession` — Background session saved when **Remember last review** is enabled (paths, identities, decisions, and view state; no media or thumbnails).
- `last-cleanup.cvsession` — Recovery snapshot written before moving or transferring files.
- `last-scan-report.txt` — Plain-text report from the most recent scan.

If you add custom translation files (`languages/*.json`), Compareel looks in `languages` next to `Compareel.exe` when running from a writable Windows folder (such as the portable `.zip`), or otherwise in `%APPDATA%\Compareel\languages` on Windows, `~/Library/Application Support/Compareel/languages` on macOS, `~/.var/app/io.github.turxlab.Compareel/data/Compareel/languages` in the Linux Flatpak, and `~/.local/share/Compareel/languages` on native Linux.

## Update checks

- **Asked first:** On your first run, Compareel asks **Would you like Compareel to check for updates automatically?** (**Yes** / **No**) before making any network request.
- **What is sent:** When enabled, Compareel requests the latest release tag from GitHub (`https://api.github.com/repos/turxlab/compareel/releases/latest`) at most once every 24 hours. Nothing about your files, folders, or usage is sent.
- **Change or check manually:** Toggle **Check for updates when Compareel starts** in **Tools** > **Settings…**, or run a one-time check with **Help** > **Check for updates…** (**Compareel** > **Check for updates…** on macOS).
- **Disabled in Flatpak:** In the Linux Flatpak (`io.github.turxlab.Compareel`), the first-run prompt, the **Settings** checkbox, and **Check for updates…** are disabled and hidden because updates are handled by Flatpak and Flathub.

Next: [FAQ](faq.md) answers common questions about matching behavior, format limits, first-run warnings, and bug reports.
