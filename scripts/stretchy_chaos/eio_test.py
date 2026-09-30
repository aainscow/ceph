#!/usr/bin/env python3
"""Medium-error handling for zone-local EC direct reads.

Writes an object, injects a BlueStore read error on one shard in the zone that
the client reads from (via `ceph tell osd.N injectdataerr`), then reads it with
LOCALIZE_READS from that zone, with BALANCE_READS, and with no flag.
Design doc 7.2/7.1.2: any direct-read failure (incl. a medium error) must be
redirected to the primary, which reconstructs; the client must get correct
data, never EIO.  Also reports whether the OSDs survived.
"""

import argparse
import json
import random
import subprocess
import sys
import time

import radosc


def ceph_json(cmd):
    r = subprocess.run(f"ceph {cmd} -f json", shell=True, capture_output=True, text=True)
    return json.loads(r.stdout)


def ceph(cmd):
    r = subprocess.run(f"ceph {cmd}", shell=True, capture_output=True, text=True)
    return r.returncode, r.stdout.strip() + r.stderr.strip()


def zone_of(osd, zones):
    return next(z for z, osds in zones.items() if osd in osds)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pool", default="ecs")
    ap.add_argument("--size", type=int, default=64 << 10)
    ap.add_argument("--rel-shard", type=int, default=1,
                    help="relative shard to corrupt in the client's zone")
    ap.add_argument("--same-zone-as-primary", action="store_true")
    ap.add_argument("--read-len", type=int, default=0, help="0 = whole object")
    args = ap.parse_args()

    tree = ceph_json("osd crush tree")
    nodes = {n["id"]: n for n in tree["nodes"]}

    def leaves(i):
        return [i] if i >= 0 else sum((leaves(c) for c in nodes[i].get("children", [])), [])
    zones = {n["name"]: leaves(n["id"]) for n in tree["nodes"]
             if n["type"] == "datacenter" and leaves(n["id"])}
    pool = next(p for p in ceph_json("osd pool ls detail") if p["pool_name"] == args.pool)
    zone_size = pool["size"] // pool["options"].get("num_zones", 1)

    oid = f"eio-{random.randint(0, 1 << 30)}"
    data = random.randbytes(args.size)
    w = radosc.Client(args.pool)
    w.write_full(oid, data)
    m = ceph_json(f"osd map {args.pool} {oid}")
    acting, primary = m["acting"], m["acting_primary"]
    prim_zone_idx = acting.index(primary) // zone_size
    zidx = prim_zone_idx if args.same_zone_as_primary else 1 - prim_zone_idx
    abs_shard = args.rel_shard + zidx * zone_size
    victim = acting[abs_shard]
    client_zone = zone_of(victim, zones)
    print(f"oid={oid} pg={m['pgid']} acting={acting} primary=osd.{primary} "
          f"(acting zone {prim_zone_idx}); injecting EIO on shard {abs_shard} "
          f"osd.{victim} ({client_zone}); client crush_location={client_zone}")
    rc, out = ceph(f"tell osd.{victim} injectdataerr {args.pool} {oid} {abs_shard}")
    print(f"  injectdataerr -> {rc} {out}")

    length = args.read_len or args.size
    results = {}
    for policy in ("localize", "balance", "none"):
        cl = radosc.Client(args.pool, crush_location=f"datacenter={client_zone}")
        outcomes = []
        for _ in range(8 if policy == "balance" else 2):
            try:
                got = cl.read(oid, 0, length, radosc.POLICY_FLAGS[policy])
                outcomes.append("ok" if got == data[:length] else "MISCOMPARE")
            except radosc.RadosError as e:
                outcomes.append(f"ERR {e.rc}")
        cl.close()
        results[policy] = outcomes
        print(f"  {policy:9s}: {outcomes}")
    time.sleep(3)
    st = ceph_json("osd dump")
    down = [o["osd"] for o in st["osds"] if not o["up"]]
    print(f"  OSDs down after test: {down}")
    bad = {p: o for p, o in results.items() if any(x != "ok" for x in o)}
    if bad or down:
        print(f"!!! FAIL: {bad} down={down}")
        sys.exit(1)
    w.remove(oid)
    print("PASS")


if __name__ == "__main__":
    main()
