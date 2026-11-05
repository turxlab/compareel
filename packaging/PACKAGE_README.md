# Compareel {{VERSION}}

Compareel finds duplicate and similar videos and images and lets you review them side by side before
anything is removed. Everything runs locally.

Install it with `Compareel-<version>-x86_64-Win.exe`, or use the portable zip without installing: keep `Compareel.exe`
together with the DLLs and plugin folders next to it.

## Getting started

1. Choose **Videos** or **Images** and a comparison mode in the toolbar.
2. Open **Scan sources…** and add folders or individual files. You can protect a folder, exclude one, or only
   compare files across different folders. **Scan** (F5) rescans the saved sources directly.
3. Each result row is a group. The first file in the current sort order is shown on the left; click another
   thumbnail in the row to compare it on the right.

### Comparison modes

- **Exact copies** — identical file content, verified with SHA-256.
- **Similar videos** — frames at 10, 30, 50, 70 and 90 % of the duration. **Recognize black borders, mirrored
  and rotated copies** in the scan rules adds tolerance for letterboxing, mirroring, rotation and light cropping.
- **Similar images** — visual signatures after orientation, color profile and transparency are normalized.

Similarity is about the sampled pictures only; it says nothing about audio, subtitles, metadata or quality.

## Reviewing

- The chain button links both players; **Offset** shifts the right video. **Auto-align partial clip**
  (Ctrl+Shift+A) searches for a shared section between a trimmed clip and its source and sets the offset.
  Alignment is never treated as proof of a duplicate.
- **Tools → Compare sampled audio** checks short audio windows of the selected pair.
- Wipe, overlay and difference views, shared zoom and pan, frame stepping and **B** to flip between sides.
- Review decisions (Ctrl+1…3): **Reviewed**, **Keep both files**, **Review later**. The last two protect the
  files from cleanup until the decision is removed. **Ctrl+M** marks the right file for cleanup, **Alt+↓** moves
  to the next pair.
- Keys: ↑/↓ groups, ←/→ candidates, Home/End first/last candidate, Ctrl+L link, Ctrl+Shift+H hide previews.

## Cleaning up

- Removed files always go to the Recycle Bin (Trash on Linux and macOS); nothing is deleted permanently.
- **Review → Move other copies to Recycle Bin…** builds a plan for all groups with a live preview. Choose what
  to keep in each group — first in sort order, highest resolution, longest, highest frame rate, largest,
  smallest, newest or oldest — and optionally a preferred folder. Protected files are never removed and every
  group keeps at least one file. Files can be taken out of the plan one by one, and the plan can be exported
  as CSV.
- Right-click a file for **Copy to folder…**, **Move to folder…** and **Quarantine…**. Copies are verified with
  SHA-256 and never overwrite existing files. **File → Undo last copy or move…** (Ctrl+Z) works even after a
  restart. Moves to the Recycle Bin are undone from the Recycle Bin itself.
- **Operation history** records every file operation and its result.

## Sessions and recovery

- **File → Save review session…** stores groups, decisions and the view in a `.cvsession` file that opens
  instantly without rescanning. Files that changed or disappeared are left out on opening.
- With **Remember last review** (Settings) the current review is saved in the background and can be reopened
  with **File → Resume last review**.
- Before a cleanup the review is saved, so an interrupted cleanup can be reopened with **File → Recover last
  cleanup review…**. Scans can be paused and resumed, also after an unexpected exit. No file operation is ever
  repeated automatically.

## Settings, privacy and languages

- **Tools → Settings…** (Ctrl+,) selects the theme and language without a restart. Scan rules are under
  **Tools → Scan rules…** (Ctrl+Shift+,).
- The eye button hides previews on screen. **Help → Data and privacy** lists the local data Compareel keeps
  (cache, sessions, decisions, operation history). **Clear scan cache** removes cached media analysis only.
- Each language is one JSON file in the `languages` folder. To add one, copy `template.json`, fill in
  `language` and `code`, and translate the values; keep `%1`, `%2` and line breaks as they are.

Supported formats and limits: [SUPPORTED_FORMATS.md](SUPPORTED_FORMATS.md).
License: GNU GPL v3 (`LICENSE`); third-party notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
