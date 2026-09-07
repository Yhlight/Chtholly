# zlib CFFI Upgrade Fixtures

These checked-in headers contain the public API surface used by the upgrade
test, copied from the official zlib releases. They intentionally keep the
versioned declarations and macros needed to exercise mechanical regeneration;
runtime probes link the host zlib library.

| Version | Upstream release |
| --- | --- |
| 1.2.11 | https://zlib.net/fossils/zlib-1.2.11.tar.gz |
| 1.3.1 | https://zlib.net/zlib-1.3.1.tar.gz |

The selected API is `zlibVersion`, `compressBound`, and the object-like
`ZLIB_VERNUM` constant. The independent consumer executes version, bound, and
compile-flag calls through the native link closure. No fixture is downloaded
during tests.
