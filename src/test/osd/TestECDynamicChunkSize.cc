// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2026 IBM
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <gtest/gtest.h>
#include <fmt/format.h>
#include "osd/ECUtil.h"
#include "test/osd/ECPeeringTestFixture.h"
#include "test/osd/TestCommon.h"

/**
 * TestECDynamicChunkSize - erasure coded objects with their own chunk size.
 *
 * The pool has FLAG_EC_DYNAMIC_CHUNK_SIZE, so the fixture gives each object
 * the chunk size PrimaryLogPG::finish_ctx() would. ec_dynamic_chunk_size_max
 * is kept small so that objects of a few stripes reach it.
 */
class TestECDynamicChunkSize : public ECPeeringTestFixture,
                               public ::testing::WithParamInterface<BackendConfig> {
public:
  TestECDynamicChunkSize() {
    const auto& config = GetParam();
    k = config.k;
    m = config.m;
    stripe_unit = config.stripe_unit;
    ec_plugin = config.ec_plugin;
    ec_technique = config.ec_technique;
    pool_flags = config.pool_flags;
    ec_dynamic_chunk_size_max = 8 * stripe_unit;
  }

protected:
  uint64_t max_chunk_size() const {
    return ec_dynamic_chunk_size_max;
  }

  uint64_t default_stripe_width() const {
    return k * stripe_unit;
  }

  uint64_t chunk_size_for(uint64_t size_hint) const {
    return get_pool().get_ec_chunk_size_for_object_size(size_hint);
  }

  std::string random_data(uint64_t size) {
    if (size == 0) {
      return {};
    }
    bufferlist bl = create_random_buffer(size);
    return std::string(bl.c_str(), bl.length());
  }

  /* Check the chunk size recorded by the primary and the size of every
   * shard against the geometry of chunk_size. */
  void check_layout(const std::string& obj_name,
                    uint64_t object_size,
                    uint64_t chunk_size,
                    const std::set<int>& skip_shards = {}) {
    SCOPED_TRACE(fmt::format("{} size {} chunk size {}",
                             obj_name, object_size, chunk_size));
    const int primary = get_primary_shard_from_osdmap();
    ASSERT_GE(primary, 0);
    const uint64_t recorded = chunk_size == stripe_unit ? 0 : chunk_size;
    object_info_t oi = read_shard_object_info(obj_name, primary);
    EXPECT_EQ(recorded, oi.ec_chunk_size);
    EXPECT_EQ(object_size, oi.size);

    ECUtil::stripe_info_base_t base(k, m, default_stripe_width());
    const ECUtil::stripe_info_t sinfo = base.for_chunk_size(chunk_size);
    const hobject_t hoid = make_test_object(obj_name);
    for (int shard = 0; shard < k + m; ++shard) {
      if (skip_shards.contains(shard)) {
        continue;
      }
      struct stat st;
      ghobject_t ghoid(hoid, ghobject_t::NO_GEN, shard_id_t(shard));
      ASSERT_EQ(0, store->stat(chs[shard], ghoid, &st)) << "shard " << shard;
      EXPECT_EQ(sinfo.object_size_to_shard_size(object_size, shard_id_t(shard)),
                static_cast<uint64_t>(st.st_size))
        << "shard " << shard;
    }
  }

  /* Read the whole object and a few ranges that start and end inside
   * chunks. */
  void verify_ranges(const std::string& obj_name, const std::string& data) {
    verify_object(obj_name, data, 0, data.size());
    const uint64_t size = data.size();
    for (uint64_t off : {uint64_t(1), stripe_unit - 1, 3 * stripe_unit + 7,
                         size / 3, size / 2 + 5}) {
      if (off >= size) {
        continue;
      }
      const uint64_t len = std::min(size - off, 5 * stripe_unit + 3);
      SCOPED_TRACE(fmt::format("read {}~{}", off, len));
      verify_object(obj_name, data.substr(off, len), off, size);
    }
  }

  /* Restores a configuration option when a test ends. */
  class ConfigGuard {
    ECPeeringTestFixture &fixture;
    std::string option;
    std::string saved;
  public:
    ConfigGuard(ECPeeringTestFixture &fixture, const std::string &option,
                const std::string &value)
      : fixture(fixture), option(option) {
      g_ceph_context->_conf.get_val(option, &saved);
      fixture.set_config(option, value);
    }
    ~ConfigGuard() {
      fixture.set_config(option, saved);
    }
  };

  /* Write objects while osd is down, bring it back and recover them. */
  void recover_after_writes(int osd, const std::vector<uint64_t>& sizes) {
    ASSERT_TRUE(all_shards_active());
    mark_osd_down(osd);

    std::vector<std::string> names;
    std::vector<std::string> datas;
    for (uint64_t size : sizes) {
      names.push_back(fmt::format("recover_osd{}_{}", osd, size));
      datas.push_back(random_data(size));
      ASSERT_EQ(0, create_and_write(names.back(), datas.back()));
    }

    mark_osd_up(osd);
    run_parallel_recovery_and_verify_callbacks(names, osd, datas);

    for (size_t i = 0; i < names.size(); ++i) {
      check_layout(names[i], sizes[i], chunk_size_for(sizes[i]));
      verify_ranges(names[i], datas[i]);
      EXPECT_FALSE(scrub_object(names[i])) << names[i];
    }
  }

  std::vector<uint64_t> recovery_sizes() const {
    return {1,
            stripe_unit + 3,
            2 * default_stripe_width() + 100,
            k * max_chunk_size(),
            2 * k * max_chunk_size() + 3 * stripe_unit + 1};
  }
};

TEST_P(TestECDynamicChunkSize, ChunkSizeFollowsObjectSize) {
  ASSERT_TRUE(all_shards_active());

  const uint64_t sw = default_stripe_width();
  const uint64_t max_sw = k * max_chunk_size();
  for (uint64_t size : {uint64_t(1), sw - 1, sw, sw + 1, 3 * sw + 5,
                        max_sw - 1, max_sw, 2 * max_sw + 4097}) {
    const std::string name = fmt::format("obj_{}", size);
    const std::string data = random_data(size);
    ASSERT_EQ(0, create_and_write(name, data));

    const uint64_t expected_chunk_size = std::clamp(
      p2roundup((size + k - 1) / k, pg_pool_t::EC_DYNAMIC_CHUNK_SIZE_ALIGN),
      stripe_unit, max_chunk_size());
    ASSERT_EQ(expected_chunk_size, chunk_size_for(size));
    check_layout(name, size, expected_chunk_size);
    verify_ranges(name, data);
  }
}

TEST_P(TestECDynamicChunkSize, AllocationHintSetsChunkSize) {
  ASSERT_TRUE(all_shards_active());

  const std::string name = "hinted";
  const uint64_t hint = k * max_chunk_size();
  expected_object_size = hint;
  std::string data = random_data(100);
  ASSERT_EQ(0, create_and_write(name, data));
  expected_object_size = 0;
  check_layout(name, data.size(), max_chunk_size());

  // Fill the object in pieces, as a client that sent the hint would.
  while (data.size() < hint) {
    const std::string more = random_data(std::min<uint64_t>(
      hint - data.size(), 3 * stripe_unit + 11));
    ASSERT_EQ(0, write(name, data.size(), more, data.size()));
    data += more;
  }
  check_layout(name, data.size(), max_chunk_size());
  verify_ranges(name, data);
  EXPECT_FALSE(scrub_object(name));
}

TEST_P(TestECDynamicChunkSize, FirstDataWriteChoosesChunkSize) {
  ASSERT_TRUE(all_shards_active());

  const std::string name = "empty_first";
  ASSERT_EQ(0, create_and_write(name, ""));
  check_layout(name, 0, stripe_unit);

  const std::string data = random_data(3 * default_stripe_width());
  ASSERT_EQ(0, write(name, 0, data, 0));
  check_layout(name, data.size(), chunk_size_for(data.size()));
  verify_ranges(name, data);
}

TEST_P(TestECDynamicChunkSize, LaterWritesKeepChunkSize) {
  ASSERT_TRUE(all_shards_active());

  const std::string name = "rewritten";
  std::string model = random_data(3 * default_stripe_width());
  ASSERT_EQ(0, create_and_write(name, model));
  const uint64_t chunk_size = chunk_size_for(model.size());
  check_layout(name, model.size(), chunk_size);

  auto apply = [&](uint64_t offset, const std::string& data) {
    if (model.size() < offset) {
      model.resize(offset, '\0');
    }
    model.replace(offset, std::min(data.size(), model.size() - offset), data);
    if (model.size() < offset + data.size()) {
      model.append(data, model.size() - offset);
    }
  };

  // Overwrite across a chunk boundary.
  {
    const uint64_t off = chunk_size - 100;
    const std::string data = random_data(200);
    ASSERT_EQ(0, write(name, off, data, model.size()));
    apply(off, data);
  }
  // Append well past the object's first stripe.
  {
    const std::string data = random_data(2 * k * chunk_size + 1234);
    ASSERT_EQ(0, write(name, model.size(), data, model.size()));
    apply(model.size(), data);
  }
  check_layout(name, model.size(), chunk_size);
  verify_ranges(name, model);

  // Truncate into the first chunk, then write past the end.
  {
    const uint64_t truncate_to = chunk_size / 2 + 1;
    const std::string data = random_data(stripe_unit);
    const uint64_t off = k * chunk_size + 5;
    ASSERT_EQ(0, truncate_and_write(name, model.size(), truncate_to,
                                    {{off, data}}));
    model.resize(truncate_to);
    apply(off, data);
  }
  check_layout(name, model.size(), chunk_size);
  verify_ranges(name, model);
  EXPECT_FALSE(scrub_object(name));
}

TEST_P(TestECDynamicChunkSize, SnapshotRollback) {
  ASSERT_TRUE(all_shards_active());

  const std::string name = "snapped";
  const std::string original = random_data(5 * default_stripe_width() + 17);
  ASSERT_EQ(0, create_and_write(name, original));
  const uint64_t chunk_size = chunk_size_for(original.size());

  ASSERT_EQ(0, create_snapshot(name, original.size()));
  const std::string overwrite = random_data(2 * stripe_unit);
  ASSERT_EQ(0, write(name, chunk_size - stripe_unit, overwrite,
                     original.size()));

  ASSERT_EQ(0, rollback(name, original.size()));
  check_layout(name, original.size(), chunk_size);
  verify_ranges(name, original);

  // Rewrite the head with another chunk size, then roll back again.
  const std::string rewritten = random_data(stripe_unit + 3);
  ASSERT_EQ(0, remove_and_write(name, rewritten));
  ASSERT_NE(chunk_size, chunk_size_for(rewritten.size()));
  check_layout(name, rewritten.size(), chunk_size_for(rewritten.size()));
  verify_ranges(name, rewritten);

  ASSERT_EQ(0, rollback(name, original.size()));
  check_layout(name, original.size(), chunk_size);
  verify_ranges(name, original);
}

/* Removing an object and writing it again in one transaction discards its
 * shards, so the object gets the chunk size of its new contents. */
TEST_P(TestECDynamicChunkSize, RewriteChoosesChunkSizeAgain) {
  ASSERT_TRUE(all_shards_active());

  const std::string name = "rewritten";
  const std::string first = random_data(k * max_chunk_size());
  ASSERT_EQ(0, create_and_write(name, first));
  check_layout(name, first.size(), chunk_size_for(first.size()));

  for (uint64_t size : {stripe_unit + 3, 3 * default_stripe_width() + 5,
                        2 * k * max_chunk_size() + 1}) {
    SCOPED_TRACE(fmt::format("rewrite with {} bytes", size));
    const std::string data = random_data(size);
    ASSERT_EQ(0, remove_and_write(name, data));
    check_layout(name, size, chunk_size_for(size));
    verify_ranges(name, data);
  }
  EXPECT_FALSE(scrub_object(name));
}

TEST_P(TestECDynamicChunkSize, RecoverDataShard) {
  recover_after_writes(1, recovery_sizes());
}

TEST_P(TestECDynamicChunkSize, RecoverParityShard) {
  recover_after_writes(k, recovery_sizes());
}

/* The primary itself is missing the objects, so recovery starts without an
 * object context and learns the chunk size from its first read. */
TEST_P(TestECDynamicChunkSize, RecoverPrimary) {
  recover_after_writes(0, recovery_sizes());
}

/* Each recovery pass reads one default chunk per shard, so large chunks take
 * several passes. */
TEST_P(TestECDynamicChunkSize, RecoverInSmallPasses) {
  ConfigGuard guard(*this, "osd_recovery_max_chunk",
                    std::to_string(default_stripe_width()));
  const std::vector<uint64_t> sizes = {
    k * max_chunk_size() - 1,
    3 * k * max_chunk_size() + 5,
  };
  recover_after_writes(1, sizes);
  recover_after_writes(0, sizes);
}

TEST_P(TestECDynamicChunkSize, DivergentAppendsRollBack) {
  ASSERT_TRUE(all_shards_active());

  const int failing_shard = k + m - 1;
  const int blocked_shard = 1;
  const std::string name = "divergent";
  const std::string original = random_data(2 * default_stripe_width() + 5);
  ASSERT_EQ(0, create_and_write(name, original));
  const uint64_t chunk_size = chunk_size_for(original.size());

  suspend_primary_to_osd(blocked_shard);
  const std::string append1 = random_data(k * chunk_size);
  ASSERT_EQ(-EINPROGRESS,
            write(name, original.size(), append1, original.size()));
  const std::string append2 = random_data(stripe_unit + 9);
  const uint64_t size1 = original.size() + append1.size();
  ASSERT_EQ(-EINPROGRESS, write(name, size1, append2, size1));
  mark_osd_down(failing_shard);
  unsuspend_primary_to_osd(blocked_shard);
  event_loop->run_until_idle();
  ASSERT_TRUE(all_shards_active());

  verify_object(name, original, 0, original.size());
  check_layout(name, original.size(), chunk_size, {failing_shard});
}

TEST_P(TestECDynamicChunkSize, DivergentOverwritesRollBack) {
  ASSERT_TRUE(all_shards_active());

  const int failing_shard = k + m - 1;
  const std::string name = "divergent_overwrite";
  const std::string original = random_data(k * max_chunk_size());
  ASSERT_EQ(0, create_and_write(name, original));
  const uint64_t chunk_size = chunk_size_for(original.size());
  ASSERT_EQ(max_chunk_size(), chunk_size);

  suspend_primary_to_osd(1);
  // Within the second chunk, so only data shard 1 and the parity shards are
  // written, then across the boundary of the first two chunks.
  const std::string patch1 = random_data(stripe_unit + 5);
  ASSERT_EQ(-EINPROGRESS,
            write(name, chunk_size + 100, patch1, original.size()));
  const std::string patch2 = random_data(2 * stripe_unit);
  ASSERT_EQ(-EINPROGRESS,
            write(name, chunk_size - stripe_unit, patch2, original.size()));
  mark_osd_down(failing_shard);
  unsuspend_primary_to_osd(1);
  event_loop->run_until_idle();
  ASSERT_TRUE(all_shards_active());

  verify_ranges(name, original);
  check_layout(name, original.size(), chunk_size, {failing_shard});
}

TEST_P(TestECDynamicChunkSize, DivergentTruncateRollsBack) {
  ASSERT_TRUE(all_shards_active());

  const int failing_shard = k + m - 1;
  const std::string name = "divergent_truncate";
  const std::string original = random_data(k * max_chunk_size() + 3 * stripe_unit);
  ASSERT_EQ(0, create_and_write(name, original));
  const uint64_t chunk_size = chunk_size_for(original.size());

  suspend_primary_to_osd(1);
  const uint64_t truncate_to = chunk_size / 2 + 1;
  const std::string data = random_data(stripe_unit);
  ASSERT_EQ(-EINPROGRESS,
            truncate_and_write(name, original.size(), truncate_to,
                               {{truncate_to + 7, data}}));
  mark_osd_down(failing_shard);
  unsuspend_primary_to_osd(1);
  event_loop->run_until_idle();
  ASSERT_TRUE(all_shards_active());

  verify_ranges(name, original);
  check_layout(name, original.size(), chunk_size, {failing_shard});
}

/* The rolled back write chose the chunk size of an empty object. */
TEST_P(TestECDynamicChunkSize, DivergentFirstWriteRollsBack) {
  ASSERT_TRUE(all_shards_active());

  const int failing_shard = k + m - 1;
  const std::string name = "divergent_first";
  ASSERT_EQ(0, create_and_write(name, ""));

  suspend_primary_to_osd(1);
  const std::string data = random_data(k * max_chunk_size());
  ASSERT_EQ(-EINPROGRESS, write(name, 0, data, 0));
  const std::string append = random_data(stripe_unit + 1);
  ASSERT_EQ(-EINPROGRESS, write(name, data.size(), append, data.size()));
  mark_osd_down(failing_shard);
  unsuspend_primary_to_osd(1);
  event_loop->run_until_idle();
  ASSERT_TRUE(all_shards_active());

  check_layout(name, 0, stripe_unit, {failing_shard});

  // The object is empty again, so the next write chooses afresh.
  const std::string again = random_data(3 * default_stripe_width() + 11);
  ASSERT_EQ(0, write(name, 0, again, 0));
  check_layout(name, again.size(), chunk_size_for(again.size()),
               {failing_shard});
  verify_ranges(name, again);
}

/* The rolled back write removed the object and wrote it again with
 * another chunk size. */
TEST_P(TestECDynamicChunkSize, DivergentRewriteRollsBack) {
  ASSERT_TRUE(all_shards_active());

  const int failing_shard = k + m - 1;
  const std::string name = "divergent_rewrite";
  const std::string original = random_data(k * max_chunk_size() - 17);
  ASSERT_EQ(0, create_and_write(name, original));
  const uint64_t chunk_size = chunk_size_for(original.size());

  suspend_primary_to_osd(1);
  const std::string rewritten = random_data(stripe_unit + 3);
  ASSERT_NE(chunk_size, chunk_size_for(rewritten.size()));
  ASSERT_EQ(-EINPROGRESS, remove_and_write(name, rewritten));
  const std::string append = random_data(2 * stripe_unit);
  ASSERT_EQ(-EINPROGRESS,
            write(name, rewritten.size(), append, rewritten.size()));
  mark_osd_down(failing_shard);
  unsuspend_primary_to_osd(1);
  event_loop->run_until_idle();
  ASSERT_TRUE(all_shards_active());

  verify_ranges(name, original);
  check_layout(name, original.size(), chunk_size, {failing_shard});
}

TEST_P(TestECDynamicChunkSize, Scrub) {
  ASSERT_TRUE(all_shards_active());

  const uint64_t max_sw = k * max_chunk_size();
  for (uint64_t size : {default_stripe_width() + 1, max_sw, 2 * max_sw + 7}) {
    const std::string name = fmt::format("scrub_{}", size);
    ASSERT_EQ(0, create_and_write(name, random_data(size)));
    EXPECT_FALSE(scrub_object(name)) << name;
  }

  // Only plugins that support CRC based parity checks detect this.
  if (ec_plugin == "isa") {
    const std::string name = fmt::format("scrub_{}", max_sw);
    corrupt_shard_data(make_test_object(name),
                       pg_shard_t(k, shard_id_t(k)));
    EXPECT_TRUE(scrub_object(name));
  }
}

/* A direct read on a data shard returns the part of the range that the
 * shard holds in the object's own geometry. */
TEST_P(TestECDynamicChunkSize, DirectRead) {
  ASSERT_TRUE(all_shards_active());

  const std::string name = "direct";
  const uint64_t size = 3 * default_stripe_width() + 5;
  const uint64_t chunk_size = chunk_size_for(size);
  ASSERT_NE(stripe_unit, chunk_size);
  const std::string data = random_data(size);
  ASSERT_EQ(0, create_and_write(name, data));

  const hobject_t hoid = make_test_object(name);
  for (int shard = 0; shard < k; ++shard) {
    const uint64_t start = shard * chunk_size;
    if (start >= size) {
      break;
    }
    SCOPED_TRACE(fmt::format("shard {}", shard));
    auto *ec_switch = dynamic_cast<ECSwitch*>(backends.at(shard).get());
    ASSERT_NE(nullptr, ec_switch);
    bufferlist bl;
    ASSERT_GE(ec_switch->objects_read_local(
                hoid, 0, size, CEPH_OSD_RMW_FLAG_EC_DIRECT_READ, &bl,
                chunk_size),
              0);
    const uint64_t len = std::min(chunk_size, size - start);
    ASSERT_EQ(len, bl.length());
    EXPECT_EQ(data.substr(start, len), std::string(bl.c_str(), bl.length()));
  }
}

namespace {
constexpr uint64_t kDynamicFlags = pg_pool_t::FLAG_EC_OVERWRITES |
                                   pg_pool_t::FLAG_EC_OPTIMIZATIONS |
                                   pg_pool_t::FLAG_EC_DYNAMIC_CHUNK_SIZE;

const std::vector<BackendConfig> kDynamicChunkSizeConfigs = {
  {PGBackendTestFixture::EC, "isa", "reed_sol_van", kDynamicFlags, 4096, 4, 2, "ISA_k4m2_su4k"},
  {PGBackendTestFixture::EC, "isa", "reed_sol_van", kDynamicFlags, 16384, 4, 2, "ISA_k4m2_su16k"},
  {PGBackendTestFixture::EC, "isa", "reed_sol_van", kDynamicFlags, 4096, 2, 1, "ISA_k2m1_su4k"},
  {PGBackendTestFixture::EC, "isa", "reed_sol_van", kDynamicFlags, 4096, 8, 3, "ISA_k8m3_su4k"},
  {PGBackendTestFixture::EC, "jerasure", "reed_sol_van", kDynamicFlags, 4096, 4, 2, "Jerasure_k4m2_su4k"},
  {PGBackendTestFixture::EC, "jerasure", "reed_sol_van", kDynamicFlags, 8192, 3, 2, "Jerasure_k3m2_su8k"},
};
}

INSTANTIATE_TEST_SUITE_P(
  DynamicChunkSize,
  TestECDynamicChunkSize,
  ::testing::ValuesIn(kDynamicChunkSizeConfigs),
  [](const ::testing::TestParamInfo<BackendConfig>& info) {
    return info.param.label;
  }
);
