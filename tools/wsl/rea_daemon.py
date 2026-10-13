"""Persistent REA MCP session exposed over local HTTP.

REA's CLI re-imports the target on every invocation (minutes for main.dol).
This daemon starts one `rea mcp` server, keeps its binary session open and
forwards tool calls:

  POST /call   {"name": "<tool>", "arguments": {...}}  -> MCP tools/call result
  GET  /tools                                          -> MCP tools/list result

Usage (inside WSL): python3 rea_daemon.py [port]
"""
import json
import os
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8765


class McpClient:
    def __init__(self, cmd, env):
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=open("/root/work/rea_mcp.stderr", "ab"), env=env)
        self.lock = threading.Lock()
        self.next_id = 1
        self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                    "clientInfo": {"name": "doubledashport", "version": "1"}})
        self.notify("notifications/initialized", {})

    def _send(self, msg):
        self.proc.stdin.write((json.dumps(msg) + "\n").encode())
        self.proc.stdin.flush()

    def notify(self, method, params):
        self._send({"jsonrpc": "2.0", "method": method, "params": params})

    def request(self, method, params):
        with self.lock:
            rid = self.next_id
            self.next_id += 1
            self._send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
            while True:
                line = self.proc.stdout.readline()
                if not line:
                    raise RuntimeError("rea mcp exited")
                msg = json.loads(line)
                if msg.get("id") == rid and ("result" in msg or "error" in msg):
                    return msg
                # server->client requests (e.g. ping) get an empty reply
                if "method" in msg and "id" in msg:
                    self._send({"jsonrpc": "2.0", "id": msg["id"], "result": {}})


client = None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _reply(self, obj):
        data = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/tools":
            self._reply(client.request("tools/list", {}))
        else:
            self._reply({"ok": True})

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        try:
            self._reply(client.request("tools/call", {"name": body["name"], "arguments": body.get("arguments", {})}))
        except Exception as e:  # keep the daemon alive; report to caller
            self._reply({"error": {"message": str(e)}})


def main():
    global client
    env = dict(os.environ)
    client = McpClient(["rea", "mcp"], env)
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print("rea daemon listening on", PORT, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
