#!/usr/bin/env python3
"""Emit a PSX_INPUT script: <seed> <start_frame> <end_frame> [buttons...] -> random presses"""
import random, sys
seed, a, b = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
btns = sys.argv[4:] or ['up', 'down', 'left', 'right', 'cross', 'circle', 'square', 'start', 'select']
random.seed(seed)
ev = []
f = a
while f < b and len(ev) < 250:
    ev.append(f'{f}:{random.choice(btns)}*{random.randint(3, 30)}')
    f += random.randint(4, 40)
print(','.join(ev))
