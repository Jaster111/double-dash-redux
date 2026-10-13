#!/bin/bash
source "$(dirname "$0")/env.sh"
grep -ho 'id="PowerPC:BE:32:[^"]*"' $GHIDRA_INSTALL_DIR/Ghidra/Processors/PowerPC/data/languages/*.ldefs | sort -u
rea doctor --json > $WORK/doctor.json 2>&1
python3 - <<'EOF'
import json
d = json.load(open('/root/work/doctor.json'))
def walk(o, depth=0):
    if isinstance(o, dict):
        if 'id' in o and ('status' in o or 'ok' in o):
            print(o.get('id'), o.get('status', o.get('ok')), (o.get('summary') or o.get('message') or '')[:150])
        for v in o.values(): walk(v, depth+1)
    elif isinstance(o, list):
        for v in o: walk(v, depth+1)
walk(d)
EOF
