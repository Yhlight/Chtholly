# SQLite safety-wrapper vertical

This example is the first application-driven standard-library vertical after
the preview closure. It keeps the physical SQLite declarations in CFDL and
adds only the resource facts that a safe Chtholly caller needs. A tiny native
adapter calls the real SQLite library so the C ABI remains honest while the
Chtholly-facing handle stays opaque:

- `chtholly_sqlite_open_memory` returns an owned `Database` and creates a `close`
  obligation;
- `chtholly_sqlite_close` consumes that obligation;
- non-zero SQLite status is projected to `Result<void, i32>`;
- Chtholly code never sees the SQLite carrier layout.

The checked-in `sqlite.cfdl` is the reviewed subset of a CFFI-generated
declaration for the adapter, with resource and error overlays retained by the
binding author. The test copies this source into an isolated project, supplies
the host SQLite and adapter library paths, checks and builds it, then runs both
a successful in-memory open/close and a deterministic invalid-path failure.
No Chtholly or CFDL syntax is added by this vertical.

To run it manually, replace `__SQLITE_PROVIDER__` and `__SQLITE_LIBRARY__` in
`chtholly.toml` with absolute library paths for the target host, then run
`chthollyc run`.
