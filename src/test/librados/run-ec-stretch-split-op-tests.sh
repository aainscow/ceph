#!/usr/bin/env bash
# run-ec-stretch-split-op-tests.sh
#
# Full integration wrapper for EC stretch split-op tests.
#
# This script:
#   1. Starts a 6-OSD vstart cluster (or verifies one is running).
#   2. Applies a 2-zone CRUSH topology (zone-0: OSD 0-2, zone-1: OSD 3-5).
#   3. Runs all split-op test suites (existing + new zone stats tests).
#   4. Fails if any test is FAILED or SKIPPED due to cluster misconfiguration.
#   5. Restores the original CRUSH map on exit.
#
# Usage:  run-ec-stretch-split-op-tests.sh [extra gtest args...]
#
# The CEPH_BIN and CEPH_ROOT environment variables must be set (they are set
# automatically when the test is registered via add_ceph_test in CMakeLists).

set -euo pipefail

CEPH_BIN="${CEPH_BIN:-${CMAKE_RUNTIME_OUTPUT_DIRECTORY:-$(dirname "$0")}}"
CEPH="${CEPH_BIN}/ceph"
TEST_BIN="${CEPH_BIN}/ceph_test_rados_api_split_op_pp"
BUILD_DIR="${CEPH_BIN}/.."
VSTART_ENV="${BUILD_DIR}/vstart_environment.sh"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

log() { echo "[run-ec-stretch-split-op-tests] $*"; }

die() { echo "[run-ec-stretch-split-op-tests] ERROR: $*" >&2; exit 1; }

wait_for_clean() {
    local tries=120
    while (( tries-- > 0 )); do
        local unclean
        unclean=$("$CEPH" -s --format json 2>/dev/null \
            | python3 -c "
import json, sys
d = json.load(sys.stdin)
pg = d.get('pgmap', {})
print(sum(s.get('count', 0)
          for s in pg.get('pgs_by_state', [])
          if 'active+clean' not in s.get('state_name', '')))
" 2>/dev/null || echo 1)
        if [[ "$unclean" == "0" ]]; then
            return 0
        fi
        log "Waiting for clean PGs (unclean=$unclean, tries left=$tries)..."
        sleep 2
    done
    log "WARNING: timed out waiting for clean PGs, continuing anyway"
    return 0
}

# ---------------------------------------------------------------------------
# Cluster setup
# ---------------------------------------------------------------------------

start_cluster() {
    if "$CEPH" -s &>/dev/null; then
        log "Cluster already running."
        return 0
    fi

    if [[ -x /root/restart_ceph_ec_cluster ]]; then
        log "Starting cluster via /root/restart_ceph_ec_cluster..."
        /root/restart_ceph_ec_cluster
    else
        die "Cluster is not running and /root/restart_ceph_ec_cluster not found."
    fi

    if [[ -f "$VSTART_ENV" ]]; then
        # shellcheck disable=SC1090
        source "$VSTART_ENV"
    fi
}

# ---------------------------------------------------------------------------
# CRUSH setup / teardown
# ---------------------------------------------------------------------------

CRUSH_BACKUP=$(mktemp /tmp/crush-backup-XXXXXX.bin)
TOPOLOGY_APPLIED=0

crush_setup() {
    log "Backing up CRUSH map to $CRUSH_BACKUP"
    "$CEPH" osd getcrushmap -o "$CRUSH_BACKUP" \
        || die "Failed to get CRUSH map"

    log "Adding datacenter buckets zone-0 and zone-1"
    "$CEPH" osd crush add-bucket zone-0 datacenter 2>/dev/null || true
    "$CEPH" osd crush add-bucket zone-1 datacenter 2>/dev/null || true

    log "Moving zone-0 and zone-1 under root=default"
    "$CEPH" osd crush move zone-0 root=default
    "$CEPH" osd crush move zone-1 root=default

    log "Moving OSD 0-2 into datacenter=zone-0"
    for i in 0 1 2; do
        "$CEPH" osd crush move "osd.$i" datacenter=zone-0
    done

    log "Moving OSD 3-5 into datacenter=zone-1"
    for i in 3 4 5; do
        "$CEPH" osd crush move "osd.$i" datacenter=zone-1
    done

    "$CEPH" osd crush dump \
        | python3 -c "
import json, sys
d = json.load(sys.stdin)
dcs = [b['name'] for b in d['buckets'] if b['type_name'] == 'datacenter']
assert len(dcs) >= 2, 'Expected >=2 datacenter buckets, got: ' + str(dcs)
print('CRUSH topology OK:', dcs)
" || die "CRUSH topology verification failed"

    "$CEPH" osd crush dump > /dev/null  # flush
    "$CEPH" osd crush tunables optimal 2>/dev/null || true
    "$CEPH" osd set-require-min-compat-client luminous 2>/dev/null || true

    TOPOLOGY_APPLIED=1

    log "Waiting for OSD map to propagate..."
    "$CEPH" osd stat > /dev/null
    wait_for_clean
    log "CRUSH topology ready."
}

crush_teardown() {
    if [[ "$TOPOLOGY_APPLIED" -eq 0 ]]; then
        return 0
    fi
    log "Restoring original CRUSH map from $CRUSH_BACKUP"
    "$CEPH" osd setcrushmap -i "$CRUSH_BACKUP" 2>/dev/null \
        || log "WARNING: failed to restore CRUSH map"
    rm -f "$CRUSH_BACKUP"
    log "CRUSH map restored."
}

trap crush_teardown EXIT

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

[[ -x "$TEST_BIN" ]] || die "Test binary not found: $TEST_BIN"

# Source vstart environment if available.
if [[ -f "$VSTART_ENV" ]]; then
    # shellcheck disable=SC1090
    source "$VSTART_ENV"
fi

[[ -x "$CEPH" ]] || die "ceph CLI not found: $CEPH"

start_cluster
crush_setup

LOG=/tmp/split_op_test_run.log

log "Running all split-op tests..."
"$TEST_BIN" \
    --gtest_filter="LibRadosSplitOpPPParamCombination/*:LibRadosSplitOpECPPParamCombination/*:LibRadosSplitOpZoneParamCombination/*:LibRadosSplitOpZoneStatsParamCombination/*" \
    "$@" 2>&1 | tee "$LOG"

TEST_RC=${PIPESTATUS[0]}

# ---------------------------------------------------------------------------
# Parse results
# ---------------------------------------------------------------------------

FAILED_TESTS=$(grep -cE "^\[  FAILED  \]" "$LOG" || true)
SKIPPED_ZONE=$(grep -E "SKIPPED.*zone|SKIPPED.*topology|SKIPPED.*CRUSH" "$LOG" \
    | grep -iE "LibRadosSplitOpZone|LibRadosSplitOpZoneStats" || true)

if [[ -n "$SKIPPED_ZONE" ]]; then
    log "FAILURE: Zone tests were SKIPPED due to cluster misconfiguration:"
    echo "$SKIPPED_ZONE"
    exit 1
fi

if [[ "$FAILED_TESTS" -gt 0 ]]; then
    log "FAILURE: $FAILED_TESTS test(s) FAILED."
    grep -E "^\[  FAILED  \]" "$LOG"
    exit 1
fi

if [[ "$TEST_RC" -ne 0 ]]; then
    log "FAILURE: Test binary exited with rc=$TEST_RC"
    exit "$TEST_RC"
fi

PASSED_TESTS=$(grep -cE "^\[  PASSED  \]" "$LOG" || true)
log "SUCCESS: $PASSED_TESTS test(s) passed, 0 failed."

# EXIT trap will call crush_teardown
