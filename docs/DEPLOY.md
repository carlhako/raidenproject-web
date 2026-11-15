# Deploying to a Proxmox LXC

The game needs a **secure context with cross-origin isolation**, so players must reach it over
HTTPS (or `http://localhost`). The Node server speaks plain HTTP and expects your reverse proxy
to terminate TLS.

## LXC

Any small Debian 12/13 or Ubuntu LXC works: 1 vCPU, 512 MB RAM, ~1 GB disk (the packed disc is
56 MB). The game runs in each player's browser; the server only serves files and relays a few
bytes per frame between the two players.

1. As root in the LXC: `sh deploy/setup-lxc.sh` (installs Node 22, user `raidenproject`, env file).
2. Make sure root can SSH in with your key, then from your build machine:
   `deploy/deploy.sh root@<lxc-ip>`
3. Optional but recommended: put a shared password in `/etc/raidenproject-web.env`
   (`ACCESS_KEY=...`) and `systemctl restart raidenproject-web`. Browsers then ask for HTTP Basic
   auth (any user name). The server hosts game-derived code and your disc data.

Environment: `PORT` (8080), `HOST` (0.0.0.0), `ACCESS_KEY`, `DIST_DIR`.

## Reverse proxy requirements

- Forward `/` and the WebSocket endpoint `/ws` to `http://<lxc-ip>:8080` (WebSocket upgrade on).
- Do not strip or override `Cross-Origin-Opener-Policy`, `Cross-Origin-Embedder-Policy`,
  `Cross-Origin-Resource-Policy` (the app sets them). Without them `crossOriginIsolated` is false
  and the page says so.
- Do not buffer or re-compress `data/disc.img` (56 MB), and allow long responses.

Caddy:

```
game.example.com {
    reverse_proxy <lxc-ip>:8080
}
```

Nginx / Nginx Proxy Manager (advanced config, with "Websockets support" on):

```
proxy_buffering off;
proxy_request_buffering off;
proxy_read_timeout 1h;
proxy_http_version 1.1;
```

Traefik: a plain router and service to `http://<lxc-ip>:8080` works; WebSockets are automatic.

## Checking it works

Open the HTTPS URL and run `crossOriginIsolated` in the console (must be `true`). `GET /healthz`
returns `ok`. The first visit downloads the disc into the browser's private storage.

## Updating

Run `deploy/deploy.sh` again. Browsers re-download the disc only if the hash in `data/disc.json` changes.
