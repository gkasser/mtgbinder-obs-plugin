# MTG Trade OBS Plugin

Plugin OBS natif (C++) qui envoie les frames de votre stream à [MTG Trade](https://mtg-trade.fr/twitch)
pour l'identification des cartes Magic, avec un timestamp proche du moment broadcast.
Il réduit fortement la latence par rapport au mode passif de l'extension Twitch.

Le plugin est **open source et inspectable** : il ne contient aucun secret. Le token
d'accès est obtenu à l'exécution via un code de liaison généré dans la configuration
de l'extension Twitch, puis stocké localement dans le profil OBS.

## Installation

Téléchargez l'archive correspondant à votre OS depuis la
[dernière release](https://github.com/gkasser/mtgbinder-obs-plugin/releases/latest)
(Windows, macOS, Linux), puis installez-la comme un plugin OBS standard.

## Utilisation

1. Dans la configuration de l'extension Twitch MTG Trade, générer un code OBS.
2. Dans OBS, ouvrir `Tools > MTG Binder OBS`.
3. Coller le code Twitch et cliquer sur `Connect plugin`.

Le plugin récupère automatiquement la chaîne Twitch vérifiée, le plan, l'intervalle
de capture et l'endpoint sécurisé. À la première connexion, si la chaîne n'existe
pas encore côté MTG Trade, elle est créée en offre gratuite.

## Contrat API

Le plugin ne dépend que de deux endpoints HTTP de `mtg-trade.fr`, documentés dans
[`docs/mtg-trade-api.md`](docs/mtg-trade-api.md). C'est la seule surface partagée
avec le backend.

## Build

Ce projet suit la structure du
[template officiel OBS](https://github.com/obsproject/obs-plugintemplate) :
CMake + `buildspec.json` (versions OBS/Qt/obs-deps) + presets.

Les dépendances OBS et Qt sont téléchargées automatiquement via `buildspec.json`.
L'encodage JPEG utilise [`stb_image_write`](https://github.com/nothings/stb)
(vendoré, domaine public), donc **aucune dépendance système libjpeg**. `libcurl`
provient d'obs-deps (Windows/macOS) ou du système (Linux).

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

Les workflows GitHub Actions buildent Linux, Windows et macOS à chaque push/PR.
Pousser un tag (`vX.Y.Z`) crée une release multi-OS téléchargeable, référencée par
la page `/twitch` du site.

## Sécurité

Aucun secret serveur ni credential. La chaîne Twitch est vérifiée par le JWT
broadcaster de l'extension ; le token local est généré par `mtg-trade.fr`, hashé
côté serveur et révocable depuis le site.
