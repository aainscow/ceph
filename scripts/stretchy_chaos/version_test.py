#!/usr/bin/env python3
"""Do partial overwrites make zone-1 localized split reads fall back to the
primary (spurious get_internal_versions mismatch -> -EAGAIN -> resubmit)?

For objects written full and then partially overwritten in one data chunk,
localized reads from the non-primary zone are compared with control objects
(full writes only).  Uses the client's own objecter perf counters (admin
socket) so other cluster IO does not matter:
  split_op_reads  : FORCE_OSD submits (sub-reads)
  op_send         : all op submissions (a resubmitted parent adds one)
"""

import json
import os
import random
import subprocess
import sys
import tempfile

import radosc

POOL = "ecs"
N = int(sys.argv[1]) if len(sys.argv) > 1 else 16
READS = 10
SIZE = 64 << 10
tmp = tempfile.mkdtemp()


def cj(cmd):
    r = subprocess.run(f"ceph {cmd} -f json", shell=True, capture_output=True, text=True)
    return json.loads(r.stdout)


def perf(sock):
    r = subprocess.run(["ceph", "--admin-daemon", sock, "perf", "dump", "objecter"],
                       capture_output=True, text=True)
    return json.loads(r.stdout)["objecter"]


tree = cj("osd crush tree")
nodes = {n["id"]: n for n in tree["nodes"]}


def leaves(i):
    return [i] if i >= 0 else sum((leaves(c) for c in nodes[i].get("children", [])), [])


zones = {n["name"]: leaves(n["id"]) for n in tree["nodes"]
         if n["type"] == "datacenter" and leaves(n["id"])}
w = radosc.Client(POOL)
results = {}
for kind in ("control", "partial"):
    per_zone = {}
    objs = []
    for i in range(N):
        oid = f"ver-{kind}-{random.randint(0, 1 << 30)}"
        data = bytearray(random.randbytes(SIZE))
        w.write_full(oid, bytes(data))
        if kind == "partial":
            patch = random.randbytes(100)
            w.write(oid, patch, 0)
            data[0:100] = patch
        m = cj(f"osd map {POOL} {oid}")
        zs = len(m["acting"]) // 2
        pz = m["acting"].index(m["acting_primary"]) // zs
        block = [o for o in m["acting"][(1 - pz) * zs:(2 - pz) * zs] if o in sum(zones.values(), [])]
        if not block:
            w.remove(oid)
            continue
        zone = next(z for z, o in zones.items() if block[0] in o)
        objs.append((oid, bytes(data), zone))
    for zone in zones:
        sock = f"{tmp}/ver-{kind}-{zone}.asok"
        cl = radosc.Client(POOL, crush_location=f"datacenter={zone}",
                           extra={"admin_socket": sock})
        before = perf(sock)
        n = 0
        for oid, data, z in objs:
            if z != zone:
                continue
            for _ in range(READS):
                got = cl.read(oid, 0, SIZE, radosc.LOCALIZE_READS)
                assert got == data, f"miscompare {oid}"
                n += 1
        after = perf(sock)
        cl.close()
        d = {k: after[k] - before[k] for k in ("op_send", "split_op_reads", "op_resend")
             if k in after}
        per_zone[zone] = {"reads": n, **d,
                          "op_send_per_read": round(d.get("op_send", 0) / n, 2) if n else None}
    results[kind] = per_zone
    for oid, _, _ in objs:
        w.remove(oid)
print(json.dumps(results, indent=1))
