# Mini serveur de signaling WebRTC pour ESP32-P4. Image minuscule : Python
# standard uniquement, aucune dépendance à installer.
FROM python:3.12-alpine

WORKDIR /app
COPY server.py /app/server.py

# Le token DOIT être fourni au run (variable d'environnement SIGNALING_TOKEN).
ENV SIGNALING_PORT=8080 \
    SIGNALING_TTL=90

EXPOSE 8080

# Vérifie que le serveur répond (route /health, sans token).
HEALTHCHECK --interval=30s --timeout=3s --retries=3 \
  CMD wget -q -O- http://127.0.0.1:8080/health || exit 1

CMD ["python", "server.py"]
