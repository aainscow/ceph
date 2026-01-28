// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#pragma once

#include <string>
#include "include/rados/librados.hpp"
#include "test/librados/test_cxx.h"

namespace ceph {
namespace test {

// Pool type enumeration for parameterized tests
enum class PoolType {
  REPLICATED,
  FAST_EC  // Fast EC with k=2, m=1
};

// Convert pool type to string for test naming
inline std::string pool_type_name(PoolType type) {
  switch (type) {
    case PoolType::REPLICATED:
      return "Replicated";
    case PoolType::FAST_EC:
      return "FastEC";
    default:
      return "Unknown";
  }
}

// Create pool based on type
inline std::string create_pool_by_type(
    const std::string& pool_name,
    librados::Rados& cluster,
    PoolType type) {
  switch (type) {
    case PoolType::REPLICATED:
      return create_one_pool_pp(pool_name, cluster);
    case PoolType::FAST_EC:
      return create_one_ec_pool_pp(pool_name, cluster, true, true);
    default:
      return "Unknown pool type";
  }
}

// Destroy pool based on type
inline int destroy_pool_by_type(
    const std::string& pool_name,
    librados::Rados& cluster,
    PoolType type) {
  switch (type) {
    case PoolType::REPLICATED:
      return destroy_one_pool_pp(pool_name, cluster);
    case PoolType::FAST_EC:
      return destroy_one_ec_pool_pp(pool_name, cluster);
    default:
      return -EINVAL;
  }
}

} // namespace test
} // namespace ceph

// Made with Bob
