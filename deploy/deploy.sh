#!/bin/sh
# usage: deploy/deploy.sh root@lxc-host   (builds locally, copies to the LXC, restarts the service)
set -eu
TARGET=${1:?usage: deploy.sh user@host}
cd "$(dirname "$0")/.."
[ -f dist/data/disc.img ] || python3 -I tools/pack_disc.py
make web
rsync -az --delete --info=progress2 dist/ "$TARGET:/opt/raidenproject-web/dist/"
rsync -az server package.json package-lock.json "$TARGET:/opt/raidenproject-web/"
scp -q deploy/raidenproject-web.service "$TARGET:/etc/systemd/system/raidenproject-web.service"
ssh "$TARGET" 'cd /opt/raidenproject-web && npm ci --omit=dev --silent && chown -R raidenproject:raidenproject . && systemctl daemon-reload && systemctl enable raidenproject-web && systemctl restart raidenproject-web && sleep 1 && . /etc/raidenproject-web.env 2>/dev/null; curl -fsS http://127.0.0.1:${PORT:-8080}/healthz && echo " deployed"'
