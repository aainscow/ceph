#!/bin/bash
# Does librbd put BALANCE/LOCALIZE on reads of an EC data pool, and do they
# become split/direct reads?  Uses the rbd client's own objecter counters so
# other cluster IO does not matter.
. "$(dirname "$0")/env.sh"
S=${SNAP:-$CEPH_BUILD}
export LD_LIBRARY_PATH=$S/lib:$LD_LIBRARY_PATH
tmp=$(mktemp -d)
img=pol-$RANDOM
rbd create --size 64M --data-pool ecs rbd/$img
rbd bench --io-type write --io-size 4M --io-total 64M --io-pattern seq rbd/$img >/dev/null 2>&1
for pol in default localize balance; do
    for sz in 16K 1M; do
        sock=$tmp/rbd-$pol-$sz.asok; rm -f $sock
        rbd bench --io-type read --io-size $sz --io-total 32M --io-pattern rand \
            --rbd_read_from_replica_policy $pol --crush_location datacenter=dc1 \
            --rbd_cache false --admin_socket $sock rbd/$img >/dev/null 2>&1 &
        bp=$!
        for i in $(seq 1 100); do [ -S $sock ] && break; sleep 0.1; done
        last=""
        while kill -0 $bp 2>/dev/null; do
            last=$(ceph --admin-daemon $sock perf dump objecter 2>/dev/null) || true
            sleep 0.5
        done
        echo "$last" | python3 -c "
import json,sys
try: d=json.load(sys.stdin)['objecter']
except Exception: print('  $pol $sz: no counters'); sys.exit()
keys=['op_r','split_op_reads','replica_read_sent','replica_read_completed','localize_zone_miss','op_resend']
print('  policy=%-8s io=%-4s ' % ('$pol','$sz') + ' '.join('%s=%s' % (k, d.get(k)) for k in keys))"
    done
done
rbd rm --no-progress rbd/$img
