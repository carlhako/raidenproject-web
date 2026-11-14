#!/usr/bin/env python3
"""Minimal MCP stdio client for REA: keeps one Ghidra session for many calls.
usage: rea_mcp.py list | rea_mcp.py batch calls.json out.json
calls.json: [{"tool": name, "args": {...}}, ...]"""
import json, subprocess, sys, os
env = dict(os.environ)
rea = os.path.expanduser('~/tools/rea/node_modules/.bin/rea')
p = subprocess.Popen([rea, '--mcp'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env, text=True, bufsize=1)
mid = 0
def rpc(method, params=None, notify=False):
    global mid
    msg = {'jsonrpc': '2.0', 'method': method}
    if params is not None: msg['params'] = params
    if not notify:
        mid += 1; msg['id'] = mid
    p.stdin.write(json.dumps(msg) + '\n'); p.stdin.flush()
    if notify: return None
    while True:
        line = p.stdout.readline()
        if not line: raise SystemExit('server closed')
        r = json.loads(line)
        if r.get('id') == mid: return r
rpc('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {}, 'clientInfo': {'name': 'puchi-recomp', 'version': '1'}})
rpc('notifications/initialized', notify=True)
if sys.argv[1] == 'list':
    r = rpc('tools/list')
    for t in r['result']['tools']:
        print(t['name'], '-', t.get('description', '')[:110].replace('\n', ' '))
    if len(sys.argv) > 2:
        for t in r['result']['tools']:
            if t['name'] == sys.argv[2]: print(json.dumps(t, indent=1))
else:
    calls = json.load(open(sys.argv[2]))
    res = []
    for c in calls:
        r = rpc('tools/call', {'name': c['tool'], 'arguments': c['args']})
        res.append({'call': c, 'result': r.get('result', r.get('error'))})
        print('done', c['tool'], json.dumps(c['args'])[:120], file=sys.stderr)
    json.dump(res, open(sys.argv[3], 'w'), indent=1)
p.stdin.close(); p.terminate()
