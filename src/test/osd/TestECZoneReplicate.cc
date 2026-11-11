 // -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include <gtest/gtest.h>
#include <algorithm>
#include <tuple>
#include "test/osd/ECPeeringTestFixture.h"
#include "test/osd/OSDMapTestHelpers.h"
#include "test/osd/TestCommon.h"
#include "messages/MOSDECSubOpWrite.h"
#include "messages/MOSDECZoneReplicate.h"
#include "msg/Message.h"
#include "crush/CrushWrapper.h"
#include "include/types.h"

using namespace std;

/**
 * TestECZoneReplicate — Unit tests for the-replicate transaction write path
 *
 * These tests verify the sending and handling of MOSDECZoneReplicate messages
 * Test Configuration:
 * - 2 zones (datacenters), 3 hosts per zone, 1 OSD per host = 6 OSDs total
 * - Stretched 2+1 EC: Each zone has complete stripe [shard.0, shard.1, shard.2]
 * - Acting set: 6 OSDs (3 per zone) with one zone primary per zone
 *
 */
class TestECZoneReplicate : public ECPeeringTestFixture {
public:
  const int SHARDS_PER_ZONE = 3;
  TestECZoneReplicate() {
    k = 2;
    m = 1;
    stripe_unit = 4096;
    ec_plugin = "isa";
    ec_technique = "reed_sol_van";
    pool_flags = pg_pool_t::FLAG_EC_OVERWRITES | pg_pool_t::FLAG_EC_OPTIMIZATIONS | pg_pool_t::FLAG_ZONE_REPLICATE;
    num_zones = 2;
  }

protected:
  /**
   * Install a proper 2-datacenter CRUSH hierarchy
   *
   * Topology (type 9 = datacenter, type 10 = root):
   *   root "default"
   *     ├─ datacenter "dc0": osd.0 (host0), osd.1 (host1), osd.2 (host2)
   *     └─ datacenter "dc1": osd.3 (host3), osd.4 (host4), osd.5 (host5)
   *
   * CRUSH rule:
   *   take default
   *   choose indep 2 type datacenter
   *   chooseleaf indep 3 type host
   *   emit
   */
  void pre_peering_hook() override {
    const int num_osds = num_zones * (k + m);

    CrushWrapper new_crush;
    new_crush.create();
    new_crush.set_type_name(10, "root");
    new_crush.set_type_name(9,  "datacenter");
    new_crush.set_type_name(1,  "host");
    new_crush.set_type_name(0,  "osd");

    // Create root bucket.
    int root_id;
    new_crush.add_bucket(0, CRUSH_BUCKET_STRAW2, CRUSH_HASH_RJENKINS1,
                         10 /*type*/, 0, NULL, NULL, &root_id);
    new_crush.set_item_name(root_id, "default");

    // Insert OSDs into their datacenter and host.
    // dc0: OSDs 0..SHARDS_PER_ZONE-1   dc1: OSDs SHARDS_PER_ZONE..num_osds-1
    for (int osd_id = 0; osd_id < num_osds; osd_id++) {
      int dc          = osd_id / SHARDS_PER_ZONE;
      std::string dc_name   = "dc" + std::to_string(dc);
      std::string host_name = "host" + std::to_string(osd_id);
      std::map<std::string, std::string> loc;
      loc["root"]       = "default";
      loc["datacenter"] = dc_name;
      loc["host"]       = host_name;
      new_crush.insert_item(g_ceph_context, osd_id, 1.0,
                            "osd." + std::to_string(osd_id), loc);
    }

    // Add an indep EC stretch rule:
    //   take default → choose indep num_zones datacenters →
    //   chooseleaf indep SHARDS_PER_ZONE hosts → emit
    std::stringstream ss;
    int r = new_crush.add_simple_stretch_rule(
      "ec_zone_rule", "default",
      "datacenter", "host",
      num_zones, SHARDS_PER_ZONE,
      "", "indep", pg_pool_t::TYPE_ERASURE, false, &ss);
    ceph_assert(r >= 0);
    new_crush.finalize();

    // Apply the new CRUSH map.
    {
      OSDMap::Incremental crush_inc(osdmap->get_epoch() + 1);
      crush_inc.fsid = osdmap->get_fsid();
      new_crush.encode(crush_inc.crush, CEPH_FEATURES_SUPPORTED_DEFAULT);
      osdmap->apply_incremental(crush_inc);
    }

    // Update the pool: set crush_rule and peering_crush_buckets
    {
      const pg_pool_t* existing = osdmap->get_pg_pool(pool_id);
      ceph_assert(existing != nullptr);
      pg_pool_t updated = *existing;
      updated.crush_rule                    = r;
      updated.peering_crush_bucket_barrier  = 9;
      updated.peering_crush_bucket_count    = static_cast<uint32_t>(num_zones);
      updated.peering_crush_bucket_target   = static_cast<uint32_t>(num_zones);
      updated.peering_crush_mandatory_member = CRUSH_ITEM_NONE;

      OSDMap::Incremental pool_inc(osdmap->get_epoch() + 1);
      pool_inc.fsid = osdmap->get_fsid();
      pool_inc.new_pools[pool_id] = updated;
      osdmap->apply_incremental(pool_inc);
    }
  }

  // Collect outgoing messages from the cluster Primary listener partitioned by message type.
  struct PrimaryMessages {
    int zone_replicate_count = 0;
    int sub_op_write_count   = 0;
    // OSD destinations of each MOSDECZoneReplicate
    vector<int> zone_replicate_dests;
    // OSD destinations of each MOSDECSubOpWrite
    vector<int> sub_op_write_dests;
  };

  PrimaryMessages capture_primary_messages() {
    auto* listener = get_primary_listener();
    EXPECT_TRUE(listener != nullptr);
    PrimaryMessages result;
    if (!listener) return result;
    for (auto& [osd, mref] : listener->sent_messages_with_dest) {
      if (mref->get_type() == MSG_OSD_EC_ZONE_REPLICATE) {
        result.zone_replicate_count++;
        result.zone_replicate_dests.push_back(osd);
      } else if (mref->get_type() == MSG_OSD_EC_WRITE) {
        result.sub_op_write_count++;
        result.sub_op_write_dests.push_back(osd);
      }
    }
    return result;
  }

  void clear_primary_messages() {
    get_primary_listener()->sent_messages_with_dest.clear();
  }

  void restore_remote_zone() {
    for (int osd = SHARDS_PER_ZONE; osd < 2 * SHARDS_PER_ZONE; ++osd) {
      mark_osd_up(osd);
    }
    enter_recovery_stretch_mode();
  }

  void expect_matching_versions(const string& obj_name) {
    for (int shard = SHARDS_PER_ZONE;
         shard < num_zones * SHARDS_PER_ZONE; ++shard) {
      SCOPED_TRACE(shard);
      const int relative = shard % SHARDS_PER_ZONE;
      auto* source = find_test_pg_for_shard(relative);
      ASSERT_NE(source, nullptr);
      ceph::buffer::ptr expected_value;
      const int expected_result =
        get_osd_fixture(source->pg_whoami.osd)->store->getattr(
          source->ch,
          ghobject_t(make_test_object(obj_name), ghobject_t::NO_GEN,
                     shard_id_t(relative)),
          OI_ATTR, expected_value);
      ASSERT_TRUE(expected_result >= 0 || expected_result == -ENOENT);
      auto* pg = find_test_pg_for_shard(shard);
      ASSERT_NE(pg, nullptr);
      ceph::buffer::ptr value;
      const int result = get_osd_fixture(pg->pg_whoami.osd)->store->getattr(
        pg->ch,
        ghobject_t(make_test_object(obj_name), ghobject_t::NO_GEN,
                   shard_id_t(shard)),
        OI_ATTR, value);
      ASSERT_EQ(result, expected_result);
      if (result == -ENOENT) {
        continue;
      }
      bufferlist expected_bl;
      expected_bl.append(expected_value);
      auto expected_p = expected_bl.cbegin();
      object_info_t expected;
      expected.decode(expected_p);
      bufferlist bl;
      bl.append(value);
      auto p = bl.cbegin();
      object_info_t oi;
      oi.decode(p);
      EXPECT_EQ(oi.version, expected.version);
      EXPECT_EQ(oi.size, expected.size);
    }
  }
};

class TestECZoneReplicateThreeZones : public TestECZoneReplicate {
public:
  TestECZoneReplicateThreeZones() {
    num_zones = 3;
  }

protected:
  void pre_peering_hook() override {
    TestECZoneReplicate::pre_peering_hook();
    auto pool = *osdmap->get_pg_pool(pool_id);
    // Keep two required zones while testing a third zone's recovery.
    pool.peering_crush_bucket_count = 2;
    OSDMap::Incremental inc(osdmap->get_epoch() + 1);
    inc.fsid = osdmap->get_fsid();
    inc.new_pools[pool_id] = pool;
    osdmap->apply_incremental(inc);
  }
};

class TestECZoneReplicateRecovery
  : public TestECZoneReplicate,
    public testing::WithParamInterface<tuple<bool, bool>> {
public:
  void SetUp() override {
    if (!get<0>(GetParam())) {
      pool_flags &= ~pg_pool_t::FLAG_ZONE_REPLICATE;
    }
    TestECZoneReplicate::SetUp();
  }
};

// TestECNonStretch fixture — single-zone EC pool (num_zones=1)
//
// Same k/m as above but no zone stretch.  Used to verify the non-stretch
// path is unaffected.
class TestECNonStretch : public ECPeeringTestFixture {
public:
  TestECNonStretch() : ECPeeringTestFixture() {
    k = 2;
    m = 1;
    stripe_unit = 4096;
    ec_plugin = "isa";
    ec_technique = "reed_sol_van";
    pool_flags = pg_pool_t::FLAG_EC_OVERWRITES | pg_pool_t::FLAG_EC_OPTIMIZATIONS;
    num_zones = 1;
  }

  void SetUp() override {
    ECPeeringTestFixture::SetUp();
  }
};

// Test: ZoneReplicateMsgSentOnWrite
//
// Verify that a write to a 2-zone stretch EC pool causes the cluster Primary
// to send exactly one MOSDECZoneReplicate to the zone-1 Zone Primary (OSD 3),
// and zero MOSDECSubOpWrite messages to zone-1 shards (OSDs 3, 4, 5).
// The Primary should still send MOSDECSubOpWrite to zone-0 peers (OSDs 1, 2).
TEST_F(TestECZoneReplicate, ZoneReplicateMsgSentOnWrite) {
  ASSERT_TRUE(all_shards_active());

  // Verify zone_primaries is populated (precondition for the new path).
  auto* primary_ps = get_primary_test_pg()->get_peering_state();
  std::cerr << "zone_primaries size=" << primary_ps->get_zone_primaries().size() << std::endl;
  for (auto& [zone, shard] : primary_ps->get_zone_primaries()) {
    std::cerr << "  zone=" << zone << " -> osd=" << shard.osd
              << " shard=" << (int)shard.shard.id << std::endl;
  }
  std::cerr << "num_zones from sinfo="
            << get_primary_test_pg()->get_backend()->ec_get_sinfo().get_num_zones()
            << std::endl;
  ASSERT_FALSE(primary_ps->get_zone_primaries().empty());
  ASSERT_EQ(primary_ps->get_zone_primaries().size(), 2u);

  // Clear any messages accumulated during peering/activation.
  get_primary_listener()->sent_messages.clear();
  get_primary_listener()->sent_messages_with_dest.clear();

  // Write an object
  const string obj_name = "test_zone_replicate_msg";
  const string test_data(stripe_unit * k, 'A');  // one full stripe
  ASSERT_EQ(create_and_write(obj_name, test_data), 0)
    << "Write should complete successfully";

  auto msgs = capture_primary_messages();

  // Exactly one MOSDECZoneReplicate must be sent (one per remote zone).
  EXPECT_EQ(msgs.zone_replicate_count, 1)
    << "Primary must send exactly 1 MOSDECZoneReplicate for 1 remote zone";

  // The destination must be the zone-1 Zone Primary: OSD 3 (first OSD of zone-1).
  ASSERT_EQ(msgs.zone_replicate_dests.size(), 1u);
  int zone1_zone_primary_osd = SHARDS_PER_ZONE;
  EXPECT_EQ(msgs.zone_replicate_dests[0], zone1_zone_primary_osd)
    << "MOSDECZoneReplicate must be addressed to the zone-1 Zone Primary";

  // No MOSDECSubOpWrite should be sent to zone-1 shards (OSDs 3,4,5).
  for (int osd : msgs.sub_op_write_dests) {
    EXPECT_LT(osd, SHARDS_PER_ZONE)
      << "MOSDECSubOpWrite must not be sent to zone-1 OSD " << osd
      << " — those shards are covered by MOSDECZoneReplicate";
  }

  // At least one MOSDECSubOpWrite must be sent to zone-0 peers (the local zone).
  // OSD 0 handles itself via handle_sub_write() locally (no message), so we
  // expect messages to OSDs 1 and 2 (zone-0, non-primary shards).
  EXPECT_GT(msgs.sub_op_write_count, 0)
    << "Primary must still send MOSDECSubOpWrite to local zone-0 peers";
}

// Test: NonStretchPoolSendsSubOpWrite
//
// Verify that a write to a non-stretch (single-zone) EC pool produces only
// MOSDECSubOpWrite messages — no MOSDECZoneReplicate — and the number sent
// equals the number of non-primary shards.
TEST_F(TestECNonStretch, NonStretchPoolSendsSubOpWrite) {
  ASSERT_TRUE(all_shards_active()) << "All shards must be active before write";

  // Verify zone_primaries is either empty or contains only the local zone.
  auto* primary_ps = get_primary_test_pg()->get_peering_state();

  const auto& zp = primary_ps->get_zone_primaries();
  // The local zone primary == cluster Primary, so there are no remote*zones.
  int primary_osd = primary_ps->get_primary().osd;
  for (auto& [zone, shard] : zp) {
    EXPECT_EQ(shard.osd, primary_osd)
      << "Single-zone pool: the only zone primary must be the cluster Primary itself";
  }

  get_primary_listener()->sent_messages.clear();
  get_primary_listener()->sent_messages_with_dest.clear();

  const string obj_name = "test_nonstretch_subop";
  const string test_data(stripe_unit * k, 'B');
  ASSERT_EQ(create_and_write(obj_name, test_data), 0)
    << "Write should complete successfully";

  int zone_replicate_count = 0;
  int sub_op_write_count   = 0;
  for (auto& [osd, mref] : get_primary_listener()->sent_messages_with_dest) {
    if (mref->get_type() == MSG_OSD_EC_ZONE_REPLICATE) {
      zone_replicate_count++;
    } else if (mref->get_type() == MSG_OSD_EC_WRITE) {
      sub_op_write_count++;
    }
  }

  EXPECT_EQ(zone_replicate_count, 0)
    << "Non-stretch pool must never send MOSDECZoneReplicate";
  EXPECT_GT(sub_op_write_count, 0)
    << "Non-stretch pool must send MOSDECSubOpWrite to peers";
}

// Test: ZonePrimaryFansOutSubOpWrite
//
// Verify that the zone-1 Zone Primary (OSD 3), upon receiving
// MOSDECZoneReplicate, fans out MOSDECSubOpWrite only to zone 1 shards
// (OSDs 3, 4, 5) and not back to zone-0 shards (OSDs 0, 1, 2).
TEST_F(TestECZoneReplicate, ZonePrimaryFansOutSubOpWrite) {
  ASSERT_TRUE(all_shards_active()) << "All shards must be active before write";

  get_primary_listener()->sent_messages.clear();
  get_primary_listener()->sent_messages_with_dest.clear();

  // Also clear zone-1 Zone Primary's sent messages before the write.
  int zone1_zp_osd = SHARDS_PER_ZONE;  // OSD 3
  auto* zone1_zp_pg = get_first_test_pg_for_osd(zone1_zp_osd);
  ASSERT_TRUE(zone1_zp_pg != nullptr && zone1_zp_pg->has_backend())
    << "Zone-1 Zone Primary (OSD " << zone1_zp_osd << ") must exist";
  zone1_zp_pg->get_backend_listener()->sent_messages.clear();
  zone1_zp_pg->get_backend_listener()->sent_messages_with_dest.clear();

  const string obj_name = "test_zone_primary_fanout";
  const string test_data(stripe_unit * k, 'C');
  ASSERT_EQ(create_and_write(obj_name, test_data), 0)
    << "Write should complete successfully";

  // Inspect messages sent by the zone-1 Zone Primary.
  auto* zp_listener = zone1_zp_pg->get_backend_listener();
  ASSERT_TRUE(zp_listener != nullptr);

  int subop_to_local_zone  = 0;  // zone-1 shards: OSDs 3,4,5
  int subop_to_remote_zone = 0;  // zone-0 shards: OSDs 0,1,2

  for (auto& [osd, mref] : zp_listener->sent_messages_with_dest) {
    if (mref->get_type() != MSG_OSD_EC_WRITE) continue;
    if (osd >= zone1_zp_osd && osd < 6) {
      subop_to_local_zone++;
    } else {
      subop_to_remote_zone++;
    }
  }

  // Zone Primary must NOT send SubOpWrite back to zone-0.
  EXPECT_EQ(subop_to_remote_zone, 0)
    << "Zone-1 Zone Primary must not send MOSDECSubOpWrite to zone-0 shards";

  // Zone Primary must fan out to its own zone-1 peers
  // The Zone Primary handles its own local shard inline — expect msgs to OSD 4 and OSD 5.
  EXPECT_EQ(subop_to_local_zone, SHARDS_PER_ZONE - 1)
    << "Zone-1 Zone Primary must send MOSDECSubOpWrite to zone-1 peers";
}

// Test: WriteAndReadRoundTrip 
// End-to-end: write data to a 2-zone stretch pool and verify it reads back correctly
TEST_F(TestECZoneReplicate, WriteAndReadRoundTrip) {
  ASSERT_TRUE(all_shards_active()) << "All shards must be active before write";

  const string obj_name = "test_zone_replicate_roundtrip";
  const string test_data(stripe_unit * k, 'D');

  create_and_write_verify(obj_name, test_data);
  ASSERT_FALSE(event_loop->has_events())
    << "Event loop should be idle after write+read round-trip";
}

// Test: PendingCommitsCountIsLocalShardsAndRemoteZones
//
// Verify that `pending_commits` on the primary Op is incremented once for the
// remote zone (via MOSDECZoneReplicate) and once per local-zone shard —
// i.e. local_shards + num_remote_zones — NOT once per total shard.
//
// Strategy: block only the reply path from the Zone Primary back to the
// cluster Primary.
// The Zone Primary still receives the MOSDECZoneReplicate, fans out
// MOSDECSubOpWrite to its local shards, and commits them — but its
// MOSDECSubOpWriteReply to osd.0 is queued, not delivered.  So the
// cluster Primary's pending_commits never drains to 0 and the write
// stays in-progress.  Releasing the reply path lets the single reply
// arrive and complete the write.

TEST_F(TestECZoneReplicate, PendingCommitsCountIsLocalShardsAndRemoteZones) {
  ASSERT_TRUE(all_shards_active());

  const int zone1_zp_osd = SHARDS_PER_ZONE;
  const int primary_osd  = get_primary_test_pg()->pg_whoami.osd;

  // Block the reply from the Zone Primary back to the cluster Primary.
  // The Zone Primary will run normally and commit all its local-zone shards,
  // but its MOSDECSubOpWriteReply to osd.0 will be held.
  event_loop->suspend_from_to_osd(zone1_zp_osd, primary_osd);

  // Issue a write.  create_and_write() calls run_until_idle() inside.
  // Because the Zone Primary's reply is blocked, pending_commits on the
  // cluster Primary Op never reaches 0, so the write stays in-progress.
  const string obj_name = "test_pending_commits";
  const string test_data(stripe_unit * k, 'E');
  int result = create_and_write(obj_name, test_data);
  EXPECT_EQ(result, -EINPROGRESS)
    << "Write must remain in-progress while the Zone Primary reply is blocked; "
       "if it completed, pending_commits did not account for the zone reply";

  // // Release the reply.  The single MOSDECSubOpWriteReply from osd.3 arrives
  // // at osd.0, pending_commits drops to 0, and try_finish_rmw() fires.
  event_loop->unsuspend_from_to_osd(zone1_zp_osd, primary_osd);
  event_loop->run_until_idle();

  // The object must now be readable — the write completed.
  // verify_object(obj_name);
}

TEST_F(TestECZoneReplicate, RecoveringZoneWithoutPrimaryReceivesDirectUpdates) {
  ScopedConfig cfg_async_recovery("osd_async_recovery_min_cost", "0");
  const string obj_name = "test_recovering_zone";
  const string initial_data(stripe_unit * k, 'A');
  const string degraded_data(stripe_unit * k, 'B');
  const string latest_data(stripe_unit * k, 'C');
  create_and_write_verify(obj_name, initial_data);

  const vector<int> targets = {3, 4, 5};
  mark_osds_down(targets);
  enter_degraded_stretch_mode(0);
  ASSERT_TRUE(get_primary_test_pg()->get_peering_state()->is_active());
  ASSERT_EQ(write(obj_name, 0, degraded_data, degraded_data.size()), 0);
  for (int osd : targets) {
    mark_osd_up(osd);
  }
  enter_recovery_stretch_mode();

  auto* primary_ps = get_primary_test_pg()->get_peering_state();
  ASSERT_TRUE(primary_ps->is_active());
  ASSERT_EQ(primary_ps->get_zone_primaries().size(), 1u);
  for (int osd : targets) {
    const pg_shard_t shard(osd, shard_id_t(osd));
    ASSERT_TRUE(primary_ps->get_async_recovery_targets().contains(shard));
    ASSERT_FALSE(primary_ps->is_acting(shard));
    ASSERT_TRUE(primary_ps->get_peer_missing().at(shard).is_missing(
      make_test_object(obj_name)));
    suspend_primary_to_osd(osd);
  }

  get_primary_listener()->sent_messages_with_dest.clear();
  ASSERT_EQ(write(obj_name, 0, latest_data, latest_data.size()), -EINPROGRESS);
  auto msgs = capture_primary_messages();
  EXPECT_EQ(msgs.zone_replicate_count, 0);
  for (int osd : targets) {
    EXPECT_EQ(std::count(msgs.sub_op_write_dests.begin(),
                         msgs.sub_op_write_dests.end(), osd), 1);
  }
  for (auto& [osd, msg] : get_primary_listener()->sent_messages_with_dest) {
    if (osd < SHARDS_PER_ZONE || msg->get_type() != MSG_OSD_EC_WRITE) {
      continue;
    }
    auto subwrite = boost::dynamic_pointer_cast<MOSDECSubOpWrite>(msg);
    ASSERT_TRUE(subwrite);
    EXPECT_TRUE(subwrite->op.t.empty());
    ASSERT_FALSE(subwrite->op.log_entries.empty());
    EXPECT_EQ(subwrite->op.at_version, primary_ps->get_info().last_update);
  }

  for (int osd : targets) {
    unsuspend_primary_to_osd(osd);
  }
  event_loop->run_until_idle();
  for (int osd : targets) {
    auto* peer_ps = get_peering_state(osd);
    EXPECT_EQ(peer_ps->get_info().last_update,
              primary_ps->get_info().last_update);
    pg_missing_item missing;
    ASSERT_TRUE(peer_ps->get_pg_log().get_missing().is_missing(
      make_test_object(obj_name), &missing));
    EXPECT_EQ(missing.need, primary_ps->get_info().last_update);
  }

  const string new_obj = "test_new_object_in_recovering_zone";
  get_primary_listener()->sent_messages_with_dest.clear();
  const int primary_osd = get_primary_test_pg()->pg_whoami.osd;
  event_loop->suspend_from_to_osd(targets.back(), primary_osd);
  ASSERT_EQ(create_and_write(new_obj, initial_data), -EINPROGRESS);
  msgs = capture_primary_messages();
  EXPECT_EQ(msgs.zone_replicate_count, 0);
  for (int osd : targets) {
    EXPECT_EQ(std::count(msgs.sub_op_write_dests.begin(),
                         msgs.sub_op_write_dests.end(), osd), 1);
  }
  event_loop->unsuspend_from_to_osd(targets.back(), primary_osd);
  event_loop->run_until_idle();
  verify_object(new_obj);
  for (int osd : targets) {
    auto* peer_ps = get_peering_state(osd);
    EXPECT_EQ(peer_ps->get_info().last_update,
              primary_ps->get_info().last_update);
    EXPECT_FALSE(peer_ps->get_pg_log().get_missing().is_missing(
      make_test_object(new_obj)));
  }

  set_config("osd_async_recovery_min_cost", "100");
  run_recovery(obj_name, false, latest_data);
  new_epoch_loop();
  ASSERT_TRUE(primary_is_clean());
  enter_healthy_stretch_mode();
  ASSERT_EQ(get_primary_test_pg()->get_peering_state()->get_zone_primaries().size(),
            2u);
  for (const string& obj : {obj_name, new_obj}) {
    for (int osd : targets) {
      EXPECT_EQ(read_shard_object_info(obj, 0).version,
                read_shard_object_info(obj, osd).version);
    }
    EXPECT_FALSE(scrub_object(obj));
  }
}

// Test: ZonePrimaryCrashMidFanOutRollsBack

// Verify that if the Zone Primary crashes after it receives
// MOSDECZoneReplicate (mid-fan-out) but before it sends its reply,
// the existing peering rollback mechanism correctly handles the partial
// write.

// Sequence:
//   1. Write an object and verify it exists ("before" version).
//   2. Block communication to the Zone Primary so the write stalls.
//   3. Issue a second write (will be in-progress).
//   4. Mark the Zone Primary down (simulates crash / interval change).
//   5. Unblock Zone Primary messages (now irrelevant — OSD is down).
//   6. Allow peering to complete on the surviving shards.
//   7. Verify the object still reads as the "before" version (no partial write).
TEST_F(TestECZoneReplicate, ZonePrimaryCrashMidFanOutRollsBack) {
  ASSERT_TRUE(all_shards_active());

  // Zone-1 Zone Primary is OSD 3.
  const int zone1_zp_osd = SHARDS_PER_ZONE;

  // Write initial version and confirm it is stable.
  const string obj_name = "test_zp_crash_rollback";
  const string data_before(stripe_unit * k, 'F');
  create_and_write_verify(obj_name, data_before);
  ASSERT_TRUE(primary_is_clean()) << "Pool must be clean before crash simulation";

  // Block the Zone Primary so the in-flight write cannot complete.
  suspend_primary_to_osd(zone1_zp_osd);

  // Issue a second write — it will stall waiting for the zone reply.
  const string data_after(stripe_unit * k, 'G');
  int result = create_and_write(obj_name, data_after);
  EXPECT_EQ(result, -EINPROGRESS)
    << "Write must stall while Zone Primary path is blocked";

  // Simulate Zone Primary crash mid-fan-out by marking it down.
  // This creates a new OSD map epoch, which will trigger a peering interval
  // change and cause the write to be rolled back.
  mark_osd_down(zone1_zp_osd);

  // Release the message block (OSD is already down; messages drop).
  unsuspend_primary_to_osd(zone1_zp_osd);

  // Allow peering to complete on the surviving shards.
  // After the interval change the primary will roll back the in-flight op.
  // We reduce min_size so the PG can go active without zone-1.
  unsigned degraded_min_size = static_cast<unsigned>((num_zones - 1) * (k + m) - m);
  set_pool_min_size(degraded_min_size);
  ASSERT_TRUE(all_shards_active())
    << "PG must re-activate on surviving shards after Zone Primary crash";

  // The object must still reflect the committed "before" version.
  verify_object(obj_name);
}

// Test:  MultipleSequentialWritesSucceed

// Verify that the zone-replicate path is stateless across consecutive writes:
// issuing several writes in sequence (each fully completing before the next
// begins) all succeed and each overwrites the previous data correctly.

// This exercises the path where the Zone Primary handle_zone_replicate() is
// called multiple times with different tids, and that the op lifecycle
// (start_rmw → cache_ready → finish_rmw) correctly resets for each write.
TEST_F(TestECZoneReplicate, MultipleSequentialWritesSucceed) {
  ASSERT_TRUE(all_shards_active());

  const string obj_name = "test_sequential_writes";
  const std::vector<char> fills = { 'H', 'I', 'J', 'K' };

  for (char fill : fills) {
    const string data(stripe_unit * k, fill);
    create_and_write_verify(obj_name, data);
    event_loop->run_until_idle();
  }

  // After all writes the pool must be clean (all shards consistent).
  ASSERT_TRUE(primary_is_clean())
    << "Pool must be clean after multiple sequential zone-replicate writes";
}

// Test: OverwriteExistingObjectViaZoneReplicate (RMW path)

// Verify that overwriting an existing object (partial write at non-zero
// offset) succeeds over the zone-replicate path.  This exercises the
// obc_map population logic on the Zone Primary — for an existing object the
// Zone Primary must load the OBC from its local store before calling
// get_write_plan(), otherwise the write-plan size check for full-stripe
// vs. RMW will compute the wrong result.

// The test writes a full stripe first, then overwrites just the first
// stripe_unit bytes and reads back the whole object verifying both the
// written region and the untouched tail.
TEST_F(TestECZoneReplicate, OverwriteExistingObjectViaZoneReplicate) {
  ASSERT_TRUE(all_shards_active());

  const string obj_name = "test_overwrite_zone_replicate";
  // Initial full-stripe write.
  const string initial_data(stripe_unit * k, 'L');
  create_and_write_verify(obj_name, initial_data);
  ASSERT_TRUE(primary_is_clean())
    << "Pool must be clean after initial write before overwrite test";

  // Overwrite the first chunk only (offset 0, length = stripe_unit).
  // The tail [stripe_unit, stripe_unit*k) remains as 'L'.
  const string overwrite_data(stripe_unit, 'M');
  write_verify(obj_name, 0, overwrite_data, static_cast<uint64_t>(stripe_unit * k));

  // Pool must be clean after the overwrite.
  event_loop->run_until_idle();
  ASSERT_TRUE(primary_is_clean())
    << "Pool must be clean after zone-replicate overwrite of existing object";

  // Scrub to detect any cross-shard inconsistency introduced by the RMW path.
  bool corrupted = scrub_object(obj_name);
  EXPECT_FALSE(corrupted)
    << "Object must have no shard inconsistency after zone-replicate overwrite";
}

// Test: LocalStoreStatEIOAbortsZoneReplicate
//
// Verify that a transient -EIO from stat() on the Zone Primary's local store
// causes handle_zone_replicate() to abort without fanning out SubOpWrite to
// zone-1 peers, rather than treating the object as non-existent and planning
// an incorrect new-object write.
//
// Without the fix, any nonzero error from get_attrs_n_size_from_disk() fell
// through to the ENOENT branch and an empty ObjectContext was constructed,
// causing an overwrite to be planned as a new-object write — potentially
// clobbering existing data.  With the fix the else-branch returns early and
// the op is abandoned before any SubOpWrite fan-out occurs.
//
// Observable invariant: with the fix the Zone Primary sends zero
// MOSDECSubOpWrite messages to its zone-1 peers after the abort.
//
// Strategy:
//  1. Write and commit an initial version of the object on both zones.
//  2. Suspend ALL messages to the ZP so we can inject the error before it
//     processes anything.
//  3. Inject a one-shot -EIO for stat() on the ZP's store.
//  4. Issue the overwrite.  Zone-0 shards commit but the write stalls
//     (ZP suspended, no zone reply).
//  5. Release the ZP.  It processes the queued MOSDECZoneReplicate, hits
//     the -EIO, returns early — no SubOpWrite fan-out to OSDs 4, 5.
//  6. Assert the ZP sent zero SubOpWrite to zone-1 peers.
//  7. Roll back the stalled write by marking the ZP down (peering interval
//     change), then verify the object reverts to the initial version.
TEST_F(TestECZoneReplicate, LocalStoreStatEIOAbortsZoneReplicate) {
  ASSERT_TRUE(all_shards_active());

  const string obj_name = "test_stat_eio_abort";
  const string initial_data(stripe_unit * k, 'N');
  create_and_write_verify(obj_name, initial_data);
  ASSERT_TRUE(primary_is_clean())
    << "Pool must be clean before injecting store error";

  const int zone1_zp_osd = SHARDS_PER_ZONE;  // OSD 3

  // Suspend the ZP so we can inject the error before it handles any message.
  suspend_osd(zone1_zp_osd);

  // Inject a one-shot -EIO for stat() on the Zone Primary's store.
  inject_stat_error_for_shard(obj_name, zone1_zp_osd, -EIO);

  // Clear the ZP's message log so we can observe exactly what it sends
  // after processing the queued MOSDECZoneReplicate.
  auto* zp_pg = get_first_test_pg_for_osd(zone1_zp_osd);
  ASSERT_TRUE(zp_pg && zp_pg->has_backend());
  zp_pg->get_backend_listener()->sent_messages_with_dest.clear();

  // Issue the overwrite.  Zone-0 shards commit but the write stalls because
  // the ZP is suspended and will never send its zone reply.
  const string overwrite_data(stripe_unit * k, 'O');
  int stall_result = create_and_write(obj_name, overwrite_data);
  ASSERT_EQ(stall_result, -EINPROGRESS)
    << "Write must stall while the Zone Primary is suspended";

  // Release the ZP.  It processes MOSDECZoneReplicate, gets -EIO from stat(),
  // and returns early without fanning out SubOpWrite to zone-1 peers.
  unsuspend_osd(zone1_zp_osd);
  event_loop->run_until_idle();
  EXPECT_TRUE(osdmap->is_up(zone1_zp_osd));
  EXPECT_EQ(get_primary_test_pg()->outstanding_writes[
              make_test_object(obj_name)], 1);

  // With the fix: the ZP must have sent zero SubOpWrite to zone-1 peers.
  // (OSDs 4 and 5 are the non-primary zone-1 shards.)
  int subop_to_zone1_peers = 0;
  for (auto& [osd, mref] : zp_pg->get_backend_listener()->sent_messages_with_dest) {
    if (mref->get_type() == MSG_OSD_EC_WRITE &&
        osd > zone1_zp_osd && osd < zone1_zp_osd + SHARDS_PER_ZONE) {
      subop_to_zone1_peers++;
    }
  }
  EXPECT_EQ(subop_to_zone1_peers, 0)
    << "Zone Primary must not fan out SubOpWrite to zone-1 peers when "
       "handle_zone_replicate() aborts due to stat() -EIO";

  // The in-flight overwrite is orphaned (ZP never replied; pending_commits==1).
  // Roll it back by marking the ZP down, which triggers a peering interval
  // change and causes the cluster Primary to roll back the uncommitted write.
  mark_osd_down(zone1_zp_osd);
  event_loop->run_until_idle();

  // Reduce min_size so the PG can re-activate on zone-0 only.
  unsigned degraded_min_size = static_cast<unsigned>((num_zones - 1) * (k + m) - m);
  set_pool_min_size(degraded_min_size);
  ASSERT_TRUE(all_shards_active())
    << "PG must re-activate on surviving shards after Zone Primary marked down";

  // After rollback the object must read as the committed initial version.
  verify_object(obj_name);
}

// Test: LocalStoreGetAttrsEIOAbortsZoneReplicate
//
// Same scenario as LocalStoreStatEIOAbortsZoneReplicate, but the error is
// injected into getattrs() rather than stat().
//
// get_attrs_n_size_from_disk() calls stat() first (succeeds), then
// objects_get_attrs_with_hinfo() which calls store->getattrs().  Injecting
// -EIO there exercises the second failure path in the same function.
TEST_F(TestECZoneReplicate, LocalStoreGetAttrsEIOAbortsZoneReplicate) {
  ASSERT_TRUE(all_shards_active());

  const string obj_name = "test_getattrs_eio_abort";
  const string initial_data(stripe_unit * k, 'P');
  create_and_write_verify(obj_name, initial_data);
  ASSERT_TRUE(primary_is_clean())
    << "Pool must be clean before injecting store error";

  const int zone1_zp_osd = SHARDS_PER_ZONE;  // OSD 3

  suspend_osd(zone1_zp_osd);
  inject_getattrs_error_for_shard(obj_name, zone1_zp_osd, -EIO);

  auto* zp_pg = get_first_test_pg_for_osd(zone1_zp_osd);
  ASSERT_TRUE(zp_pg && zp_pg->has_backend());
  zp_pg->get_backend_listener()->sent_messages_with_dest.clear();

  const string overwrite_data(stripe_unit * k, 'Q');
  int stall_result = create_and_write(obj_name, overwrite_data);
  ASSERT_EQ(stall_result, -EINPROGRESS)
    << "Write must stall while the Zone Primary is suspended";

  // Release the ZP.  stat() succeeds but getattrs() returns -EIO; the op
  // aborts before any SubOpWrite is sent to zone-1 peers.
  unsuspend_osd(zone1_zp_osd);
  event_loop->run_until_idle();
  EXPECT_TRUE(osdmap->is_up(zone1_zp_osd));
  EXPECT_EQ(get_primary_test_pg()->outstanding_writes[
              make_test_object(obj_name)], 1);

  // With the fix: zero SubOpWrite to zone-1 peers (OSDs 4, 5).
  int subop_to_zone1_peers = 0;
  for (auto& [osd, mref] : zp_pg->get_backend_listener()->sent_messages_with_dest) {
    if (mref->get_type() == MSG_OSD_EC_WRITE &&
        osd > zone1_zp_osd && osd < zone1_zp_osd + SHARDS_PER_ZONE) {
      subop_to_zone1_peers++;
    }
  }
  EXPECT_EQ(subop_to_zone1_peers, 0)
    << "Zone Primary must not fan out SubOpWrite to zone-1 peers when "
       "handle_zone_replicate() aborts due to getattrs() -EIO";

  // Roll back the stalled write by crashing the ZP.
  mark_osd_down(zone1_zp_osd);
  event_loop->run_until_idle();

  unsigned degraded_min_size = static_cast<unsigned>((num_zones - 1) * (k + m) - m);
  set_pool_min_size(degraded_min_size);
  ASSERT_TRUE(all_shards_active())
    << "PG must re-activate on surviving shards after Zone Primary marked down";

  verify_object(obj_name);
}

TEST_P(TestECZoneReplicateRecovery, MissingRemoteShardReceivesLatestVersion) {
  ScopedConfig cfg_async_recovery("osd_async_recovery_min_cost", "0");
  const string obj = "partial_zone_recovery";
  const string initial(stripe_unit * k, 'A');
  const string degraded(stripe_unit * k, 'B');
  create_and_write_verify(obj, initial);
  set_pool_min_size(k);
  mark_osd_down(4);
  ASSERT_EQ(write(obj, 0, degraded, degraded.size()), 0);
  mark_osd_up(4);

  auto* ps = get_primary_test_pg()->get_peering_state();
  ASSERT_TRUE(ps->is_active());
  ASSERT_EQ(ps->get_zone_primaries().size(), 2u);
  ASSERT_TRUE(ps->get_async_recovery_targets().contains(
    pg_shard_t(4, shard_id_t(4))));
  ASSERT_TRUE(get_peering_state(4)->get_pg_log().get_missing().is_missing(
    make_test_object(obj)));

  string expected = degraded;
  const size_t offset = get<1>(GetParam()) ? 17 : 0;
  const string update(get<1>(GetParam()) ? 113 : initial.size(), 'C');
  expected.replace(offset, update.size(), update);
  clear_primary_messages();
  ASSERT_EQ(write(obj, offset, update, expected.size()), 0);
  EXPECT_EQ(capture_primary_messages().zone_replicate_count,
            get<0>(GetParam()) ? 1 : 0);
  pg_missing_item missing;
  ASSERT_TRUE(get_peering_state(4)->get_pg_log().get_missing().is_missing(
    make_test_object(obj), &missing));
  ASSERT_EQ(missing.need, ps->get_info().last_update);

  set_config("osd_async_recovery_min_cost", "100");
  run_recovery(obj, false, expected);
  new_epoch_loop();
  ASSERT_TRUE(primary_is_clean());
  expect_matching_versions(obj);
  EXPECT_FALSE(scrub_object(obj));
}

INSTANTIATE_TEST_SUITE_P(
  FullAndUnalignedWrites, TestECZoneReplicateRecovery,
  testing::Combine(testing::Bool(), testing::Bool()),
  [](const testing::TestParamInfo<tuple<bool, bool>>& info) {
    return string(get<0>(info.param) ? "R2" : "Direct") +
      (get<1>(info.param) ? "Unaligned" : "FullStripe");
  });

TEST_F(TestECZoneReplicate, FallbackThenZoneReplicationPreservesPartialWrites) {
  ScopedConfig cfg_async_recovery("osd_async_recovery_min_cost", "0");
  const string obj = "fallback_transition";
  string expected(stripe_unit * k, 'A');
  create_and_write_verify(obj, expected);
  mark_osds_down({3, 4, 5});
  enter_degraded_stretch_mode(0);
  ASSERT_EQ(write(obj, 31, string(97, 'B'), expected.size()), 0);
  expected.replace(31, 97, string(97, 'B'));
  restore_remote_zone();
  ASSERT_EQ(get_primary_test_pg()->get_peering_state()->get_zone_primaries().size(),
            1u);

  clear_primary_messages();
  ASSERT_EQ(write(obj, stripe_unit - 23, string(71, 'C'), expected.size()), 0);
  expected.replace(stripe_unit - 23, 71, string(71, 'C'));
  EXPECT_EQ(capture_primary_messages().zone_replicate_count, 0);
  set_config("osd_async_recovery_min_cost", "100");
  run_recovery(obj, false, expected);
  new_epoch_loop();
  ASSERT_TRUE(primary_is_clean());
  enter_healthy_stretch_mode();

  clear_primary_messages();
  ASSERT_EQ(write(obj, 7, string(109, 'D'), expected.size()), 0);
  expected.replace(7, 109, string(109, 'D'));
  EXPECT_EQ(capture_primary_messages().zone_replicate_count, 1);
  verify_object(obj, expected, 0, expected.size());
  expect_matching_versions(obj);
  EXPECT_FALSE(scrub_object(obj));
}

TEST_F(TestECZoneReplicate, ZoneReplyWaitsForEveryLocalShard) {
  const string obj = "delayed_local_commit";
  const string data(stripe_unit * k, 'A');
  event_loop->suspend_from_to_osd(5, 3);
  ASSERT_EQ(create_and_write(obj, data), -EINPROGRESS);
  auto* zp = get_first_test_pg_for_osd(3)->get_backend_listener();
  for (const auto& [dest, msg] : zp->sent_messages_with_dest) {
    EXPECT_FALSE(dest == 0 && msg->get_type() == MSG_OSD_EC_WRITE_REPLY);
  }
  event_loop->unsuspend_from_to_osd(5, 3);
  event_loop->run_until_idle();
  int replies = 0;
  for (const auto& [dest, msg] : zp->sent_messages_with_dest) {
    replies += dest == 0 && msg->get_type() == MSG_OSD_EC_WRITE_REPLY;
  }
  EXPECT_EQ(replies, 1);
  verify_object(obj);
  expect_matching_versions(obj);
  EXPECT_FALSE(scrub_object(obj));
}

TEST_F(TestECZoneReplicate, ZonePrimaryCrashAfterPartialFanOut) {
  const string obj = "partial_fanout_crash";
  const string initial(stripe_unit * k, 'A');
  const string update(stripe_unit * k, 'B');
  create_and_write_verify(obj, initial);
  set_pool_min_size(k);
  event_loop->suspend_from_to_osd(3, 5);
  clear_primary_messages();
  ASSERT_EQ(write(obj, 0, update, initial.size()), -EINPROGRESS);
  const auto pending_version =
    get_primary_test_pg()->get_peering_state()->get_info().last_update;
  EXPECT_EQ(read_shard_object_info(obj, 4).version, pending_version);
  EXPECT_LT(read_shard_object_info(obj, 5).version, pending_version);

  mark_osd_down(3);
  event_loop->unsuspend_from_to_osd(3, 5);
  event_loop->run_until_idle();
  ASSERT_TRUE(get_primary_test_pg()->get_peering_state()->is_active());
  verify_object(obj);
  mark_osd_up(3);
  ASSERT_TRUE(all_shards_active());
  verify_object(obj);
}

TEST_F(TestECZoneReplicate, DurableZoneWriteSurvivesLostReplyAndPrimaryChange) {
  const string obj = "durable_lost_reply";
  const string initial(stripe_unit * k, 'A');
  const string update(stripe_unit * k, 'B');
  create_and_write_verify(obj, initial);
  set_pool_min_size(k);
  event_loop->suspend_from_to_osd(3, 0);
  ASSERT_EQ(write(obj, 0, update, initial.size()), -EINPROGRESS);
  expect_matching_versions(obj);

  mark_osd_down(3);
  ASSERT_TRUE(get_primary_test_pg()->get_peering_state()->is_active());
  verify_object(obj, update, 0, update.size());
  const int interrupted_writes =
    get_primary_test_pg()->outstanding_writes[make_test_object(obj)];
  clear_primary_messages();
  event_loop->suspend_from_to_osd(5, 0);
  const string expected(update.size(), 'C');
  ASSERT_EQ(write(obj, 0, expected, expected.size()), -EINPROGRESS);
  auto msgs = capture_primary_messages();
  ASSERT_EQ(msgs.zone_replicate_dests.size(), 1u);
  EXPECT_EQ(msgs.zone_replicate_dests.front(), 5);
  event_loop->unsuspend_from_to_osd(3, 0);
  event_loop->run_until_idle();
  EXPECT_EQ(get_primary_test_pg()->outstanding_writes[make_test_object(obj)],
            interrupted_writes + 1);
  event_loop->unsuspend_from_to_osd(5, 0);
  event_loop->run_until_idle();
  EXPECT_EQ(get_primary_test_pg()->outstanding_writes[make_test_object(obj)],
            interrupted_writes);
  verify_object(obj, expected, 0, expected.size());
}

TEST_F(TestECZoneReplicate, DeleteRecreateRemovesOldData) {
  const string obj = "delete_recreate";
  const size_t size = stripe_unit * k;
  create_and_write_verify(obj, string(2 * size, 'A'));
  ASSERT_EQ(delete_object(obj), 0);
  for (int shard = 0; shard < num_zones * SHARDS_PER_ZONE; ++shard) {
    auto* pg = find_test_pg_for_shard(shard);
    ASSERT_NE(pg, nullptr);
    struct stat st;
    EXPECT_EQ(get_osd_fixture(pg->pg_whoami.osd)->store->stat(
      pg->ch, ghobject_t(make_test_object(obj), ghobject_t::NO_GEN,
                        shard_id_t(shard)), &st), -ENOENT);
  }
  create_and_write_verify(obj, string(size, 'B'));
  expect_matching_versions(obj);
  EXPECT_FALSE(scrub_object(obj));
}

TEST_F(TestECZoneReplicate, UnalignedOverwriteAfterTruncate) {
  const string obj = "truncate_and_overwrite";
  const size_t size = stripe_unit * k;
  create_and_write_verify(obj, string(size, 'B'));
  const size_t truncated = stripe_unit / 2;
  ASSERT_EQ(truncate_and_write(obj, size, truncated,
                              {{17, string(53, 'C')}}), 0);
  string expected(truncated, 'B');
  expected.replace(17, 53, string(53, 'C'));
  verify_object(obj, expected, 0, expected.size());
  expect_matching_versions(obj);
  EXPECT_FALSE(scrub_object(obj));
}

TEST_F(TestECZoneReplicate, AttributeOverwriteReachesBothZones) {
  const string obj = "attribute_overwrite";
  create_and_write_verify(obj, string(stripe_unit * k, 'A'));
  ASSERT_EQ(write_attribute(obj, "key", "old", false), 0);
  ASSERT_EQ(write_attribute(obj, "key", "new", false), 0);
  for (int shard = 0; shard < num_zones * SHARDS_PER_ZONE; ++shard) {
    if (get_pool().is_nonprimary_shard(shard_id_t(shard))) {
      continue;
    }
    auto* pg = find_test_pg_for_shard(shard);
    ASSERT_NE(pg, nullptr);
    ceph::buffer::ptr value;
    ASSERT_GE(get_osd_fixture(pg->pg_whoami.osd)->store->getattr(
      pg->ch, ghobject_t(make_test_object(obj), ghobject_t::NO_GEN,
                        shard_id_t(shard)), "key", value), 0);
    EXPECT_EQ(string(value.c_str(), value.length()), "new");
  }
  expect_matching_versions(obj);
  EXPECT_FALSE(scrub_object(obj));
}

TEST_F(TestECZoneReplicate, SnapshotRollbackRestoresBothZones) {
  const string obj = "snapshot_rollback";
  const string initial(stripe_unit * k, 'A');
  create_and_write_verify(obj, initial);
  {
    SCOPED_TRACE("create snapshot");
    ASSERT_EQ(create_snapshot(obj, initial.size()), 0);
  }
  ASSERT_EQ(write(obj, 17, string(113, 'B'), initial.size()), 0);
  {
    SCOPED_TRACE("rollback to snapshot");
    ASSERT_EQ(rollback(obj, initial.size()), 0);
  }
  verify_object(obj, initial, 0, initial.size());
  expect_matching_versions(obj);
  EXPECT_FALSE(scrub_object(obj));
}

TEST_F(TestECZoneReplicateThreeZones, WaitsForBothRemoteZoneReplies) {
  const string obj = "three_zone_commit";
  const string data(stripe_unit * k, 'A');
  event_loop->suspend_from_to_osd(3, 0);
  event_loop->suspend_from_to_osd(6, 0);
  clear_primary_messages();
  ASSERT_EQ(create_and_write(obj, data), -EINPROGRESS);
  auto msgs = capture_primary_messages();
  sort(msgs.zone_replicate_dests.begin(), msgs.zone_replicate_dests.end());
  EXPECT_EQ(msgs.zone_replicate_dests, (vector<int>{3, 6}));
  event_loop->unsuspend_from_to_osd(3, 0);
  event_loop->run_until_idle();
  EXPECT_GT(get_primary_test_pg()->outstanding_writes[make_test_object(obj)], 0);
  event_loop->unsuspend_from_to_osd(6, 0);
  event_loop->run_until_idle();
  EXPECT_EQ(get_primary_test_pg()->outstanding_writes[make_test_object(obj)], 0);
  verify_object(obj);
  expect_matching_versions(obj);
  EXPECT_FALSE(scrub_object(obj));
}

TEST_F(TestECZoneReplicateThreeZones, MixedZoneReplicationAndRecoveryFallback) {
  ScopedConfig cfg_async_recovery("osd_async_recovery_min_cost", "0");
  const string obj = "mixed_zone_routing";
  const string initial(stripe_unit * k, 'A');
  const string degraded(stripe_unit * k, 'B');
  const string latest(stripe_unit * k, 'C');
  create_and_write_verify(obj, initial);
  mark_osds_down({6, 7, 8});
  set_pool_min_size(k);
  ASSERT_TRUE(get_primary_test_pg()->get_peering_state()->is_active());
  ASSERT_EQ(write(obj, 0, degraded, degraded.size()), 0);
  for (int osd : {6, 7, 8}) {
    mark_osd_up(osd);
  }
  auto* ps = get_primary_test_pg()->get_peering_state();
  ASSERT_TRUE(ps->is_active());
  ASSERT_EQ(ps->get_zone_primaries().size(), 2u);
  for (int osd : {6, 7, 8}) {
    ASSERT_TRUE(ps->get_async_recovery_targets().contains(
      pg_shard_t(osd, shard_id_t(osd))));
  }
  clear_primary_messages();
  ASSERT_EQ(write(obj, 0, latest, latest.size()), 0);
  const auto msgs = capture_primary_messages();
  EXPECT_EQ(msgs.zone_replicate_dests, (vector<int>{3}));
  for (int osd : {6, 7, 8}) {
    int updates = 0;
    for (const auto& [dest, msg] :
         get_primary_listener()->sent_messages_with_dest) {
      auto subwrite = boost::dynamic_pointer_cast<MOSDECSubOpWrite>(msg);
      if (dest == osd && subwrite &&
          subwrite->op.at_version == ps->get_info().last_update &&
          subwrite->op.soid == make_test_object(obj)) {
        ++updates;
        EXPECT_TRUE(subwrite->op.t.empty());
        EXPECT_FALSE(subwrite->op.log_entries.empty());
      }
    }
    EXPECT_EQ(updates, 1);
    EXPECT_EQ(get_peering_state(osd)->get_info().last_update,
              ps->get_info().last_update);
  }
  set_config("osd_async_recovery_min_cost", "100");
  run_recovery(obj, false, latest);
  new_epoch_loop();
  ASSERT_TRUE(primary_is_clean());
  expect_matching_versions(obj);
  EXPECT_FALSE(scrub_object(obj));
}
