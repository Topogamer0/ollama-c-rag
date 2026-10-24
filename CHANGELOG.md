# Changelog

All notable changes to this project are documented in this file. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [0.2.1] - 2026-10-24

### Added

- Homebrew: `brew install topogamer0/tap/ollama-c topogamer0/tap/ollama-c-rag` on macOS and Linux
  (see the README). The release workflow updates the formula of the
  [tap](https://github.com/Topogamer0/homebrew-tap) when a `v*` tag is pushed.

### Changed

- Ollama-C v0.2.1. With `RAG_USE_SYSTEM_OLLAMA_C`, the installed Ollama-C must be 0.2.1 or later,
  and so do the CMake package and the pkg-config file of ollama-rag-c.
- Release workflow: when the release of the tag is published from the GitHub page while the
  workflow is creating one, the workflow attaches the zip to it instead of failing. A release made
  from the page before, published or draft, was already used as it is.

### Fixed

- `OLLAMA_HOST=0.0.0.0` (or `::`), which makes the Ollama server listen on every interface, now
  works on every system: like the `ollama` command, the library and `ragc` connect to `127.0.0.1`
  (or `::1`). Windows refused connections to `0.0.0.0` and `[::]`, and a bare `::` was refused as an
  invalid host on every system; it is now accepted like `[::]`. The same goes for `--host` and the
  `host` option.

## [0.2.0] - 2026-10-21

### Added

- Windows support with MSVC (x64), static library and DLL. Paths are UTF-8 and may contain spaces
  and any character, with `\` or `/`, in any case, and longer than `MAX_PATH`. The index stores
  them with `\` and spelled as on the disk. Files with the hidden or system attribute are skipped.
  An unreachable network share is an I/O error, not a missing directory, so its files stay in the
  index.
- `ragc.exe` for Windows x64, with the DLLs it needs, attached to each release as
  `ragc-<version>-windows-x64.zip`. It reads its arguments and environment as UTF-16 and writes
  UTF-8 to the console.
- CMake options `RAG_WERROR` (warnings as errors, for ollama-rag-c only) and
  `RAG_INSTALL_RUNTIME_DLLS` (Windows: install the DLLs of `ragc.exe` next to it). A DLL build
  exports the functions of `ollama_rag.h` (`RAG_SHARED` for its users, set by the CMake target).
- CI on Windows (MSVC, static and DLL, `/W4 /WX`), and a release workflow.

### Changed

- Ollama-C v0.2.0, which builds with MSVC. The CMake package requires Ollama-C 0.2.
- An Ollama host that the Python client rejects (the `host` option, `--host` or `OLLAMA_HOST`:
  unbalanced brackets, a port that is not a number up to 65535, ...) is refused with
  `RAG_ERROR_INVALID` and a message that names it; 0.1.0 connected to whatever URL Ollama-C made of
  it. An `OLLAMA_API_KEY` with a line break is `RAG_ERROR_INVALID` instead of `RAG_ERROR_MEMORY`.
- UTF-16 text files (written by Windows PowerShell 5 and by Notepad's "Unicode" encodings) are
  reported as such, with what to do about them, instead of as binary files.

### Fixed

- A file renamed in another case only (`Fruit.md` to `fruit.md`) stayed in the index under its old
  name where names do not depend on case (Windows, and macOS usually). On Windows the same holds
  for a directory renamed in another case.
- `ragc search` showed documents with CRLF line ends with stray spaces and blank lines.
- A directory listing that fails half-way is reported as an error, instead of looking complete:
  the files it did not list were skipped without a word.

## [0.1.0] - 2026-10-21

First release: a C99 library and the `ragc` command over Ollama (through Ollama-C) and SQLite
3.54.1 (bundled, or the system one). Indexing of text and Markdown files and directories, with
chunks cut between paragraphs, sentences or words, and incremental updates by SHA-256; embeddings
in batches, stored as normalized float32 BLOBs; exact cosine search with top-k and a minimum score;
answers from a chat model with numbered sources; one embedding model per database; prefixes for
the embedding models that expect them; CMake and pkg-config packages, examples and tests against a
fake Ollama server.

[0.2.1]: https://github.com/Topogamer0/ollama-c-rag/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/Topogamer0/ollama-c-rag/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/Topogamer0/ollama-c-rag/releases/tag/v0.1.0
