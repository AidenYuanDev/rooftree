#!/usr/bin/env bash
# certbot --deploy-hook: after a certificate was issued or renewed, copies it where the blog user can
# read it (certbot's private keys are root's only) and has the server load it (SIGHUP, no restart).
# Open connections keep the old certificate, new ones get the new one.
#
#   certbot certonly --webroot -w /srv/blog/acme -d blog.example.com \
#     --deploy-hook /opt/blog/src/apps/blog/deploy/certbot-deploy-hook.sh
#
# Environment: RENEWED_LINEAGE (set by certbot, /etc/letsencrypt/live/<domain>), BLOG_TLS_DIR
# (/etc/blog/tls, Server.Https.CertificateFile and PrivateKeyFile point there)

set -euo pipefail

BLOG_TLS_DIR=${BLOG_TLS_DIR:-/etc/blog/tls}

install -d -m 750 -o root -g blog "$BLOG_TLS_DIR"
# Both in place before the server is told: it reads them only on the reload below
install -m 640 -o root -g blog "$RENEWED_LINEAGE/fullchain.pem" "$BLOG_TLS_DIR/fullchain.pem"
install -m 640 -o root -g blog "$RENEWED_LINEAGE/privkey.pem" "$BLOG_TLS_DIR/privkey.pem"

# Not running yet (the first certificate): it reads them when it starts
if systemctl is-active --quiet blog; then
  systemctl reload blog
elif [[ -f /srv/blog/compose.yaml ]]; then
  # The Docker deployment (README.md, "用 Docker 上线")
  docker compose -f /srv/blog/compose.yaml kill -s HUP blog
fi
echo "certbot-deploy-hook.sh: $BLOG_TLS_DIR updated"
