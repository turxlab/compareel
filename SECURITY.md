# Security Policy

Compareel is an unfunded volunteer project maintained by one person. Only the latest release receives fixes.

## Supported versions

| Version | Supported |
|---|---|
| 1.0.x (latest release) | Yes |
| Older versions | No |

Fixes are published as a new release; older versions do not receive backports.

## Reporting a vulnerability

Do not open a public issue or pull request for a security problem.

Report vulnerabilities through GitHub private vulnerability reporting only:

1. Open the [Security](https://github.com/turxlab/compareel/security) tab of `turxlab/compareel`.
2. Click **Report a vulnerability**.
3. Include the Compareel version, operating system, package type, steps to reproduce, and, if needed, a minimal test file containing no private data.

Reports are reviewed on a best-effort basis. Compareel has no funding and does not offer a bug bounty or paid rewards.

## Scope

Compareel is a local desktop application. It runs no background service, opens no listening network sockets, and never sends media files or scan results over the network. Its only network request is the optional GitHub release check (**Help > Check for updates…** or startup update check when enabled in **Tools > Settings…**; disabled in the Linux Flatpak).

### Untrusted media and third-party decoders

Scanning or previewing untrusted videos and images passes file bytes to third-party and system decoders:

- Bundled decode-only **FFmpeg** (built with encoders, muxers, devices and network protocols disabled, with dav1d, zimg, libjxl, zlib and liblzma)
- Bundled **libheif** (built with external plugin loading and encoders disabled)
- **Qt** (`QtBase`, `QtSvg` and `Qt Image Formats`; SVG inputs are checked before rendering to reject scripts, `<foreignObject>`, DTDs/entities, external references and excessive `<use>` expansion)
- Operating-system media frameworks (Media Foundation and WIC on Windows; VideoToolbox and ImageIO on macOS)

Issues in Compareel's own code (`src/`), its SVG and image size guards, its file operation safety checks, or its build and packaging scripts are in scope here.

Bugs inside unmodified upstream libraries (FFmpeg, libheif, Qt, dav1d, libjxl, zimg) or operating-system codecs should be reported upstream. If a bug in a bundled library affects Compareel, it is reported upstream and Compareel updates its bundled version once an upstream release or patch is available.

### Local data

As noted in **Help > Data and privacy**, the local scan cache, saved `.cvsession` files, operation history and quarantine copies are stored unencrypted under the user's account. **View > Privacy mode** hides previews on screen only; protecting local disk data from other users with access to the same account or disk is outside Compareel's scope.
