---
title: Cleaning up
description: Safely move duplicate copies to the Recycle Bin or Trash, protect reference folders, copy, move, or quarantine files, and undo file operations.
nav_order: 5
---

# Cleaning up

Compareel never permanently deletes files and never modifies or overwrites your media files. Every removal sends files to the Windows Recycle Bin (Trash on macOS and Linux).

## Safety checks before any removal

Before moving a file to the Recycle Bin (Trash), Compareel checks on disk that:

1. The file's identity (volume, file index, size, and modified time) has not changed since the scan.
2. At least one unchanged matching copy in the group still exists on disk and is being kept. If an external drive was unplugged or every other copy was already moved outside Compareel, the removal is refused so you do not lose the last copy.
3. A recovery snapshot of your current review (`last-cleanup.cvsession`) has been saved first.

## Protected files

A file is **Protected** and cannot be marked for cleanup, moved, quarantined, or sent to the Recycle Bin (Trash) when any of the following apply:

- **Protected folder** — Its folder is ticked as **Protected** in **File** > **Scan sources…**.
- **Per-file lock** — You clicked **Protect this file** (the lock icon in the left or right player header, or right-clicked a thumbnail and toggled **Protect this file**). Pairs opened from **Tools** > **Find partial clips in folders…** also start with both files locked until you unlock one.
- **Review decision** — The file belongs to a pair marked **Keep both files** (`Ctrl+2`) or **Review later** (`Ctrl+3`).

You can still copy a protected file with **Copy to folder…**.

## Removing a single file

- Click **Move to Recycle Bin** (**Move to Trash** on macOS and Linux), the trash icon on either player panel header.
- Or click either preview to focus it and press `Delete` (`Delete` or `Cmd+Backspace` on macOS) for **Review** > **Move focused file to Recycle Bin…** (**Move focused file to Trash…**), which asks for confirmation first.

## Marking files as you review

If you prefer to pick files one by one or review proposed marks before removing anything:

1. Press `Ctrl+M` (`Cmd+M` on macOS) or click **Mark right file for cleanup** in the review bar (or right-click a thumbnail) to mark or unmark candidates. Marked thumbnails are dimmed and badged **Marked for cleanup**.
2. Alternatively, choose **Review** > **Preview cleanup marks…** to propose marks across all groups at once (keeping the left file in each group and all protected files). Uncheck any row you do not want marked, then click **Apply marks**.
3. When ready, click **Clean up marked (N)…** in the review bar or choose **Review** > **Move marked files to Recycle Bin…** (**Move marked files to Trash…**). Compareel verifies that each marked file still has an unmarked matching copy in its group and asks for confirmation before moving them.
4. Choose **Review** > **Clear cleanup marks** to remove all marks without moving any files.

## Building a cleanup plan across all groups

Choose **Review** > **Move other copies to Recycle Bin…** (**Move other copies to Trash…**) to open a live preview covering all result groups (including groups currently hidden by the text or match-type filter).

1. **Keep in each group** — Choose which file to retain in every group:
   - **Reference file (shown on the left)**
   - **Highest resolution**
   - **Longest duration** (videos only)
   - **Highest frame rate (FPS)** (videos only)
   - **Largest file size (not a quality guarantee)**
   - **Smallest file size**
   - **Newest modified date**
   - **Oldest modified date**
   - **Oldest creation date**
   - **Oldest date taken (EXIF)** (images only)
2. **Preferred folder** — Keep **Any folder (no folder preference)**, or pick a specific folder where kept copies should stay whenever possible.
3. **Priority order** — Click **How this plan is chosen** in the dialog to see the tie-breaker rules:
   1. **Preferred folder** (if selected)
   2. Selected **Keep in each group** rule
   3. **Protected** status
   4. Current **Reference** file
4. **Adjust or export the plan** — Every group keeps at least one file, a file retained by any group is never removed by another group, and indirect matches that do not directly match the retained file are left out. Uncheck individual files in the tree to keep them as well, or click **Export plan…** to save the planned actions as a CSV file.
5. **Confirm** — Click **Move N files to Recycle Bin** (**Move N files to Trash**). **Cancel** is focused by default so pressing `Enter` cannot start a cleanup by mistake.

## Copy, move, and quarantine

Right-click a file's preview, filename, or thumbnail (or use **File** > **Left file** / **Right file**) to transfer individual files without overwriting anything at the destination:

- **Copy to folder…** — Writes a new copy into your chosen folder and verifies the written file with a SHA-256 hash. Existing files are never overwritten.
- **Move to folder…** — Creates a SHA-256 verified copy at the destination first, and only then moves the original file to the Recycle Bin (Trash).
- **Quarantine…** — Works like **Move to folder…**, placing the verified copy inside a new timestamped subfolder (`Compareel-YYYYMMDD-HHmmss-...`) in your chosen folder and moving the original to the Recycle Bin (Trash). Quarantine copies are plain media files, not encrypted archives.

To copy or move every unique file across all result groups at once (skipping protected files when moving), use **File** > **All result files** > **Copy all result files…** or **Move all result files…**. If destination file names collide, you can choose **Rename all conflicts: _1**, **Rename all conflicts: (1)**, or **Manual names (edit target column)**.

## Undo and Operation history

- **Undo copies, moves, and quarantine:** Choose **File** > **Undo last copy or move…** (`Ctrl+Z`, `Cmd+Z` on macOS). Undo works even after you close and reopen Compareel. Undoing a move or quarantine restores a verified copy back to the original path (and moves the destination copy to the Recycle Bin / Trash); undoing a copy moves the created copy to the Recycle Bin (Trash). If either file changed or the target path is now occupied, Compareel refuses to overwrite it. Batch transfers are undone one file at a time.
- **Restore Recycle Bin / Trash removals:** Files removed directly with **Move to Recycle Bin** (**Move to Trash**) are restored from your operating system's Recycle Bin (Trash).
- **Operation history:** Choose **Tools** > **Operation history** to view every recorded removal, copy, move, quarantine, and undo, along with timestamps, outcomes, and paths. Use **Export history…** to save the log as CSV, or **Clear history…** to clear finished entries (note that clearing operation history disables **Undo last copy or move…** for earlier transfers).

## Sessions and cleanup recovery

- **Save and open sessions:** Choose **File** > **Save review session…** (`Ctrl+S`, `Cmd+S` on macOS) and **File** > **Open review session…** (`Ctrl+Shift+L`, `Cmd+Shift+L` on macOS) to save or reopen `.cvsession` files (up to 100,000 files or 32 MiB) without rescanning. Files that changed or disappeared since the session was saved are left out on opening.
- **Remember last review:** When **Remember last review** is enabled in **Tools** > **Settings…** (on by default), Compareel saves your active review in the background (`last-review.cvsession`) so you can reopen it with **File** > **Resume last review** (or from the welcome card). Use **Forget last review** in **Settings** (or **File** > **Forget saved last review…**) to delete that saved review and turn remembering off.
- **Recover an interrupted cleanup:** Before any Recycle Bin (Trash) removal or file transfer, Compareel writes `last-cleanup.cvsession`. If Compareel or your computer stops mid-cleanup, choose **File** > **Recover last cleanup review…** to reopen the review. Compareel never repeats interrupted file operations automatically.

Next: [Privacy and local data](privacy.md) explains where local caches and sessions are stored, how **Hide all previews** works, and how to clear local records.
