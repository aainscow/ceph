#!/bin/bash
# Medium error on a zone-local EC shard, read through librbd with
# rbd_read_from_replica_policy (sparse reads >= rbd_sparse_read_threshold_bytes).
# Expect: correct data, no client EIO, no OSD crash.
. "$(dirname "$0")/env.sh"
policy=${1:-localize}
img=eio-$RANDOM
set -e
rbd create --size 16M --data-pool ecs rbd/$img
tmp=$(mktemp -d)
head -c 16M /dev/urandom > $tmp/$img.src
rbd import --no-progress $tmp/$img.src - < /dev/null >/dev/null 2>&1 || true
rbd rm --no-progress rbd/$img >/dev/null 2>&1
rbd import --no-progress --data-pool ecs $tmp/$img.src rbd/$img
prefix=$(rbd info rbd/$img --format json | jq -r .block_name_prefix)
obj=$prefix.0000000000000000
read -r acting primary < <(ceph osd map ecs $obj -f json | jq -r '"\(.acting|join(",")) \(.acting_primary)"')
IFS=, read -ra A <<< "$acting"
zs=$(( ${#A[@]} / 2 ))
for i in "${!A[@]}"; do [ "${A[$i]}" = "$primary" ] && pidx=$i; done
pz=$(( pidx / zs )); oz=$(( 1 - pz ))
shard=$(( 1 + oz * zs )); victim=${A[$shard]}
zone=$(ceph osd find $victim -f json | jq -r '.crush_location.datacenter')
echo "img=$img obj=$obj acting=[$acting] primary=osd.$primary; EIO on shard $shard osd.$victim ($zone); policy=$policy"
ceph tell osd.$victim injectdataerr ecs $obj $shard
set +e
rbd export --no-progress rbd/$img $tmp/$img.out \
    --rbd_read_from_replica_policy $policy --crush_location datacenter=$zone \
    --rbd_cache false 2>&1 | tail -3
rc=$?
if cmp -s $tmp/$img.src $tmp/$img.out; then echo "export data OK"; else echo "!!! export data MISMATCH/incomplete (rc=$rc)"; fi
sleep 5
ceph osd stat
grep -l 'FAILED ceph_assert\|Caught signal' $CEPH_BUILD/out/osd.$victim.log && \
    grep -m3 'FAILED ceph_assert' $CEPH_BUILD/out/osd.$victim.log
rm -f $tmp/$img.src $tmp/$img.out
