# Contrat API MTG Trade (OBS)

Ce document décrit **l'intégralité** de la surface API que ce plugin consomme sur
`https://mtg-trade.fr`. C'est le seul contrat partagé avec le backend ; rien d'autre
du système MTG Trade n'est nécessaire pour construire ou comprendre ce plugin.

Aucun secret n'est embarqué : le plugin n'obtient un token qu'à l'exécution via le
code de liaison généré dans la configuration de l'extension Twitch.

---

## 1. Connexion du plugin

`POST /api/obs/connect`

Échange un code de liaison (généré dans la configuration de l'extension Twitch
MTG Trade) contre un token d'accès et la configuration de capture.

### Requête

```
Content-Type: application/json
```

```json
{
  "link_code": "MTGB-XXXX-XXXX",
  "device_name": "OBS Studio"
}
```

### Réponse `200`

```json
{
  "access_token": "<bearer-token-opaque>",
  "channel": "<twitch-channel>",
  "plan": "free | pro | premium",
  "tcg_type": "mtg | pokemon | ...",
  "frame_interval_secs": 25,
  "endpoint": "https://mtg-trade.fr/api/obs/frames"
}
```

| Champ | Type | Notes |
|---|---|---|
| `access_token` | string | Bearer token, stocké dans le profil OBS. Obligatoire. |
| `channel` | string | Chaîne Twitch vérifiée. Obligatoire. |
| `plan` | string | Niveau d'abonnement. |
| `tcg_type` | string | Jeu de cartes ciblé. |
| `frame_interval_secs` | int | Intervalle de capture conseillé (défaut 25). |
| `endpoint` | string | URL d'upload des frames (sinon `<baseUrl>/api/obs/frames`). |

### Erreurs

Statut HTTP non-2xx avec corps `{"error": "<message>"}`.

---

## 2. Upload d'une frame

`POST /api/obs/frames` (ou l'`endpoint` renvoyé par `/api/obs/connect`)

Envoie une frame JPEG capturée depuis OBS.

### Requête

```
Authorization: Bearer <access_token>
Content-Type: image/jpeg
x-obs-frame-index: <uint64 monotone>
x-obs-pts-ms: <int64 epoch ms>
x-obs-source-width: <uint32>
x-obs-source-height: <uint32>
```

Corps : binaire JPEG brut (résolution de capture, 1280×720 par défaut).

### Réponse

Tout statut `2xx` vaut succès. Sinon le corps peut contenir `{"error": "..."}`.
