# MTG Trade OBS Plugin

Native OBS plugin (C++) that sends your stream frames to [MTG Trade](https://mtg-trade.fr/twitch)
for Magic card identification, with a timestamp close to the broadcast moment.
It greatly reduces latency compared to the Twitch extension's passive mode.

The plugin is **open source and inspectable**: it contains no secrets. Frames are
sent with the Twitch channel name. The server accepts or rejects each image
according to the channel rate limit (with a small timing slack) and returns the
allowed interval on every response.

## Installation

Download the archive for your OS from the
[latest release](https://github.com/gkasser/mtgbinder-obs-plugin/releases/latest)
(Windows, macOS, Linux), then install it like any standard OBS plugin.

## Usage

1. In OBS, open `Tools > MTG Binder OBS`.
2. Enter the Twitch channel name and click `Start capture`.

No link code. The first accepted frame creates the channel on the free plan if
it does not exist yet. Each response carries `frame_interval_secs` (and
`retry_after_ms` when the frame is refused) so the plugin follows the server cadence.

## API contract

The plugin only depends on two HTTP endpoints of `mtg-trade.fr`, documented in
[`docs/mtg-trade-api.md`](docs/mtg-trade-api.md). This is the only surface shared
with the backend.

## Build

This project follows the structure of the
[official OBS template](https://github.com/obsproject/obs-plugintemplate):
CMake + `buildspec.json` (OBS/Qt/obs-deps versions) + presets.

OBS and Qt dependencies are downloaded automatically via `buildspec.json`.
JPEG encoding uses [`stb_image_write`](https://github.com/nothings/stb)
(vendored, public domain), so there is **no system libjpeg dependency**. `libcurl`
comes from obs-deps (Windows/macOS) or the system (Linux).

```bash
# Linux
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64

# Windows (PowerShell)
cmake --preset windows-x64
cmake --build --preset windows-x64

# macOS
cmake --preset macos
cmake --build --preset macos
```

## CI / Releases

GitHub Actions workflows build Linux, Windows and macOS on every push/PR.
Pushing a tag (`X.Y.Z`) creates a multi-OS downloadable release, referenced by the
site's `/twitch` page.

## Security

No server secrets. Rate limiting is enforced per channel on `mtg-trade.fr`. A frame
that arrives slightly early (about 10% of the interval) is still accepted.
