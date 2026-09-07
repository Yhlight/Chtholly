# libcurl CFFI Upgrade Fixtures

These are minimal checked-in snapshots of the public declarations used by the
upgrade test, taken from official libcurl releases. They preserve the real
opaque-handle, error-code, string-return and version-macro shapes without
vendoring the complete generated header set.

| Version | Source |
| --- | --- |
| 7.88.1 | https://curl.se/download/curl-7.88.1.tar.xz |
| 8.12.1 | https://curl.se/download/curl-8.12.1.tar.xz |

The independent consumer exercises opaque-handle initialization, pause,
error-string, and cleanup through the native link closure.
