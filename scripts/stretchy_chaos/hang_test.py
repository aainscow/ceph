#!/usr/bin/env python3
"""Does an EC split read complete when a sub-read's target OSD dies or is
marked down while the sub-read is outstanding?

Writes a multi-chunk object, picks a non-primary data shard OSD in the zone
the client reads from, freezes it (SIGSTOP) so a localized/balanced split read
blocks on it, then either marks it down (`ceph osd down`), kills it
(SIGKILL), or just waits for heartbeat failure.  The read must complete
(with correct data) shortly after the OSD map changes, via the primary.
"""

import argparse
import json
import os
import random
import signal
import subprocess
import sys
import threading
import time

import radosc


def ceph_json(cmd):
    r = subprocess.run(f"ceph {cmd} -f json", shell=True, capture_output=True, text=True)
    return json.loads(r.stdout)


def osd_pid(o):
    return int(open(f"{os.environ['CEPH_BUILD']}/out/osd.{o}.pid").read())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pool", default="ecs")
    ap.add_argument("--policy", default="localize", choices=["localize", "balance"])
    ap.add_argument("--mode", default="markdown", choices=["markdown", "kill", "heartbeat"])
    ap.add_argument("--rel-shard", type=int, default=1)
    ap.add_argument("--limit", type=int, default=120)
    ap.add_argument("--size", type=int, default=64 << 10)
    ap.add_argument("--log", default=None, help="client log file (debug_objecter=20)")
    args = ap.parse_args()

    tree = ceph_json("osd crush tree")
    nodes = {n["id"]: n for n in tree["nodes"]}

    def leaves(i):
        return [i] if i >= 0 else sum((leaves(c) for c in nodes[i].get("children", [])), [])
    zones = {n["name"]: leaves(n["id"]) for n in tree["nodes"]
             if n["type"] == "datacenter" and leaves(n["id"])}
    pool = next(p for p in ceph_json("osd pool ls detail") if p["pool_name"] == args.pool)
    zs = pool["size"] // pool["options"].get("num_zones", 1)

    oid = f"hang-{random.randint(0, 1 << 30)}"
    data = random.randbytes(args.size)
    w = radosc.Client(args.pool)
    w.write_full(oid, data)
    m = ceph_json(f"osd map {args.pool} {oid}")
    acting, primary = m["acting"], m["acting_primary"]
    pz = acting.index(primary) // zs
    shard = args.rel_shard + (1 - pz) * zs
    victim = acting[shard]
    zone = next(z for z, o in zones.items() if victim in o)
    print(f"oid={oid} pg={m['pgid']} acting={acting} primary=osd.{primary}; "
          f"freezing osd.{victim} (shard {shard}, {zone}); client {args.policy} {zone}; mode={args.mode}",
          flush=True)
    extra = {"rados_osd_op_timeout": 0}
    if args.log:
        extra.update({"log_file": args.log, "debug_objecter": "20", "debug_ms": "1"})
    cl = radosc.Client(args.pool, crush_location=f"datacenter={zone}", extra=extra)
    flags = radosc.POLICY_FLAGS[args.policy]
    for _ in range(3):
        assert cl.read(oid, 0, args.size, flags) == data
    res = {}

    def reader():
        t0 = time.time()
        try:
            got = cl.read(oid, 0, args.size, flags)
            res["ok"] = got == data
        except radosc.RadosError as e:
            res["err"] = str(e)
        res["t"] = time.time() - t0

    pid = osd_pid(victim)
    os.kill(pid, signal.SIGSTOP)
    th = threading.Thread(target=reader, daemon=True)
    t_start = time.time()
    th.start()
    time.sleep(3)
    if args.mode == "markdown":
        subprocess.run(f"ceph osd down {victim}", shell=True, capture_output=True)
    elif args.mode == "kill":
        os.kill(pid, signal.SIGKILL)
    t_event = time.time()
    th.join(args.limit)
    hung = th.is_alive()
    st = ceph_json("osd dump")
    up = next(o["up"] for o in st["osds"] if o["osd"] == victim)
    print(f"  after {time.time() - t_start:.0f}s: read {'HUNG' if hung else res} "
          f"(osd.{victim} up={up})", flush=True)
    if args.mode != "kill":
        os.kill(pid, signal.SIGCONT)
    else:
        env = dict(os.environ)
        env.pop("CEPH_KEYRING", None)
        subprocess.run("bin/ceph-osd -i %d -c ceph.conf" % victim, shell=True,
                       cwd=os.environ["CEPH_BUILD"], env=env, capture_output=True)
    if hung:
        th.join(120)
        print(f"  after resuming osd.{victim}: read {'STILL HUNG' if th.is_alive() else res}",
              flush=True)
        sys.exit(1)
    w.remove(oid)
    sys.exit(0 if res.get("ok") else 1)


if __name__ == "__main__":
    main()
