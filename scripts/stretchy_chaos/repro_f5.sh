#!/bin/bash
# F5 recipe: stretch EC PG left incomplete after its zone blocks are swapped by
# an upmap and swapped back once the PG log has been trimmed.
#  1. write enough to one PG that its log trims
#  2. pg-upmap the PG to the same OSD set with the zone blocks swapped; wait clean
#  3. more writes (trim again), then rm-pg-upmap (swap back); wait
# Expect: active+clean.  Bug: remapped+incomplete (acting built from OSDs whose
# on-disk shard ids differ from their positions).
. "$(dirname "$0")/env.sh"
pg=${1:-2.3}
writes=${2:-3000}
wait_clean() {
    for i in $(seq 1 ${1:-60}); do
        st=$(ceph pg ls-by-pool ecs 2>/dev/null | awk -v p=$pg '$1==p{print $11}')
        [ "$st" = "active+clean" ] && { echo "  $pg active+clean after ${i}0s"; return 0; }
        sleep 10
    done
    echo "  $pg NOT clean: $st"; return 1
}
objs=()
i=0
while [ ${#objs[@]} -lt 8 ]; do
    i=$((i+1)); o=f5-$pg-$i
    [ "$(ceph osd map ecs $o -f json | jq -r .pgid)" = "$pg" ] && objs+=($o)
done
echo "objects in $pg: ${objs[*]}"
writeburst() {
    python3 - "$pg" "$writes" "${objs[@]}" <<'EOF'
import os, sys, random
sys.path.insert(0, os.environ["CHAOS_DIR"])
import radosc
pg, n, objs = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
c = radosc.Client("ecs")
for k in range(n):
    c.write(random.choice(objs), random.randbytes(512), random.randrange(0, 60000))
print(f"  {n} writes done")
EOF
}
ceph osd rm-pg-upmap $pg >/dev/null 2>&1
wait_clean
writeburst
up=$(ceph pg map $pg -f json | jq -r '.up|join(" ")')
read -ra U <<< "$up"
flip="${U[3]} ${U[4]} ${U[5]} ${U[0]} ${U[1]} ${U[2]}"
echo "up=[$up] -> upmap [$flip]"
ceph osd pg-upmap $pg $flip
wait_clean 90 || exit 1
writeburst
echo "rm-pg-upmap (swap back)"
ceph osd rm-pg-upmap $pg
wait_clean 60 && { echo "RESULT: OK"; exit 0; }
ceph pg $pg query | jq -c '{state, up, acting, rs: [.recovery_state[] | .comment]}'
echo "RESULT: F5 reproduced"
exit 1
