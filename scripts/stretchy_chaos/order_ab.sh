#!/bin/bash
# A/B: does a write-heavy ceph_test_rados see out-of-order write acks while OSDs
# are killed/restarted?  Client binary+librados from snapshot $1 (daemons stay
# on whatever the cluster runs).  ceph_test_rados aborts on out-of-order acks.
. "$(dirname "$0")/env.sh"
C=${1:?client snapshot dir}; DUR=${2:-600}; OUT=${3:?log}
B=$CEPH_BUILD
D=${DAEMON_SNAP:-$CEPH_BUILD}   # daemons
export CEPH_ARGS="--erasure_code_dir=$D/lib --plugin_dir=$D/lib"
env LD_LIBRARY_PATH=$C/lib:$LD_LIBRARY_PATH $C/bin/ceph_test_rados --pool ecs \
    --max-ops 1000000 --objects 32 --max-in-flight 16 --size 200000 \
    --min-stride-size 1000 --max-stride-size 50000 --max-seconds $DUR \
    --localize-reads --crush-location datacenter=dc1 \
    --op write 100 --op append 50 --op read 50 > $OUT 2>&1 &
cp=$!
end=$((SECONDS+DUR))
while [ $SECONDS -lt $end ] && kill -0 $cp 2>/dev/null; do
    o=$((RANDOM % 8)); pid=$(cat $B/out/osd.$o.pid)
    kill -9 $pid; sleep $((5 + RANDOM % 15))
    env -u CEPH_KEYRING LD_LIBRARY_PATH=$D/lib:$LD_LIBRARY_PATH $D/bin/ceph-osd -i $o -c $B/ceph.conf >/dev/null 2>&1
    sleep $((10 + RANDOM % 20))
done
wait $cp; rc=$?
echo "client rc=$rc ($C)"; grep -m2 -E 'Error: finished tid|last_acked_tid|ceph_abort|FAILED' $OUT
