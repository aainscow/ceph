#!/usr/bin/env bash
#
# Copyright (C) 2026 IBM
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Library Public License for more details.
#
# Erasure coded pools with allow_ec_dynamic_chunk_size: the pool settings,
# the chunk size each object gets, and that objects with their own chunk size
# survive overwrites, snapshots, copies, recovery and scrub.

source $CEPH_ROOT/qa/standalone/ceph-helpers.sh

POOL=dcs
K=4
M=2
STRIPE_UNIT=4096

function run() {
    local dir=$1
    shift

    export CEPH_MON="127.0.0.1:7157" # git grep '\<7157\>' : there must be only one
    export CEPH_ARGS
    CEPH_ARGS+="--fsid=$(uuidgen) --auth_cluster_required=none --auth_service_required=none --auth_client_required=none "
    CEPH_ARGS+="--mon-host=$CEPH_MON "
    CEPH_ARGS+="--osd_mclock_override_recovery_settings=true "

    local funcs=${@:-$(set | sed -n -e 's/^\(TEST_[0-9a-z_]*\) .*/\1/p')}
    for func in $funcs ; do
        setup $dir || return 1
        run_mon $dir a || return 1
        run_mgr $dir x || return 1
        for id in $(seq 0 $((K + M))) ; do
            run_osd $dir $id || return 1
        done
        $func $dir || return 1
        teardown $dir || return 1
    done
}

function create_ec_profile() {
    ceph osd erasure-code-profile set dcs-profile \
        k=$K m=$M stripe_unit=$STRIPE_UNIT crush-failure-domain=osd || return 1
}

function create_dynamic_pool() {
    local poolname=$1

    create_ec_profile || return 1
    create_pool $poolname 1 1 erasure dcs-profile || return 1
    ceph osd pool set $poolname allow_ec_overwrites true || return 1
    ceph osd pool set $poolname allow_ec_optimizations true || return 1
    ceph osd pool set $poolname allow_ec_dynamic_chunk_size true \
        --yes-i-really-mean-it || return 1
    wait_for_clean || return 1
}

# The chunk size the pool gives an object of the given size hint:
# ceil(size / K) rounded up to 4 KiB, between the stripe unit and max.
function expected_chunk_size() {
    local size=$1
    local max=${2:-1048576}
    local cs=$(( ((size + K - 1) / K + 4095) / 4096 * 4096 ))
    [ $cs -lt $STRIPE_UNIT ] && cs=$STRIPE_UNIT
    [ $cs -gt $max ] && cs=$max
    echo $cs
}

# Print "<ec_chunk_size> <size>" from an object's object_info, read from its
# primary while that OSD is stopped.
function dump_chunk_size() {
    local dir=$1
    local obj=$2
    local primary=$(get_primary $POOL $obj)
    kill_daemons $dir TERM osd.$primary >&2 < /dev/null || return 1
    _objectstore_tool_nodown $dir $primary $obj dump | \
        jq -r '"\(.info.ec_chunk_size) \(.info.size)"' || return 1
    activate_osd $dir $primary >&2 || return 1
    wait_for_clean >&2 || return 1
}

# Check that an object records chunk_size (0 for the stripe unit).
function check_chunk_size() {
    local dir=$1
    local obj=$2
    local chunk_size=$3
    local recorded=$chunk_size
    [ $recorded -eq $STRIPE_UNIT ] && recorded=0
    local found=$(dump_chunk_size $dir $obj | cut -d' ' -f1)
    if [ "$found" != "$recorded" ] ; then
        echo "$obj: ec_chunk_size $found, expected $recorded"
        return 1
    fi
}

function put_random() {
    local dir=$1
    local obj=$2
    local size=$3
    dd if=/dev/urandom of=$dir/$obj bs=$size count=1 2>/dev/null || return 1
    rados --pool $POOL put $obj $dir/$obj || return 1
}

function check_content() {
    local dir=$1
    local obj=$2
    rados --pool $POOL get $obj $dir/$obj.read || return 1
    cmp $dir/$obj $dir/$obj.read || return 1
    rm -f $dir/$obj.read
}

function TEST_pool_settings() {
    local dir=$1

    create_pool rep 1 1 replicated || return 1
    expect_failure $dir "erasure coded pool" \
        ceph osd pool set rep allow_ec_dynamic_chunk_size true || return 1

    create_ec_profile || return 1
    create_pool ec 1 1 erasure dcs-profile || return 1
    expect_failure $dir "allow_ec_optimizations" \
        ceph osd pool set ec allow_ec_dynamic_chunk_size true || return 1
    expect_failure $dir "allow_ec_optimizations" \
        ceph osd pool set ec ec_dynamic_chunk_size_max 1M || return 1

    ceph osd pool set ec allow_ec_optimizations true || return 1
    expect_failure $dir "yes-i-really-mean-it" \
        ceph osd pool set ec allow_ec_dynamic_chunk_size true || return 1
    ceph osd pool set ec allow_ec_dynamic_chunk_size true \
        --yes-i-really-mean-it || return 1
    ceph osd pool get ec allow_ec_dynamic_chunk_size | \
        grep 'allow_ec_dynamic_chunk_size: true' || return 1
    ceph osd pool ls detail | grep ec_dynamic_chunk_size || return 1
    ceph osd pool set ec allow_ec_dynamic_chunk_size true || return 1
    expect_failure $dir "cannot be disabled" \
        ceph osd pool set ec allow_ec_dynamic_chunk_size false || return 1

    for bad in 1000 8M 1050000 2048 ; do
        expect_failure $dir "ec_dynamic_chunk_size_max must be" \
            ceph osd pool set ec ec_dynamic_chunk_size_max $bad || return 1
    done
    ceph osd pool set ec ec_dynamic_chunk_size_max 2M || return 1
    ceph osd pool get ec ec_dynamic_chunk_size_max | \
        grep 'ec_dynamic_chunk_size_max: 2097152' || return 1
    ceph osd pool set ec ec_dynamic_chunk_size_max 4096 || return 1
    ceph osd pool set ec ec_dynamic_chunk_size_max 0 || return 1
    expect_failure $dir "not set" \
        ceph osd pool get ec ec_dynamic_chunk_size_max || return 1

    ceph config set mon osd_pool_default_flag_ec_optimizations true || return 1
    ceph config set mon osd_pool_default_flag_ec_dynamic_chunk_size true || return 1
    create_pool ec2 1 1 erasure dcs-profile || return 1
    ceph osd pool get ec2 allow_ec_dynamic_chunk_size | \
        grep 'allow_ec_dynamic_chunk_size: true' || return 1
}

function TEST_chunk_sizes() {
    local dir=$1

    create_dynamic_pool $POOL || return 1
    ceph osd pool set $POOL ec_dynamic_chunk_size_max 512K || return 1

    local sizes="1000 16384 16385 65536 1000000 2097152 3000000 5000000"
    local objects=""
    for size in $sizes ; do
        put_random $dir obj_$size $size || return 1
        objects+="obj_$size "
    done

    # An allocation hint sent before the data chooses the chunk size.
    rados --pool $POOL set-alloc-hint hinted 2097152 4096 || return 1
    put_random $dir hinted 1000 || return 1

    for obj in $objects hinted ; do
        check_content $dir $obj || return 1
        local size=$(stat -c %s $dir/$obj)
        local hint=$size
        [ $obj = hinted ] && hint=2097152
        # rados put writes 4 MiB at a time, so larger objects are sized
        # from their first write.
        [ $hint -gt 4194304 ] && hint=4194304
        check_chunk_size $dir $obj $(expected_chunk_size $hint 524288) || return 1
    done
}

function TEST_existing_objects_keep_layout() {
    local dir=$1

    create_ec_profile || return 1
    create_pool $POOL 1 1 erasure dcs-profile || return 1
    ceph osd pool set $POOL allow_ec_overwrites true || return 1
    ceph osd pool set $POOL allow_ec_optimizations true || return 1
    wait_for_clean || return 1

    put_random $dir old 1000000 || return 1
    ceph osd pool set $POOL allow_ec_dynamic_chunk_size true \
        --yes-i-really-mean-it || return 1
    check_chunk_size $dir old $STRIPE_UNIT || return 1

    # A rewrite keeps the layout of an object that holds data.
    put_random $dir old 2000000 || return 1
    check_content $dir old || return 1
    check_chunk_size $dir old $STRIPE_UNIT || return 1

    # Once deleted, the object is new again.
    rados --pool $POOL rm old || return 1
    put_random $dir old 2000000 || return 1
    check_content $dir old || return 1
    check_chunk_size $dir old $(expected_chunk_size 2000000) || return 1
}

function TEST_overwrite_append_truncate() {
    local dir=$1

    create_dynamic_pool $POOL || return 1
    put_random $dir obj 300000 || return 1
    local chunk_size=$(expected_chunk_size 300000)

    # Overwrite across a chunk boundary.
    dd if=/dev/urandom of=$dir/patch bs=1000 count=1 2>/dev/null || return 1
    rados --pool $POOL put obj $dir/patch --offset $((chunk_size - 500)) || return 1
    dd if=$dir/patch of=$dir/obj bs=1 seek=$((chunk_size - 500)) \
        conv=notrunc 2>/dev/null || return 1
    check_content $dir obj || return 1

    # Append well past the first stripe.
    dd if=/dev/urandom of=$dir/tail bs=700000 count=1 2>/dev/null || return 1
    rados --pool $POOL append obj $dir/tail || return 1
    cat $dir/tail >> $dir/obj || return 1
    check_content $dir obj || return 1

    # Truncate into the first chunk.
    rados --pool $POOL truncate obj 12345 || return 1
    truncate -s 12345 $dir/obj || return 1
    check_content $dir obj || return 1

    check_chunk_size $dir obj $chunk_size || return 1
}

function TEST_snapshot_rollback() {
    local dir=$1

    create_dynamic_pool $POOL || return 1
    put_random $dir obj 1500000 || return 1
    cp $dir/obj $dir/obj.orig || return 1
    rados --pool $POOL mksnap snap1 || return 1

    dd if=/dev/urandom of=$dir/patch bs=200000 count=1 2>/dev/null || return 1
    rados --pool $POOL put obj $dir/patch --offset 100000 || return 1
    rados --pool $POOL -s snap1 get obj $dir/obj.snap || return 1
    cmp $dir/obj.orig $dir/obj.snap || return 1

    rados --pool $POOL rollback obj snap1 || return 1
    check_content $dir obj || return 1
    check_chunk_size $dir obj $(expected_chunk_size 1500000) || return 1

    # Recreate the head with another chunk size and roll back again.
    rados --pool $POOL rm obj || return 1
    put_random $dir obj 30000 || return 1
    check_chunk_size $dir obj $(expected_chunk_size 30000) || return 1
    rados --pool $POOL rollback obj snap1 || return 1
    cp $dir/obj.orig $dir/obj || return 1
    check_content $dir obj || return 1
    check_chunk_size $dir obj $(expected_chunk_size 1500000) || return 1
}

# A deleted object chooses its chunk size again when it is recreated. A
# write_full of an object that holds data keeps its layout.
function TEST_recreate() {
    local dir=$1

    create_dynamic_pool $POOL || return 1
    put_random $dir obj 1500000 || return 1
    check_chunk_size $dir obj $(expected_chunk_size 1500000) || return 1

    rados --pool $POOL rm obj || return 1
    put_random $dir obj 30000 || return 1
    check_content $dir obj || return 1
    check_chunk_size $dir obj $(expected_chunk_size 30000) || return 1

    put_random $dir obj 1500000 || return 1
    check_content $dir obj || return 1
    check_chunk_size $dir obj $(expected_chunk_size 30000) || return 1
}

# A copy that needs several copy_get replies goes through a temporary
# object, which must get the source's chunk size.
function TEST_copy_from() {
    local dir=$1

    create_dynamic_pool $POOL || return 1
    ceph config set osd osd_copyfrom_max_chunk 65536 || return 1
    put_random $dir src 1200000 || return 1
    put_random $dir small 30000 || return 1

    rados --pool $POOL cp src dst || return 1
    rados --pool $POOL cp small small_dst || return 1
    cp $dir/src $dir/dst || return 1
    cp $dir/small $dir/small_dst || return 1
    for obj in dst small_dst ; do
        check_content $dir $obj || return 1
    done
    check_chunk_size $dir dst $(expected_chunk_size 1200000) || return 1
    check_chunk_size $dir small_dst $(expected_chunk_size 30000) || return 1
}

function write_objects() {
    local dir=$1
    local prefix=$2
    for size in 5000 100000 1048576 4000000 ; do
        put_random $dir ${prefix}_$size $size || return 1
    done
}

function check_objects() {
    local dir=$1
    local prefix=$2
    for size in 5000 100000 1048576 4000000 ; do
        check_content $dir ${prefix}_$size || return 1
    done
}

function deep_scrub_clean() {
    local dir=$1
    local pgid=$(get_pg $POOL obj_any)
    pg_deep_scrub $pgid || return 1
    rados list-inconsistent-obj $pgid | jq '.inconsistents | length' | \
        grep -qx 0 || return 1
}

# Objects written while a shard's OSD is down are recovered, including by
# a primary that is itself missing them and so starts without an object
# context.
function TEST_recovery() {
    local dir=$1

    create_dynamic_pool $POOL || return 1
    ceph osd set noout || return 1
    ceph config set osd osd_recovery_max_chunk 65536 || return 1

    local osds=($(get_osds $POOL obj_any))
    local primary=${osds[0]}
    local data_shard=${osds[1]}

    kill_daemons $dir TERM osd.$data_shard || return 1
    ceph osd down osd.$data_shard || return 1
    write_objects $dir peer || return 1
    activate_osd $dir $data_shard || return 1
    wait_for_clean || return 1
    check_objects $dir peer || return 1

    kill_daemons $dir TERM osd.$primary || return 1
    ceph osd down osd.$primary || return 1
    write_objects $dir primary || return 1
    activate_osd $dir $primary || return 1
    wait_for_clean || return 1
    test $(get_primary $POOL obj_any) = $primary || return 1
    check_objects $dir primary || return 1

    deep_scrub_clean $dir || return 1
    ceph osd unset noout || return 1
}

# Replacing a shard's OSD backfills it with each object's layout.
function TEST_backfill() {
    local dir=$1

    create_dynamic_pool $POOL || return 1
    write_objects $dir obj || return 1

    local osds=($(get_osds $POOL obj_any))
    ceph osd out osd.${osds[2]} || return 1
    wait_for_clean || return 1
    check_objects $dir obj || return 1
    deep_scrub_clean $dir || return 1
}

main test-erasure-code-dynamic-chunk-size "$@"

# Local Variables:
# compile-command: "cd ../../../build ; make -j4 && ../qa/run-standalone.sh test-erasure-code-dynamic-chunk-size.sh"
# End:
