---
title: Getting started
description: Download and install Compareel on Windows, macOS, or Linux, handle first-run prompts, and start your first duplicate video or image scan.
nav_order: 2
---

# Getting started

Download the package for your system from [GitHub Releases](https://github.com/turxlab/compareel/releases/latest). Every release includes a `SHA256SUMS.txt` file if you want to verify your download.

Compareel packages are not signed with paid commercial code-signing certificates, so Windows and macOS show a warning the first time you open a new release.

## Install on Windows

Compareel runs on 64-bit Windows 10 and Windows 11. No separate codec pack is needed.

- **Installer (`Compareel-<version>-x86_64-Win.exe`)** — Run the installer to install for your user account (no administrator rights needed) or for all users. It adds a Start menu entry and an optional desktop shortcut.
- **Portable archive (`Compareel-<version>-x86_64-Win.zip`)** — Extract the whole folder anywhere and run `Compareel.exe`. Keep `Compareel.exe` together with the `.dll` files and plugin folders next to it.

**First start on Windows:** If Windows SmartScreen blocks the installer or program, click **More info**, then click **Run anyway**.

If you want to open two files directly from File Explorer, open **Tools** > **Settings…** (`Ctrl+,`) and tick **Show Compareel in Explorer's Send To menu**. You can then select two files in Explorer, right-click them, and choose **Send to** > **Compareel**.

## Install on macOS

Compareel runs on macOS 12 or later on both Apple Silicon and Intel Macs.

1. Open `Compareel-<version>.dmg`.
2. Drag `Compareel.app` to **Applications**.

**First start on macOS:** Because the app is ad-hoc signed rather than notarized with Apple, double-clicking it the first time shows a warning. Control-click `Compareel` in Finder (or **Applications**) and choose **Open**, then confirm **Open** once. If your macOS version does not show an **Open** button in that dialog, open **System Settings** > **Privacy & Security** and click **Open Anyway**.

## Install on Linux

Compareel is distributed as an x86_64 Flatpak bundle (`Compareel-<version>-x86_64.flatpak`) with application ID `io.github.turxlab.Compareel`. It uses the KDE 6.8 Flatpak runtime (`org.kde.Platform//6.8`) from Flathub.

Install the downloaded bundle for your user account:

```sh
flatpak install --user Compareel-<version>-x86_64.flatpak
```

If Compareel is available from your configured Flathub remote, you can also install it by ID:

```sh
flatpak install flathub io.github.turxlab.Compareel
```

Start Compareel from your desktop application launcher or from a terminal:

```sh
flatpak run io.github.turxlab.Compareel
```

You can also build Compareel natively from source using the instructions in the repository `README.md`.

## First-start update prompt

A few seconds after you open Compareel for the first time (except in the Linux Flatpak, where Flatpak handles updates), Compareel asks: **Would you like Compareel to check for updates automatically?** Choose **Yes** or **No**. Compareel makes no network connection before you answer. You can change this later in **Tools** > **Settings…** (or **Compareel** > **Settings…** on macOS).

## Run your first scan

When you start Compareel with no results open, the welcome card (**Find duplicate videos and images**) guides you through your first scan:

1. **Choose folders or files.** Drag folders or files onto **Sources · drop folders or files here**, or click **Choose…** (or **File** > **Scan sources…**, `Ctrl+O`, `Cmd+O` on macOS).
2. **Confirm your source settings.** In **Scan sources**, click **Add folder…** or **Add files…** to build your list. Leave **Subfolders** ticked for folders whose subfolders should be included, and tick **Protected** next to any folder whose files must never be removed. Click **Save** to return to the main window, or click **Save and scan** to start right away.
3. **Pick media type and comparison mode.** Next to **Find**, choose **Videos** or **Images**. Then choose **Exact copies**, **Similar videos** (or **Similar images**), or **Advanced criteria**. Click **Rules** if you want to adjust thresholds or file filters first.
4. **Start the scan.** Click **Scan** (or press `F5`). A progress bar and status message show how many files have been found and analyzed. You can click **Pause scan** (`Ctrl+Shift+P`) to pause and continue later, or **Stop scan** (`Esc`) to cancel.
5. **Inspect the groups.** When the scan finishes, each row in the lower list is one group of matching files. The first file in the row is loaded on the left (**Ref**), and the selected candidate is loaded on the right for comparison. Click any other thumbnail in a row to load it on the right.

Once results are open, the top toolbar replaces the welcome card so you can change **Scan sources…**, switch between **Videos** and **Images**, pick a mode, adjust **Similarity ≥ %**, or click **Scan** (`F5`) again at any time.

## Compare just two files

If you already have two specific videos or two specific images to check, you do not need to scan a folder. Click **Open two files…** (`Ctrl+Shift+O`, `Cmd+Shift+O` on macOS) on the welcome card or under **File**, or drag two videos or two images directly onto the Compareel window.

Next: [Scanning](scanning.md) explains the comparison modes, folder rules, partial clips, and scan cache in detail.
