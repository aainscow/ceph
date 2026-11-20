// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#pragma once

#include <limits>
#include <utility>

#include "crush/CrushWrapper.h"
#include "osd/OSDMap.h"

namespace ceph::zone_primary {

inline std::map<int, pg_shard_t> calc_zone_primaries(
  const std::vector<int>& acting,
  const pg_pool_t& pool,
  const OSDMap& osdmap)
{
  std::map<int, pg_shard_t> result;
  for (unsigned i = 0; i < acting.size(); ++i) {
    if (acting[i] == CRUSH_ITEM_NONE) {
      continue;
    }
    const auto shard = pool.is_erasure() ? shard_id_t(i) : shard_id_t::NO_SHARD;
    if (pool.is_nonprimary_shard(shard)) {
      continue;
    }
    const int zone = osdmap.crush->get_parent_of_type(
      acting[i], pool.peering_crush_bucket_barrier, pool.crush_rule);
    result.try_emplace(zone, pg_shard_t(acting[i], shard));
  }
  return result;
}

inline std::optional<pg_shard_t> find_local_target(
  const std::vector<int>& acting,
  const pg_pool_t& pool,
  const OSDMap& osdmap,
  CephContext* cct,
  const std::multimap<std::string, std::string>& crush_location)
{
  if (!pool.is_erasure() || !pool.is_stretch_pool() ||
      !pool.supports_zone_replicate() || crush_location.empty()) {
    return std::nullopt;
  }

  std::optional<pg_shard_t> target;
  int best_distance = std::numeric_limits<int>::max();
  for (const auto& [zone, primary] : calc_zone_primaries(acting, pool, osdmap)) {
    if (zone == 0) { // CRUSH did not find a bucket in the pool's rule.
      continue;
    }
    const int distance = osdmap.crush->get_common_ancestor_distance(
      cct, primary.osd, crush_location);
    // A common ancestor above the zone (e.g. the root) is not locality.
    if (distance < 0 ||
        std::cmp_greater(distance, pool.peering_crush_bucket_barrier)) {
      continue;
    }
    if (distance < best_distance) {
      best_distance = distance;
      target = primary;
    } else if (distance == best_distance) {
      target.reset();
    }
  }
  if (target && !osdmap.is_up(target->osd)) {
    return std::nullopt;
  }
  return target;
}

} // namespace ceph::zone_primary
