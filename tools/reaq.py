"""Client for tools/wsl/rea_daemon.py.

  python reaq.py tools                       list MCP tools (names + short descriptions)
  python reaq.py schema <tool>               show a tool's input schema
  python reaq.py call <tool> '<json args>'   call a tool, print JSON result
"""
import json
import sys
import urllib.request

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

URL = "http://127.0.0.1:8765"


def call(name, arguments=None, timeout=7200):
    req = urllib.request.Request(URL + "/call", data=json.dumps({"name": name, "arguments": arguments or {}}).encode(),
                                 headers={"Content-Type": "application/json"})
    msg = json.load(urllib.request.urlopen(req, timeout=timeout))
    if "error" in msg:
        raise RuntimeError(msg["error"])
    res = msg["result"]
    if res.get("structuredContent") is not None:
        return res["structuredContent"]
    texts = [c.get("text", "") for c in res.get("content", []) if c.get("type") == "text"]
    joined = "\n".join(texts)
    try:
        return json.loads(joined)
    except ValueError:
        return joined


def tools():
    return json.load(urllib.request.urlopen(URL + "/tools"))["result"]["tools"]


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "tools":
        for t in tools():
            print("%-40s %s" % (t["name"], (t.get("description") or "").split("\n")[0][:110]))
    elif cmd == "schema":
        for t in tools():
            if t["name"] == sys.argv[2]:
                print(json.dumps(t["inputSchema"], indent=1))
    elif cmd == "call":
        r = call(sys.argv[2], json.loads(sys.argv[3]) if len(sys.argv) > 3 else {})
        print(json.dumps(r, indent=1) if not isinstance(r, str) else r)
