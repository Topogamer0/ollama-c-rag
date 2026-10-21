# SQLite

The SQLite amalgamation, version 3.53.2
(`SQLITE_SOURCE_ID` 2026-06-03 19:12:13 d6e03d8c777cfa2d35e3b60d8ec3e0187f3e9f99d8e2ee9cac695fd6fcdf1a24),
unmodified. It is compiled into the library so that SQLite does not need to be installed;
configure with `-DRAG_USE_SYSTEM_SQLITE=ON` to link the system SQLite instead.

| File | SHA3-256 |
| --- | --- |
| `sqlite3.c` | `44fd61b9f93b4155105cb2d80c957ae6c64a8b5bd6ed51a4992f0dbd438e4e11` |
| `sqlite3.h` | `ed9ca761da18dd8c709938821ca4b21d3353660ad91889e921837cd872f564ab` |

SQLite is in the public domain: https://sqlite.org/copyright.html

To upgrade, replace both files with the ones of a newer amalgamation from
https://sqlite.org/download.html and update this file.
