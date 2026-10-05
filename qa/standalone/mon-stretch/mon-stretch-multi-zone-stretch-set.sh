#!/usr/bin/env bash

source $CEPH_ROOT/qa/standalone/ceph-helpers.sh
source $CEPH_ROOT/qa/standalone/mon-stretch/mon-stretch-helpers.sh

function run() {
    export CEPH_MON_A="127.0.0.1:7350" # git grep '\<7350\>' : there must be only one
    export CEPH_MON_B="127.0.0.1:7351" # git grep '\<7351\>' : there must be only one
    export CEPH_MON_C="127.0.0.1:7352" # git grep '\<7352\>' : there must be only one
    export CEPH_MON="$CEPH_MON_A,$CEPH_MON_B,$CEPH_MON_C"
    run_stretch_tests "$@"
}

# Stretch set and unset configure individual stretch pools and must fail for
# every pool while a pool with num_zones > 1 exists. A num_zones 2 pool
# enables stretch mode.
function TEST_stretch_pool_commands_with_two_zone_pools() {
    local dir=$1
    two_zone_cluster $dir || return 1

    ceph osd pool create rep 8 8 replicated replicated_rule || return 1
    local size=$(pool_field rep size)
    ceph osd pool create rep2 replicated --num-zones 2 || return 1
    ceph osd pool create ec2 erasure --num-zones 2 --k 2 --m 1 || return 1
    test "$(ceph osd dump -f json | jq .stretch_mode.stretch_mode_enabled)" = true || return 1

    for pool in rep rep2 ec2; do
        local rule=$(ceph osd pool get $pool crush_rule -f json | jq -r .crush_rule)
        expect_failure $dir "while stretch mode is enabled" \
            ceph osd pool stretch set $pool 2 2 datacenter $rule 6 3 || return 1
        expect_failure $dir "while stretch mode is enabled" \
            ceph osd pool stretch unset $pool $rule 3 2 || return 1
    done

    test "$(pool_field rep peering_crush_bucket_count)" = 0 || return 1
    test "$(pool_field rep size)" = $size || return 1
    for pool in rep2 ec2; do
        test "$(pool_field $pool peering_crush_bucket_count)" = 2 || return 1
        test "$(pool_field $pool peering_crush_bucket_target)" = 2 || return 1
    done
    test "$(pool_field rep2 size)" = 4 || return 1
    test "$(pool_field ec2 size)" = 6 || return 1
}

# A num_zones 3 pool does not enable stretch mode.
function TEST_stretch_pool_commands_with_three_zone_pool() {
    local dir=$1
    two_zone_cluster $dir || return 1
    # a third datacenter with two hosts for the 3-zone pool's rule
    ceph osd crush add-bucket dc3 datacenter || return 1
    ceph osd crush move dc3 root=default || return 1
    for osd in 6 7; do
        run_osd $dir $osd || return 1
        ceph osd crush add-bucket host$osd host || return 1
        ceph osd crush move host$osd datacenter=dc3 || return 1
        ceph osd crush set osd.$osd 1.0 host=host$osd || return 1
    done

    ceph osd pool create rep 8 8 replicated replicated_rule || return 1
    local size=$(pool_field rep size)
    ceph osd pool create rep3 replicated --num-zones 3 || return 1
    test "$(ceph osd dump -f json | jq .stretch_mode.stretch_mode_enabled)" = false || return 1
    local size3=$(pool_field rep3 size)

    for pool in rep rep3; do
        local rule=$(ceph osd pool get $pool crush_rule -f json | jq -r .crush_rule)
        expect_failure $dir "'rep3' has num_zones 3" \
            ceph osd pool stretch set $pool 2 2 datacenter $rule 6 3 || return 1
        expect_failure $dir "'rep3' has num_zones 3" \
            ceph osd pool stretch unset $pool $rule 3 2 || return 1
    done

    test "$(pool_field rep peering_crush_bucket_count)" = 0 || return 1
    test "$(pool_field rep size)" = $size || return 1
    test "$(pool_field rep3 peering_crush_bucket_count)" = 0 || return 1
    test "$(pool_field rep3 size)" = $size3 || return 1

    # the 3-zone pool was the only obstacle
    ceph osd pool delete rep3 rep3 --yes-i-really-really-mean-it || return 1
    ceph osd pool stretch set rep 2 2 datacenter replicated_rule 4 2 || return 1
    test "$(pool_field rep peering_crush_bucket_count)" = 2 || return 1
}

# The reverse: no pool with num_zones > 1 is created while an individual
# stretch pool exists.
function TEST_multi_zone_pool_create_with_stretch_pool() {
    local dir=$1
    two_zone_cluster $dir || return 1

    ceph osd pool create rep 8 8 replicated replicated_rule || return 1
    ceph osd pool stretch set rep 2 2 datacenter replicated_rule 4 2 || return 1

    expect_failure $dir "'rep' is an individual stretch pool" \
        ceph osd pool create rep2 replicated --num-zones 2 || return 1
    expect_failure $dir "'rep' is an individual stretch pool" \
        ceph osd pool create ec2 erasure --num-zones 2 --k 2 --m 1 || return 1
    for name in rep2 ec2; do
        ! ceph osd pool ls | grep -qx $name || return 1
        ! ceph osd crush rule ls | grep -qx $name || return 1
    done
    ! ceph osd erasure-code-profile ls | grep -qx ec2-k2-m1 || return 1
    test "$(ceph osd dump -f json | jq .stretch_mode.stretch_mode_enabled)" = false || return 1

    test "$(pool_field rep peering_crush_bucket_count)" = 2 || return 1
    test "$(pool_field rep peering_crush_bucket_target)" = 2 || return 1
    test "$(pool_field rep size)" = 4 || return 1
    test "$(pool_field rep min_size)" = 2 || return 1

    # the stretch pool was the only obstacle
    ceph osd pool stretch unset rep replicated_rule 3 2 || return 1
    ceph osd pool create rep2 replicated --num-zones 2 || return 1
    test "$(pool_field rep2 peering_crush_bucket_count)" = 2 || return 1
}

main mon-stretch-multi-zone-stretch-set "$@"
