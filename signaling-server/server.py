#!/usr/bin/env python3
"""
Mini serveur de signaling WebRTC pour ESP32-P4 <-> ESP32-P4.

Remplace le serveur AppRTC public (webrtc.espressif.com) par une brique que tu
héberges toi-même (Unraid, VPS...). Protocole HTTP simple, un seul port, SANS
TLS ni WebSocket : les deux P4 déposent/récupèrent leurs messages (offre,
réponse, candidats ICE) par polling. Robuste et sans dépendance externe
(bibliothèque standard Python uniquement).

PROTECTION : chaque requête doit porter l'en-tête  X-Auth-Token: <token>
(valeur de la variable d'environnement SIGNALING_TOKEN). Sans token valide -> 401.
Indispensable dès que le serveur est redirigé sur Internet.

Modèle : chaque room a un JOURNAL de messages (append-only) et chaque pair a un
CURSEUR. Un pair reçoit tous les messages des AUTRES pairs postés depuis son
dernier curseur — donc un pair qui rejoint en retard récupère quand même l'offre
déjà envoyée (pas de course à l'ordre d'arrivée).

Routes (protégées par le token, sauf /health) :
  POST /join/<room>            body {"client":"<id>"}
       -> {"client_id","initiator":bool,"messages":[...]}
       Le PREMIER arrivé dans la room est "initiator" (le firmware peut de toute
       façon figer le rôle via role: caller/callee).
  POST /msg/<room>/<client>    body = message JSON brut (offre/réponse/candidat)
       -> {"ok":true,"delivered":N}   (ajouté au journal de la room)
  GET  /msg/<room>/<client>
       -> {"messages":[...]}    (messages des autres depuis le curseur du pair)
  POST /leave/<room>/<client>  -> {"ok":true}
  GET  /health                 -> {"ok":true}   (sans token, pour le healthcheck)

Variables d'environnement :
  SIGNALING_TOKEN   (REQUIS) le secret partagé. Mets la MÊME valeur dans le YAML
                    des P4 (signaling_token:).
  SIGNALING_PORT    port d'écoute (défaut 8080)
  SIGNALING_TTL     secondes avant d'oublier un pair inactif (défaut 90)
"""
import json
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

TOKEN = os.environ.get("SIGNALING_TOKEN", "")
PORT = int(os.environ.get("SIGNALING_PORT", "8080"))
TTL = int(os.environ.get("SIGNALING_TTL", "90"))

# room_id -> {"log": [(from_client, msg_str), ...],
#             "peers": {client_id: {"cursor": int, "last_seen": float}}}
_rooms = {}
_lock = threading.Lock()


def _now():
    return time.time()


def _room_locked(room):
    return _rooms.setdefault(room, {"log": [], "peers": {}})


def _purge_locked(room):
    """Retire les pairs inactifs (crash/coupure) et vide la room si vide."""
    r = _rooms.get(room)
    if not r:
        return
    dead = [c for c, p in r["peers"].items() if _now() - p["last_seen"] > TTL]
    for c in dead:
        del r["peers"][c]
    if not r["peers"]:
        _rooms.pop(room, None)  # plus personne -> on oublie le journal


class Server(ThreadingHTTPServer):
    daemon_threads = True       # les threads de requête ne bloquent pas l'arrêt
    request_queue_size = 128    # backlog TCP large (rafales de connexions du P4)
    allow_reuse_address = True


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"  # Content-Length fourni -> connexions propres

    # CRUCIAL : timeout de lecture par connexion. Sans lui, une connexion keep-alive
    # dont le client a disparu (reset/crash/coupure Wi-Fi) laisse le thread de
    # traitement BLOQUÉ pour toujours sur la lecture de la requête suivante. Au fil
    # des cycles connexion/reset, ces threads zombies s'accumulent jusqu'à ce que le
    # ThreadingHTTPServer ne puisse plus accepter de nouvelles connexions -> les P4
    # se prennent des "connection reset" et il faut REDÉMARRER le conteneur. Les P4
    # sondent toutes les ~300 ms, donc une connexion vivante n'atteint jamais ce
    # délai ; seules les connexions mortes sont récupérées (au bout de timeout s).
    timeout = 20

    def log_message(self, fmt, *args):
        pass  # on gère nos propres logs

    def handle_one_request(self):
        # Une coupure client (reset/timeout) ne doit pas remonter en trace ni tuer
        # salement le thread : on ferme la connexion proprement (le thread se termine).
        try:
            super().handle_one_request()
        except (ConnectionError, TimeoutError, OSError):
            self.close_connection = True

    def _send(self, code, obj=None):
        body = b"" if obj is None else json.dumps(obj).encode("utf-8")
        try:
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if body:
                self.wfile.write(body)
        except (ConnectionError, TimeoutError, OSError):
            # Client parti pendant l'écriture : on abandonne cette réponse, la
            # connexion sera fermée. Ne surtout pas laisser l'exception bloquer/tuer
            # le thread de façon à figer le serveur.
            self.close_connection = True

    def _auth_ok(self):
        if not TOKEN:
            return False  # jamais ouvert sans token configuré
        return self.headers.get("X-Auth-Token", "") == TOKEN

    def _read_body(self):
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            n = 0
        return self.rfile.read(n) if n > 0 else b""

    def _parts(self):
        return [p for p in urlparse(self.path).path.split("/") if p != ""]

    def do_POST(self):
        if not self._auth_ok():
            self._send(401, {"error": "unauthorized"})
            return
        parts = self._parts()
        body = self._read_body()

        if len(parts) == 2 and parts[0] == "join":
            room = parts[1]
            try:
                client = json.loads(body).get("client", "") if body else ""
            except (ValueError, AttributeError):
                client = ""
            if not client:
                self._send(400, {"error": "missing client"})
                return
            with _lock:
                _purge_locked(room)
                r = _room_locked(room)
                peers = r["peers"]
                initiator = len(peers) == 0
                prev = peers[client]["cursor"] if client in peers else 0
                msgs = [m for (frm, m) in r["log"][prev:] if frm != client]
                peers[client] = {"cursor": len(r["log"]), "last_seen": _now()}
                npeers = len(peers)
            print("JOIN room=%s client=%s initiator=%s peers=%d backlog=%d"
                  % (room, client, initiator, npeers, len(msgs)), flush=True)
            self._send(200, {"client_id": client, "initiator": initiator,
                             "messages": msgs})
            return

        if len(parts) == 3 and parts[0] == "msg":
            room, sender = parts[1], parts[2]
            msg = body.decode("utf-8", "ignore")
            with _lock:
                _purge_locked(room)
                r = _room_locked(room)
                if sender in r["peers"]:
                    r["peers"][sender]["last_seen"] = _now()
                r["log"].append((sender, msg))
                delivered = sum(1 for c in r["peers"] if c != sender)
            print("MSG  room=%s from=%s -> %d peer(s) (%d bytes)"
                  % (room, sender, delivered, len(msg)), flush=True)
            self._send(200, {"ok": True, "delivered": delivered})
            return

        if len(parts) == 3 and parts[0] == "leave":
            room, client = parts[1], parts[2]
            with _lock:
                r = _rooms.get(room)
                if r:
                    r["peers"].pop(client, None)
                    if not r["peers"]:
                        _rooms.pop(room, None)
            print("LEAVE room=%s client=%s" % (room, client), flush=True)
            self._send(200, {"ok": True})
            return

        self._send(404, {"error": "not found"})

    def do_GET(self):
        parts = self._parts()
        if len(parts) == 1 and parts[0] == "health":
            self._send(200, {"ok": True})
            return
        if not self._auth_ok():
            self._send(401, {"error": "unauthorized"})
            return
        if len(parts) == 3 and parts[0] == "msg":
            room, client = parts[1], parts[2]
            with _lock:
                _purge_locked(room)
                r = _room_locked(room)
                p = r["peers"].get(client)
                if p is None:
                    p = r["peers"][client] = {"cursor": 0, "last_seen": _now()}
                prev = p["cursor"]
                msgs = [m for (frm, m) in r["log"][prev:] if frm != client]
                p["cursor"] = len(r["log"])
                p["last_seen"] = _now()
            self._send(200, {"messages": msgs})
            return
        self._send(404, {"error": "not found"})


def main():
    if not TOKEN:
        print("ERREUR : SIGNALING_TOKEN non défini. Refuse de démarrer sans "
              "protection. Définis la variable d'environnement SIGNALING_TOKEN.",
              flush=True)
        raise SystemExit(1)
    srv = Server(("0.0.0.0", PORT), Handler)
    print("Signaling ESP32-P4 démarré sur 0.0.0.0:%d (TTL pair=%ds)"
          % (PORT, TTL), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
