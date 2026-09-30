#!/bin/bash
# F5 recipe (derived from two chaos hits, PGs 2.f and 2.5):
#  1. pg-upmap a PG with its zone blocks swapped; wait active+clean; write
#     enough to trim the PG log
#  2. fail the zone that holds the PG's current zone-0 block (all OSDs + mon),
#     wait for degraded stretch mode, then also kill one OSD of the surviving
#     zone that is in the PG
#  3. while degraded: rm-pg-upmap (the up set swaps back)
#  4. revive everything (mon first), wait
# Expect: active+clean.  Bug: incomplete forever.
. "$(dirname "$0")/env.sh"
pg=${1:-2.3}
writes=${2:-3000}
B=$CEPH_BUILD
st() { ceph pg ls-by-pool ecs 2>/dev/null | awk -v p=$pg '$1==p{print $11" "$16}'; }
wait_state() {  # regex, tries
    for i in $(seq 1 ${2:-60}); do s=$(st); [[ "$s" =~ $1 ]] && { echo "  $pg: $s (${i}0s)"; return 0; }; sleep 10; done
    echo "  $pg: $s (timeout)"; return 1
}
start() { env -u CEPH_KEYRING $B/bin/ceph-$1 -i $2 -c $B/ceph.conf >/dev/null 2>&1; }
objs=(); i=0
while [ ${#objs[@]} -lt 8 ]; do i=$((i+1)); o=f5b-$pg-$i
    [ "$(ceph osd map ecs $o -f json | jq -r .pgid)" = "$pg" ] && objs+=($o); done
writeburst() {
    python3 - "$writes" "${objs[@]}" <<'EOF'
import os, sys, random
sys.path.insert(0, os.environ["CHAOS_DIR"])
import radosc
n, objs = int(sys.argv[1]), sys.argv[2:]
c = radosc.Client("ecs")
for k in range(n):
    c.write(random.choice(objs), random.randbytes(512), random.randrange(0, 60000))
EOF
}
read -ra U <<< "$(ceph pg map $pg -f json | jq -r '.up|join(" ")')"
flip="${U[3]} ${U[4]} ${U[5]} ${U[0]} ${U[1]} ${U[2]}"
echo "1. up=[${U[*]}] -> upmap [$flip]"
ceph osd pg-upmap $pg $flip >/dev/null
wait_state "active\+clean \[${flip// /,}\]" 90 || exit 1
writeburst; echo "   $writes writes"
read -ra F <<< "$flip"
zone=$(ceph osd find ${F[0]} -f json | jq -r .crush_location.datacenter)
zone_osds=$(ceph osd crush ls-os $zone 2>/dev/null || ceph osd ls-tree $zone)
mon=$(ceph mon dump -f json | jq -r --arg z $zone '.mons[] | select(.crush_location | tostring | contains($z)) | .name')
extra=${F[3]}
echo "2. kill zone $zone osds [$(echo $zone_osds)] mon.$mon, then osd.$extra"
for o in $zone_osds; do kill -9 $(cat $B/out/osd.$o.pid); done
kill -9 $(cat $B/out/mon.$mon.pid)
for i in $(seq 1 60); do [ "$(ceph osd dump -f json | jq .stretch_mode.degraded_stretch_mode)" = 1 ] && break; sleep 5; done
echo "   degraded_stretch_mode=$(ceph osd dump -f json | jq .stretch_mode.degraded_stretch_mode)"
kill -9 $(cat $B/out/osd.$extra.pid)
sleep 20
echo "3. rm-pg-upmap while degraded"
ceph osd rm-pg-upmap $pg >/dev/null
sleep 30
echo "   $pg: $(st)"
writeburst; echo "   $writes writes while degraded"
echo "   $pg: $(st)"
echo "4. revive mon.$mon, zone osds, osd.$extra"
for k in $(seq 1 12); do start mon $mon && break; sleep 10; done
for i in $(seq 1 30); do [ $(ceph quorum_status -f json | jq '.quorum_names|length') = 3 ] && break; sleep 5; done
for o in $zone_osds $extra; do start osd $o; done
if wait_state 'active\+clean' 60; then echo "RESULT: OK"; exit 0; fi
ceph pg $pg query | jq -c '{state, up, acting, rs: [.recovery_state[] | .comment]}'
echo "RESULT: F5 reproduced"
exit 1
