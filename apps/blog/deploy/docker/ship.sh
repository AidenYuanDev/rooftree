#!/usr/bin/env bash
# Builds the image here (the C++ tests run during the build), loads it on the host, restarts the blog
# there and rebuilds the prerendered site with the new pages:
#
#   apps/blog/deploy/docker/ship.sh user@host
#
# The host needs docker with compose, the user in the docker group, and /srv/blog, /etc/blog set up
# (README.md, "用 Docker 上线").

set -euo pipefail

host=${1:?usage: ship.sh user@host}
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"

# The files git knows of, committed or not; never the ignored ones (build directories, node_modules)
git ls-files -z -co --exclude-standard | tar --null -T - -c \
  | docker build -t blog:latest -f apps/blog/deploy/docker/Dockerfile -

docker save blog:latest | gzip -1 | ssh "$host" docker load
scp apps/blog/deploy/docker/compose.yaml "$host:/srv/blog/compose.yaml"

# --wait: until PostgreSQL is healthy and the blog runs (its migrations done)
ssh "$host" docker compose -f /srv/blog/compose.yaml up -d --wait
ssh "$host" docker image prune -f
ssh "$host" docker compose -f /srv/blog/compose.yaml exec -T blog /opt/blog/src/apps/blog/deploy/rebuild.sh
ssh "$host" docker compose -f /srv/blog/compose.yaml kill -s HUP blog
