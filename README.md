# ollama-rag-c

Retrieval-Augmented Generation on your own documents, entirely local and without Python: a C99
library and a command line tool, `ragc`, that index folders of `.txt` and `.md` files, find the
passages that answer a question, and let a local model answer it from them, citing its sources.

It is built on [Ollama](https://ollama.com) through [Ollama-C](https://github.com/Topogamer0/Ollama-C)
for embeddings and chat, and on SQLite for storage. SQLite is included, so the only library to
install is libcurl. It runs on Linux, macOS and Windows, where a ready-made `ragc.exe` is
[available](#ragc-on-windows).

```console
$ ragc index notes
indexed  notes/shopping.txt (1 chunk)
indexed  notes/trips/holidays.md (1 chunk)
indexed  notes/work.md (1 chunk)
3 indexed, 0 unchanged, 0 removed, 0 skipped (3 chunks added)

$ ragc -k 2 ask "When do we go to Sicily?"
In August, for a week: first Palermo, then the beaches near Cefalù [1].

Sources:
  [1] notes/trips/holidays.md:1-5 (score 0.71)
  [2] notes/work.md:1-3 (score 0.12)
```

## Features

- **Indexing** of text files and of whole directories, recursively. Chunks of about 800 characters
  (configurable) that overlap by 100, cut between paragraphs when possible, then after sentences, at
  line breaks or between words. Markdown headings are preferred places to cut, and a chunk never
  ends with one.
- **Embeddings** computed by Ollama in batches that span files, normalized and stored in SQLite as
  float32 BLOBs, with the text of each chunk, its file and its position (byte offsets and lines).
- **Incremental**: the index keeps a SHA-256 of every file, so only new and modified files are
  processed again, and files deleted from disk are removed from the index. An interrupted run is
  resumed by the next one.
- **One model per index**: the database remembers its embedding model and refuses to mix in
  embeddings of another one, with a clear error. It also remembers its chunking settings.
- **Search**: exact cosine similarity over all chunks, with top-k and an optional minimum score.
- **Answers**: the chunks found are given to a chat model as numbered passages; the answer is
  streamed through a callback and comes back with its sources.
- **Library API** in the style of Ollama-C: status codes and error messages, option structs with
  designated initializers, no global state, all memory released by `*_free()` functions.
- Prefixes for embedding models that expect them (`embeddinggemma`, `nomic-embed-text`,
  `mxbai-embed-large`, ...).

## Building

Requirements: a C99 compiler, CMake 3.14 or later (3.20 for the `ctest --test-dir` and
`cmake --install` commands below; with older versions run `ctest` and `make install` in `build`), git
(CMake downloads Ollama-C v0.2.0), and
libcurl with its development headers (`libcurl4-openssl-dev` on Debian/Ubuntu, `libcurl-devel` on
Fedora; it comes with macOS). The tests also need Python 3.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build            # optional: runs the tests against a fake Ollama server
sudo cmake --install build        # installs ragc, the library, ollama_rag.h, CMake and pkg-config files
```

### On Windows

With Visual Studio 2022 (or its Build Tools) and the "Desktop development with C++" workload,
which come with CMake, take libcurl from [vcpkg](https://vcpkg.io), then build from a developer
command prompt:

```bat
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg install curl:x64-windows

cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build -C Release
```

`ragc.exe` is then in `build\bin\Release`, next to the DLLs of libcurl and zlib (vcpkg copies the DLLs
that programs need), along with the examples and the tests. Configure with
`-DRAG_INSTALL_RUNTIME_DLLS=ON` to have `cmake --install build --config Release --prefix C:\ragc`
put these DLLs, and the C runtime of the compiler, next to the installed `ragc.exe`, so that the
directory works on any computer.

| CMake option | Default | |
| --- | --- | --- |
| `BUILD_SHARED_LIBS` | `OFF` | build a shared library |
| `RAG_BUILD_CLI` | `ON` | build `ragc` |
| `RAG_BUILD_EXAMPLES`, `RAG_BUILD_TESTS` | `ON` | build the examples and the tests (top-level builds only) |
| `RAG_INSTALL` | `ON` | generate the install target (top-level builds only) |
| `RAG_USE_SYSTEM_SQLITE` | `OFF` | link the system SQLite instead of the bundled one |
| `RAG_USE_SYSTEM_OLLAMA_C` | `OFF` | use an installed Ollama-C (`find_package(ollama-c)`) instead of downloading it |
| `RAG_WERROR` | `OFF` | treat the warnings of ollama-rag-c as errors (`-Werror`, `/WX`), not those of its dependencies |
| `RAG_INSTALL_RUNTIME_DLLS` | `OFF` | Windows: install the DLLs that `ragc.exe` needs next to it (CMake 3.21) |

To build offline, point CMake to a copy of Ollama-C: `-DFETCHCONTENT_SOURCE_DIR_OLLAMA_C=/path/to/Ollama-C`.

You also need Ollama running, with an embedding model and a chat model:

```sh
ollama pull embeddinggemma        # or nomic-embed-text, mxbai-embed-large, ...
ollama pull gemma3                # or any chat model
```

### Using the library

```cmake
find_package(ollama-rag-c REQUIRED)          # after installing
target_link_libraries(my_app PRIVATE ollama::rag)

add_subdirectory(ollama-rag-c)               # or without installing
target_link_libraries(my_app PRIVATE ollama::rag)
```

```sh
cc my_app.c $(pkg-config --cflags --libs ollama-rag-c)
```

## Example

Index a folder of notes and ask a question about them ([`examples/quickstart.c`](examples/quickstart.c)):

```c
#include <ollama_rag.h>
#include <stdio.h>

static int print(const char *text, void *data) {
    (void)data;
    fputs(text, stdout);
    return 0;
}

int main(void) {
    rag_t *rag;
    rag_answer_t *answer;
    rag_ask_options_t options = {.on_token = print};

    if (rag_open("notes.db", NULL, &rag) == RAG_OK && rag_add_dir(rag, "notes", NULL) == RAG_OK &&
        rag_ask(rag, "What did I plan for the holidays?", &options, &answer) == RAG_OK) {
        for (size_t i = 0; i < answer->sources_count; i++)
            printf("\n[%zu] %s, line %zu", i + 1, answer->sources[i].path, answer->sources[i].line_start);
        putchar('\n');
        rag_answer_free(answer);
    }
    int failed = rag_last_error(rag)->code != RAG_OK;
    if (failed) fprintf(stderr, "error: %s\n", rag_last_error(rag)->message);
    rag_close(rag);
    return failed;
}
```

The [`examples`](examples) directory also has [`index-notes`](examples/index-notes.c), which indexes a
folder and reports the progress, and [`chat-docs`](examples/chat-docs.c), an interactive chat over your
documents. They are built in `build/examples/` (`build\bin\<config>` on Windows, where they get their
arguments in UTF-8 on Windows 10 1903 or later; `chat-docs` switches the console to UTF-8 itself, for
`index-notes` run `chcp 65001` first).

## The `ragc` command

```
ragc [options] <command> [arguments]
```

| Command | |
| --- | --- |
| `index <path>...` | index files and directories; only changes are processed, deleted files are removed |
| `search <question>` | show the passages most similar to the question |
| `ask <question>` | answer the question from the documents, with the sources at the end |
| `stats` | show what the database contains |
| `remove <path>...` | remove files, or whole directories, from the index |

| Option | |
| --- | --- |
| `-d`, `--db PATH` | database file (default: `$RAGC_DB`, or `ragc.db` in the current directory) |
| `-e`, `--embed-model NAME` | embedding model (default: `$RAGC_EMBED_MODEL`, the database's, or `embeddinggemma`) |
| `-m`, `--chat-model NAME` | chat model (default: `$RAGC_CHAT_MODEL`, or `gemma3`) |
| `-k`, `--top-k N` | number of passages to use (default: 5) |
| `-s`, `--min-score X` | ignore passages with a similarity below X, from -1 to 1 (default: none) |
| `-H`, `--host URL` | Ollama server (default: `$OLLAMA_HOST`, or `http://127.0.0.1:11434`) |
| `--chunk-size N`, `--chunk-overlap N` | chunking, in characters (default: the database's, or 800 and 100; the default overlap is at most a quarter of the chunk size) |
| `--batch-size N` | chunks per embedding request (default: 32, at most 65536) |
| `--dimensions N` | size of the embeddings, for models that can shorten them |
| `--system TEXT` | system prompt of the chat model (`--system=` for none) |
| `--timeout SECONDS` | network timeout (default: none) |
| `-q`, `--quiet` | print only the results: no progress, no sources |

The question does not need quotes: everything after `search` or `ask` is the question, so options go
before the command (`ragc -k 3 ask when is the report due`). Paths below the current directory are
shown relative to it. The exit status is 0 on success, 1 on errors and 2 for
invalid arguments.

```console
$ ragc -k 2 search ferry to Sicily
1. notes/trips/holidays.md:1-5  (score 0.354)
   # Holidays

   In August we go to Sicily for a week: Palermo first, then the beaches near Cefalù.

   Book the ferry from Naples before June.

2. notes/shopping.txt:1  (score 0.000)
   Shopping list: bread, olive oil, tomatoes, basil.

$ ragc stats
database         ragc.db (128.0 KB)
embedding model  embeddinggemma (768 dimensions)
files            3
chunks           3 (266 bytes of text)
chunking         800 characters, 100 shared
last indexed     2026-10-21 11:03:50

$ ragc -e nomic-embed-text search holidays
ragc: the index 'ragc.db' was built with the embedding model 'embeddinggemma', not 'nomic-embed-text': use the same model, or another database for the other model
Choose another database with --db.
```

### ragc on Windows

Every [release](https://github.com/Topogamer0/ollama-c-rag/releases) has a
`ragc-<version>-windows-x64.zip`: `ragc.exe` with the DLLs it needs (libcurl, zlib, the C
runtime), for Windows 10 or later on x64. It holds a `ragc-<version>-windows-x64` folder: extract it
anywhere, for example in `C:\Tools`, and add `C:\Tools\ragc-<version>-windows-x64` to `PATH`
(Settings > System > About > Advanced system settings > Environment Variables), or run `ragc.exe`
by its full path. Windows may warn about a program downloaded from
the internet ("Windows protected your PC": More info > Run anyway, or `Unblock-File ragc.exe` in
PowerShell). You also need [Ollama for Windows](https://ollama.com/download/windows), running, and
the models:

```bat
ollama pull embeddinggemma
ollama pull gemma3
```

Then, in a Command Prompt or in PowerShell:

```console
C:\Users\me\Documents> ragc index Notes
indexed  Notes\shopping.txt (1 chunk)
indexed  Notes\trips\holidays.md (1 chunk)
indexed  Notes\work.md (1 chunk)
3 indexed, 0 unchanged, 0 removed, 0 skipped (3 chunks added)

C:\Users\me\Documents> ragc -k 2 ask When do we go to Sicily?
In August, for a week: first Palermo, then the beaches near Cefalù [1].

Sources:
  [1] Notes\trips\holidays.md:1-5 (score 0.71)
  [2] Notes\work.md:1-3 (score 0.12)
```

- **Paths** can use `\` or `/`, in any case, with spaces (in quotes) and any character:
  `ragc index "C:\Users\me\OneDrive\Città e note"`. The index stores them with `\`, spelled as on
  the disk, so `notes\a.md` and `NOTES/A.MD` are the same file. A quoted path must not end with a
  backslash (`"C:\My Notes\"` passes a `"` to the program): write `"C:\My Notes"`. Neither the Command
  Prompt nor PowerShell expands `*.md` for programs like `ragc`: give the directory instead.
- **Hidden files**, with the hidden attribute or a name starting with `.`, are skipped.
- **Text files** must be UTF-8 (or ASCII), with any line ends. UTF-16 files, which Windows
  PowerShell 5 writes with `>` and `Out-File`, and Notepad with its "Unicode" encodings, are skipped
  with a message: save them as UTF-8. In files of the ANSI code page, letters like `è` are lost.
- **The database** is `ragc.db` in the current directory, or `--db`, or the `RAGC_DB` variable:
  `set RAGC_DB=C:\Users\me\notes.db` in the Command Prompt, `$env:RAGC_DB = "C:\Users\me\notes.db"` in
  PowerShell, `setx RAGC_DB C:\Users\me\notes.db` to keep it for the next windows.
- **Questions** need quotes when they contain characters that the shell interprets: `&`, `|`, `<`,
  `>`, `^` in the Command Prompt; in PowerShell, anything but letters, digits, spaces and
  `. , : ? !` (`#` starts a comment there, so `ragc ask step #3` would ask "step").
- **The output** is UTF-8: `ragc` switches the console to UTF-8 while it runs. When PowerShell reads
  it (in a pipe, or in a variable), it decodes it with `[Console]::OutputEncoding`: run
  `[Console]::OutputEncoding = [Text.Encoding]::UTF8` first.
- **OLLAMA_HOST**: if it is `0.0.0.0` (as the Ollama documentation suggests to reach the server
  from other computers), Windows refuses to connect to that address: use
  `ragc --host http://127.0.0.1:11434`, or set `OLLAMA_HOST` to `127.0.0.1` in that window.

## How it works

**Chunking.** A chunk holds at most `chunk_size` characters (Unicode code points). When the rest of
the file does not fit, the cut is chosen in the second half of the window, preferring a paragraph
break (a blank line, or the line before a Markdown heading), then a line that ends a sentence, the
end of a sentence, a line break, and a space between words; only text without any of them is cut
inside a word. A chunk never ends right after a heading, and `#` lines inside fenced code blocks
are not headings. The next chunk starts `chunk_overlap` characters earlier, moved to the start of
a line, a sentence or a word. Invalid UTF-8 is replaced before being sent to Ollama; files that
contain NUL bytes are considered binary and skipped (UTF-16 files are reported as such).

**Storage.** The SQLite database has four tables: `meta` (the embedding model, the size of the
embeddings and the chunking settings), `files` (path, SHA-256 of the content, settings used),
`chunks` (text, byte offsets, first and last line) and `vectors` (one normalized float32 BLOB per
chunk, in a table of its own so that searches only read vectors). Paths are stored absolute, with
the links of their directories resolved; on Windows with `\` separators and the names spelled as
on the disk, since names there do not depend on case.

**Incremental indexing.** A file is processed again when its hash or the settings it was indexed
with (chunk size, overlap, embedding size, document prefix) change. Embeddings are requested in
batches of `batch_size` chunks, which can span several small files, and every file is written as
soon as all its chunks have been embedded, so an interruption loses at most the files in
progress. `rag_add_dir` / `ragc index <dir>` also removes the indexed files below the directory
that no longer exist.

**Search.** The question is embedded with the model of the index (with its query prefix, see
below), and compared with every stored vector: the vectors are normalized, so the cosine
similarity is a dot product. A heap keeps the `top_k` best chunks.

**Answers.** `rag_ask` gives the chunks found to the chat model as numbered passages
(`[1] holidays.md, lines 1-5` followed by the text), after a system prompt that asks to answer
only from them, to say so when they do not contain the answer, and to cite them as `[1]`. The
`sources` of the answer are these passages, in the same order.

**Prefixes.** Some embedding models are trained to embed questions and documents differently.
For well-known models the right prefixes are used automatically; others get none, and
`rag_options_t.query_prefix` / `document_prefix` can set them.

| Model | Query prefix | Document prefix |
| --- | --- | --- |
| `embeddinggemma` | `task: search result \| query: ` | `title: none \| text: ` |
| `nomic-embed-text` | `search_query: ` | `search_document: ` |
| `mxbai-embed-large`, `snowflake-arctic-embed` | `Represent this sentence for searching relevant passages: ` | |
| `snowflake-arctic-embed2` | `query: ` | |
| `qwen3-embedding` | `Instruct: Given a web search query, retrieve relevant passages that answer the query` + newline + `Query: ` | |

## Library API

Everything is declared in [`include/ollama_rag.h`](include/ollama_rag.h).

| Function | |
| --- | --- |
| `rag_open(path, &options, &rag)` / `rag_close(rag)` | open (or create) an index; `options` can be `NULL` |
| `rag_add_file(rag, path, &stats)` | index a file, whatever its extension |
| `rag_add_dir(rag, path, &stats)` | index the text files below a directory, remove the deleted ones |
| `rag_remove_file(rag, path, &stats)` | remove a file, or every file below a directory |
| `rag_search(rag, query, &options, &results)` / `rag_results_free()` | the most similar chunks |
| `rag_ask(rag, question, &options, &answer)` / `rag_answer_free()` | answer a question, streaming it |
| `rag_stats(rag, &stats)` | model, sizes and counts of the index |
| `rag_last_error(rag)` | code, message and HTTP status of the last error |

Options use designated initializers, and fields left at zero take their default:

```c
rag_options_t options = {
    .embed_model = "nomic-embed-text", /* NULL: the database's, or embeddinggemma */
    .chat_model = "llama3.2",          /* NULL: gemma3 */
    .host = "http://localhost:11434",  /* NULL: $OLLAMA_HOST */
    .chunk_size = 600,                 /* 0: the database's, or 800 */
    .progress = on_progress,           /* called for every file indexed, unchanged, removed or skipped */
};
```

**Errors.** Every function returns `RAG_OK` or an error code, and `rag_last_error(rag)` gives a
message (and the HTTP status code for errors returned by Ollama):

| Code | |
| --- | --- |
| `RAG_ERROR_INVALID` | invalid argument or option |
| `RAG_ERROR_IO` | a file or directory could not be read (or is binary) |
| `RAG_ERROR_DATABASE` | SQLite error, or not an ollama-rag database |
| `RAG_ERROR_MODEL_MISMATCH` | the index was built with another embedding model, or another embedding size |
| `RAG_ERROR_OLLAMA` | Ollama could not be reached, the model is missing, ... |
| `RAG_ERROR_NOT_FOUND` | the file, directory or database does not exist, or is not indexed |
| `RAG_ERROR_EMPTY` | `rag_ask` on an index without documents |
| `RAG_ERROR_ABORTED` | a callback returned nonzero |
| `RAG_ERROR_MEMORY` | out of memory |

`rag_open` sets the handle even when it fails (unless out of memory), so that `rag_last_error` can
tell why; close it with `rag_close` in any case.

**Memory.** Results and answers belong to the caller and are released with `rag_results_free()` and
`rag_answer_free()`; the strings of `rag_stats_t` belong to the handle. The library has no global
state: every handle has its own SQLite connection and its own Ollama client.

**Threads and processes.** A handle must not be used by several threads at the same time; use one
handle per thread. Several handles, or several `ragc` processes, can share the same database: the
embedding model and size are checked again under SQLite's write lock before anything is stored. In multi-threaded programs, call
`curl_global_init(CURL_GLOBAL_DEFAULT)` once at startup.

## Limits

- **Exact search.** Every search reads all the vectors: this is fine up to about 100,000 chunks
  (with 768 dimensions that is about 300 MB per question), but there is no approximate index for
  larger collections.
- **Text only.** Plain text and Markdown, read as UTF-8. Other text formats can be added file by
  file with `rag_add_file`, but there is no extraction of text from PDF, Word or HTML documents.
- **Characters, not tokens.** Chunk sizes are counted in characters; very long chunks are truncated
  by the embedding model.
- **One embedding model per database.** Changing it means building another database.
- **Single questions.** `rag_ask` has no conversation history.
- **Platforms.** Linux, macOS and Windows (MSVC, x64) are tested. The Windows zip is for x64 only.
- **Bundled SQLite.** A static build contains SQLite's symbols: if the program links another
  SQLite, configure with `-DRAG_USE_SYSTEM_SQLITE=ON`.

## Tests

`ctest --test-dir build` runs the unit tests (chunking, UTF-8, similarity, database, files) and the
integration tests, which drive the library and `ragc` against a fake Ollama server
([`tests/mock_server.py`](tests/mock_server.py)) that returns deterministic embeddings: no Ollama
or model is needed. The CI builds and tests on Linux (GCC and Clang, static and shared, with
AddressSanitizer and UndefinedBehaviorSanitizer), on macOS, and on Windows (MSVC, static and DLL
builds, with libcurl from vcpkg). A tag `v*` builds the Windows zip and attaches it to the release.

## License

[MIT](LICENSE). The bundled [SQLite](third_party/sqlite) is in the public domain; Ollama-C is released
under the MIT License.
