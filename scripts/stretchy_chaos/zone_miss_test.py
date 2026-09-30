#!/usr/bin/env python3
"""Cross-check the client objecter 'localize_zone_miss' counter.

Localized multi-chunk reads from a client in zone Z: with the F1 fix every
data sub-read stays in Z and only the primary's version check may leave Z.
So per read the expected number of out-of-zone sub-ops is 1 when the PG
primary is outside Z, else 0.  Compare that with the counter delta.
"""

import json
import random
import subprocess
import sys
import tempfile

import radosc

POOL = "ecs"
tmp = tempfile.mkdtemp()
zone = sys.argv[1] if len(sys.argv) > 1 else "dc1"
N = 24


def cj(cmd):
    r = subprocess.run(f"ceph {cmd} -f json", shell=True, capture_output=True, text=True)
    return json.loads(r.stdout)


def perf(sock):
    r = subprocess.run(["ceph", "--admin-daemon", sock, "perf", "dump", "objecter"],
                       capture_output=True, text=True)
    return json.loads(r.stdout)["objecter"]


zone_osds = {n["name"]: n for n in cj("osd crush tree")["nodes"]}
w = radosc.Client(POOL)
objs = []
for i in range(N):
    oid = f"zm-{random.randint(0, 1 << 30)}"
    w.write_full(oid, random.randbytes(64 << 10))
    m = cj(f"osd map {POOL} {oid}")
    pz = cj(f"osd find {m['acting_primary']}")["crush_location"]["datacenter"]
    objs.append((oid, pz))
sock = f"{tmp}/zm-{zone}.asok"
cl = radosc.Client(POOL, crush_location=f"datacenter={zone}", extra={"admin_socket": sock})
before = perf(sock)
expected = 0
for oid, pz in objs:
    for _ in range(5):
        cl.read(oid, 0, 64 << 10, radosc.LOCALIZE_READS)
        expected += (pz != zone)
after = perf(sock)
d = {k: after[k] - before[k] for k in ("op_r", "split_op_reads", "localize_zone_miss")}
print(f"client zone {zone}: reads={N * 5} primaries_outside={sum(pz != zone for _, pz in objs)}/{N} "
      f"expected_remote_subops~{expected} counters={d}")
for oid, _ in objs:
    w.remove(oid)
