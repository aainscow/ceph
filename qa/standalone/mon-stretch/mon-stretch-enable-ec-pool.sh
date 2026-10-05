#!/usr/bin/env bash

source $CEPH_ROOT/qa/standalone/ceph-helpers.sh
source $CEPH_ROOT/qa/standalone/mon-stretch/mon-stretch-helpers.sh

function run() {
    export CEPH_MON_A="127.0.0.1:7361" # git grep '\<7361\>' : there must be only one
    export CEPH_MON_B="127.0.0.1:7362" # git grep '\<7362\>' : there must be only one
    export CEPH_MON_C="127.0.0.1:7363" # git grep '\<7363\>' : there must be only one
    export CEPH_MON="$CEPH_MON_A,$CEPH_MON_B,$CEPH_MON_C"
    run_stretch_tests "$@"
}

# As on main, global stretch mode is not enabled while an erasure coded pool
# exists, whatever the rule given.
function TEST_enable_stretch_mode_with_ec_pool() {
    local dir=$1

    two_zone_cluster $dir 2 || return 1
    ceph mon set election_strategy connectivity || return 1

    ceph osd erasure-code-profile set p22 k=2 m=2 crush-failure-domain=osd || return 1
    ceph osd pool create ec 8 8 erasure p22 || return 1
    local min_size=$(pool_field ec min_size)

    for rule in ec replicated_rule; do
        expect_failure $dir "stretched pools must be replicated; 'ec' is erasure-coded" \
            ceph mon enable_stretch_mode c $rule datacenter || return 1
    done
    test "$(ceph mon dump -f json | jq .global_stretch_mode)" = false || return 1
    test "$(ceph osd dump -f json | jq .stretch_mode.stretch_mode_enabled)" = false || return 1
    test "$(pool_field ec size)" = 4 || return 1
    test "$(pool_field ec min_size)" = $min_size || return 1
    ceph osd pool get ec crush_rule | grep -w ec || return 1

    # the erasure coded pool was the only obstacle
    ceph osd pool delete ec ec --yes-i-really-really-mean-it || return 1
    enable_global_stretch_mode || return 1
}

main mon-stretch-enable-ec-pool "$@"
