# Third-party dependencies

This repository is a local development project. No license for the application's original source has been selected yet. Dependencies retain their own licenses; a distributable release needs the corresponding license notices and source/relinking materials where required.

| Dependency | Use | Source / license information |
|---|---|---|
| Qt 6.8.3 Core, Gui, Widgets, OpenGL, OpenGLWidgets, Sql, Test | Native UI, rendering integration, database, tests | https://www.qt.io/licensing/open-source-lgpl-obligations |
| libmpv, pinned Windows development build | Video decoding, seeking, audio, GPU render API | https://github.com/mpv-player/mpv/blob/master/Copyright |
| Windows libmpv build | Bundled runtime and public headers | https://github.com/shinchiro/mpv-winbuild-cmake |
| FFprobe / FFmpeg | Metadata, export decode/encode/audio/muxing, and synthetic fixtures. Local gyan.dev 7.0 essentials GPLv3 binaries pinned by SHA-256 | https://ffmpeg.org/legal.html and https://www.gyan.dev/ffmpeg/builds/ |
| SQLite through Qt's QSQLITE plugin | Local database | https://www.sqlite.org/copyright.html |
| aqtinstall | Development SDK download tool | https://github.com/miurahr/aqtinstall |

The supplied mpv Windows build is a GPL build with its own bundled dependencies. Qt Quick 3D is not used. Dependency downloads and generated binaries are ignored by Git. The runtime archive URL and hash are tracked in `dependencies.lock.json`.
