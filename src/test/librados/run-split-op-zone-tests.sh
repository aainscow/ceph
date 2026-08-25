#!/usr/bin/env bash
# run-split-op-zone-tests.sh
#
# Wrapper for the LibRadosSplitOpZone gtest suite.
#
# The zone-aware split-read tests require a 2-zone CRUSH topology: two
# datacenter-level buckets ("zone-0" and "zone-1") with three OSDs each.
# This script:
#   1. Saves the current CRUSH map so it can be restored on exit.
#   2. Adds the datacenter buckets and moves OSD 0-2 into zone-0, 3-5 into zone-1.
#   3. Waits for the OSD map to propagate and all PGs to become active+clean.
#   4. Runs ceph_test_rados_api_split_op_pp filtered to LibRadosSplitOpZone.
#   5. Restores the original CRUSH map (even on failure).
#
# Usage:  run-split-op-zone-tests.sh [extra gtest args...]
#
# The CEPH_BIN and CEPH_ROOT environment variables must be set (they are set
# automatically when the test is registered via add_ceph_test in CMakeLists).

set -euo pipefail

CEPH_BIN="${CEPH_BIN:-${CMAKE_RUNTIME_OUTPUT_DIRECTORY:-$(dirname "$0")}}"
CEPH="${CEPH_BIN}/ceph"
TEST_BIN="${CEPH_BIN}/ceph_test_rados_api_split_op_pp"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

log() { echo "[run-split-op-zone-tests] $*"; }

die() { echo "[run-split-op-zone-tests] ERROR: $*" >&2; exit 1; }

wait_for_clean() {
    local tries=120
    while (( tries-- > 0 )); do
        local unclean
        unclean=$("$CEPH" -s --format json 2>/dev/null \
            | python3 -c "
import json, sys
d = json.load(sys.stdin)
pg = d.get('pgmap', {})
print(pg.get('pgs_by_state', [{}])[0].get('count', 0) if False else
      sum(s.get('count', 0)
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
# CRUSH setup / teardown
# ---------------------------------------------------------------------------

CRUSH_BACKUP=$(mktemp /tmp/crush-backup-XXXXXX.bin)
TOPOLOGY_APPLIED=0

crush_setup() {
    log "Backing up CRUSH map to $CRUSH_BACKUP"
    "$CEPH" osd getcrushmap -o "$CRUSH_BACKUP" \
        || die "Failed to get CRUSH map"

    log "Adding datacenter buckets zone-0 and zone-1"
    "$CEPH" osd crush add-bucket zone-0 datacenter
    "$CEPH" osd crush add-bucket zone-1 datacenter

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
    "$CEPH" osd crush rule create-simple replicated_rule_datacenter datacenter osd 2>/dev/null || true

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
[[ -x "$CEPH"     ]] || die "ceph CLI not found: $CEPH"

crush_setup

log "Running LibRadosSplitOpZone tests..."
"$TEST_BIN" \
    --gtest_filter="LibRadosSplitOpZoneParamCombination/*" \
    "$@"

# EXIT trap will call crush_teardown
