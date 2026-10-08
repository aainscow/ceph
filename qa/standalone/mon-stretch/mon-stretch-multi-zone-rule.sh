#!/usr/bin/env bash

source $CEPH_ROOT/qa/standalone/ceph-helpers.sh
source $CEPH_ROOT/qa/standalone/mon-stretch/mon-stretch-helpers.sh

function run() {
    export CEPH_MON_A="127.0.0.1:7268" # git grep '\<7268\>' : there must be only one
    export CEPH_MON_B="127.0.0.1:7269" # git grep '\<7269\>' : there must be only one
    export CEPH_MON_C="127.0.0.1:7270" # git grep '\<7270\>' : there must be only one
    export CEPH_MON="$CEPH_MON_A,$CEPH_MON_B,$CEPH_MON_C"
    run_stretch_tests "$@"
}

function rule_of() {
    ceph osd pool get $1 crush_rule -f json | jq -r .crush_rule
}

function num_zones_of() {
    ceph osd pool get $1 num_zones -f json | jq -r .num_zones
}

# A multi-zone pool needs a rule that places a full set of shards or replicas
# in each zone. A rule built for one zone, such as one made by
# 'osd crush rule create-erasure' without --num_zones, chooses OSDs across
# the datacenters, so a zone's block of shards can span both.
function TEST_create_ec_pool_with_one_zone_rule() {
    local dir=$1
    two_zone_cluster $dir 4 || return 1

    ceph osd erasure-code-profile set ec22 plugin=jerasure \
        technique=reed_sol_van k=2 m=2 crush-failure-domain=osd || return 1
    ceph osd crush rule create-erasure one_zone || return 1
    expect_failure $dir "does not place 4 OSDs in each of 2 datacenter buckets" \
        ceph osd pool create ec 8 8 erasure ec22 one_zone --num_zones 2 || return 1
    ! ceph osd pool ls | grep -qx ec || return 1

    ceph osd crush rule create-erasure two_zones ec22 --num_zones 2 || return 1
    ceph osd pool create ec 8 8 erasure ec22 two_zones --num_zones 2 || return 1
    test "$(rule_of ec)" = two_zones || return 1
    add_zone_take_rule $dir zone_takes erasure 4 || return 1
    ceph osd pool create ec_takes 8 8 erasure ec22 zone_takes --num_zones 2 || return 1
    ceph osd pool create ec_auto 8 8 erasure --k 2 --m 2 --num_zones 2 || return 1
    test "$(rule_of ec_auto)" = ec_auto || return 1
}

function TEST_create_replicated_pool_with_one_zone_rule() {
    local dir=$1
    two_zone_cluster $dir 2 || return 1

    expect_failure $dir "does not place 2 OSDs in each of 2 datacenter buckets" \
        ceph osd pool create rep 8 8 replicated replicated_rule --num_zones 2 || return 1
    ! ceph osd pool ls | grep -qx rep || return 1

    add_zone_take_rule $dir zone_takes replicated 2 || return 1
    ceph osd pool create rep 8 8 replicated zone_takes --num_zones 2 || return 1
    test "$(rule_of rep)" = zone_takes || return 1
    ceph osd crush rule create-stretch-replicated --rule_name two_zones || return 1
    ceph osd pool create rep2 8 8 replicated two_zones --num_zones 2 || return 1
    ceph osd pool create rep_auto 8 --num_zones 2 || return 1
    test "$(rule_of rep_auto)" = rep_auto || return 1
}

function TEST_set_one_zone_rule_on_multi_zone_pool() {
    local dir=$1
    two_zone_cluster $dir 4 || return 1

    ceph osd pool create ec 8 8 erasure --k 2 --m 2 --num_zones 2 || return 1
    ceph osd crush rule create-erasure one_zone || return 1
    expect_failure $dir "does not place 4 OSDs in each of 2 datacenter buckets" \
        ceph osd pool set ec crush_rule one_zone || return 1
    test "$(rule_of ec)" = ec || return 1
    ceph osd crush rule create-erasure two_zones ec-k2-m2 --num_zones 2 || return 1
    ceph osd pool set ec crush_rule two_zones || return 1
    test "$(rule_of ec)" = two_zones || return 1

    ceph osd pool create rep 8 --num_zones 2 || return 1
    expect_failure $dir "does not place 2 OSDs in each of 2 datacenter buckets" \
        ceph osd pool set rep crush_rule replicated_rule || return 1
    test "$(rule_of rep)" = rep || return 1
    add_zone_take_rule $dir zone_takes replicated 2 || return 1
    ceph osd pool set rep crush_rule zone_takes || return 1
    test "$(rule_of rep)" = zone_takes || return 1

    # a replica change reuses an existing rule named <pool>-replica-<replica>
    ceph osd pool create rep2 8 --num_zones 2 || return 1
    ceph osd crush rule create-replicated rep2-replica-3 default host || return 1
    expect_failure $dir "does not place 3 OSDs in each of 2 datacenter buckets" \
        ceph osd pool set rep2 replica 3 || return 1
    test "$(rule_of rep2)" = rep2 || return 1
    ceph osd pool set rep2 replica 4 || return 1
    test "$(rule_of rep2)" = rep2-replica-4 || return 1
}

function TEST_stretch_pool_keeping_one_zone_rule() {
    local dir=$1
    two_zone_cluster $dir 4 || return 1

    ceph osd pool create rep 8 8 replicated || return 1
    expect_failure $dir "does not place 2 OSDs in each of 2 datacenter buckets" \
        ceph osd pool set rep num_zones 2 --zone_failure_domain datacenter \
        --replica 2 --osd_failure_domain host --crush_rule replicated_rule || return 1
    test "$(num_zones_of rep)" = 1 || return 1
    ceph osd pool set rep num_zones 2 --zone_failure_domain datacenter \
        --replica 2 --osd_failure_domain host || return 1
    test "$(rule_of rep)" = rep || return 1

    ceph osd pool create ec 8 8 erasure --k 2 --m 2 || return 1
    test "$(rule_of ec)" = ec || return 1
    expect_failure $dir "does not place 4 OSDs in each of 2 datacenter buckets" \
        ceph osd pool set ec num_zones 2 --zone_failure_domain datacenter \
        --crush_rule ec || return 1
    test "$(num_zones_of ec)" = 1 || return 1
    ceph osd pool set ec num_zones 2 --zone_failure_domain datacenter || return 1
    test "$(rule_of ec)" = ec-stretch || return 1
}

main mon-stretch-multi-zone-rule "$@"
