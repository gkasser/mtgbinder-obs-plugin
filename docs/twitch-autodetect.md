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
| `/tmp/obs-studio-src` (master at clone) | `33.0.1` | `65c320b46e1af5c7a555161aea6182cdf7a8c524` |
| `/tmp/obs-studio-32` | `32.2.2` | `ba2f32bdf791005443988a4955e963663e16b1ed` |
| `/tmp/obs-studio-31` | `31.1.2` | `7778070cbd8e4689d91d90068091ced467c5fdef` |

Plugin `buildspec.json` pins OBS **31.1.1**; behaviour matches **31.1.2** for
these auth keys.

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

### 1. Profile config `basic.ini` (authoritative for plugins)

When Twitch OAuth succeeds, Helix `users` returns `login`, stored in memory as
`TwitchAuth::name`, then persisted under section **`Twitch`**, key **`Name`**.

| Action | File | Line @ commit |
| --- | --- | --- |
| Service id `"Twitch"` | `frontend/oauth/TwitchAuth.cpp` | `31` @ `65c320b` (33.0.1); same pattern @ 32.2.2 / 31.1.2 |
| `name = json["data"][0]["login"]` | `frontend/oauth/TwitchAuth.cpp` | `140` @ `65c320b`; `139` @ `ba2f32b` (32.2.2); `130` @ `7778070` (31.1.2) |
| `config_set_string(..., service(), "Name", name)` | `frontend/oauth/TwitchAuth.cpp` | `164` @ `65c320b`; `163` @ `ba2f32b`; `153` @ `7778070` |
| Load `Name` | `frontend/oauth/TwitchAuth.cpp` | `186` @ `65c320b`; `174` @ `7778070` |
| `Auth` / `Type` = `"Twitch"` | `frontend/oauth/Auth.cpp` | `84` @ `65c320b`; `83` @ `7778070` |

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

OAuth secret writes (do **not** read these):

| Key | File | Line @ `65c320b` |
| --- | --- | --- |
| `RefreshToken`, `Token` | `frontend/oauth/OAuth.cpp` | `52`-`53` |
| `ExpireTime`, `ScopeVer` | `frontend/oauth/OAuth.cpp` | `54`-`55` |

Persistence goes through `Auth::Save()` (`frontend/oauth/Auth.cpp` `72`-`86` @
`65c320b`), which writes into `OBSBasic::Config()` and `config_save_safe`.

### 2. `OBSBasic::Config()` == profile `basic.ini` == frontend API

| Fact | File | Line @ `65c320b` |
| --- | --- | --- |
| Profile file name `basic.ini` | `frontend/widgets/OBSBasic_Profiles.cpp` | `36` |
| `OBSBasic::Config()` returns `activeConfiguration` | `frontend/widgets/OBSBasic.cpp` | `1734`-`1737` |
| `obs_frontend_get_profile_config()` returns same | `frontend/OBSStudioAPI.cpp` | `426`-`428` (33.0.1); `442` @ 31.1.2 |

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

| Fact | File | Line @ `65c320b` |
| --- | --- | --- |
| File name `service.json` | `frontend/widgets/OBSBasic_Service.cpp` | `22` |
| Settings blob includes `key` | `frontend/widgets/OBSBasic_Service.cpp` | `35`-`38` |
| UI reads `service` and `key` | `frontend/settings/OBSBasicSettings_Stream.cpp` | `110`-`112` |
| `rtmp_common` copies `service` / `key` | `plugins/rtmp-services/rtmp-common.c` | `151`-`154` |
| Stream key property | `plugins/rtmp-services/rtmp-common.c` | `715` |
| OAuth injects `key` into service | `frontend/oauth/OAuth.cpp` | `212`-`222` |

`service.json` does **not** store the Twitch login. At most, plugins may read
the non-secret field `"service"` (e.g. `"Twitch"`) via:

```c
obs_service_t *svc = obs_frontend_get_streaming_service(); /* no addref */
obs_data_t *settings = obs_service_get_settings(svc);      /* addref @ libobs/obs-service.c:208 */
const char *service = obs_data_get_string(settings, "service");
obs_data_release(settings);
/* never obs_data_get_string(settings, "key") */
```

API declarations: `frontend/api/obs-frontend-api.h` (`obs_frontend_get_profile_config`
~`210`, `obs_frontend_get_streaming_service` ~`216` @ `65c320b`).

### 4. Connect Account UI flow

| Step | File | Notes |
| --- | --- | --- |
| Connect button | `OBSBasicSettings_Stream.cpp` `on_connectAccount_clicked` (~`868`) | OAuth login |
| Apply stream settings | `SaveStream1Settings` (~`267`) | Sets `main->auth`, saves `service.json` including `key` |
| Auth type + Twitch `Name` flush | `Auth::Save()` | On profile switch / process save paths (see caveats) |

## Method retained by this plugin

1. Primary: `obs_frontend_get_profile_config()` → require `Auth`/`Type` ==
   `"Twitch"` → read only `Twitch`/`Name` → normalize (trim, lower case, strip
   leading `@`).
2. Optional confirmation: `obs_frontend_get_streaming_service()` + settings
   field `"service"` only (must be `"Twitch"` when present); never `"key"`.
3. Refresh on `OBS_FRONTEND_EVENT_FINISHED_LOADING`,
   `OBS_FRONTEND_EVENT_PROFILE_CHANGED`,
   `OBS_FRONTEND_EVENT_STREAMING_STARTING` /
   `OBS_FRONTEND_EVENT_STREAMING_STARTED`
   (`frontend/api/obs-frontend-api.h` `17`, `32`, `46`), plus a light UI-thread
   `QTimer` (5-10 s).
4. Manual channel field remains for stream-key-only setups (no connected
   account / empty `Name`).
5. Explicit Stop is persisted in the plugin section so autodetection does not
   restart capture against the user's wish.

## Caveats / open points

1. **When `Name` is written:** `TwitchAuth::SaveInternal` runs from
   `Auth::Save()`, which is not invoked inside every Apply of Stream settings.
   Typical paths that call `Auth::Save()` include profile changes
   (`OBSBasic_Profiles.cpp` ~`682`) and shutdown `saveAll()`
   (`OBSBasic.cpp` ~`1747`). After a brand-new Connect Account in the same
   session, `Twitch`/`Name` may be absent from the in-memory profile config
   until one of those saves runs. After a normal OBS restart with an already
   connected account, `Auth::Load()` restores `Name` and autodetection works at
   `FINISHED_LOADING`.
2. **Stale `Name` after disconnect:** clearing auth removes `Auth`/`Type` on
   `Auth::Save()` when `auth` is null (`Auth.cpp` ~`76`-`78`), but does not
   always wipe `Twitch`/`Name`. Gating on `Auth`/`Type` == `"Twitch"` reduces
   false positives.
3. **CEF / Wayland:** `TwitchAuth::LoadInternal` returns false without CEF;
   `RegisterTwitchAuth` skips Linux Wayland. Connect Account may be unavailable
   there; users fall back to manual channel entry.
4. **No channel in `service.json`:** stream-key-only users never get a `Name`;
   manual entry is required.
5. **Multibyte / display names:** stored value is Twitch **login** (Helix), not
   display name.
