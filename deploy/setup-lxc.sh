#!/bin/sh
# Run once as root inside the LXC (Debian/Ubuntu): installs Node 22, a service user and the data dir.
set -eu
apt-get update -qq
apt-get install -y -qq curl ca-certificates rsync
if ! command -v node >/dev/null || [ "$(node -p 'process.versions.node.split(".")[0]')" -lt 20 ]; then
  curl -fsSL https://deb.nodesource.com/setup_22.x | bash -
  apt-get install -y -qq nodejs
fi
id raidenproject >/dev/null 2>&1 || useradd --system --home /opt/raidenproject-web --shell /usr/sbin/nologin raidenproject
mkdir -p /opt/raidenproject-web
chown -R raidenproject:raidenproject /opt/raidenproject-web
if [ ! -f /etc/raidenproject-web.env ]; then
  printf '# PORT=8080\n# HOST=0.0.0.0\n# ACCESS_KEY=choose-a-shared-password\n' > /etc/raidenproject-web.env
  chmod 600 /etc/raidenproject-web.env
fi
echo "Setup done. Now run deploy/deploy.sh from your build machine."
