#include <common/perf_counters_collection.h>

#include "test/librados/test_cxx.h"
#include "test/librados/testcase_cxx.h"
#include "crimson_utils.h"
#include "cls/fifo/cls_fifo_ops.h"
#include "cls/version/cls_version_ops.h"
#include "common/json/OSDStructures.h"
#include "librados/librados_asio.h"

#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <climits>
#include <thread>
#include <chrono>

using namespace std;
using namespace librados;

typedef RadosTestECOptimisedPP LibRadosOmapECPP;

TEST_F(LibRadosOmapECPP, OmapReads) {
  SKIP_IF_CRIMSON();
  enable_omap();
  bufferlist bl_write, omap_val_bl, omap_header_bl;
  const std::string omap_key_1 = "omap_key_1_palomino";
  const std::string omap_key_2 = "omap_key_2_chestnut";
  const std::string omap_key_3 = "omap_key_3_bay";
  const std::string omap_key_4 = "omap_key_4_dun";
  const std::string omap_key_5 = "omap_key_5_black";
  const std::string omap_key_6 = "omap_key_6_grey";
  const std::string omap_value = "omap_value_1_horse";
  const std::string omap_header = "my_omap_header";

  encode(omap_value, omap_val_bl);
  encode(omap_header, omap_header_bl);

  std::map<std::string, bufferlist> omap_map = {
    {omap_key_1.c_str(), omap_val_bl},
    {omap_key_2.c_str(), omap_val_bl},
    {omap_key_3.c_str(), omap_val_bl},
    {omap_key_4.c_str(), omap_val_bl},
    {omap_key_5.c_str(), omap_val_bl},
    {omap_key_6.c_str(), omap_val_bl}
  };

  bl_write.append("ceph");
  ObjectWriteOperation write1;
  write1.write(0, bl_write);
  write1.omap_set_header(omap_header_bl);
  write1.omap_set(omap_map);

  int ret = ioctx.operate("my_object", &write1);
  EXPECT_EQ(ret, 0);

  int err = 0;
  bufferlist bl_read;
  ObjectReadOperation read;

  // OMAP GET VALS TESTING

  read.read(0, bl_write.length(), &bl_read, nullptr);

  std::map<std::string, bufferlist> returned_vals_1;
  std::map<std::string, bufferlist> returned_vals_2;

  read.omap_get_vals2(omap_key_1, 1, &returned_vals_1, nullptr, &err);
  read.omap_get_vals2("omap", 4, &returned_vals_2, nullptr, &err);

  ret = ioctx.operate("my_object", &read, nullptr);
  EXPECT_EQ(ret, 0);

  ASSERT_EQ(0, memcmp(bl_read.c_str(), "ceph", 4));
  ASSERT_EQ(0, err);
  ASSERT_EQ(1u, returned_vals_1.size());
  ASSERT_EQ(4u, returned_vals_2.size());

  // OMAP GET KEYS TESTING

  std::set<std::string> returned_keys;

  read.omap_get_keys2("", LONG_MAX, &returned_keys, nullptr, &err);

  ret = ioctx.operate("my_object", &read, nullptr);
  EXPECT_EQ(ret, 0);

  ASSERT_EQ(0, err);
  ASSERT_EQ(6u, returned_keys.size());

  // OMAP GET HEADER TESTING

  bufferlist returned_header_bl;

  read.omap_get_header(&returned_header_bl, &err);

  ret = ioctx.operate("my_object", &read, nullptr);
  EXPECT_EQ(ret, 0);

  std::string returned_header_str;
  decode(returned_header_str, returned_header_bl);

  ASSERT_EQ(0, err);
  ASSERT_EQ(returned_header_str, omap_header);

  // OMAP GET VALS BY KEYS TESTING

  std::set<std::string> key_filter = { omap_key_1, omap_key_3 };
  std::map<std::string, bufferlist> returned_vals_by_keys;

  read.omap_get_vals_by_keys(key_filter, &returned_vals_by_keys, &err);

  ret = ioctx.operate("my_object", &read, nullptr);
  EXPECT_EQ(ret, 0);

  ASSERT_EQ(0, err);
  ASSERT_EQ(2u, returned_vals_by_keys.size());

  // OMAP CMP TESTING

  map<string, pair<bufferlist, int>> cmp_results;
  bufferlist cmp_val_bl;

  encode(omap_value, cmp_val_bl);

  cmp_results["omap_key_1_palomino"] = pair<bufferlist, int>(cmp_val_bl, CEPH_OSD_CMPXATTR_OP_EQ);

  read.omap_cmp(cmp_results, &err);

  ret = ioctx.operate("my_object", &read, nullptr);
  EXPECT_EQ(ret, 0);

  ASSERT_EQ(0, err);

  // OMAP REMOVE KEYS TESTING

  std::set<std::string> keys_to_remove;
  std::set<std::string> returned_keys_with_removed;

  keys_to_remove.insert("omap_key_2_chestnut");

  write1.omap_rm_keys(keys_to_remove);

  ret = ioctx.operate("my_object", &write1);
  EXPECT_EQ(ret, 0);

  read.omap_get_keys2("", LONG_MAX, &returned_keys_with_removed, nullptr, &err);

  ret = ioctx.operate("my_object", &read, nullptr);
  EXPECT_EQ(ret, 0);

  ASSERT_EQ(0, err);
  ASSERT_EQ(5u, returned_keys_with_removed.size());

  // OMAP REMOVE RANGE TESTING

  std::set<std::string> returned_keys_with_removed_range;

  write1.omap_rm_range("omap_key_3_bay", "omap_key_5_black");

  ret = ioctx.operate("my_object", &write1);
  EXPECT_EQ(ret, 0);

  read.omap_get_keys2("", 10, &returned_keys_with_removed_range, nullptr, &err);

  ret = ioctx.operate("my_object", &read, nullptr);
  EXPECT_EQ(ret, 0);

  ASSERT_EQ(0, err);
  ASSERT_EQ(3u, returned_keys_with_removed_range.size());

  // OMAP CLEAR TESTING

  write1.omap_clear();

  ret = ioctx.operate("my_object", &write1);
  EXPECT_EQ(ret, 0);

  read.omap_get_keys2("", LONG_MAX, &returned_keys, nullptr, &err);

  ret = ioctx.operate("my_object", &read, nullptr);
  EXPECT_EQ(ret, 0);

  ASSERT_EQ(0, err);
  ASSERT_TRUE(returned_keys.empty());

}

// This test is destructive to other tests running in parallel
// Do not merge this into the main branch
TEST_F(LibRadosOmapECPP, OmapRecovery) {
  SKIP_IF_CRIMSON();
  enable_omap();
  turn_balancing_off();
  bufferlist bl_write, omap_val_bl, xattr_val_bl;
  const std::string omap_key_1 = "key_a";
  const std::string omap_key_2 = "key_b";
  const std::string omap_key_3 = "key_c";
  const std::string omap_value = "val_omap_recovery";
  encode(omap_value, omap_val_bl);
  std::map<std::string, bufferlist> omap_map = {
    {omap_key_1, omap_val_bl},
    {omap_key_2, omap_val_bl},
    {omap_key_3, omap_val_bl}
  };
  const std::string header = "recovery_header_z";
  bufferlist header_bl;
  encode(header, header_bl);
  bl_write.append("ceph");

  // 1. Write to OMAP
  ObjectWriteOperation write1;
  write1.write(0, bl_write);
  write1.omap_set(omap_map);
  write1.omap_set_header(header_bl);
  int ret = ioctx.operate("omap_recovery_oid", &write1);
  EXPECT_EQ(ret, 0);

  // 2. Find Primary OSD
  ceph::messaging::osd::OSDMapReply reply;
  int res = request_osd_map("omap_recovery_oid", &reply);
  EXPECT_EQ(res, 0);
  int victim_osd = reply.acting_primary;
  std::string pgid = reply.pgid;

  std::cout << "Target Object in PG " << pgid << " on Primary OSD." << victim_osd << std::endl;

  // 3. Mark Primary OSD as Down and Out
  std::cout << "Marking OSD." << victim_osd << " down and out..." << std::endl;
  bufferlist inbl, outbl;
  std::ostringstream cmd_down, cmd_out;
  cmd_out << "{\"prefix\": \"osd out\", \"ids\": [\"" << victim_osd << "\"]}";
  ret = cluster.mon_command(cmd_out.str(), std::move(inbl), &outbl, nullptr);
  EXPECT_EQ(ret, 0);
  cmd_down << "{\"prefix\": \"osd down\", \"ids\": [\"" << victim_osd << "\"]}";
  ret = cluster.mon_command(cmd_down.str(), std::move(inbl), &outbl, nullptr);
  EXPECT_EQ(ret, 0);

  // 4. Wait for PG to be Active + Clean
  std::cout << "Waiting for PG " << pgid << " to recover (active+clean)..." << std::endl;

  bool recovered = false;
  for (int i = 0; i < 120; ++i) {
    std::string state;
    bufferlist inbl2, outbl2;
    std::string cmd = "{\"prefix\": \"pg dump\", \"pgid\": \"" + pgid + "\"}";
    cluster.mon_command(std::move(cmd), std::move(inbl2), &outbl2, nullptr);
    std::string out_str = outbl2.to_str();
    if (out_str.find("active+clean") != std::string::npos) {
      recovered = true;
      break;
    }
    sleep(1);
  }
  EXPECT_TRUE(recovered) << "Timed out waiting for recovery on " << pgid;

  // 5. Read OMAP
  check_omap_read("omap_recovery_oid", omap_key_1, omap_value, 3, 0);

  // 6. Deep Scrub the PG
  std::cout << "Forcing Deep Scrub to verify OMAP integrity..." << std::endl;
  std::ostringstream cmd_scrub;
  cmd_scrub << "{\"prefix\": \"pg deep-scrub\", \"pgid\": \"" << pgid << "\"}";
  cluster.mon_command(cmd_scrub.str(), bufferlist(), &outbl, nullptr);

  // 7. Wait for deep scrub to finish
  sleep(5);
  bool clean_after_scrub = false;
  for (int i = 0; i < 60; ++i) {
    bufferlist in, out;
    std::string cmd = "{\"prefix\": \"pg dump\", \"pgid\": \"" + pgid + "\"}";
    cluster.mon_command(std::move(cmd), std::move(in), &out, nullptr);
    if (out.to_str().find("active+clean") != std::string::npos) {
      clean_after_scrub = true;
      break;
    }
    sleep(1);
  }
  EXPECT_TRUE(clean_after_scrub);

  // 8. Check for inconsistency
  bufferlist q_in, q_out;
  std::string q_cmd = "{\"prefix\": \"pg query\", \"pgid\": \"" + pgid + "\"}";
  cluster.mon_command(std::move(q_cmd), std::move(q_in), &q_out, nullptr);
  std::string query_res = q_out.to_str();
  if (query_res.find("inconsistent") != std::string::npos) {
    ADD_FAILURE() << "PG " << pgid << " is inconsistent after OMAP recovery! Scrub failed.";
  }

  // 7. Bring Old Primary OSD Back In
  std::ostringstream cmd_in;
  bufferlist inbl3;
  cmd_in << "{\"prefix\": \"osd in\", \"ids\": [\"" << victim_osd << "\"]}";
  ret = cluster.mon_command(cmd_in.str(), std::move(inbl3), &outbl, nullptr);
  EXPECT_EQ(ret, 0);

  turn_balancing_on();
}

TEST_F(LibRadosOmapECPP, ChangeUpmap) {
  SKIP_IF_CRIMSON();
  enable_omap();
  turn_balancing_off();
  bufferlist bl_write, omap_val_bl, xattr_val_bl;
  const std::string omap_key_1 = "key_a";
  const std::string omap_key_2 = "key_b";
  const std::string omap_key_3 = "key_c";
  const std::string omap_value = "val_12345";
  encode(omap_value, omap_val_bl);
  std::map<std::string, bufferlist> omap_map = {
    {omap_key_1, omap_val_bl},
    {omap_key_2, omap_val_bl},
    {omap_key_3, omap_val_bl}
  };
  const std::string header = "upmap_header_z";
  bufferlist header_bl;
  encode(header, header_bl);
  bl_write.append("ceph");
  
  // 1. Write data to omap
  ObjectWriteOperation write1;
  write1.write(0, bl_write);
  write1.omap_set(omap_map);
  write1.omap_set_header(header_bl);
  int ret = ioctx.operate("change_upmap_oid", &write1);
  EXPECT_EQ(ret, 0);

  // 2. Find up osds
  ceph::messaging::osd::OSDMapReply reply;
  int res = request_osd_map("change_upmap_oid", &reply);
  EXPECT_TRUE(res == 0);
  std::vector<int> prev_up_osds = reply.up;
  std::string pgid = reply.pgid;
  print_osd_map("Previous up osds: ", prev_up_osds);
  
  // 3. Find unused osd to be new primary
  int prev_primary = prev_up_osds[0];
  int new_primary = 0;
  while (true) {
    auto it = std::find(prev_up_osds.begin(), prev_up_osds.end(), new_primary);
    if (it == prev_up_osds.end()) {
      break;
    }
    new_primary++;
  }
  std::vector<int> new_up_osds = prev_up_osds;
  new_up_osds[0] = new_primary;
  std::cout << "Previous primary osd: " << prev_primary << std::endl;
  std::cout << "New primary osd: " << new_primary << std::endl;
  print_osd_map("Desired up osds: ", new_up_osds);

  // 4. Set new up map
  int rc = set_osd_upmap(pgid, new_up_osds);
  EXPECT_TRUE(rc == 0);

  // 5. Wait for new upmap to appear as acting set of osds
  int res2 = wait_for_upmap("change_upmap_oid", new_primary, 60s);
  EXPECT_TRUE(res2 == 0);
  
  // 6. Read omap
  check_omap_read("change_upmap_oid", omap_key_1, omap_value, 3, 0);

  turn_balancing_on();
}

TEST_F(LibRadosOmapECPP, NoOmapRecovery) {
  SKIP_IF_CRIMSON();
  enable_omap();
  turn_balancing_off();
  bufferlist bl_write;
  bl_write.append("ceph");

  // 1. Write data to omap
  ObjectWriteOperation write1;
  write1.write(0, bl_write);
  int ret = ioctx.operate("no_omap_oid", &write1);
  EXPECT_EQ(ret, 0);

  // 2. Find up osds
  ceph::messaging::osd::OSDMapReply reply;
  int res = request_osd_map("no_omap_oid", &reply);
  EXPECT_TRUE(res == 0);
  std::vector<int> prev_up_osds = reply.up;
  std::string pgid = reply.pgid;
  print_osd_map("Previous up osds: ", prev_up_osds);

  // 3. Find unused osd to be new primary
  int prev_primary = prev_up_osds[0];
  int new_primary = 0;
  while (true) {
    auto it = std::find(prev_up_osds.begin(), prev_up_osds.end(), new_primary);
    if (it == prev_up_osds.end()) {
      break;
    }
    new_primary++;
  }
  std::vector<int> new_up_osds = prev_up_osds;
  new_up_osds[0] = new_primary;
  std::cout << "Previous primary osd: " << prev_primary << std::endl;
  std::cout << "New primary osd: " << new_primary << std::endl;
  print_osd_map("Desired up osds: ", new_up_osds);

  // 4. Set new up map
  int rc = set_osd_upmap(pgid, new_up_osds);
  EXPECT_TRUE(rc == 0);

  // 5. Wait for new upmap to appear as acting set of osds
  int res2 = wait_for_upmap("no_omap_oid", new_primary, 60s);
  EXPECT_TRUE(res2 == 0);

  // 6. Read data
  bufferlist bl_read;
  ObjectReadOperation read;
  read.read(0, bl_write.length(), &bl_read, nullptr);
  ret = ioctx.operate("no_omap_oid", &read, nullptr);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(0, memcmp(bl_read.c_str(), "ceph", 4));

  turn_balancing_on();
}

TEST_F(LibRadosOmapECPP, LargeOmapRecovery) {
  SKIP_IF_CRIMSON();
  enable_omap();
  turn_balancing_off();
  bufferlist bl_write, header_bl;
  const std::string huge_val(1024, 'x');
  std::map<std::string, bufferlist> omap_map;
  for (int i = 0; i < 10000; ++i) {
    char key_buf[32];
    snprintf(key_buf, sizeof(key_buf), "key_%06d", i);
    bufferlist omap_val_bl;
    encode(huge_val, omap_val_bl);
    omap_map[std::string(key_buf)] = omap_val_bl;
  }

  const std::string header = "large_header";
  encode(header, header_bl);
  bl_write.append("ceph");

  // 3. Write data to omap
  ObjectWriteOperation write1;
  write1.write(0, bl_write);
  write1.omap_set(omap_map);
  write1.omap_set_header(header_bl);

  int ret = ioctx.operate("large_oid", &write1);
  EXPECT_EQ(ret, 0);

  // 2. Find up osds
  ceph::messaging::osd::OSDMapReply reply;
  int res = request_osd_map("large_oid", &reply);
  EXPECT_TRUE(res == 0);
  std::vector<int> prev_up_osds = reply.up;
  std::string pgid = reply.pgid;
  print_osd_map("Previous up osds: ", prev_up_osds);

  // 3. Find unused osd to be new primary
  int prev_primary = prev_up_osds[0];
  int new_primary = 0;
  while (true) {
    auto it = std::find(prev_up_osds.begin(), prev_up_osds.end(), new_primary);
    if (it == prev_up_osds.end()) {
      break;
    }
    new_primary++;
  }
  std::vector<int> new_up_osds = prev_up_osds;
  new_up_osds[0] = new_primary;
  std::cout << "Previous primary osd: " << prev_primary << std::endl;
  std::cout << "New primary osd: " << new_primary << std::endl;
  print_osd_map("Desired up osds: ", new_up_osds);

  // 4. Set new up map
  int rc = set_osd_upmap(pgid, new_up_osds);
  EXPECT_TRUE(rc == 0);

  // 5. Wait for new upmap to appear as acting set of osds
  int res2 = wait_for_upmap("large_oid", new_primary, 60s);
  EXPECT_TRUE(res2 == 0);

  // 6. Read omap
  check_omap_read("large_oid", "key_000000", huge_val, 1024, 0);

  turn_balancing_on();
}
