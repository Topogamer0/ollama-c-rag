# SQLite

The SQLite amalgamation, version 3.54.1
(`SQLITE_SOURCE_ID` 2026-10-16 14:45:20 555e07ef33f69a0957d24d8e555b702464a131d6f1607f4c3c1e4551e8e6ca76),
unmodified, from https://sqlite.org/2026/sqlite-amalgamation-3540100.zip (the SHA3-256
hash of `sqlite3.c` matches the one published in the release notes,
https://sqlite.org/releaselog/3_54_1.html). It is compiled into the library so that
SQLite does not need to be installed; configure with `-DRAG_USE_SYSTEM_SQLITE=ON` to
link the system SQLite instead.

| File | SHA3-256 |
| --- | --- |
| `sqlite3.c` | `e2b869d3003b33c942b7dd7063604fbc53553e1b66609570df39b9b7b558ee81` |
| `sqlite3.h` | `15ef3f5dbc4a7a980f828fa0d1e473cee5a124e7bb77e9bc70fc0bfa3f9a2af8` |

SQLite is in the public domain: https://sqlite.org/copyright.html

To upgrade, replace both files with the ones of a newer amalgamation from
https://sqlite.org/download.html and update this file.
