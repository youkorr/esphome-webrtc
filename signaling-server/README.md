# Serveur de signaling ESP32-P4 (auto-hébergé)

Remplace `webrtc.espressif.com` par ta propre brique de signaling, protégée par
un token. HTTP simple, un seul port, **sans TLS ni WebSocket** → plus de
`ESP_ERR_HTTP_CONNECT` ni de `8089 timeout`, et prêt pour le maison-à-maison.

Le firmware webrtc parle déjà ce protocole quand tu renseignes `signaling_url`
et `signaling_token` dans le YAML.

## 1. Construire l'image sur Unraid

Dans le terminal Unraid :

```bash
mkdir -p /mnt/cache/appdata/signaling
# copie server.py et Dockerfile dans ce dossier (SMB, ou git clone),
# puis construis l'image :
cd /mnt/cache/appdata/signaling
docker build -t esp-signaling .
```

## 2. Lancer le conteneur

Choisis un token long et privé (le MÊME que dans le YAML des P4) :

```bash
docker run -d --name esp-signaling --restart unless-stopped \
  --network br0 --ip 192.168.1.13 \
  -e SIGNALING_TOKEN="change-moi-secret-partage" \
  esp-signaling
```

> `--network br0 --ip 192.168.1.13` : donne une IP LAN dédiée (comme coturn).
> Adapte l'IP à ton réseau. En mode "bridge" classique, ajoute `-p 8080:8080`
> à la place.

Vérifie :

```bash
docker logs esp-signaling          # -> "Signaling ESP32-P4 démarré sur 0.0.0.0:8080"
curl http://192.168.1.13:8080/health   # -> {"ok": true}
```

## 3. Configurer les deux P4

Dans le bloc `webrtc:` des DEUX P4 (mêmes valeurs) :

```yaml
webrtc:
  room_id: "maison"                       # le rendez-vous (identique aux deux)
  signaling_url: "http://192.168.1.13:8080"
  signaling_token: "change-moi-secret-partage"
  # role: caller  (un P4)   /   role: callee  (l'autre)
```

Reflashe, lance l'appel. Dans les logs P4 tu verras :
`simple signaling joined: room=maison ... initiator=...`
Et côté serveur : `JOIN`, `MSG ... -> 1 peer(s)`.

## 4. Maison-à-maison (plus tard)

Comme pour coturn : sur la Livebox, redirige le port `8080` vers l'IP du
serveur, mets `external-ip` sur coturn, et dans le YAML du P4 distant mets
`signaling_url: "http://<ton-ip-publique>:8080"`. Le token protège l'accès.

> ⚠️ En HTTP le token circule en clair. Sur Internet, passe idéalement derrière
> un reverse-proxy HTTPS (ou un VPN type Tailscale/WireGuard) — le firmware
> accepte aussi une `signaling_url` en `https://` si tu as un vrai certificat.

## Variables d'environnement

| Variable | Rôle | Défaut |
|----------|------|--------|
| `SIGNALING_TOKEN` | secret partagé (REQUIS) | — (refuse de démarrer sans) |
| `SIGNALING_PORT` | port d'écoute | 8080 |
| `SIGNALING_TTL` | secondes avant d'oublier un pair inactif | 90 |
