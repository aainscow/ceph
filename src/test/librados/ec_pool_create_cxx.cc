// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include <errno.h>
#include <optional>
#include <string>
#include "gtest/gtest.h"
#include "include/rados/librados.hpp"
#include "test/librados/test_cxx.h"

using namespace librados;

// Helper function to create EC pool with K, M, and optional num_zones
int create_ec_pool_with_params(Rados &cluster, const std::string &pool_name,
                                const int k, const int m, const std::optional<int> num_zones = std::nullopt) {
  std::string cmd = "{\"prefix\": \"osd pool create\", \"pool\": \"" +
    pool_name + "\", \"pool_type\": \"erasure\", \"pg_num\": 8, " +
    "\"ec_k\": " + std::to_string(k) + ", \"ec_m\": " + std::to_string(m);
  
  if (num_zones.has_value()) {
    cmd += ", \"ec_num_zones\": " + std::to_string(num_zones.value());
  }
  cmd += "}";
  
  bufferlist outbl;
  const int ret = cluster.mon_command(std::move(cmd), {}, &outbl, nullptr);
  if (ret == 0) {
    cluster.wait_for_latest_osdmap();
  }
  return ret;
}

// Helper function to verify EC profile exists
int verify_ec_profile(Rados &cluster, const std::string &profile_name) {
  std::string cmd = "{\"prefix\": \"osd erasure-code-profile get\", "
                    "\"name\": \"" + profile_name + "\"}";
  bufferlist outbl;
  return cluster.mon_command(std::move(cmd), {}, &outbl, nullptr);
}

// Helper function to generate expected profile name
std::string get_ec_profile_name(const std::string &pool_name, const int k, const int m, const std::optional<int> num_zones = std::nullopt) {
  std::string name = pool_name + "-ec-k" + std::to_string(k) + "-m" + std::to_string(m);
  if (!num_zones.has_value()) {
    name += "-z1";
    }
  else {
    name += "-z" + std::to_string(num_zones.value());
  }
  return name;
}

// Helper function to cleanup EC pool
int cleanup_ec_pool(Rados &cluster, const std::string &pool_name) {
  // Delete pool first
  int ret = cluster.pool_delete(pool_name.c_str());
  if (ret) {
    return ret;
  }
  cluster.wait_for_latest_osdmap();
  return 0;
}

// Helper function to clean up EC profile
int cleanup_ec_profile(Rados &cluster, const std::string &profile_name) {

  std::string cmd = "{\"prefix\": \"osd erasure-code-profile rm\", "
                    "\"name\": \"" + profile_name + "\"}";
  int ret = cluster.mon_command(std::move(cmd), {}, nullptr, nullptr);
  if (ret) {
    return ret;
  }
  return 0;
}


// Test basic EC pool creation with K and M parameters
TEST(ECPoolCreatePP, BasicKM) {
  Rados cluster;
  ASSERT_EQ("", connect_cluster_pp(cluster));
  
  const std::string pool_name = get_temp_pool_name("ec_basic_");
  constexpr int k = 4, m = 2;
  
  ASSERT_EQ(0, create_ec_pool_with_params(cluster, pool_name, k, m));
  
  const std::string profile_name = get_ec_profile_name(pool_name, k, m);
  ASSERT_EQ(0, verify_ec_profile(cluster, profile_name));
  
  ASSERT_EQ(0, cleanup_ec_pool(cluster, pool_name));
  ASSERT_EQ(0, cleanup_ec_profile(cluster, profile_name));
  cluster.shutdown();
}

// Test stretch EC pool creation with K, M, and num_zones
TEST(ECPoolCreatePP, StretchECWithNumZones) {
  Rados cluster;
  ASSERT_EQ("", connect_cluster_pp(cluster));
  
  const std::string pool_name = get_temp_pool_name("ec_stretch_");
  constexpr int k = 4, m = 2, num_zones = 2;
  
  ASSERT_EQ(0, create_ec_pool_with_params(cluster, pool_name, k, m, num_zones));
  
  const std::string profile_name = get_ec_profile_name(pool_name, k, m, num_zones);
  ASSERT_EQ(0, verify_ec_profile(cluster, profile_name));
  
  ASSERT_EQ(0, cleanup_ec_pool(cluster, pool_name));
  ASSERT_EQ(0, cleanup_ec_profile(cluster, profile_name));
  cluster.shutdown();
}

// Test that creating two pools with same K/M creates separate profiles
TEST(ECPoolCreatePP, ProfileReuse) {
  Rados cluster;
  ASSERT_EQ("", connect_cluster_pp(cluster));
  
  const std::string pool1 = get_temp_pool_name("ec_reuse1_");
  const std::string pool2 = get_temp_pool_name("ec_reuse2_");
  constexpr int k = 3, m = 2;
  
  ASSERT_EQ(0, create_ec_pool_with_params(cluster, pool1, k, m));
  ASSERT_EQ(0, create_ec_pool_with_params(cluster, pool2, k, m));
  
  const std::string profile1 = get_ec_profile_name(pool1, k, m);
  const std::string profile2 = get_ec_profile_name(pool2, k, m);
  ASSERT_EQ(0, verify_ec_profile(cluster, profile1));
  ASSERT_EQ(0, verify_ec_profile(cluster, profile2));
  ASSERT_NE(profile1, profile2);  // Verify they're different
  
  ASSERT_EQ(0, cleanup_ec_pool(cluster, pool1));
  ASSERT_EQ(0, cleanup_ec_pool(cluster, pool2));
  ASSERT_EQ(0, cleanup_ec_profile(cluster, profile1));
  ASSERT_EQ(0, cleanup_ec_profile(cluster, profile2));
  cluster.shutdown();
}

// Test mutual exclusivity between new params and erasure_code_profile
TEST(ECPoolCreatePP, MutualExclusivity) {
  Rados cluster;
  ASSERT_EQ("", connect_cluster_pp(cluster));
  
  const std::string pool_name = get_temp_pool_name("ec_mutex_");
  
  // Try to create pool with both ec_k/ec_m AND erasure_code_profile
  std::string cmd = "{\"prefix\": \"osd pool create\", \"pool\": \"" + 
    pool_name + "\", \"pool_type\": \"erasure\", \"pg_num\": 8, " +
    "\"ec_k\": 4, \"ec_m\": 2, \"erasure_code_profile\": \"default\"}";
  
  bufferlist outbl;
  ASSERT_EQ(-EINVAL, cluster.mon_command(std::move(cmd), {}, &outbl, NULL));
  cluster.shutdown();
}

// Test invalid K value (K=0)
TEST(ECPoolCreatePP, InvalidK) {
  Rados cluster;
  ASSERT_EQ("", connect_cluster_pp(cluster));
  
  const std::string pool_name = get_temp_pool_name("ec_invalidk_");
  
  ASSERT_EQ(-EINVAL, create_ec_pool_with_params(cluster, pool_name, 0, 2));
  cluster.shutdown();
}

// Test invalid M value (M=0)
TEST(ECPoolCreatePP, InvalidM) {
  Rados cluster;
  ASSERT_EQ("", connect_cluster_pp(cluster));
  
  const std::string pool_name = get_temp_pool_name("ec_invalidm_");
  
  ASSERT_EQ(-EINVAL, create_ec_pool_with_params(cluster, pool_name, 4, 0));
  cluster.shutdown();
}

// Test invalid num_zones value (num_zones=0 when provided)
TEST(ECPoolCreatePP, InvalidNumZones) {
  Rados cluster;
  ASSERT_EQ("", connect_cluster_pp(cluster));
  
  const std::string pool_name = get_temp_pool_name("ec_invalidz_");
  
  ASSERT_EQ(-EINVAL, create_ec_pool_with_params(cluster, pool_name, 4, 2, 0));
  cluster.shutdown();
}

// Test backward compatibility with traditional erasure_code_profile workflow
TEST(ECPoolCreatePP, BackwardCompatibility) {
  Rados cluster;
  ASSERT_EQ("", connect_cluster_pp(cluster));
  
  const std::string pool_name = get_temp_pool_name("ec_compat_");
  const std::string profile_name = "testprofile-" + pool_name;
  
  // Create profile manually (old way)
  std::string cmd = "{\"prefix\": \"osd erasure-code-profile set\", "
    "\"name\": \"" + profile_name + "\", "
    "\"profile\": [\"k=2\", \"m=1\", \"crush-failure-domain=osd\"]}";
  bufferlist outbl;
  ASSERT_EQ(0, cluster.mon_command(std::move(cmd), {}, &outbl, NULL));
  
  // Create pool using profile (old way)
  cmd = "{\"prefix\": \"osd pool create\", \"pool\": \"" +
    pool_name + "\", \"pool_type\": \"erasure\", \"pg_num\": 8, " +
    "\"erasure_code_profile\": \"" + profile_name + "\"}";
  ASSERT_EQ(0, cluster.mon_command(std::move(cmd), {}, &outbl, NULL));
  cluster.wait_for_latest_osdmap();
  
  ASSERT_EQ(0, cleanup_ec_pool(cluster, pool_name));
  ASSERT_EQ(0, cleanup_ec_profile(cluster, profile_name));
  cluster.shutdown();
}

// Test with large K and M values
TEST(ECPoolCreatePP, LargeKM) {
  Rados cluster;
  ASSERT_EQ("", connect_cluster_pp(cluster));
  
  const std::string pool_name = get_temp_pool_name("ec_large_");
  constexpr int k = 8, m = 4;
  
  ASSERT_EQ(0, create_ec_pool_with_params(cluster, pool_name, k, m));
  
  const std::string profile_name = get_ec_profile_name(pool_name, k, m);
  ASSERT_EQ(0, verify_ec_profile(cluster, profile_name));
  
  ASSERT_EQ(0, cleanup_ec_pool(cluster, pool_name));
  ASSERT_EQ(0, cleanup_ec_profile(cluster, profile_name));
  cluster.shutdown();
}

// Test that conflicting profile with different parameters returns an error
TEST(ECPoolCreatePP, ConflictingProfile) {
  Rados cluster;
  ASSERT_EQ("", connect_cluster_pp(cluster));
  
  const std::string pool_name = get_temp_pool_name("ec_conflict_");
  const std::string profile_name = pool_name + "-ec-k4-m2-z1";
  
  // Manually create a profile with the expected auto-generated name but DIFFERENT parameters
  // Profile has k=3, m=2 but we'll try to create pool with k=4, m=2
  std::string cmd = "{\"prefix\": \"osd erasure-code-profile set\", "
    "\"name\": \"" + profile_name + "\", "
    "\"profile\": [\"k=3\", \"m=2\", \"num_zones=1\"]}";
  bufferlist outbl;
  ASSERT_EQ(0, cluster.mon_command(std::move(cmd), {}, &outbl, NULL));
  
  ASSERT_EQ(-EEXIST, create_ec_pool_with_params(cluster, pool_name, 4, 2));
  
  cleanup_ec_profile(cluster, profile_name);
  cluster.shutdown();
}

// Test that profile with matching parameters allows pool creation
TEST(ECPoolCreatePP, MatchingProfileAllowsPoolCreation) {
  Rados cluster;
  ASSERT_EQ("", connect_cluster_pp(cluster));
  
  const std::string pool_name = get_temp_pool_name("ec_match_");
  const std::string profile_name = pool_name + "-ec-k4-m2-z1";
  
  std::string cmd = "{\"prefix\": \"osd erasure-code-profile set\", "
    "\"name\": \"" + profile_name + "\", "
    "\"profile\": [\"k=4\", \"m=2\", \"num_zones=1\"]}";
  bufferlist outbl;
  ASSERT_EQ(0, cluster.mon_command(std::move(cmd), {}, &outbl, NULL));
  
  ASSERT_EQ(0, create_ec_pool_with_params(cluster, pool_name, 4, 2));
  
  ASSERT_EQ(0, verify_ec_profile(cluster, profile_name));
  
  ASSERT_EQ(0, cleanup_ec_pool(cluster, pool_name));
  ASSERT_EQ(0, cleanup_ec_profile(cluster, profile_name));
  cluster.shutdown();
}

// Test valid case where M > K
TEST(ECPoolCreatePP, MGreaterThanK) {
  Rados cluster;
  ASSERT_EQ("", connect_cluster_pp(cluster));
  
  const std::string pool_name = get_temp_pool_name("ec_mgtk_");
  constexpr int k = 2, m = 4;
  
  ASSERT_EQ(0, create_ec_pool_with_params(cluster, pool_name, k, m));
  
  const std::string profile_name = get_ec_profile_name(pool_name, k, m);
  ASSERT_EQ(0, verify_ec_profile(cluster, profile_name));
  
  ASSERT_EQ(0, cleanup_ec_pool(cluster, pool_name));
  ASSERT_EQ(0, cleanup_ec_profile(cluster, profile_name));
  cluster.shutdown();
}

// Test no params uses default profile
TEST(ECPoolCreatePP, DefaultProfile) {
  Rados cluster;
  ASSERT_EQ("", connect_cluster_pp(cluster));

  const std::string pool_name = get_temp_pool_name("ec_default_");

  std::string cmd = "{\"prefix\": \"osd pool create\", \"pool\": \"" +
    pool_name + "\", \"pool_type\": \"erasure\", \"pg_num\": 8}";

  bufferlist outbl;
  ASSERT_EQ(0, cluster.mon_command(std::move(cmd), {}, &outbl, nullptr));
  cluster.wait_for_latest_osdmap();

  ASSERT_EQ(0, verify_ec_profile(cluster, "default"));

  ASSERT_EQ(0, cleanup_ec_pool(cluster, pool_name));
  cluster.shutdown();
}