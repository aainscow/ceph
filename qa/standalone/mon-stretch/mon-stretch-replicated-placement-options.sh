#!/usr/bin/env bash

source $CEPH_ROOT/qa/standalone/ceph-helpers.sh
source $CEPH_ROOT/qa/standalone/mon-stretch/mon-stretch-helpers.sh

function run() {
    export CEPH_MON="127.0.0.1:7359" # git grep '\<7359\>' : there must be only one
    run_stretch_tests "$@"
}

# A single-zone replicated pool uses the default rule, so options that
# would build a rule for it are refused rather than ignored.
function TEST_single_zone_replicated_placement_options() {
    local dir=$1
    run_mon $dir a || return 1

    for zones in "" "--num-zones 1"; do
        for opt in "--root default" "--zone_failure_domain datacenter" \
                   "--osd_failure_domain host" "--class hdd"; do
            expect_failure $dir "require num_zones > 1" \
                ceph osd pool create rep 8 replicated $zones $opt || return 1
        done
    done
    ! ceph osd pool ls | grep -qx rep || return 1
    ceph osd pool create plain 8 replicated --num-zones 1 || return 1
}

main mon-stretch-replicated-placement-options "$@"
