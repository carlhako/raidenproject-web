#!/bin/bash
# Regenerate docs/screenshots from scripted headless runs (needs the built ./raiden and python3 + Pillow).
set -e
cd "$(dirname "$0")/.."
OUT=docs/screenshots
mkdir -p $OUT shots
tmp=$(mktemp -d)

run() {  # run <input-script> <frames> <frame list>
    rm -f shots/frame_*.png
    PSX_HEADLESS=1 PSX_INPUT="$1" PSX_FRAMES=$2 PSX_SHOT_FRAMES="$3" ./raiden -q > /dev/null 2>&1
}
pick() {  # pick <frame> <name>
    python3 -I - "$1" "$OUT/$2.png" <<'PY'
import sys
from PIL import Image
im = Image.open('shots/frame_%05d.png' % int(sys.argv[1])).convert('RGB')
im.resize((im.width * 2, im.height * 2), Image.NEAREST).save(sys.argv[2])
PY
}
mash() {  # mash <seed> <from> <to>: random direction presses
    python3 -I - "$@" <<'PY'
import random, sys
seed, a, b = map(int, sys.argv[1:4]); random.seed(seed)
f = a; ev = []
while f < b and len(ev) < 160:
    ev.append('%d:%s*%d' % (f, random.choice(['left', 'right', 'up', 'down']), random.randint(15, 40))); f += random.randint(20, 45)
print(','.join(ev))
PY
}
starts() { python3 -I -c "print(','.join('%d:start*8' % f for f in range($1, $2, 50)))"; }

# launcher menu
run "" 700 "600"
pick 600 launcher

# settings menu
run "700:down*6,780:down*6,860:circle*8,1100:circle*8" 1300 "1000,1200"
pick 1000 settings-menu
pick 1200 screen-options

# Raiden (arcade original): title, carrier deck, stage 1, continue screen
run "700:circle*10,$(starts 1000 1500),1700:cross*5000,$(mash 8 1800 5000)" 5000 "1000,1400,2800,3800,5000"
pick 1000 raiden-title
pick 1400 raiden-takeoff
pick 2800 raiden-gameplay
pick 3800 raiden-gameplay2
pick 5000 raiden-continue

# Raiden II
run "700:down*8,760:circle*10,$(starts 1100 2000),2200:cross*5000,$(mash 9 2300 6500)" 6500 "1000,2000,2750,3500,3750"
pick 1000 raiden2-loading
pick 2000 raiden2-takeoff
pick 2750 raiden2-gameplay
pick 3500 raiden2-gameplay2
pick 3750 raiden2-gameplay3
rm -rf "$tmp"
