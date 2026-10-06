#!/usr/bin/env bash

source $CEPH_ROOT/qa/standalone/ceph-helpers.sh
source $CEPH_ROOT/qa/standalone/mon-stretch/mon-stretch-helpers.sh

function run() {
    export CEPH_MON_A="127.0.0.1:7356" # git grep '\<7356\>' : there must be only one
    export CEPH_MON_B="127.0.0.1:7357" # git grep '\<7357\>' : there must be only one
    export CEPH_MON_C="127.0.0.1:7358" # git grep '\<7358\>' : there must be only one
    export CEPH_MON="$CEPH_MON_A,$CEPH_MON_B,$CEPH_MON_C"
    run_stretch_tests "$@"
}

# The stretch mode transitions must not change a multi-zone pool's min_size.
function TEST_multi_zone_replicated_min_size() {
    local dir=$1

    two_zone_cluster $dir || return 1
    ceph osd pool create rep_default replicated --num-zones 2 || return 1
    ceph config set global osd_pool_default_min_size 1 || return 1
    ceph osd pool create rep_min1 replicated --num-zones 2 || return 1
    wait_for_clean || return 1
    test "$(pool_field rep_default min_size)" = 2 || return 1
    test "$(pool_field rep_min1 min_size)" = 1 || return 1

    lose_dc2 $dir || return 1
    ceph osd pool create rep_min1_degraded replicated --num-zones 2 || return 1
    ceph osd pool ls detail
    test "$(pool_field rep_default min_size)" = 2 || return 1
    test "$(pool_field rep_min1 min_size)" = 1 || return 1
    test "$(pool_field rep_min1_degraded min_size)" = 1 || return 1

    restore_dc2 $dir || return 1
    ceph osd pool ls detail
    test "$(pool_field rep_default min_size)" = 2 || return 1
    test "$(pool_field rep_min1 min_size)" = 1 || return 1
    test "$(pool_field rep_min1_degraded min_size)" = 1 || return 1
}

# Global stretch mode is the legacy stretch mode and keeps halving min_size on
# the degraded transition and setting mon_stretch_pool_min_size on the
# healthy one.
function TEST_global_stretch_mode_min_size() {
    local dir=$1

    two_zone_cluster $dir || return 1
    enable_global_stretch_mode || return 1
    wait_for_clean || return 1
    test "$(pool_field stretched min_size)" = 2 || return 1

    lose_dc2 $dir || return 1
    test "$(pool_field stretched min_size)" = 1 || return 1

    restore_dc2 $dir || return 1
    test "$(pool_field stretched min_size)" = 2 || return 1
}

main mon-stretch-multi-zone-min-size "$@"
