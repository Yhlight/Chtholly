# SQLite CFFI Upgrade Fixtures

These are unmodified `sqlite3.h` files extracted from official SQLite
amalgamation archives. SQLite is in the public domain:
https://www.sqlite.org/copyright.html

| Version | Archive | Header SHA-256 |
| --- | --- | --- |
| 3.40.1 | `https://www.sqlite.org/2022/sqlite-amalgamation-3400100.zip` | `dc419c400665bd43b335d04d7562d78e1d5dcd464fa0d6150c9d5c3bc5d705f4` |
| 3.53.4 | `https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip` | `919e7f2e8ed1d8f56ac17b412b8971c76aa5d1a879752cc6058f75e7d5910e1d` |

Tests consume these checked-in files directly and never download fixtures at
runtime. The independent consumer executes version and initialization calls
through the native link closure.
