#!/usr/bin/env bash

source $CEPH_ROOT/qa/standalone/ceph-helpers.sh
source $CEPH_ROOT/qa/standalone/mon-stretch/mon-stretch-helpers.sh

function run() {
    export CEPH_MON_A="127.0.0.1:7353" # git grep '\<7353\>' : there must be only one
    export CEPH_MON_B="127.0.0.1:7354" # git grep '\<7354\>' : there must be only one
    export CEPH_MON_C="127.0.0.1:7355" # git grep '\<7355\>' : there must be only one
    export CEPH_MON="$CEPH_MON_A,$CEPH_MON_B,$CEPH_MON_C"
    run_stretch_tests "$@"
}

# Global stretch mode is the legacy stretch mode: multi-zone pools are not
# created in it.
function TEST_multi_zone_pool_create_in_global_stretch_mode() {
    local dir=$1
    two_zone_cluster $dir 2 || return 1
    enable_global_stretch_mode || return 1

    expect_failure $dir "num_zones > 1" \
        ceph osd pool create rep2 replicated --num-zones 2 || return 1
    expect_failure $dir "num_zones > 1" \
        ceph osd pool create ec2 erasure --num-zones 2 --k 2 --m 1 || return 1
    ! ceph osd pool ls | grep -qx rep2 || return 1
    ! ceph osd pool ls | grep -qx ec2 || return 1
    ! ceph osd crush rule ls | grep -qx rep2 || return 1
    ! ceph osd crush rule ls | grep -qx ec2 || return 1
    ! ceph osd erasure-code-profile ls | grep -qx ec2-k2-m1 || return 1
}

# A multi-zone pool keeps its own stretch values, which global stretch mode
# would overwrite.
function TEST_enable_stretch_mode_with_multi_zone_pool() {
    local dir=$1
    two_zone_cluster $dir 2 || return 1
    ceph osd pool create rep2 replicated --num-zones 2 || return 1
    local rule=$(ceph osd pool get rep2 crush_rule -f json | jq -r .crush_rule)

    expect_failure $dir "'rep2' has num_zones 2" \
        ceph mon enable_stretch_mode c $rule datacenter || return 1
    test "$(ceph mon dump -f json | jq .global_stretch_mode)" = false || return 1
}

main mon-stretch-global-mode-multi-zone "$@"
