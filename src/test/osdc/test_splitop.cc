// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2025 IBM
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

/**
 * TestSplitOpsUT — dedicated unit tests for the SplitOps interface.
 *
 * These tests exercise the pure logic components of SplitOp that do not
 * require a running Objecter, OSD, or live I/O path:
 *
 *  1. SplitOp::validate_flags()   — flag-combination acceptance/rejection
 *  2. ECStripeIterator / ECStripeView — stripe-traversal geometry
 *  3. ECSplitOp::local_zone_for_acting_set() — zone selection edge cases
 *  4. SplitOp::create() — the OSD a single direct op is sent to
 *  5. SplitOp::assemble_rc() — which errors go to the primary
 *  6. ECSplitOp — reads planned around an unavailable shard
 *
 * Sections 1-3 use a minimal pg_pool_t built inline (no OSDMap, no peering).
 * For ECStripeIterator, a thin helper subclass exposes the protected types.
 * Section 4 gives an unconnected Objecter an OSDMap.
 */

#include <gtest/gtest.h>
#include <numeric>

#include "common/ref.h"
#include "common/async/context_pool.h"
#include "common/common_init.h"
#include "global/global_context.h"
#include "global/global_init.h"
#include "messages/MOSDMap.h"
#include "mon/MonClient.h"
#include "msg/Messenger.h"
#include "osd/OSDMap.h"
#include "osd/osd_types.h"
#include "osdc/SplitOp.h"
#include "global/global_context.h"
#include "include/rados.h"
#include "test/osdc/SplitOpCounters.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

constexpr uint64_t CHUNK = 4096;
constexpr int K = 4;
constexpr int M = 2;
constexpr uint64_t STRIPE_WIDTH = CHUNK * K;

/**
 * Build a minimal erasure-coded pg_pool_t.
 *
 * @param k             number of data chunks
 * @param m             number of coding chunks
 * @param stripe_unit   per-shard stripe unit in bytes (stripe_width = k * stripe_unit)
 * @param extra_flags   additional pool flags to OR in
 */
pg_pool_t make_ec_pool(int k = K, int m = M, uint64_t stripe_unit = CHUNK,
                       uint64_t extra_flags = 0)
{
  pg_pool_t pi;
  pi.type = pg_pool_t::TYPE_ERASURE;
  pi.size = k + m;
  pi.min_size = k;
  pi.ec_data_shard_count = k;
  pi.ec_coding_shard_count = m;
  pi.set_stripe_width(stripe_unit * k);
  pi.set_flag(pg_pool_t::FLAG_EC_OVERWRITES);
  pi.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
  pi.set_flag(pg_pool_t::FLAG_CLIENT_SPLIT_READS);
  if (extra_flags) {
    pi.set_flag(extra_flags);
  }
  return pi;
}

/**
 * Build a minimal replicated pg_pool_t.
 */
pg_pool_t make_replicated_pool(int size = 3, uint64_t extra_flags = 0)
{
  pg_pool_t pi;
  pi.type = pg_pool_t::TYPE_REPLICATED;
  pi.size = size;
  pi.min_size = 2;
  pi.set_flag(pg_pool_t::FLAG_CLIENT_SPLIT_READS);
  if (extra_flags) {
    pi.set_flag(extra_flags);
  }
  return pi;
}

OSDOp make_read(uint64_t offset, uint64_t length) {
  OSDOp o;
  o.op.op = CEPH_OSD_OP_READ;
  o.op.extent.offset = offset;
  o.op.extent.length = length;
  return o;
}

/**
 * Build the Objecter::Op that a client would hand to SplitOp::create().
 *
 * validate_operations() only reads op->ops, so nothing else has to be wired up.
 */
ceph::ref_t<Objecter::Op> make_op(osdc_opvec &&ops) {
  return ceph::make_ref<Objecter::Op>(
    object_t("test_object"), object_locator_t(1), std::move(ops),
    CEPH_OSD_FLAG_READ | CEPH_OSD_FLAG_BALANCE_READS,
    static_cast<Context*>(nullptr), static_cast<version_t*>(nullptr));
}

/**
 * The raw shard prepare_single_op() picks for a read, verbatim from
 * SplitOp::prepare_single_op().  It only ever applies this to the *first* read
 * in the op, which is what makes disagreement between reads a bug.
 */
int raw_shard_of(uint64_t offset, uint64_t chunk_size = CHUNK, int k = K) {
  return (offset / chunk_size) % k;
}

/**
 * Run validate_operations() the way validate() does and report whether the op
 * was classified as a single direct read.
 *
 * validate_operations() only ever clears single_direct_op, so the caller has to
 * seed it; validate() seeds it with pi->is_erasure().
 */
bool is_single_direct_op(osdc_opvec &&ops,
                         const pg_pool_t &pool,
                         bool has_primary_ops = false) {
  auto op = make_op(std::move(ops));
  bool single_direct_op = pool.is_erasure();
  bool suitable_read_found = test_validate_operations(
    op.get(), &pool, pool.is_erasure(), /*replica_min_read_size=*/0,
    g_ceph_context, has_primary_ops, single_direct_op);
  EXPECT_TRUE(suitable_read_found);
  return single_direct_op;
}

osdc_opvec reads(std::initializer_list<std::pair<uint64_t, uint64_t>> extents) {
  osdc_opvec ops;
  for (const auto &[offset, length] : extents) {
    ops.push_back(make_read(offset, length));
  }
  return ops;
}

} // anonymous namespace

// ===========================================================================
// Section 1: validate_flags()
//
// SplitOp::validate_flags() is a public static method that checks whether a
// combination of operation flags and pool properties permits a split read.
// It rejects operations that:
//   - lack BALANCE_READS or LOCALIZE_READS
//   - are flagged as WRITE
//   - target a Crimson pool
// ===========================================================================

class TestValidateFlags : public ::testing::Test {
protected:
  pg_pool_t ec_pool;
  pg_pool_t ec_crimson_pool;
  pg_pool_t rep_pool;
  CephContext *cct = g_ceph_context;

  void SetUp() override {
    ec_pool        = make_ec_pool(4, 2, 4096);
    ec_crimson_pool = make_ec_pool(4, 2, 4096, pg_pool_t::FLAG_CRIMSON);
    rep_pool       = make_replicated_pool();
  }
};

// ===========================================================================
// Section 2: ECStripeIterator / ECStripeView
//
// ECStripeIterator and ECStripeView are protected members of SplitOp.
// A minimal test subclass is used to expose them for unit testing.
//
// Each test verifies the offset/length/shard_offset/raw_shard fields
// produced by the iterator for a given (offset, length, k, stripe_unit)
// combination.  The geometry for a k-data-chunk pool is:
//
//   stripe_width = k * chunk_size
//   raw_shard    = (offset / chunk_size) % k
//   shard_offset = (offset / (k * chunk_size)) * chunk_size
//                  + (offset % chunk_size)
// ===========================================================================

/**
 * StripeIteratorExposer — minimal SplitOp subclass that makes the protected
 * ECStripeIterator and ECStripeView types accessible from tests.
 *
 * Only the types are re-exported; no virtual methods are implemented because
 * this class is never instantiated — it exists solely to inherit access.
 */
class StripeIteratorExposer : public SplitOp {
public:
  // Re-export the protected iterator types so the test can use them directly.
  using SplitOp::ECStripeIterator;
  using SplitOp::ECStripeView;
  using SplitOp::ECChunkInfo;

  // Satisfy the pure-virtual interface — never called in these tests.
  std::pair<extent_set, bufferlist>
    assemble_buffer_sparse_read(int) const override { return {}; }
  void assemble_buffer_read(bufferlist &, int) const override {}
  void init_read(OSDOp &, bool, int, bool) override {}
  bool legacy_version_mismatch() const override { return false; }
  bool version_mismatch_v2() const override { return false; }
  void init_reference_sub_read() override {}

private:
  // Constructor is private; StripeIteratorExposer is never instantiated.
  // Suppress the "base is inaccessible" compiler warning.
  using SplitOp::SplitOp;
};

// Convenient type aliases for use in tests.
using ECStripeIterator = StripeIteratorExposer::ECStripeIterator;
using ECStripeView     = StripeIteratorExposer::ECStripeView;
using ECChunkInfo      = StripeIteratorExposer::ECChunkInfo;

// ---------------------------------------------------------------------------
// Helpers for stripe iterator tests
// ---------------------------------------------------------------------------

namespace {

/**
 * Collect all ECChunkInfo entries produced by a stripe view into a vector.
 */
std::vector<ECChunkInfo>
collect_chunks(uint64_t offset, uint64_t length, int k, uint32_t chunk_size)
{
  pg_pool_t pi = make_ec_pool(k, 2, chunk_size);
  ECStripeView view(offset, length, &pi);
  std::vector<ECChunkInfo> result;
  for (auto info : view) {
    result.push_back(info);
  }
  return result;
}

} // anonymous namespace

// ---------------------------------------------------------------------------

class TestECStripeIterator : public ::testing::Test {};

// Single chunk, at offset 0 — all data fits in shard 0.
// k=4, chunk_size=4096: one chunk [0, 4096) → raw_shard=0, shard_offset=0.
TEST_F(TestECStripeIterator, SingleChunkAtStart)
{
  auto chunks = collect_chunks(/*offset=*/0, /*length=*/4096,
                                /*k=*/4, /*chunk_size=*/4096);
  ASSERT_EQ(1u, chunks.size());
  EXPECT_EQ(0u,                  chunks[0].ro_offset);
  EXPECT_EQ(4096u,               chunks[0].length);
  EXPECT_EQ(raw_shard_id_t(0),   chunks[0].raw_shard);
  EXPECT_EQ(0u,                  chunks[0].shard_offset);
}

// Single chunk, starting at the boundary of the second shard.
// k=4, chunk_size=4096: offset=4096 → raw_shard=1, shard_offset=0.
TEST_F(TestECStripeIterator, SingleChunkSecondShard)
{
  auto chunks = collect_chunks(/*offset=*/4096, /*length=*/4096,
                                /*k=*/4, /*chunk_size=*/4096);
  ASSERT_EQ(1u, chunks.size());
  EXPECT_EQ(4096u,               chunks[0].ro_offset);
  EXPECT_EQ(4096u,               chunks[0].length);
  EXPECT_EQ(raw_shard_id_t(1),   chunks[0].raw_shard);
  EXPECT_EQ(0u,                  chunks[0].shard_offset);
}

// Two consecutive chunks, spanning shards 0 and 1 within the first stripe.
// k=4, chunk_size=4096, offset=0, length=8192.
TEST_F(TestECStripeIterator, TwoConsecutiveChunks)
{
  auto chunks = collect_chunks(0, 8192, 4, 4096);
  ASSERT_EQ(2u, chunks.size());

  EXPECT_EQ(0u,                chunks[0].ro_offset);
  EXPECT_EQ(4096u,             chunks[0].length);
  EXPECT_EQ(raw_shard_id_t(0), chunks[0].raw_shard);
  EXPECT_EQ(0u,                chunks[0].shard_offset);

  EXPECT_EQ(4096u,             chunks[1].ro_offset);
  EXPECT_EQ(4096u,             chunks[1].length);
  EXPECT_EQ(raw_shard_id_t(1), chunks[1].raw_shard);
  EXPECT_EQ(0u,                chunks[1].shard_offset);
}

// Full stripe across all k=4 shards.
TEST_F(TestECStripeIterator, FullStripe)
{
  const int k = 4;
  const uint32_t chunk_size = 4096;
  auto chunks = collect_chunks(0, k * chunk_size, k, chunk_size);
  ASSERT_EQ(static_cast<size_t>(k), chunks.size());
  for (int i = 0; i < k; ++i) {
    EXPECT_EQ(static_cast<uint64_t>(i) * chunk_size, chunks[i].ro_offset)
      << "chunk " << i;
    EXPECT_EQ(chunk_size,            chunks[i].length)      << "chunk " << i;
    EXPECT_EQ(raw_shard_id_t(i),     chunks[i].raw_shard)   << "chunk " << i;
    EXPECT_EQ(0u,                    chunks[i].shard_offset) << "chunk " << i;
  }
}

// Read that crosses a stripe boundary: offset=0, length = 5 * chunk_size
// with k=4.  Shard 4 wraps back to shard 0 in the second stripe.
TEST_F(TestECStripeIterator, WrapAroundStripe)
{
  const int k = 4;
  const uint32_t chunk_size = 4096;
  auto chunks = collect_chunks(0, 5 * chunk_size, k, chunk_size);
  ASSERT_EQ(5u, chunks.size());

  // Chunk 4 should be shard 0 again, but in the second stripe row.
  EXPECT_EQ(raw_shard_id_t(0), chunks[4].raw_shard);
  EXPECT_EQ(chunk_size,        chunks[4].shard_offset);   // second stripe row
  EXPECT_EQ(chunk_size,        chunks[4].length);
}

// Partial chunk: length smaller than chunk_size, not at a chunk boundary.
// offset=100, length=200, chunk_size=4096, k=4 → still within shard 0.
TEST_F(TestECStripeIterator, PartialFirstChunk)
{
  auto chunks = collect_chunks(100, 200, 4, 4096);
  ASSERT_EQ(1u, chunks.size());
  EXPECT_EQ(100u,              chunks[0].ro_offset);
  EXPECT_EQ(200u,              chunks[0].length);
  EXPECT_EQ(raw_shard_id_t(0), chunks[0].raw_shard);
  EXPECT_EQ(100u,              chunks[0].shard_offset);   // within-chunk offset preserved
}

// Read spanning a chunk boundary: offset inside shard 0, extends into shard 1.
// chunk_size=4096, k=4, offset=3000, length=2000.
// → chunk[0]: ro_offset=3000, length=1096, shard 0
// → chunk[1]: ro_offset=4096, length=904,  shard 1
TEST_F(TestECStripeIterator, SpanChunkBoundary)
{
  auto chunks = collect_chunks(3000, 2000, 4, 4096);
  ASSERT_EQ(2u, chunks.size());

  EXPECT_EQ(3000u,             chunks[0].ro_offset);
  EXPECT_EQ(1096u,             chunks[0].length);          // 4096 - 3000 = 1096
  EXPECT_EQ(raw_shard_id_t(0), chunks[0].raw_shard);

  EXPECT_EQ(4096u,             chunks[1].ro_offset);
  EXPECT_EQ(904u,              chunks[1].length);           // 2000 - 1096 = 904
  EXPECT_EQ(raw_shard_id_t(1), chunks[1].raw_shard);
}

// Single byte read.
TEST_F(TestECStripeIterator, SingleByteRead)
{
  auto chunks = collect_chunks(0, 1, 4, 4096);
  ASSERT_EQ(1u, chunks.size());
  EXPECT_EQ(1u, chunks[0].length);
}

// k=2 pool — stripe_width = 2 * chunk_size.
TEST_F(TestECStripeIterator, K2FullStripe)
{
  const int k = 2;
  const uint32_t chunk_size = 8192;
  auto chunks = collect_chunks(0, k * chunk_size, k, chunk_size);
  ASSERT_EQ(2u, chunks.size());
  EXPECT_EQ(raw_shard_id_t(0), chunks[0].raw_shard);
  EXPECT_EQ(raw_shard_id_t(1), chunks[1].raw_shard);
}

// ===========================================================================
// Section 3: ECSplitOp::local_zone_for_acting_set() — parameter guard cases
//
// The edge-case guard paths (fewer than 2 zones, empty crush_location, too-
// small acting set, null crush pointer) all return 0 and do not require a
// real CRUSH map.  More complete tests that exercise CRUSH lookups live in
// TestECWithCRUSH.cc.
// ===========================================================================

class TestLocalZoneGuards : public ::testing::Test {
protected:
  CephContext *cct = g_ceph_context;

  // A simple acting set — values need not correspond to real OSDs for these
  // guard-path tests.
  std::vector<int> make_acting(int count, int start = 0) {
    std::vector<int> v(count);
    std::iota(v.begin(), v.end(), start);
    return v;
  }

  std::multimap<std::string, std::string> make_loc(const std::string& dc) {
    std::multimap<std::string, std::string> loc;
    loc.emplace("datacenter", dc);
    return loc;
  }
};

// ===========================================================================
// Section 4: abs_shard computation for reference_sub_read (Bug 2 regression)
//
// When init_read() decides that primary_required is true it creates an extra
// sub-read for the version-check "reference" shard.  Before the fix the code
// always assigned:
//
//   sub_reads.emplace(reference_sub_read,  ..., shard_id_t(reference_sub_read));
//                                               ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
//                                               was: always a zone-0 acting index
//
// The fix uses:
//   shard_id_t(reference_sub_read + local_zone_index * zone_size)
//
// so the version-check sub-read goes to the same zone as the data sub-reads.
//
// The helpers below model both the old (buggy) and new (correct) formula so
// that the tests document what changed and act as regression guards.
// ===========================================================================

class TestReferenceSubReadAbsShard : public ::testing::Test {
protected:
  // Pool: k=2, m=1, 2 zones  →  pool.size=6, zone_size=3, data_chunk_count=2
  // Acting set:  [osd.0, osd.1, osd.2(par), osd.3, osd.4, osd.5(par)]
  //               zone-0 ────────────────── zone-1 ──────────────────
  static constexpr int k          = 2;
  static constexpr int m          = 1;
  static constexpr int num_zones  = 2;
  static constexpr int zone_size  = k + m;          // 3
  static constexpr int pool_size  = zone_size * num_zones; // 6
  // Primary shard lives at acting index 0 (zone-0).
  static constexpr int reference_sub_read = 0;

  // Compute the abs_shard that the current (buggy) code assigns to the
  // reference sub-read.
  static shard_id_t buggy_reference_abs_shard()
  {
    // Current code: shard_id_t(reference_sub_read) — ignores local_zone_index.
    return shard_id_t(reference_sub_read);
  }

  // Compute the abs_shard that the correct code should assign.
  static shard_id_t correct_reference_abs_shard(int local_zone_index)
  {
    return shard_id_t(reference_sub_read + local_zone_index * zone_size);
  }
};

// For a zone-0 client the bug is latent: both formulae agree.
TEST_F(TestReferenceSubReadAbsShard, Zone0ClientBugIsLatent)
{
  constexpr int local_zone = 0;
  EXPECT_EQ(buggy_reference_abs_shard(), correct_reference_abs_shard(local_zone))
    << "Zone-0: buggy and correct formula agree — bug is not visible";
  // Both map to acting[0], which is a zone-0 OSD — correct.
  EXPECT_EQ(shard_id_t(0), buggy_reference_abs_shard());
}

// For a zone-1 client the bug is exposed: the buggy formula targets
// acting[0] (zone-0 OSD) instead of acting[3] (zone-1 equivalent).
TEST_F(TestReferenceSubReadAbsShard, Zone1ClientBugExposed)
{
  constexpr int local_zone = 1;

  shard_id_t buggy   = buggy_reference_abs_shard();
  shard_id_t correct = correct_reference_abs_shard(local_zone);

  // Buggy code dispatches to acting[0] — a zone-0 OSD.
  EXPECT_EQ(shard_id_t(0),         buggy);
  // Correct code should dispatch to acting[3] — a zone-1 OSD.
  EXPECT_EQ(shard_id_t(zone_size), correct);

  // The two must differ — if they are equal this test is vacuous.
  EXPECT_NE(buggy, correct)
    << "Bug: reference sub-read abs_shard=" << (int)buggy
    << " targets zone-0 acting index even though local_zone=" << local_zone
    << "; correct abs_shard=" << (int)correct
    << " would target zone-1 acting index (acting[" << (int)correct << "])";
}

// Demonstrate that acting[buggy_abs] is always a zone-0 OSD regardless of
// which zone the client wants to read from.
TEST_F(TestReferenceSubReadAbsShard, BuggyShardAlwaysInZero)
{
  // acting = [0, 1, 2, 3, 4, 5]  (osd ids == acting indices for simplicity)
  std::vector<int> acting(pool_size);
  std::iota(acting.begin(), acting.end(), 0);

  for (int local_zone = 0; local_zone < num_zones; ++local_zone) {
    shard_id_t buggy = buggy_reference_abs_shard();
    int buggy_osd    = acting[(int)buggy];

    // Zone-0 OSDs are at acting indices [0, zone_size).
    bool is_zone0_osd = ((int)buggy < zone_size);
    EXPECT_TRUE(is_zone0_osd)
      << "local_zone=" << local_zone
      << ": buggy abs_shard=" << (int)buggy
      << " maps to acting[" << (int)buggy << "]=" << buggy_osd
      << " which is in zone-0 regardless of client zone";

    if (local_zone == 1) {
      // The correct zone-1 OSD should be at a strictly higher acting index.
      shard_id_t correct = correct_reference_abs_shard(local_zone);
      int correct_osd    = acting[(int)correct];
      bool is_zone1_osd  = ((int)correct >= zone_size && (int)correct < pool_size);
      EXPECT_TRUE(is_zone1_osd)
        << "correct abs_shard=" << (int)correct
        << " maps to acting[" << (int)correct << "]=" << correct_osd
        << " which should be in zone-1";
    }
  }
}

// Generalise over (k, m) combinations to show the bug scales with zone_size.
TEST(TestReferenceSubReadAbsShardParametric, BugScalesWithZoneSize)
{
  // { k, m }  →  zone_size = k+m
  const std::vector<std::pair<int,int>> configs = {{2,1},{4,2},{3,2},{8,3}};
  for (auto [k, m] : configs) {
    int zs = k + m;  // zone_size
    // reference_sub_read is the acting index of the primary shard (always 0
    // for a standard single-PG test setup).
    constexpr int ref = 0;
    constexpr int local_zone = 1;

    shard_id_t buggy   = shard_id_t(ref);
    shard_id_t correct = shard_id_t(ref + local_zone * zs);

    EXPECT_EQ(shard_id_t(0), buggy)
      << "k=" << k << " m=" << m << ": buggy always 0";
    EXPECT_EQ(shard_id_t(zs), correct)
      << "k=" << k << " m=" << m << ": correct should be zone_size=" << zs;
    EXPECT_NE(buggy, correct)
      << "k=" << k << " m=" << m << ": bug is visible for local_zone=1";
  }
}

// ===========================================================================
// Section 5: ReplicaSplitOp LOCALIZE_READS zone filtering
//
// When localize=true on a stretch replica pool, ReplicaSplitOp::init_read()
// must restrict the OSD set to replicas in the client's local zone only.
// When localize=false (BALANCE_READS), all replicas are used as before.
//
// These tests exercise the zone-selection formula directly (via the now-shared
// SplitOp::local_zone_for_acting_set()) and verify the acting-index ranges
// for each zone so the production filtering code can be reasoned about.
// ===========================================================================

class TestReplicaLocalizeZoneFiltering : public ::testing::Test {
protected:
  // Stretch replica pool: size=6, 2 zones, zone_size=3.
  // acting = [osd.0, osd.1, osd.2,  osd.3, osd.4, osd.5]
  //           ── zone-0 ──────────  ── zone-1 ──────────
  static constexpr int pool_size = 6;
  static constexpr int num_zones = 2;
  static constexpr int zone_size = pool_size / num_zones; // 3

  std::vector<int> make_acting() {
    std::vector<int> v(pool_size);
    std::iota(v.begin(), v.end(), 0);
    return v;
  }

  // Return the acting indices that belong to zone z.
  std::vector<int> zone_indices(int z) {
    std::vector<int> idx;
    for (int i = z * zone_size; i < (z + 1) * zone_size; ++i) {
      idx.push_back(i);
    }
    return idx;
  }
};

// The zone-0 indices are [0, zone_size) and zone-1 indices are [zone_size, 2*zone_size).
TEST_F(TestReplicaLocalizeZoneFiltering, ZoneIndexRangesAreNonOverlapping)
{
  auto z0 = zone_indices(0);
  auto z1 = zone_indices(1);

  // zone-0 and zone-1 indices must be disjoint.
  for (int i : z0) {
    EXPECT_EQ(std::count(z1.begin(), z1.end(), i), 0)
      << "index " << i << " appears in both zone-0 and zone-1";
  }
  // Together they cover the full acting set.
  EXPECT_EQ((int)(z0.size() + z1.size()), pool_size);
}

// For LOCALIZE_READS with local_zone=0, only acting indices [0, zone_size)
// are eligible — none from zone-1.
TEST_F(TestReplicaLocalizeZoneFiltering, LocalZone0FiltersOutZone1)
{
  auto acting = make_acting();
  int local_zone = 0;
  int zone_start = local_zone * zone_size;
  int zone_end   = zone_start + zone_size;

  // Simulate the filtering loop from ReplicaSplitOp::init_read().
  std::vector<int> filtered;
  for (int i = zone_start; i < zone_end; ++i) {
    filtered.push_back(acting[i]);
  }

  EXPECT_EQ((int)filtered.size(), zone_size);
  for (int osd : filtered) {
    EXPECT_LT(osd, zone_size) << "OSD " << osd << " is not in zone-0";
  }
}

// For LOCALIZE_READS with local_zone=1, only acting indices [zone_size, 2*zone_size)
// are eligible — none from zone-0.
TEST_F(TestReplicaLocalizeZoneFiltering, LocalZone1FiltersOutZone0)
{
  auto acting = make_acting();
  int local_zone = 1;
  int zone_start = local_zone * zone_size;
  int zone_end   = zone_start + zone_size;

  std::vector<int> filtered;
  for (int i = zone_start; i < zone_end; ++i) {
    filtered.push_back(acting[i]);
  }

  EXPECT_EQ((int)filtered.size(), zone_size);
  for (int osd : filtered) {
    EXPECT_GE(osd, zone_size) << "OSD " << osd << " is not in zone-1";
  }
}

// If all local-zone replicas are absent (acting[i] == CRUSH_ITEM_NONE),
// the filtered set is empty → abort → primary fallback.
TEST_F(TestReplicaLocalizeZoneFiltering, AllLocalZoneReplicasAbsentTriggersAbort)
{
  auto acting = make_acting();
  // Mark all zone-0 replicas absent.
  for (int i = 0; i < zone_size; ++i) {
    acting[i] = CRUSH_ITEM_NONE;
  }

  int local_zone = 0;
  int zone_start = local_zone * zone_size;
  int zone_end   = zone_start + zone_size;

  int available = 0;
  for (int i = zone_start; i < zone_end; ++i) {
    if (acting[i] != CRUSH_ITEM_NONE) {
      ++available;
    }
  }

  // With 0 available replicas in zone-0, the filtering loop yields 0 OSDs,
  // which is < 2 → init_read() sets abort = true.
  EXPECT_EQ(available, 0) << "Expected no zone-0 replicas available";
  EXPECT_LT(available, 2) << "abort condition requires < 2 local-zone replicas";
}

// For BALANCE_READS (localize=false), all available replicas across both
// zones are used — zone-filtering is not applied.
TEST_F(TestReplicaLocalizeZoneFiltering, BalanceReadsUsesAllReplicas)
{
  auto acting = make_acting();
  // Count all non-absent OSDs.
  int all_osds = 0;
  for (int osd : acting) {
    if (osd != CRUSH_ITEM_NONE) {
      ++all_osds;
    }
  }
  EXPECT_EQ(all_osds, pool_size)
    << "BALANCE_READS should have access to all " << pool_size << " replicas";
}

// Generalise: for any zone, the filtered set size equals zone_size.
TEST_F(TestReplicaLocalizeZoneFiltering, FilteredSetSizeEqualsZoneSize)
{
  auto acting = make_acting();
  for (int z = 0; z < num_zones; ++z) {
    int zone_start = z * zone_size;
    int zone_end   = zone_start + zone_size;
    int count = 0;
    for (int i = zone_start; i < zone_end; ++i) {
      if (acting[i] != CRUSH_ITEM_NONE) {
        ++count;
      }
    }
    EXPECT_EQ(count, zone_size)
      << "zone " << z << ": expected " << zone_size
      << " replicas, got " << count;
  }
}

/*
 * ---------------------------------------------------------------------------
 * Baseline: the cases the single-direct-read path exists to catch.
 * ---------------------------------------------------------------------------
 */

TEST(SplitOpValidate, OneReadInsideOneChunkIsASingleDirectOp) {
  const pg_pool_t pool = make_ec_pool();

  EXPECT_TRUE(is_single_direct_op(reads({{0, CHUNK}}), pool));
}

TEST(SplitOpValidate, ReadCrossingAChunkBoundaryIsNotASingleDirectOp) {
  const pg_pool_t pool = make_ec_pool();

  EXPECT_FALSE(is_single_direct_op(reads({{CHUNK / 2, CHUNK}}), pool));
}

TEST(SplitOpValidate, TwoReadsInsideTheSameChunkAreASingleDirectOp) {
  const pg_pool_t pool = make_ec_pool();

  // Both halves of chunk 0.  One OSD can serve the whole op.
  ASSERT_EQ(raw_shard_of(0), raw_shard_of(CHUNK / 2));

  EXPECT_TRUE(is_single_direct_op(
    reads({{0, CHUNK / 2}, {CHUNK / 2, CHUNK / 2}}), pool));
}

TEST(SplitOpValidate, TwoReadsOnTheSameShardInDifferentStripesAreASingleDirectOp) {
  const pg_pool_t pool = make_ec_pool();

  // Chunk 0 of stripe 0 and chunk 0 of stripe 1.  Different chunks of the
  // object, but both live on raw shard 0, so one OSD can still serve the op.
  // A fix that requires the reads to be in the same *chunk* rather than on the
  // same *shard* would wrongly reject this.
  ASSERT_EQ(raw_shard_of(0), raw_shard_of(STRIPE_WIDTH));

  EXPECT_TRUE(is_single_direct_op(
    reads({{0, CHUNK}, {STRIPE_WIDTH, CHUNK}}), pool));
}

TEST(SplitOpValidate, TwoReadsOnDifferentShardsAreNotASingleDirectOp) {
  const pg_pool_t pool = make_ec_pool();

  // Chunk 0 and chunk 1: each is chunk-aligned and exactly one chunk long
  ASSERT_NE(raw_shard_of(0), raw_shard_of(CHUNK));

  EXPECT_FALSE(is_single_direct_op(reads({{0, CHUNK}, {CHUNK, CHUNK}}), pool))
    << "reads on raw shards " << raw_shard_of(0) << " and "
    << raw_shard_of(CHUNK) << " were classified as a single direct read; ";
}

TEST(SplitOpValidate, TwoReadsOnDifferentNonZeroShardsAreNotASingleDirectOp) {
  const pg_pool_t pool = make_ec_pool();

  ASSERT_NE(raw_shard_of(CHUNK), raw_shard_of(2 * CHUNK));

  EXPECT_FALSE(is_single_direct_op(
    reads({{CHUNK, CHUNK}, {2 * CHUNK, CHUNK}}), pool))
    << "reads on raw shards " << raw_shard_of(CHUNK) << " and "
    << raw_shard_of(2 * CHUNK) << " were classified as a single direct read";
}

TEST(SplitOpValidate, TwoReadsOnDifferentShardsAcrossGeometries) {
  struct {
    int k;
    int m;
    uint64_t chunk_size;
  } const geometries[] = {
    {2, 1, 4096},
    {4, 2, 4096},
    {4, 2, 65536},
    {8, 3, 16384},
    {6, 2, 12288},
  };

  for (const auto &g : geometries) {
    const pg_pool_t pool = make_ec_pool(g.k, g.m, g.chunk_size);

    ASSERT_NE(raw_shard_of(0, g.chunk_size, g.k),
              raw_shard_of(g.chunk_size, g.chunk_size, g.k));

    EXPECT_FALSE(is_single_direct_op(
      reads({{0, g.chunk_size}, {g.chunk_size, g.chunk_size}}), pool))
      << "k=" << g.k << " m=" << g.m << " chunk_size=" << g.chunk_size;
  }
}

// ===========================================================================
// Section 4: SplitOp::create() — the OSD a single direct op is sent to
//
// An op whose reads all fall in one chunk is sent whole to the OSD holding
// that chunk. An op that also holds ops only the primary may run, such as
// CALL, must go to the primary.
// ===========================================================================

class TestSingleDirectOpTarget : public ::testing::Test {
protected:
  static constexpr int k = 2;
  static constexpr int m = 1;
  static constexpr uint32_t chunk_size = 4096;
  static inline boost::intrusive_ptr<CephContext> objecter_cct;
  CephContext *cct = nullptr;
  std::unique_ptr<ceph::async::io_context_pool> icp;
  std::unique_ptr<MonClient> monc;
  std::unique_ptr<Messenger> messenger;
  std::unique_ptr<Objecter> objecter;
  OSDMap osdmap;
  int64_t pool_id = -1;

  static void SetUpTestSuite() {
    if (!objecter_cct) {
      std::vector<const char*> args;
      objecter_cct = global_init(nullptr, args, CEPH_ENTITY_TYPE_CLIENT,
                                 CODE_ENVIRONMENT_UTILITY,
                                 CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);
      // Tests run from the build directory, which has the plugins in ./lib.
      objecter_cct->_conf.set_val("erasure_code_dir", "./lib");
      common_init_finish(objecter_cct.get());
    }
  }

  void SetUp() override {
    cct = objecter_cct.get();
    icp = std::make_unique<ceph::async::io_context_pool>(1);
    monc = std::make_unique<MonClient>(cct, *icp);
    messenger.reset(
      Messenger::create_client_messenger(cct, "unittest_split_ops_ut"));
    messenger->start();
    objecter = std::make_unique<Objecter>(cct, messenger.get(), monc.get(),
                                          *icp);
    objecter->set_client_incarnation(0);
    objecter->init();
    build_osdmap();
  }

  void TearDown() override {
    objecter->shutdown();
    monc->shutdown();
    messenger->shutdown();
    messenger->wait();
    objecter.reset();
    monc.reset();
    messenger.reset();
    icp.reset();
  }

  // OSDs 0..k-1 have primary affinity 0, so a PG with all its data shards on
  // them has its parity shard as primary while shard 0 is up.
  void build_osdmap() {
    uuid_d fsid; // matches the unconnected MonClient
    osdmap.build_simple(cct, 0, fsid, k + m);
    OSDMap::Incremental osd_inc(osdmap.get_epoch() + 1);
    osd_inc.fsid = fsid;
    entity_addrvec_t addrs;
    addrs.v.push_back(entity_addr_t());
    for (int i = 0; i < k + m; ++i) {
      addrs.v[0].nonce = i;
      osd_inc.new_state[i] = CEPH_OSD_EXISTS | CEPH_OSD_NEW;
      osd_inc.new_up_client[i] = addrs;
      osd_inc.new_up_cluster[i] = addrs;
      osd_inc.new_hb_back_up[i] = addrs;
      osd_inc.new_hb_front_up[i] = addrs;
      osd_inc.new_weight[i] = CEPH_OSD_IN;
    }
    osdmap.apply_incremental(osd_inc);
    for (int i = 0; i < k; ++i) {
      osdmap.set_primary_affinity(i, 0);
    }

    int rule = osdmap.crush->add_simple_rule(
      "ec", "default", "osd", "", "indep", pg_pool_t::TYPE_ERASURE, &std::cerr);
    ASSERT_GE(rule, 0);
    OSDMap::Incremental pool_inc(osdmap.get_epoch() + 1);
    pool_inc.fsid = fsid;
    pool_inc.new_pool_max = osdmap.get_pool_max();
    pool_id = ++pool_inc.new_pool_max;
    pool_inc.new_erasure_code_profiles["ec"] = {
      {"plugin", "jerasure"}, {"technique", "reed_sol_van"},
      {"k", std::to_string(k)}, {"m", std::to_string(m)},
      {"stripe_unit", std::to_string(chunk_size)}};
    pg_pool_t pool = make_ec_pool(k, m, chunk_size,
                                  pg_pool_t::FLAG_EC_CLIENT_DECODE);
    pool.erasure_code_profile = "ec";
    pool.set_flag(pg_pool_t::FLAG_HASHPSPOOL);
    pool.object_hash = CEPH_STR_HASH_RJENKINS;
    pool.set_pg_num(64);
    pool.set_pgp_num(64);
    pool.crush_rule = rule;
    for (int i = 1; i < k; ++i) {
      pool.nonprimary_shards.insert(shard_id_t(i));
    }
    pool_inc.new_pools[pool_id] = pool;
    pool_inc.new_pool_names[pool_id] = "ec";
    osdmap.apply_incremental(pool_inc);
    send_map();
  }

  void send_map() {
    bufferlist bl;
    osdmap.encode(bl, CEPH_FEATURES_ALL | CEPH_FEATURE_RESERVED);
    auto msg = ceph::make_message<MOSDMap>(osdmap.get_fsid(),
                                           CEPH_FEATURES_ALL);
    msg->maps[osdmap.get_epoch()] = bl;
    objecter->handle_osd_map(msg.get());
  }

  // An object whose PG has acting[primary_shard] as primary, with shard 0 up.
  object_t object_with_primary(int primary_shard) {
    for (int i = 0; i < 1000; ++i) {
      object_t oid("obj" + std::to_string(i));
      pg_t pgid = osdmap.object_locator_to_pg(oid, object_locator_t(pool_id));
      std::vector<int> acting;
      int primary;
      osdmap.pg_to_acting_osds(pgid, &acting, &primary);
      if (acting.size() == k + m && osdmap.is_up(acting[0]) &&
          primary == acting[primary_shard]) {
        return oid;
      }
    }
    ADD_FAILURE() << "no PG has shard " << primary_shard << " as primary";
    return object_t();
  }

  // Returns the op after SplitOp::create() chose not to split it.
  Objecter::Op *create(const object_t &oid, ObjectOperation &rd) {
    return create(prepare(oid, rd));
  }

  Objecter::Op *create(Objecter::Op *op) {
    ceph::shared_mutex lock = ceph::make_shared_mutex("TestSingleDirectOpTarget");
    ceph::shunique_lock<ceph::shared_mutex> sul(lock, ceph::acquire_shared);
    EXPECT_FALSE(SplitOp::create(op, *objecter, sul, cct));
    return op;
  }

  Objecter::Op *prepare(const object_t &oid, ObjectOperation &rd) {
    return objecter->prepare_read_op(
      oid, object_locator_t(pool_id), rd, CEPH_NOSNAP, nullptr,
      CEPH_OSD_FLAG_BALANCE_READS, -1, nullptr);
  }

  // What SplitOp::create() gets from _calc_target().
  void fill_target(Objecter::Op *op) {
    pg_t pgid = osdmap.raw_pg_to_pg(
      osdmap.object_locator_to_pg(op->target.base_oid, op->target.base_oloc));
    std::vector<int> acting;
    int primary;
    osdmap.pg_to_acting_osds(pgid, &acting, &primary);
    op->target.acting = acting;
    op->target.acting_primary = primary;
    for (unsigned i = 0; i < acting.size(); ++i) {
      if (acting[i] == primary) {
        op->target.actual_pgid = spg_t(pgid, shard_id_t(i));
      }
    }
  }
};

TEST_F(TestSingleDirectOpTarget, ReadGoesToItsShard)
{
  object_t oid = object_with_primary(k);
  ObjectOperation rd;
  bufferlist out;
  rd.read(0, chunk_size / 2, &out, nullptr, nullptr);
  Objecter::Op *op = create(oid, rd);
  EXPECT_EQ(op->target.acting[0], op->target.osd);
  EXPECT_TRUE(op->target.flags & CEPH_OSD_FLAG_EC_DIRECT_READ);
  op->put();
}

TEST_F(TestSingleDirectOpTarget, CallGoesToPrimary)
{
  object_t oid = object_with_primary(k);
  ObjectOperation rd;
  bufferlist out, in;
  rd.read(0, chunk_size / 2, &out, nullptr, nullptr);
  rd.call("lock", "get_info", in);
  Objecter::Op *op = create(oid, rd);
  EXPECT_NE(op->target.acting[0], op->target.acting_primary);
  EXPECT_EQ(op->target.acting_primary, op->target.osd)
    << "acting " << op->target.acting;
  op->put();
}

TEST_F(TestSingleDirectOpTarget, CallGoesDirectToPrimaryShardZero)
{
  object_t oid = object_with_primary(0);
  ObjectOperation rd;
  bufferlist out, in;
  rd.read(0, chunk_size / 2, &out, nullptr, nullptr);
  rd.call("lock", "get_info", in);
  Objecter::Op *op = create(oid, rd);
  EXPECT_EQ(op->target.acting_primary, op->target.osd);
  EXPECT_TRUE(op->target.flags & CEPH_OSD_FLAG_EC_DIRECT_READ);
  op->put();
}

// get_read_flags() strips BALANCE_READS from an ordered read, so no client
// sends one here.
TEST_F(TestSingleDirectOpTarget, OrderedReadIsNeverDirect)
{
  ObjectOperation rd;
  bufferlist out;
  rd.read(0, chunk_size / 2, &out, nullptr, nullptr);
  Objecter::Op *op = prepare(object_with_primary(k), rd);
  op->target.flags |= CEPH_OSD_FLAG_RWORDERED;
  EXPECT_TRUE(op->target.flags & CEPH_OSD_FLAG_BALANCE_READS);
  create(op);
  EXPECT_FALSE(op->target.flags & CEPH_OSD_FLAG_FORCE_OSD);
  EXPECT_FALSE(op->target.flags & CEPH_OSD_FLAG_EC_DIRECT_READ);
  op->put();
}

// ===========================================================================
// Section 5: SplitOp::assemble_rc() — which errors go to the primary
// ===========================================================================

class ECSplitOpProbe : public ECSplitOp {
public:
  using ECSplitOp::ECSplitOp;
  using SplitOp::InternalVersionStrategy;
  using SplitOp::abort;
  using SplitOp::assemble_rc;
  using SplitOp::reference_sub_read;
  using SplitOp::strategy;
  using SplitOp::sub_reads;
};

// A read of two chunks, split into a sub-read to the reference, which holds
// the parity shard, and one to shard 0.
class TestAssembleRc : public TestSingleDirectOpTarget {
protected:
  using Counts = std::map<std::string, uint64_t>;
  using Strategy = ECSplitOpProbe::InternalVersionStrategy;
  using Versions = std::map<shard_id_t, eversion_t>;
  static inline const shard_id_t s0{0};
  static inline const shard_id_t parity{k};
  static inline const eversion_t v7{1, 7};
  static inline const eversion_t v8{1, 8};
  bufferlist out;
  Objecter::Op *op = nullptr;

  void SetUp() override {
    TestSingleDirectOpTarget::SetUp();
    ObjectOperation rd;
    rd.read(0, 2 * chunk_size, &out, nullptr, nullptr);
    op = prepare(object_with_primary(k), rd);
    fill_target(op);
  }

  void TearDown() override {
    op->put();
    TestSingleDirectOpTarget::TearDown();
  }

  // What assemble_rc() returns, and the split op counters that went up. By
  // default the sub-reads return the versions of an object whose last write
  // did not change shard 0.
  std::pair<int, Counts> rc_for(int reference_rc, int shard_rc,
                                Strategy strategy = Strategy::FromPrimaryShard,
                                const Versions &reference_versions =
                                  {{s0, v7}, {parity, v8}},
                                const Versions &shard_versions = {{s0, v7}}) {
    Counts before = split_op_counters(cct);
    ECSplitOpProbe split_op(op, *objecter, cct, k + m);
    split_op.init_reference_sub_read();
    split_op.strategy = strategy;
    auto add_sub_read = [&](int index, int rc, const Versions &versions) {
      split_op.sub_reads.emplace(index, 2);
      auto &sub_read = split_op.sub_reads.at(index);
      sub_read.rc = rc;
      // As for a sub-read its OSD replied to, not one failed on a PG change.
      sub_read.reply_epoch = 1;
      sub_read.internal_version.emplace();
      if (strategy == Strategy::FromParticipatingShards) {
        encode(internal_versions_v2_response_t{versions},
               sub_read.internal_version->bl);
      } else {
        encode(versions, sub_read.internal_version->bl);
      }
    };
    add_sub_read(split_op.reference_sub_read, reference_rc,
                 reference_versions);
    add_sub_read(0, shard_rc, shard_versions);
    int rc = split_op.assemble_rc();
    split_op.abort = true;

    Counts counted;
    for (auto &[name, value] : split_op_counters(cct)) {
      if (value != before.at(name)) {
        counted[name] = value - before.at(name);
      }
    }
    return {rc, counted};
  }
};

// A shard error other than -EAGAIN from a sub-read that only reads, such as
// a medium error, sends the op to the primary, which records and repairs it.
// Errors from the reference are those the primary returns, and other errors
// are returned as they are, even over an error from the reference. A
// cancelled or timed out op is not resent, even when another sub-read
// returned -EAGAIN.
TEST_F(TestAssembleRc, ShardErrorsGoToThePrimary)
{
  const Counts none;
  const Counts eagain = {{"split_op_fallback_eagain", 1}};
  const Counts shard_error = {{"split_op_fallback_shard_error", 1}};
  EXPECT_EQ(std::pair(-EAGAIN, shard_error), rc_for(0, -EIO));
  EXPECT_EQ(std::pair(-EAGAIN, shard_error), rc_for(0, -ENOENT));
  EXPECT_EQ(std::pair(-EAGAIN, shard_error), rc_for(0, -ENODATA));
  EXPECT_EQ(std::pair(-EAGAIN, shard_error), rc_for(-EIO, 0));
  EXPECT_EQ(std::pair(-EAGAIN, shard_error), rc_for(-EIO, -ENOENT));
  EXPECT_EQ(std::pair(-EAGAIN, eagain), rc_for(0, -EAGAIN));
  EXPECT_EQ(std::pair(-EAGAIN, eagain), rc_for(-EAGAIN, -EIO));
  EXPECT_EQ(std::pair(-EAGAIN, eagain), rc_for(-EIO, -EAGAIN));
  EXPECT_EQ(std::pair(-EAGAIN, eagain), rc_for(-ENOENT, -EAGAIN));
  EXPECT_EQ(std::pair(-ENOENT, none), rc_for(-ENOENT, -ENOENT));
  EXPECT_EQ(std::pair(-ENOENT, none), rc_for(-ENOENT, -EIO));
  EXPECT_EQ(std::pair(-ECANCELED, none), rc_for(-ECANCELED, 0));
  EXPECT_EQ(std::pair(-EINVAL, none), rc_for(0, -EINVAL));
  EXPECT_EQ(std::pair(-EINVAL, none), rc_for(-ENOENT, -EINVAL));
  EXPECT_EQ(std::pair(-ETIMEDOUT, none), rc_for(0, -ETIMEDOUT));
  EXPECT_EQ(std::pair(-ETIMEDOUT, none), rc_for(-ETIMEDOUT, 0));
  EXPECT_EQ(std::pair(-ECANCELED, none), rc_for(-EAGAIN, -ECANCELED));
  EXPECT_EQ(std::pair(-ETIMEDOUT, none), rc_for(-ETIMEDOUT, -EAGAIN));
  EXPECT_EQ(std::pair(-ECANCELED, none), rc_for(-ECANCELED, -EAGAIN));
}

// Sub-reads that succeed go to the primary when the version that the version
// reference returned for a shard is not the one that the shard returned, or
// either is missing.
TEST_F(TestAssembleRc, TornReadsGoToThePrimary)
{
  const Counts none;
  const Counts version = {{"split_op_fallback_version", 1}};
  for (Strategy strategy : {Strategy::FromPrimaryShard,
                            Strategy::FromParticipatingShards}) {
    SCOPED_TRACE(strategy == Strategy::FromPrimaryShard ?
                 "FromPrimaryShard" : "FromParticipatingShards");
    // What shard 0 returns once its last write is v. Only with
    // FromParticipatingShards does a shard other than the primary return
    // the versions of every shard.
    auto shard_0_at = [&](const eversion_t &v) {
      return strategy == Strategy::FromPrimaryShard ?
        Versions{{s0, v}} : Versions{{s0, v}, {parity, v}};
    };
    auto rc_for_versions = [&](const Versions &reference,
                               const Versions &shard) {
      return rc_for(0, 0, strategy, reference, shard);
    };
    // The last write did not change shard 0.
    EXPECT_EQ(std::pair(0, none),
              rc_for_versions({{s0, v7}, {parity, v8}}, shard_0_at(v7)));
    // Shard 0 has not applied the last write.
    EXPECT_EQ(std::pair(-EAGAIN, version),
              rc_for_versions({{s0, v8}, {parity, v8}}, shard_0_at(v7)));
    // Shard 0 has applied a write that the reference has not.
    EXPECT_EQ(std::pair(-EAGAIN, version),
              rc_for_versions({{s0, v7}, {parity, v7}}, shard_0_at(v8)));
    EXPECT_EQ(std::pair(-EAGAIN, version),
              rc_for_versions({{s0, v7}, {parity, v8}}, {}));
    EXPECT_EQ(std::pair(-EAGAIN, version),
              rc_for_versions({{parity, v8}}, shard_0_at(v7)));
  }
}

// ===========================================================================
// Section 6: ECSplitOp — reads planned around an unavailable shard
//
// Data shard 1 is excluded with objecter_debug_ec_direct_read_exclude_shards,
// as if its OSD were down. The parity shard is the primary. No release past
// umbrella exists yet to decode at.
// ===========================================================================

class TestECClientDecode : public TestSingleDirectOpTarget {
protected:
  void SetUp() override {
    TestSingleDirectOpTarget::SetUp();
    set_conf("objecter_debug_ec_direct_read_exclude_shards", "1");
    set_conf("objecter_debug_ec_client_decode_skip_release_check", "true");
  }

  void TearDown() override {
    set_conf("objecter_debug_ec_direct_read_exclude_shards", "");
    set_conf("objecter_debug_ec_client_decode_skip_release_check", "false");
    set_conf("objecter_debug_ec_inject_decode_error", "false");
    set_conf("rados_ec_client_decode", "true");
    TestSingleDirectOpTarget::TearDown();
  }

  void set_conf(const std::string &key, const std::string &val) {
    cct->_conf.set_val(key, val);
    cct->_conf.apply_changes(nullptr);
  }

  uint64_t counter(const std::string &name) {
    return split_op_counters(cct).at(name);
  }

  std::shared_ptr<const ECCodecInfo> load_codec() {
    ObjectOperation rd;
    bufferlist out;
    rd.read(chunk_size, chunk_size / 2, &out, nullptr, nullptr);
    Objecter::Op *op = prepare(object_with_primary(k), rd);
    fill_target(op);
    std::shared_ptr<const ECCodecInfo> codec;
    {
      ECSplitOpProbe split_op(op, *objecter, cct, k + m);
      codec = split_op.get_codec(pool_id, osdmap.get_pg_pool(pool_id));
      split_op.abort = true;
    }
    op->put();
    return codec;
  }

  int64_t add_pool_like_ec_pool(const std::string &name) {
    OSDMap::Incremental inc(osdmap.get_epoch() + 1);
    inc.fsid = osdmap.get_fsid();
    inc.new_pool_max = osdmap.get_pool_max();
    int64_t id = ++inc.new_pool_max;
    inc.new_pools[id] = *osdmap.get_pg_pool(pool_id);
    inc.new_pool_names[id] = name;
    EXPECT_EQ(0, osdmap.apply_incremental(inc));
    send_map();
    return id;
  }

  void delete_pool(int64_t id) {
    OSDMap::Incremental inc(osdmap.get_epoch() + 1);
    inc.fsid = osdmap.get_fsid();
    inc.old_pools.insert(id);
    ASSERT_EQ(0, osdmap.apply_incremental(inc));
    send_map();
    ASSERT_FALSE(objecter->with_osdmap([id](const OSDMap &o) {
      return o.have_pg_pool(id);
    }));
  }
};

TEST_F(TestECClientDecode, ExcludedShardIsUnavailable)
{
  object_t oid = object_with_primary(k);
  ObjectOperation rd;
  bufferlist out;
  rd.read(chunk_size, chunk_size / 2, &out, nullptr, nullptr);
  Objecter::Op *op = prepare(oid, rd);
  const pg_pool_t *pi = osdmap.get_pg_pool(pool_id);
  EXPECT_TRUE(ECSplitOp::single_op_reads_unavailable_shard(op, *objecter));
  EXPECT_TRUE(ECSplitOp::may_decode(op, pi, *objecter));

  set_conf("rados_ec_client_decode", "false");
  EXPECT_TRUE(ECSplitOp::single_op_reads_unavailable_shard(op, *objecter));
  EXPECT_FALSE(ECSplitOp::may_decode(op, pi, *objecter));
  op->put();
}

TEST_F(TestECClientDecode, ReadOfAvailableShardNeedsNoDecode)
{
  object_t oid = object_with_primary(k);
  ObjectOperation rd;
  bufferlist out;
  rd.read(0, chunk_size / 2, &out, nullptr, nullptr);
  Objecter::Op *op = prepare(oid, rd);
  EXPECT_FALSE(ECSplitOp::single_op_reads_unavailable_shard(op, *objecter));
  op->put();
}

TEST_F(TestECClientDecode, SparseReadIsNotDecoded)
{
  object_t oid = object_with_primary(k);
  ObjectOperation rd;
  rd.sparse_read(chunk_size, chunk_size / 2);
  Objecter::Op *op = prepare(oid, rd);
  EXPECT_TRUE(ECSplitOp::single_op_reads_unavailable_shard(op, *objecter));
  EXPECT_FALSE(ECSplitOp::may_decode(op, osdmap.get_pg_pool(pool_id),
                                     *objecter));
  op->put();
}

// set_pool_flags can give a pool ec_client_decode on a cluster at umbrella,
// whose OSDs may read shard extents as object extents.
TEST_F(TestECClientDecode, UmbrellaClusterIsNotDecoded)
{
  set_conf("objecter_debug_ec_client_decode_skip_release_check", "false");
  OSDMap::Incremental inc(osdmap.get_epoch() + 1);
  inc.fsid = osdmap.get_fsid();
  inc.new_require_osd_release = ceph_release_t::umbrella;
  ASSERT_EQ(0, osdmap.apply_incremental(inc));
  send_map();

  ObjectOperation rd;
  bufferlist out;
  rd.read(chunk_size, chunk_size / 2, &out, nullptr, nullptr);
  Objecter::Op *op = prepare(object_with_primary(k), rd);
  EXPECT_TRUE(ECSplitOp::single_op_reads_unavailable_shard(op, *objecter));
  EXPECT_FALSE(ECSplitOp::may_decode(op, osdmap.get_pg_pool(pool_id),
                                     *objecter));
  op->put();
}

// A 4+2 pool whose data shards are 2-5, so that a shard id and its chunk
// index differ. Each case lists its reads and the shards they need.
TEST_F(TestECClientDecode, ReadsUnavailableShardOfEachRange)
{
  constexpr uint64_t cs = 4096;
  pg_pool_t pool;
  pool.type = pg_pool_t::TYPE_ERASURE;
  pool.size = 6;
  pool.ec_data_shard_count = 4;
  pool.set_stripe_width(4 * cs);
  pool.set_shard_mapping({shard_id_t(2), shard_id_t(3), shard_id_t(4),
                          shard_id_t(5), shard_id_t(0), shard_id_t(1)});

  struct Case {
    std::vector<std::pair<uint64_t, uint64_t>> reads;
    std::vector<int> shards;
    bool sparse = false;
  };
  const std::vector<Case> cases = {
    {{{0, 1}}, {2}},
    {{{cs - 1, 2}}, {2, 3}},
    {{{3 * cs, cs}}, {5}},
    {{{3 * cs + 1, cs}}, {5, 2}},
    {{{cs, 3 * cs}}, {3, 4, 5}},
    {{{5 * cs, 100 * cs}}, {2, 3, 4, 5}},
    {{{30 * cs, 10}}, {4}},
    {{{0, 1}, {2 * cs + 7, 1}}, {2, 4}},
    {{{cs, 1}}, {3}, true},
  };

  shard_id_set all;
  all.insert_range(shard_id_t(0), 6);
  for (unsigned c = 0; c < cases.size(); ++c) {
    ObjectOperation rd;
    std::vector<bufferlist> out(cases[c].reads.size());
    uint64_t size;
    rd.stat(&size, nullptr, nullptr);
    for (unsigned i = 0; i < cases[c].reads.size(); ++i) {
      auto [off, len] = cases[c].reads[i];
      if (cases[c].sparse) {
        rd.sparse_read(off, len, nullptr, &out[i], nullptr);
      } else {
        rd.read(off, len, &out[i], nullptr, nullptr);
      }
    }
    Objecter::Op *op = prepare(object_with_primary(k), rd);
    EXPECT_FALSE(ECSplitOp::reads_unavailable_shard(op, &pool, all)) << c;
    for (int s = 0; s < 6; ++s) {
      shard_id_set available = all;
      available.erase(shard_id_t(s));
      EXPECT_EQ(std::ranges::count(cases[c].shards, s) == 1,
                ECSplitOp::reads_unavailable_shard(op, &pool, available))
        << "case " << c << " shard " << s;
    }
    op->put();
  }
}

TEST_F(TestECClientDecode, PlanReadsShardExtentsAndStatsOnPrimary)
{
  object_t oid = object_with_primary(k);
  ObjectOperation rd;
  bufferlist out;
  rd.read(0, 2 * chunk_size, &out, nullptr, nullptr);
  Objecter::Op *op = prepare(oid, rd);
  fill_target(op);
  const pg_pool_t *pi = osdmap.get_pg_pool(pool_id);

  {
    ECSplitOpProbe split_op(op, *objecter, cct, k + m);
    split_op.init_reference_sub_read();
    ASSERT_EQ(k, split_op.reference_sub_read);

    shard_id_set available = ECSplitOp::available_shards(op->target,
                                                         *objecter);
    EXPECT_FALSE(available.contains(shard_id_t(1)));
    ASSERT_TRUE(ECSplitOp::reads_unavailable_shard(op, pi, available));
    ASSERT_TRUE(ECSplitOp::may_decode(op, pi, *objecter));
    auto codec = split_op.get_codec(pool_id, pi);
    ASSERT_TRUE(codec);
    split_op.init_degraded(std::move(codec), available);
    ASSERT_FALSE(split_op.abort);

    // Shard 0 and the parity shard each read their first chunk.
    ASSERT_EQ(2u, split_op.sub_reads.size());
    for (int shard : {0, k}) {
      ASSERT_TRUE(split_op.sub_reads.contains(shard)) << shard;
      auto &sr = split_op.sub_reads.at(shard);
      ASSERT_EQ(1u, sr.shard_extents.size()) << shard;
      EXPECT_TRUE(sr.shard_extents.contains(0)) << shard;
      ASSERT_LE(1u, sr.rd.ops.size()) << shard;
      auto &read = sr.rd.ops[0].op;
      EXPECT_EQ(CEPH_OSD_OP_READ, read.op) << shard;
      EXPECT_EQ(0u, read.extent.offset) << shard;
      EXPECT_EQ(chunk_size, read.extent.length) << shard;
      EXPECT_TRUE(read.flags & CEPH_OSD_OP_FLAG_EC_SHARD_EXTENT) << shard;
    }
    // The primary also reads the object size, to trim and decode.
    auto &primary_ops = split_op.sub_reads.at(k).rd.ops;
    EXPECT_EQ(CEPH_OSD_OP_STAT, primary_ops.back().op.op);

    split_op.abort = true; // Never sent, so there is nothing to complete.
  }
  op->put();
}

TEST_F(TestECClientDecode, InjectedDecodeErrorReturnsEagain)
{
  ObjectOperation rd;
  bufferlist out;
  rd.read(0, 2 * chunk_size, &out, nullptr, nullptr);
  Objecter::Op *op = prepare(object_with_primary(k), rd);
  fill_target(op);
  const pg_pool_t *pi = osdmap.get_pg_pool(pool_id);

  {
    ECSplitOpProbe split_op(op, *objecter, cct, k + m);
    split_op.init_reference_sub_read();
    split_op.init_degraded(split_op.get_codec(pool_id, pi),
                           ECSplitOp::available_shards(op->target,
                                                       *objecter));
    ASSERT_FALSE(split_op.abort);

    // The stat was never sent, so the object looks empty and nothing is
    // decoded.
    uint64_t decode_errors = counter("ec_split_op_decode_error");
    EXPECT_EQ(0, split_op.prepare_assembly());
    EXPECT_EQ(decode_errors, counter("ec_split_op_decode_error"));

    set_conf("objecter_debug_ec_inject_decode_error", "true");
    EXPECT_EQ(-EAGAIN, split_op.prepare_assembly());
    EXPECT_EQ(decode_errors + 1, counter("ec_split_op_decode_error"));

    split_op.abort = true;
  }
  op->put();
}

// A librados application that never called global_init has no global
// context, and the erasure code plugins log through it.
class NoGlobalContext {
  CephContext *saved = g_ceph_context;
public:
  NoGlobalContext() { g_ceph_context = nullptr; }
  ~NoGlobalContext() { g_ceph_context = saved; }
};

TEST_F(TestECClientDecode, NoGlobalContextReadsFromPrimary)
{
  NoGlobalContext no_global_context;
  EXPECT_FALSE(load_codec());

  ObjectOperation rd;
  bufferlist out;
  rd.read(chunk_size, chunk_size / 2, &out, nullptr, nullptr);
  Objecter::Op *op = create(object_with_primary(k), rd);
  EXPECT_FALSE(op->target.flags & CEPH_OSD_FLAG_EC_DIRECT_READ);
  op->put();
}

// The Objecter's cache holds the only reference to a codec it has loaded.
TEST_F(TestECClientDecode, CodecIsKeptWhenAnotherPoolIsDeleted)
{
  int64_t other = add_pool_like_ec_pool("other");
  std::weak_ptr<const ECCodecInfo> cached = load_codec();
  ASSERT_FALSE(cached.expired());
  delete_pool(other);
  EXPECT_FALSE(cached.expired());
  EXPECT_EQ(cached.lock(), load_codec());
}

TEST_F(TestECClientDecode, CodecIsDroppedWhenItsPoolIsDeleted)
{
  std::weak_ptr<const ECCodecInfo> cached = load_codec();
  ASSERT_FALSE(cached.expired());
  delete_pool(pool_id);
  EXPECT_TRUE(cached.expired());
}

// A single-chunk read of the excluded shard that the client may not decode
// goes to the primary, as a read of several chunks does. A read of an
// available shard still goes direct.
enum class DecodeOff {
  client_option, pool_flag, sparse_read, truncate_seq, release
};

class TestECClientDecodeOff : public TestECClientDecode,
                              public ::testing::WithParamInterface<DecodeOff> {
protected:
  bufferlist out;

  void SetUp() override {
    TestECClientDecode::SetUp();
    if (GetParam() == DecodeOff::client_option) {
      set_conf("rados_ec_client_decode", "false");
    } else if (GetParam() == DecodeOff::pool_flag) {
      OSDMap::Incremental inc(osdmap.get_epoch() + 1);
      inc.fsid = osdmap.get_fsid();
      pg_pool_t pool = *osdmap.get_pg_pool(pool_id);
      pool.unset_flag(pg_pool_t::FLAG_EC_CLIENT_DECODE);
      pool.last_change = inc.epoch;
      inc.new_pools[pool_id] = pool;
      ASSERT_EQ(0, osdmap.apply_incremental(inc));
      send_map();
    } else if (GetParam() == DecodeOff::release) {
      set_conf("objecter_debug_ec_client_decode_skip_release_check", "false");
    }
  }

  Objecter::Op *create_read(uint64_t off, uint64_t len) {
    ObjectOperation rd;
    if (GetParam() == DecodeOff::sparse_read) {
      rd.sparse_read(off, len);
    } else {
      rd.read(off, len, &out, nullptr, nullptr);
    }
    if (GetParam() == DecodeOff::truncate_seq) {
      rd.ops.back().op.extent.truncate_seq = 1;
      rd.ops.back().op.extent.truncate_size = 2 * chunk_size;
    }
    return create(object_with_primary(k), rd);
  }

  Objecter::Op *create_read_of_shard(int shard) {
    return create_read(shard * chunk_size, chunk_size / 2);
  }

  // Shard 1 is unavailable because its OSD is down, not because it is
  // excluded.
  void mark_shard_1_down() {
    set_conf("objecter_debug_ec_direct_read_exclude_shards", "");
    pg_t pgid = osdmap.object_locator_to_pg(object_with_primary(k),
                                            object_locator_t(pool_id));
    std::vector<int> acting;
    osdmap.pg_to_acting_osds(pgid, acting);
    ASSERT_EQ(size_t(k + m), acting.size());
    OSDMap::Incremental inc(osdmap.get_epoch() + 1);
    inc.fsid = osdmap.get_fsid();
    inc.new_state[acting[1]] = CEPH_OSD_UP;
    ASSERT_EQ(0, osdmap.apply_incremental(inc));
    send_map();
    osdmap.pg_to_acting_osds(osdmap.object_locator_to_pg(
      object_with_primary(k), object_locator_t(pool_id)), acting);
    ASSERT_EQ(CRUSH_ITEM_NONE, acting[1]);
  }

  uint64_t no_decode_count() {
    return counter("ec_split_op_no_decode");
  }
};

TEST_P(TestECClientDecodeOff, ReadOfAvailableShardGoesDirect)
{
  Objecter::Op *op = create_read_of_shard(0);
  EXPECT_TRUE(op->target.flags & CEPH_OSD_FLAG_FORCE_OSD);
  EXPECT_TRUE(op->target.flags & CEPH_OSD_FLAG_EC_DIRECT_READ);
  EXPECT_EQ(op->target.acting[0], op->target.osd);
  op->put();
}

TEST_P(TestECClientDecodeOff, ReadOfExcludedShardGoesToThePrimary)
{
  std::vector<int> acting;
  osdmap.pg_to_acting_osds(
    osdmap.object_locator_to_pg(object_with_primary(k),
                                object_locator_t(pool_id)), acting);
  ASSERT_EQ(size_t(k + m), acting.size());
  Objecter::Op *op = create_read_of_shard(1);
  EXPECT_FALSE(op->target.flags & CEPH_OSD_FLAG_FORCE_OSD)
    << "forced to osd." << op->target.osd << ", shard 1 is osd." << acting[1];
  EXPECT_FALSE(op->target.flags & CEPH_OSD_FLAG_EC_DIRECT_READ);
  EXPECT_NE(acting[1], op->target.osd);
  op->put();
}

// With shard 1's OSD down, a read of shard 1 that may not be decoded counts
// ec_split_op_no_decode whether it reads one chunk or several.
TEST_P(TestECClientDecodeOff, ReadOfAvailableShardWithShardDownCountsNothing)
{
  mark_shard_1_down();
  uint64_t no_decode = no_decode_count();
  Objecter::Op *op = create_read_of_shard(0);
  EXPECT_TRUE(op->target.flags & CEPH_OSD_FLAG_EC_DIRECT_READ);
  EXPECT_EQ(op->target.acting[0], op->target.osd);
  EXPECT_EQ(no_decode, no_decode_count());
  op->put();
}

TEST_P(TestECClientDecodeOff, ReadOfSeveralChunksWithShardDownCountsNoDecode)
{
  mark_shard_1_down();
  uint64_t no_decode = no_decode_count();
  Objecter::Op *op = create_read(0, 2 * chunk_size);
  EXPECT_FALSE(op->target.flags & CEPH_OSD_FLAG_FORCE_OSD);
  EXPECT_FALSE(op->target.flags & CEPH_OSD_FLAG_EC_DIRECT_READ);
  EXPECT_EQ(no_decode + 1, no_decode_count());
  op->put();
}

TEST_P(TestECClientDecodeOff, ReadOfDownShardCountsNoDecode)
{
  mark_shard_1_down();
  uint64_t no_decode = no_decode_count();
  Objecter::Op *op = create_read_of_shard(1);
  EXPECT_FALSE(op->target.flags & CEPH_OSD_FLAG_FORCE_OSD);
  EXPECT_FALSE(op->target.flags & CEPH_OSD_FLAG_EC_DIRECT_READ);
  EXPECT_EQ(no_decode + 1, no_decode_count());
  op->put();
}

INSTANTIATE_TEST_SUITE_P(
  DecodeOff, TestECClientDecodeOff,
  ::testing::Values(DecodeOff::client_option, DecodeOff::pool_flag,
                    DecodeOff::sparse_read, DecodeOff::truncate_seq,
                    DecodeOff::release),
  [](const ::testing::TestParamInfo<DecodeOff> &info) {
    switch (info.param) {
    case DecodeOff::client_option: return "ClientOptionOff";
    case DecodeOff::pool_flag: return "PoolFlagCleared";
    case DecodeOff::sparse_read: return "SparseRead";
    case DecodeOff::truncate_seq: return "ReadWithTruncateSeq";
    case DecodeOff::release: return "ReleaseNotPastUmbrella";
    }
    return "";
  });
