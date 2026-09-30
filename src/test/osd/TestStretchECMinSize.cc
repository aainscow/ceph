// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2026 IBM
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Library Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library Public License for more details.
 *
 */

/**
 * Unit tests for two per-zone min_size helpers on OSDMap:
 *
 *   at_least_one_zone_has_min_size(pool, acting)
 *     Returns true if ANY zone in the CRUSH topology has >= pool.min_size
 *     acting OSDs.
 *
 *   stretch_ec_num_acting_below_min_size(pool, acting)
 *     Returns the total per-zone deficit: sum over all zones of
 *     max(0, min_size - zone_acting_count).  Returns 0 for non-stretch or
 *     non-erasure pools.
 *
 * Topology used by all tests
 * ─────────────────────────
 *   root "default"
 *     datacenter "dc0"  (type 9): OSDs 0, 1, 2, 6
 *     datacenter "dc1"  (type 9): OSDs 3, 4, 5, 7
 *
 *   pool: erasure, size=8, min_size=2
 */

#include <gtest/gtest.h>
#include "test/osd/OSDMapTestHelpers.h"
#include "osd/OSDMap.h"
#include "osd/osd_types.h"
#include "crush/CrushWrapper.h"
#include "crush/crush.h"

using namespace std;

static std::shared_ptr<OSDMap> make_stretch_ec_osdmap()
{
  auto osdmap = std::make_shared<OSDMap>();
  osdmap->set_max_osd(8);
  for (int i = 0; i < 8; ++i) {    
    osdmap->set_state(i, CEPH_OSD_EXISTS | CEPH_OSD_UP);
  }
  osdmap->set_epoch(1);

  CrushWrapper crush;
  crush.create();
  crush.set_type_name(10, "root");
  crush.set_type_name(9,  "datacenter");
  crush.set_type_name(1,  "host");
  crush.set_type_name(0,  "osd");

  int root_id;
  crush.add_bucket(0, CRUSH_BUCKET_STRAW2, CRUSH_HASH_RJENKINS1,
                   10, 0, nullptr, nullptr, &root_id);
  crush.set_item_name(root_id, "default");

    // Insert OSDs with location hierarchy
    // dc0: OSDs 0,1,2,6 in hosts host0, host1, host2, host6
    // dc1: OSDs 3,4,5,7 in hosts host3, host4, host5, host7
  for (int dc = 0; dc < 2; ++dc) {
    std::string dc_name = (dc == 0) ? "dc0" : "dc1";
    for (int h = 0; h < 4; ++h) {
      int osd_id = (dc == 0) ? (h < 3 ? h : 6) : (h < 3 ? h + 3 : 7);
      std::map<std::string, std::string> loc;
      loc["root"]       = "default";
      loc["datacenter"] = dc_name;
      loc["host"]       = "host" + std::to_string(osd_id);
      crush.insert_item(g_ceph_context, osd_id, 1.0,
                        "osd." + std::to_string(osd_id), loc);
    }
  }

  // CRUSH rule: choose 2 datacenters, chooseleaf indep 3 hosts
  int rule_id = 0;
  int steps = 6;
  crush_rule *rule = crush_make_rule(steps, pg_pool_t::TYPE_ERASURE);
  int step = 0;
  crush_rule_set_step(rule, step++, CRUSH_RULE_SET_CHOOSELEAF_TRIES, 5, 0);
  crush_rule_set_step(rule, step++, CRUSH_RULE_SET_CHOOSE_TRIES, 100, 0);
  crush_rule_set_step(rule, step++, CRUSH_RULE_TAKE, root_id, 0);
  crush_rule_set_step(rule, step++, CRUSH_RULE_CHOOSE_INDEP, 2, 9 /* datacenter */);
  crush_rule_set_step(rule, step++, CRUSH_RULE_CHOOSELEAF_INDEP, 3, 1 /* host */);
  crush_rule_set_step(rule, step++, CRUSH_RULE_EMIT, 0, 0);
  ceph_assert(step == steps);
  int r = crush_add_rule(crush.get_crush_map(), rule, rule_id);
  ceph_assert(r >= 0);
  crush.set_rule_name(rule_id, "stretch_ec_rule");

  OSDMap::Incremental inc(2);
  inc.fsid = osdmap->get_fsid();
  crush.encode(inc.crush, CEPH_FEATURES_SUPPORTED_DEFAULT);
  osdmap->apply_incremental(inc);

  int64_t pool_id = 1;
  pg_pool_t pool;
  pool.type     = pg_pool_t::TYPE_ERASURE;
  pool.size     = 6;
  pool.min_size = 2;
  pool.crush_rule = rule_id;
  pool.set_pg_num(8);
  pool.set_pgp_num(8);
  pool.set_flag(pg_pool_t::FLAG_EC_OVERWRITES);
  pool.peering_crush_bucket_barrier = 9; // datacenter
  pool.peering_crush_bucket_target  = 2;
  pool.peering_crush_bucket_count   = 2;
  pool.peering_crush_mandatory_member = CRUSH_ITEM_NONE;

  OSDMapTestHelpers::add_pool(osdmap, pool_id, pool, "test_ec_pool");
  return osdmap;
}

class StretchECMinSizeTest : public ::testing::Test {
protected:
  std::shared_ptr<OSDMap> osdmap;
  const pg_pool_t *pool = nullptr;

  void SetUp() override {
    osdmap = make_stretch_ec_osdmap();
    pool = osdmap->get_pg_pool(1);
    ASSERT_NE(pool, nullptr);
  }
};

  // ===========================================================================
  // at_least_one_zone_has_min_size
  // ===========================================================================

// Both zones fully populated - both exceed min_size
TEST_F(StretchECMinSizeTest, ZoneHasMinSize_BothZonesHealthy)
{
  vector<int> acting = {0, 1, 2, 3, 4, 5}; // dc0: 3 OSDs, dc1: 3 OSDs
  EXPECT_TRUE(osdmap->at_least_one_zone_has_min_size(*pool, acting));
}

// dc0 full, dc1 completely absent - at least one zone qualifies
TEST_F(StretchECMinSizeTest, ZoneHasMinSize_OnlyOneDCPresent)
{
  vector<int> acting = {0, 1, 2,
                        CRUSH_ITEM_NONE, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE};
  EXPECT_TRUE(osdmap->at_least_one_zone_has_min_size(*pool, acting));
}

// dc0 exactly at min_size (2), dc1 has only 1 - dc0 still qualifies
TEST_F(StretchECMinSizeTest, ZoneHasMinSize_OneZoneExactlyAtMinSize)
{
  vector<int> acting = {0, 1, CRUSH_ITEM_NONE,   // dc0: 2 = min_size
                        3, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE}; // dc1: 1 < min_size
  EXPECT_TRUE(osdmap->at_least_one_zone_has_min_size(*pool, acting));
}

// Both zones have only 1 OSD each (< min_size=2)
TEST_F(StretchECMinSizeTest, ZoneHasMinSize_BothZonesBelowMinSize)
{
  vector<int> acting = {0, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE,
                        3, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE};
  EXPECT_FALSE(osdmap->at_least_one_zone_has_min_size(*pool, acting));
}

// Completely empty acting set - no zone qualifies
TEST_F(StretchECMinSizeTest, ZoneHasMinSize_EmptyActingSet)
{
  vector<int> acting(6, CRUSH_ITEM_NONE);
  EXPECT_FALSE(osdmap->at_least_one_zone_has_min_size(*pool, acting));
}

// ===========================================================================
// stretch_ec_num_acting_below_min_size
// ===========================================================================

// All zones fully populated
TEST_F(StretchECMinSizeTest, NumActingBelowMinSize_AllZonesHealthy)
{
  vector<int> acting = {0, 1, 2, 3, 4, 5};
  EXPECT_EQ(0u, osdmap->stretch_ec_num_acting_below_min_size(*pool, acting));
}

// One zone exactly at min_size
TEST_F(StretchECMinSizeTest, NumActingBelowMinSize_OneZoneAtMinSize)
{
  vector<int> acting = {0, 1, CRUSH_ITEM_NONE,   // dc0: 2 = min_size
                        3, 4, 5};                 // dc1: 3
  EXPECT_EQ(0u, osdmap->stretch_ec_num_acting_below_min_size(*pool, acting));
}

// dc0 has 1 OSD (< min_size=2) - deficit 1; dc1 OK
TEST_F(StretchECMinSizeTest, NumActingBelowMinSize_OneZoneBelowMinSize)
{
  vector<int> acting = {0, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE,
                        3, 4, 5};
  EXPECT_EQ(1u, osdmap->stretch_ec_num_acting_below_min_size(*pool, acting));
}

// Both zones have 1 OSD each - deficit 1+1 = 2
TEST_F(StretchECMinSizeTest, NumActingBelowMinSize_BothZonesBelowMinSize)
{
  vector<int> acting = {0, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE,
                        3, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE};
  EXPECT_EQ(2u, osdmap->stretch_ec_num_acting_below_min_size(*pool, acting));
}

// dc1 completely empty - deficit = min_size = 2
TEST_F(StretchECMinSizeTest, NumActingBelowMinSize_OneZoneCompletelyEmpty)
{
  vector<int> acting = {0, 1, 2,
                        CRUSH_ITEM_NONE, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE};
  EXPECT_EQ(2u, osdmap->stretch_ec_num_acting_below_min_size(*pool, acting));
}

// Both zones empty - deficit = 2 * min_size = 4
TEST_F(StretchECMinSizeTest, NumActingBelowMinSize_BothZonesEmpty)
{
  vector<int> acting(6, CRUSH_ITEM_NONE);
  EXPECT_EQ(4u, osdmap->stretch_ec_num_acting_below_min_size(*pool, acting));
}

// Non-stretch pool - always 0
TEST_F(StretchECMinSizeTest, NumActingBelowMinSize_NonStretchPool)
{
  pg_pool_t non_stretch = *pool;
  non_stretch.peering_crush_bucket_count = 0;
  vector<int> acting = {0, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE,
                        3, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE};
  EXPECT_EQ(0u, osdmap->stretch_ec_num_acting_below_min_size(non_stretch, acting));
}

// Non-erasure pool - always 0
TEST_F(StretchECMinSizeTest, NumActingBelowMinSize_ReplicatedPool)
{
  pg_pool_t rep = *pool;
  rep.type = pg_pool_t::TYPE_REPLICATED;
  vector<int> acting = {0, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE,
                        3, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE};
  EXPECT_EQ(0u, osdmap->stretch_ec_num_acting_below_min_size(rep, acting));
}

// ===========================================================================
// get_osd_zone
// ===========================================================================

class GetOsdZoneTest : public ::testing::Test {
protected:
  std::shared_ptr<OSDMap> osdmap;
  const pg_pool_t *pool = nullptr;
  int dc0_id = 0;
  int dc1_id = 0;

  void SetUp() override {
    osdmap = make_stretch_ec_osdmap();
    pool = osdmap->get_pg_pool(1);
    ASSERT_NE(pool, nullptr);
    dc0_id = osdmap->crush->get_item_id("dc0");
    dc1_id = osdmap->crush->get_item_id("dc1");
    ASSERT_LT(dc0_id, 0); // CRUSH bucket IDs are negative
    ASSERT_LT(dc1_id, 0);
  }
};

// Every OSD in dc0 resolves to dc0's bucket ID
TEST_F(GetOsdZoneTest, DC0OSDs_MapToDC0)
{
  for (int osd : {0, 1, 2, 6}) {
    EXPECT_EQ(dc0_id,
              osdmap->get_osd_zone(osd, pool->crush_rule,
                                   pool->peering_crush_bucket_barrier))
      << "OSD " << osd << " should resolve to dc0";
  }
}

// Every OSD in dc1 resolves to dc1's bucket ID
TEST_F(GetOsdZoneTest, DC1OSDs_MapToDC1)
{
  for (int osd : {3, 4, 5, 7}) {
    EXPECT_EQ(dc1_id,
              osdmap->get_osd_zone(osd, pool->crush_rule,
                                   pool->peering_crush_bucket_barrier))
      << "OSD " << osd << " should resolve to dc1";
  }
}

// An OSD that doesn't exist in the CRUSH map returns 0
TEST_F(GetOsdZoneTest, UnknownOSD_ReturnsZero)
{
  EXPECT_EQ(0, osdmap->get_osd_zone(99, pool->crush_rule,
                                     pool->peering_crush_bucket_barrier));
}

// dc0 and dc1 resolve to distinct bucket IDs
TEST_F(GetOsdZoneTest, TwoZones_DistinctIDs)
{
  EXPECT_NE(dc0_id, dc1_id);
  EXPECT_NE(osdmap->get_osd_zone(0, pool->crush_rule,
                                  pool->peering_crush_bucket_barrier),
            osdmap->get_osd_zone(3, pool->crush_rule,
                                  pool->peering_crush_bucket_barrier));
}

// ===========================================================================
// stretch_set_can_peer
// ===========================================================================

class StretchSetCanPeerTest : public ::testing::Test {
protected:
  std::shared_ptr<OSDMap> osdmap;
  const pg_pool_t *pool = nullptr;

  void SetUp() override {
    osdmap = make_stretch_ec_osdmap();
    pool = osdmap->get_pg_pool(1);
    ASSERT_NE(pool, nullptr);
  }
};

// Both DCs represented — can peer
TEST_F(StretchSetCanPeerTest, BothDCsPresent_CanPeer)
{
  set<int> want = {0, 1, 3, 4};
  EXPECT_TRUE(pool->stretch_set_can_peer(want, *osdmap, nullptr));
}

// Only dc0 OSDs — cannot satisfy barrier_count=2
TEST_F(StretchSetCanPeerTest, OnlyOneDC_CannotPeer)
{
  set<int> want = {0, 1, 2};
  EXPECT_FALSE(pool->stretch_set_can_peer(want, *osdmap, nullptr));
}

// Empty want set — cannot peer
TEST_F(StretchSetCanPeerTest, EmptyWant_CannotPeer)
{
  set<int> want;
  EXPECT_FALSE(pool->stretch_set_can_peer(want, *osdmap, nullptr));
}

// CRUSH_ITEM_NONE entries are skipped; remaining OSDs span both zones
TEST_F(StretchSetCanPeerTest, CrushItemNone_Ignored)
{
  set<int> want = {0, CRUSH_ITEM_NONE, 3, CRUSH_ITEM_NONE};
  EXPECT_TRUE(pool->stretch_set_can_peer(want, *osdmap, nullptr));
}

// vector overload — both DCs present
TEST_F(StretchSetCanPeerTest, VectorOverload_BothDCs_CanPeer)
{
  vector<int> want = {0, 1, 3, 4};
  EXPECT_TRUE(pool->stretch_set_can_peer(want, *osdmap, nullptr));
}

// vector overload — only dc0
TEST_F(StretchSetCanPeerTest, VectorOverload_OneDC_CannotPeer)
{
  vector<int> want = {0, 1, 2, CRUSH_ITEM_NONE};
  EXPECT_FALSE(pool->stretch_set_can_peer(want, *osdmap, nullptr));
}

// Non-stretch pool always returns true regardless of want set
TEST_F(StretchSetCanPeerTest, NonStretchPool_AlwaysTrue)
{
  pg_pool_t non_stretch = *pool;
  non_stretch.peering_crush_bucket_count = 0;
  set<int> want = {0}; // would fail for a real stretch pool
  EXPECT_TRUE(non_stretch.stretch_set_can_peer(want, *osdmap, nullptr));
}

// Mandatory member (dc0) is absent from want — fails even though both
// zone count is satisfied via dc1 duplicates (not possible in 2-DC, but
// the mandatory check runs after the count check)
TEST_F(StretchSetCanPeerTest, MandatoryMember_Absent_CannotPeer)
{
  int dc0_id = osdmap->crush->get_item_id("dc0");
  ASSERT_LT(dc0_id, 0);

  pg_pool_t with_mandatory = *pool;
  with_mandatory.peering_crush_mandatory_member = dc0_id;

  // Only dc1 OSDs — dc0 (mandatory) missing
  set<int> want = {3, 4, 5};
  EXPECT_FALSE(with_mandatory.stretch_set_can_peer(want, *osdmap, nullptr));
}

// Mandatory member (dc0) is present — both zones covered, passes
TEST_F(StretchSetCanPeerTest, MandatoryMember_Present_CanPeer)
{
  int dc0_id = osdmap->crush->get_item_id("dc0");
  ASSERT_LT(dc0_id, 0);

  pg_pool_t with_mandatory = *pool;
  with_mandatory.peering_crush_mandatory_member = dc0_id;

  set<int> want = {0, 3}; // dc0 (mandatory) + dc1
  EXPECT_TRUE(with_mandatory.stretch_set_can_peer(want, *osdmap, nullptr));
}

// ===========================================================================
// StretchZoneCache — correctness and invalidation
// ===========================================================================

class StretchZoneCacheTest : public ::testing::Test {
protected:
  std::shared_ptr<OSDMap> osdmap;
  const pg_pool_t *pool = nullptr;

  void SetUp() override {
    osdmap = make_stretch_ec_osdmap();
    pool = osdmap->get_pg_pool(1);
    ASSERT_NE(pool, nullptr);
  }
};

// Calling the same helper twice on the same map gives identical results.
// This exercises the cache hit path on the second call.
TEST_F(StretchZoneCacheTest, RepeatedCalls_SameResult)
{
  vector<int> acting = {0, 1, CRUSH_ITEM_NONE, 3, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE};

  unsigned r1 = osdmap->stretch_ec_num_acting_below_min_size(*pool, acting);
  unsigned r2 = osdmap->stretch_ec_num_acting_below_min_size(*pool, acting);
  EXPECT_EQ(r1, r2);

  bool b1 = osdmap->at_least_one_zone_has_min_size(*pool, acting);
  bool b2 = osdmap->at_least_one_zone_has_min_size(*pool, acting);
  EXPECT_EQ(b1, b2);
}

// Different acting sets on the same map (same cache entry) give correct
// independent results — the cache stores zone membership, not per-call state.
TEST_F(StretchZoneCacheTest, DifferentActingSets_IndependentResults)
{
  vector<int> full    = {0, 1, 2, 3, 4, 5};
  vector<int> dc0only = {0, 1, 2, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE};
  vector<int> dc1only = {CRUSH_ITEM_NONE, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE, 3, 4, 5};

  EXPECT_EQ(0u, osdmap->stretch_ec_num_acting_below_min_size(*pool, full));
  EXPECT_EQ(2u, osdmap->stretch_ec_num_acting_below_min_size(*pool, dc0only));
  EXPECT_EQ(2u, osdmap->stretch_ec_num_acting_below_min_size(*pool, dc1only));

  // Re-query full — confirms the cache wasn't corrupted by interleaved calls
  EXPECT_EQ(0u, osdmap->stretch_ec_num_acting_below_min_size(*pool, full));
}

// apply_incremental (pool min_size change) must invalidate the cache.
// The helpers must reflect the new pool parameters, not the stale cached ones.
TEST_F(StretchZoneCacheTest, CacheInvalidated_ByPoolChange)
{
  vector<int> acting = {0, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE,
                         3, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE};

  // Warm the cache: min_size=2, 1 OSD per zone -> deficit 1+1=2
  EXPECT_EQ(2u, osdmap->stretch_ec_num_acting_below_min_size(*pool, acting));

  // Advance the map with a new min_size=3
  OSDMapTestHelpers::set_pool_min_size(*osdmap, 1, 3);

  // Pool pointer is now stale after apply_incremental; re-fetch
  pool = osdmap->get_pg_pool(1);
  ASSERT_NE(pool, nullptr);
  ASSERT_EQ(3u, pool->min_size);

  // With min_size=3 and 1 OSD per zone: deficit = (3-1)+(3-1) = 4
  EXPECT_EQ(4u, osdmap->stretch_ec_num_acting_below_min_size(*pool, acting));
}

// apply_incremental must also invalidate at_least_one_zone_has_min_size.
TEST_F(StretchZoneCacheTest, CacheInvalidated_ZoneHasMinSize_AfterPoolChange)
{
  // With min_size=2: a zone with 2 OSDs qualifies
  vector<int> acting = {0, 1, CRUSH_ITEM_NONE,
                         3, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE};
  EXPECT_TRUE(osdmap->at_least_one_zone_has_min_size(*pool, acting));

  // Raise min_size to 4 — now 2 OSDs per zone is no longer enough
  OSDMapTestHelpers::set_pool_min_size(*osdmap, 1, 4);
  pool = osdmap->get_pg_pool(1);
  ASSERT_NE(pool, nullptr);

  EXPECT_FALSE(osdmap->at_least_one_zone_has_min_size(*pool, acting));
}

// deepish_copy_from must not carry the source's cache to the destination.
// After deepish_copy_from + apply_incremental, the copy must compute fresh
// results reflecting its own (potentially different) pool parameters.
TEST_F(StretchZoneCacheTest, CacheNotCarried_AcrossDeepishCopy)
{
  vector<int> acting = {0, 1, 2, 3, 4, 5};

  // Warm the cache in the source (deficit = 0, all zones healthy)
  EXPECT_EQ(0u, osdmap->stretch_ec_num_acting_below_min_size(*pool, acting));

  // Build a copy at the next epoch (no topology change, just epoch advance)
  OSDMap copy;
  copy.deepish_copy_from(*osdmap);
  OSDMap::Incremental inc(copy.get_epoch() + 1);
  inc.fsid = copy.get_fsid();
  copy.apply_incremental(inc);

  const pg_pool_t *copy_pool = copy.get_pg_pool(1);
  ASSERT_NE(copy_pool, nullptr);

  // Full acting set should still give 0 on the copy
  EXPECT_EQ(0u, copy.stretch_ec_num_acting_below_min_size(*copy_pool, acting));

  // Partial acting set should give the correct non-zero deficit on the copy,
  // not a stale 0 from the source's warmed cache
  vector<int> partial = {0, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE,
                          3, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE};
  EXPECT_EQ(2u, copy.stretch_ec_num_acting_below_min_size(*copy_pool, partial));
}

// deepish_copy_from + incremental that changes min_size: copy uses updated
// parameters, not the source's min_size that was in the cache.
TEST_F(StretchZoneCacheTest, CacheNotCarried_CopyWithDifferentMinSize)
{
  vector<int> acting = {0, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE,
                         3, CRUSH_ITEM_NONE, CRUSH_ITEM_NONE};

  // Source: min_size=2, warm the cache -> deficit 2
  EXPECT_EQ(2u, osdmap->stretch_ec_num_acting_below_min_size(*pool, acting));

  // Build a copy that immediately raises min_size to 3
  OSDMap copy;
  copy.deepish_copy_from(*osdmap);

  pg_pool_t updated_pool = *copy.get_pg_pool(1);
  updated_pool.min_size = 3;
  OSDMap::Incremental inc(copy.get_epoch() + 1);
  inc.fsid = copy.get_fsid();
  inc.new_pools[1] = updated_pool;
  copy.apply_incremental(inc);

  const pg_pool_t *copy_pool = copy.get_pg_pool(1);
  ASSERT_NE(copy_pool, nullptr);
  ASSERT_EQ(3u, copy_pool->min_size);

  // With min_size=3 on the copy: deficit = (3-1)+(3-1) = 4, not the
  // source's cached deficit of 2
  EXPECT_EQ(4u, copy.stretch_ec_num_acting_below_min_size(*copy_pool, acting));

  // Source must be unaffected
  EXPECT_EQ(2u, osdmap->stretch_ec_num_acting_below_min_size(*pool, acting));
}
