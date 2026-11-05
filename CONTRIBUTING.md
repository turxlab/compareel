# Contributing to Compareel

Compareel is an unfunded volunteer project maintained by one person. Bug fixes and interface translations are welcome. For larger changes, new features or interface changes, please open an issue first so the scope can be agreed on before you write code.

## Translations

Each interface language is a single UTF-8 JSON file in `languages/`.

1. Copy `languages/template.json` to `languages/<code>.json` (for example `languages/de.json` or `languages/pt-BR.json`).
2. Set `"language"` to the language's own name (for example `"Deutsch"`) and `"code"` to its language tag (for example `"de"` or `"pt-BR"`).
3. In `"strings"`, translate the values on the right side. Keep the English keys on the left unchanged.
4. Keep placeholders such as `%1`, `%2`, `%3`, `%4` and line breaks (`\n`) intact.

Missing or empty entries fall back to English automatically. Right-to-left languages mirror the interface layout automatically.

To test a translation without rebuilding, open **Tools > Settings…**, click **languages folder**, place your JSON file there, and select the language in **Language**. Including the file under `languages/` in a pull request embeds it into future builds.

## Code rules

- **C++20 and Qt 6**: Use portable C++20 and Qt 6 (`6.5+`; packaged builds use `6.8.3`). Keep compiler warnings clean (`/W4 /permissive-` on MSVC, `-Wall -Wextra` on GCC and Clang).
- **Platform code**: Operating-system calls and platform headers belong only in `src/platform/`. Everything else in `src/` and `src/qt/` must remain portable across Windows, macOS and Linux.
- **Interface text**: Wrap user-visible text in `Language::text("...")` using the English string as the key, and keep `languages/template.json` in sync.
- **File safety**: Source files are never modified or overwritten. Removed files always go to the Recycle Bin / Trash, never permanent deletion.
- **Dependencies**: Use only decode-only media libraries under licences compatible with GPL-3.0 (such as LGPL-3.0, BSD, MIT, ISC or 0BSD). Do not add encoders, muxers or network protocols to the media readers.

## Building and checks

Follow the build steps for your platform in [README.md](README.md), then run the self-checks before opening a pull request:

- Windows: `.\tools\Run-Checks.ps1`
- macOS and Linux: `tools/run-checks.sh --build <build-dir>`

Every check prints `0` when it passes. Setting `COMPAREEL_TRACE=1` writes step timing output for diagnosing slow operations.

## Licence

By submitting a contribution (code, documentation or translation), you agree to license your work under the [GNU General Public License v3.0](LICENSE) (`GPL-3.0-only`).
