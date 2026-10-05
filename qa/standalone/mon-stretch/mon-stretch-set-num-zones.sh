#!/usr/bin/env bash

source $CEPH_ROOT/qa/standalone/ceph-helpers.sh
source $CEPH_ROOT/qa/standalone/mon-stretch/mon-stretch-helpers.sh

function run() {
    export CEPH_MON="127.0.0.1:7360" # git grep '\<7360\>' : there must be only one
    run_stretch_tests "$@"
}

# send osd pool set as a mon command, which the ceph CLI refuses for a
# variable it does not offer
function mon_command_pool_set() {
    python3 - "$@" <<'EOF'
import json, sys, rados
cluster = rados.Rados(conffile='/dev/null')
cluster.conf_parse_env()
cluster.connect()
cmd = {'prefix': 'osd pool set', 'pool': sys.argv[1], 'var': sys.argv[2], 'val': sys.argv[3]}
ret, _, outs = cluster.mon_command(json.dumps(cmd), b'')
print(outs)
sys.exit(1 if ret else 0)
EOF
}

# num_zones is set at pool creation and cannot be changed afterwards.
function TEST_set_num_zones() {
    local dir=$1
    run_mon $dir a || return 1

    ceph osd pool create rep 8 || return 1
    ceph osd erasure-code-profile set p21 k=2 m=1 crush-failure-domain=osd || return 1
    ceph osd pool create ec 8 8 erasure p21 || return 1

    ! ceph osd pool set rep num_zones 2 || return 1
    for pool in rep ec; do
        expect_failure $dir "num_zones cannot be changed" \
            mon_command_pool_set $pool num_zones 2 || return 1
        test "$(ceph osd pool get $pool num_zones -f json | jq .num_zones)" = 1 || return 1
    done
    test "$(pool_field ec size)" = 3 || return 1
}

main mon-stretch-set-num-zones "$@"
