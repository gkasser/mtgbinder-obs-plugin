# Twitch channel autodetection (OBS Studio)

This document is the research basis for autodetection of the connected Twitch
login in the MTG Binder OBS plugin. It maps where OBS stores the channel name
after **Settings > Stream > Connect Account**, and how a frontend plugin may
read it safely.

**Security rule:** the plugin must never read, copy, compare, or log the stream
key (`key` in service settings), OAuth access token, refresh token, or expiry
(`Token`, `RefreshToken`, `ExpireTime`, `ScopeVer`). Only the channel login
name is used.

## OBS source revisions consulted

| Tree | Tag / describe | Commit |
| --- | --- | --- |
| obsproject/obs-studio | `31.1.2` | `7778070cbd8e4689d91d90068091ced467c5fdef` |
| obsproject/obs-studio | `32.2.2` | `ba2f32bdf791005443988a4955e963663e16b1ed` |
| obsproject/obs-studio | `33.0.1` | `65c320b46e1af5c7a555161aea6182cdf7a8c524` |

Plugin `buildspec.json` pins OBS **31.1.1**; behaviour matches **31.1.2** for
these auth keys. Line numbers below are for **31.1.2** unless noted.

**Path note:** classic names `UI/auth-twitch.cpp`, `UI/auth-oauth.cpp`,
`UI/auth-base.*`, `UI/window-basic-settings-stream.cpp` were moved under
`frontend/` (OBS 31+). Equivalents:

| Historic (`UI/`) | Current (`frontend/`) |
| --- | --- |
| `auth-twitch.cpp` / `.hpp` | `oauth/TwitchAuth.cpp` / `.hpp` |
| `auth-oauth.cpp` / `.hpp` | `oauth/OAuth.cpp` / `.hpp` |
| `auth-base.cpp` / `.hpp` | `oauth/Auth.cpp` / `.hpp` |
| `window-basic-settings-stream.cpp` | `settings/OBSBasicSettings_Stream.cpp` |

## Where the Twitch login is stored

### 1. Profile config `basic.ini` (persistent)

When Twitch OAuth succeeds, Helix `users` returns `login`, stored in memory as
`TwitchAuth::name`, then persisted under section **`Twitch`**, key **`Name`**.

| Action | File | Line @ 31.1.2 (`7778070`) |
| --- | --- | --- |
| Service id `"Twitch"` | `frontend/oauth/TwitchAuth.cpp` | `31` (`twitchDef`) |
| `name = json["data"][0]["login"]` | `frontend/oauth/TwitchAuth.cpp` | `130` |
| `config_set_string(..., service(), "Name", name)` | `frontend/oauth/TwitchAuth.cpp` | `153` (`SaveInternal`) |
| Load `Name` | `frontend/oauth/TwitchAuth.cpp` | `174` |
| `Auth` / `Type` = `"Twitch"` | `frontend/oauth/Auth.cpp` | `83` (`Auth::Save`) |

`service()` is `"Twitch"` from `twitchDef`, so the INI entry is:

```ini
[Auth]
Type=Twitch

[Twitch]
Name=<twitch_login>
```

(Same `[Twitch]` section also holds secrets the plugin must ignore: `Token`,
`RefreshToken`, `ExpireTime`, `ScopeVer`, plus `UUID` / `DockState` /
`AddonChoice`.)

OAuth secret writes (do **not** read these), `frontend/oauth/OAuth.cpp` @ 31.1.2:

| Key | Line |
| --- | --- |
| `RefreshToken`, `Token` | `52`-`53` |
| `ExpireTime`, `ScopeVer` | `54`-`55` |

Persistence goes through `Auth::Save()` (`frontend/oauth/Auth.cpp` `72`-`86`),
which writes into `OBSBasic::Config()` and `config_save_safe`.

### 2. `OBSBasic::Config()` == profile `basic.ini` == frontend API

| Fact | File | Line @ 31.1.2 |
| --- | --- | --- |
| Profile file name `basic.ini` | `frontend/widgets/OBSBasic_Profiles.cpp` | (constant near profile helpers) |
| `obs_frontend_get_profile_config()` | `frontend/OBSStudioAPI.cpp` | ~`440` |

**Plugin API:**

```c
config_t *cfg = obs_frontend_get_profile_config();
const char *authType = config_get_string(cfg, "Auth", "Type");   /* expect "Twitch" */
const char *name = config_get_string(cfg, "Twitch", "Name");     /* channel login */
```

Do not call `config_get_string` for `Token`, `RefreshToken`, or `ExpireTime`.

### 3. Streaming service / `service.json` (no channel login)

`rtmp_common` settings hold platform name and stream key, saved as profile
`service.json`:

| Fact | File | Notes |
| --- | --- | --- |
| File name `service.json` | `frontend/widgets/OBSBasic_Service.cpp` | `OBSServiceFileName` |
| Settings include `key` | same + `OBSBasicSettings_Stream.cpp` `SaveStream1Settings` | never read `key` |
| `rtmp_common` copies `service` / `key` | `plugins/rtmp-services/rtmp-common.c` | |

`service.json` does **not** store the Twitch login. At most, plugins may read
the non-secret field `"service"` (e.g. `"Twitch"`) via:

```c
obs_service_t *svc = obs_frontend_get_streaming_service(); /* no addref */
obs_data_t *settings = obs_service_get_settings(svc);      /* addref */
const char *service = obs_data_get_string(settings, "service");
obs_data_release(settings);
/* never obs_data_get_string(settings, "key") */
```

### 4. Connect Account UI flow and when `Name` is written

| Step | File | Notes |
| --- | --- | --- |
| Connect button | `OBSBasicSettings_Stream.cpp` `on_connectAccount_clicked` | OAuth login; `GetChannelInfo` sets in-memory `name` |
| Apply stream settings | `SaveStream1Settings` | Sets `main->auth`, calls `LoadUI()`, saves `service.json` including `key`; does **not** call `Auth::Save()` |
| Auth type + Twitch `Name` flush | `Auth::Save()` | Profile switch (`OBSBasic_Profiles.cpp` ~`676`) and OBS exit (`OBSBasic.cpp` ~`1689`) |

So right after a first Connect Account + Apply in the same session, `[Twitch] Name`
may still be missing from the profile config until exit or a profile switch.

### 5. Live panel URL (same-session Connect / reconnect)

`TwitchAuth::LoadUI` (`TwitchAuth.cpp` ~`205`+) creates dock `twitchChat` with CEF
URLs that embed the Helix login, for example:

- `https://www.twitch.tv/popout/<login>/chat`
- `https://dashboard.twitch.tv/popout/u/<login>/stream-manager/...`

`QCefWidget` (obs-browser `panel/browser-panel.hpp`) exposes `urlChanged(const QString &)`.
The plugin attaches to that signal on the chat dock children (by meta-object signal
name only; no CEF headers, no tokens). This yields the login as soon as docks load,
and follows a reconnect when OBS rebuilds the docks with a new login, even before
`Auth::Save()` updates `Twitch/Name`.

`OBS_FRONTEND_EVENT_FINISHED_LOADING` is emitted **before** `Auth::Load()`
(`OBSBasic::OnFirstLoad`, `OBSBasic.cpp` ~`1296`-`1311`), so docks appear shortly
after that event; the plugin re-scans with a short `QTimer::singleShot` and a 7 s
poll.

## Method retained by this plugin (0.2.1)

1. **Live (preferred when available):** parse Helix login from Twitch chat dock
   `urlChanged` URLs. Never read stream key or OAuth tokens.
2. **Persistent:** `obs_frontend_get_profile_config()` → require `Auth`/`Type` ==
   `"Twitch"` → read only `Twitch`/`Name` → normalize (trim, lower case, strip
   leading `@`). Skipped if docks were seen then removed (stale `Name` after
   disconnect before `Auth::Save`).
3. Optional confirmation: streaming service settings field `"service"` only
   (must be `"Twitch"` when present); never `"key"`.
4. Refresh on `OBS_FRONTEND_EVENT_FINISHED_LOADING`,
   `OBS_FRONTEND_EVENT_PROFILE_CHANGED`,
   `OBS_FRONTEND_EVENT_STREAMING_STARTING` /
   `OBS_FRONTEND_EVENT_STREAMING_STARTED`,
   deferred single-shot after load/profile change, plus a light UI-thread
   `QTimer` (7 s).
5. **Capture only while streaming** (auto and manual):
   - start uploads on `OBS_FRONTEND_EVENT_STREAMING_STARTED` (and at plugin
     load / `FINISHED_LOADING` when `obs_frontend_streaming_active()` is true);
   - pause uploads on `OBS_FRONTEND_EVENT_STREAMING_STOPPED` and on
     `OBS_FRONTEND_EVENT_EXIT` (armed channel is kept for the next go-live);
   - a manual channel remains armed across stream ends unless the user clicks
     Stop.
6. Manual channel field remains for stream-key-only setups (no connected
   account / empty `Name` / no CEF docks).
7. Explicit Stop is persisted in the plugin section so autodetection does not
   restart capture against the user's wish. Disconnect of a Twitch-sourced
   session stops capture without setting that flag, so a later reconnect can
   auto-start again.

## Caveats

1. **When `Name` is written:** `Auth::Save()` is not part of Settings Apply.
   Live dock URLs cover the same-session Connect / reconnect case when CEF
   panels load. Config `Name` covers the next OBS start.
2. **Stale `Name` after disconnect:** clearing auth removes docks immediately but
   may leave `Auth`/`Type` and `Twitch`/`Name` until `Auth::Save()`. The plugin
   ignores config `Name` once docks were seen and then removed.
3. **CEF / Wayland:** `TwitchAuth::LoadInternal` returns false without CEF;
   `RegisterTwitchAuth` skips Linux Wayland. Connect Account may be unavailable;
   users fall back to manual channel entry (and config `Name` if present from
   another session/machine).
4. **No channel in `service.json`:** stream-key-only users never get a `Name`;
   manual entry is required.
5. **Multibyte / display names:** stored value is Twitch **login** (Helix), not
   display name.
