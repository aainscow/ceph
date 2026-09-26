// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2016 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <gtest/gtest.h>
#include "osd/PGTransaction.h"
#include "osd/ECTransaction.h"
#include "common/debug.h"
#include "osd/ECBackend.h"

#include "test/unit.cc"

struct mydpp : public DoutPrefixProvider {
  std::ostream& gen_prefix(std::ostream& out) const override { return out << "foo"; }
  CephContext *get_cct() const override { return g_ceph_context; }
  unsigned get_subsys() const override { return ceph_subsys_osd; }
} dpp;

#define dout_context g_ceph_context

struct ECTestOp : ECCommon::RMWPipeline::Op {
  PGTransactionUPtr t;
};

TEST(ectransaction, two_writes_separated_append)
{
  hobject_t h;
  PGTransaction::ObjectOperation op;
  bufferlist a, b;
  a.append_zero(565760);
  op.buffer_updates.insert(0, a.length(), PGTransaction::ObjectOperation::BufferUpdate::Write{a, 0});
  b.append_zero(2437120);
  op.buffer_updates.insert(669856, b.length(), PGTransaction::ObjectOperation::BufferUpdate::Write{b, 0});

  pg_pool_t pool;
  pool.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
  ECUtil::stripe_info_base_t sinfo_base(2, 2, 8192, &pool);
  ECUtil::stripe_info_t sinfo = sinfo_base.for_default();
  shard_id_set shards;
  shards.insert_range(shard_id_t(), 4);
  ECTransaction::WritePlanObj plan(
    h,
    op,
    sinfo,
    shards,
    shards,
    false,
    0,
    std::nullopt,
    std::nullopt,
    0);

  generic_derr << "plan " << plan << dendl;

  ASSERT_FALSE(plan.to_read);
  ASSERT_EQ(4u, plan.will_write.shard_count());
}

TEST(ectransaction, two_writes_separated_misaligned_overwrite)
{
  hobject_t h;
  PGTransaction::ObjectOperation op;
  bufferlist a, b;
  a.append_zero(565760);
  op.buffer_updates.insert(0, a.length(), PGTransaction::ObjectOperation::BufferUpdate::Write{a, 0});
  b.append_zero(2437120);
  op.buffer_updates.insert(669856, b.length(), PGTransaction::ObjectOperation::BufferUpdate::Write{b, 0});

  pg_pool_t pool;
  pool.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
  ECUtil::stripe_info_base_t sinfo_base(2, 2, 8192, &pool, std::vector<shard_id_t>(0));
  ECUtil::stripe_info_t sinfo = sinfo_base.for_default();
  object_info_t oi;
  oi.size = 3112960;
  shard_id_set shards;
  shards.insert_range(shard_id_t(), 4);

  ECTransaction::WritePlanObj plan(
    h,
    op,
    sinfo,
    shards,
    shards,
    false,
    oi.size,
    oi,
    std::nullopt,
    0);

  generic_derr << "plan " << plan << dendl;

  ASSERT_EQ(2u, (*plan.to_read).shard_count());
  ASSERT_EQ(4u, plan.will_write.shard_count());
}

// Test writing to an object at an offset which is beyond the end of the
// current object.
TEST(ectransaction, partial_write)
{
  hobject_t h;
  PGTransaction::ObjectOperation op;
  bufferlist a;

  // Start by writing 8 bytes to the start of an object.
  a.append_zero(8);
  op.buffer_updates.insert(0, a.length(), PGTransaction::ObjectOperation::BufferUpdate::Write{a, 0});

  pg_pool_t pool;
  pool.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
  ECUtil::stripe_info_base_t sinfo_base(2, 1, 8192, &pool, std::vector<shard_id_t>(0));
  ECUtil::stripe_info_t sinfo = sinfo_base.for_default();
  object_info_t oi;
  oi.size = 8;
  shard_id_set shards;
  shards.insert_range(shard_id_t(), 3);

  ECTransaction::WritePlanObj plan(
    h,
    op,
    sinfo,
    shards,
    shards,
    false,
    0,
    oi,
    std::nullopt,
    0);

  generic_derr << "plan " << plan << dendl;

  // The object is empty, so we should have no reads and an 4k write.
  ASSERT_FALSE(plan.to_read);
  extent_set ref_write;
  ref_write.insert(0, EC_ALIGN_SIZE);
  ASSERT_EQ(2u, plan.will_write.shard_count());
  ASSERT_EQ(ref_write, plan.will_write.at(shard_id_t(0)));
  ASSERT_EQ(ref_write, plan.will_write.at(shard_id_t(2)));
}

TEST(ectransaction, overlapping_write_non_aligned)
{
  hobject_t h;
  PGTransaction::ObjectOperation op;
  bufferlist a;

  // Start by writing 8 bytes to the start of an object.
  a.append_zero(8);
  op.buffer_updates.insert(0, a.length(), PGTransaction::ObjectOperation::BufferUpdate::Write{a, 0});

  pg_pool_t pool;
  pool.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
  ECUtil::stripe_info_base_t sinfo_base(2, 1, 8192, &pool, std::vector<shard_id_t>(0));
  ECUtil::stripe_info_t sinfo = sinfo_base.for_default();
  object_info_t oi;
  oi.size = 8;
  shard_id_set shards;
  shards.insert_range(shard_id_t(), 4);
  ECTransaction::WritePlanObj plan(
    h,
    op,
    sinfo,
    shards,
    shards,
    false,
    8,
    oi,
    std::nullopt,
    0);

  generic_derr << "plan " << plan << dendl;

  // There should be no overlap of this read.
  ASSERT_EQ(1u, (*plan.to_read).shard_count());
  extent_set ref;
  ref.insert(0, EC_ALIGN_SIZE);
  ASSERT_EQ(2u, plan.will_write.shard_count());
  ASSERT_EQ(1u, (*plan.to_read).shard_count());
  ASSERT_EQ(ref, plan.will_write.at(shard_id_t(0)));
  ASSERT_EQ(ref, plan.will_write.at(shard_id_t(2)));
}

TEST(ectransaction, test_appending_write_non_aligned)
{
  hobject_t h;
  PGTransaction::ObjectOperation op;
  bufferlist a;

  // Start by writing 8 bytes to the start of an object.
  a.append_zero(4096);
  op.buffer_updates.insert(3*4096, a.length(), PGTransaction::ObjectOperation::BufferUpdate::Write{a, 0});

  pg_pool_t pool;
  pool.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
  ECUtil::stripe_info_base_t sinfo_base(2, 1, 8192, &pool, std::vector<shard_id_t>(0));
  ECUtil::stripe_info_t sinfo = sinfo_base.for_default();
  object_info_t oi;
  oi.size = 4*4096;
  shard_id_set shards;
  shards.insert_range(shard_id_t(), 4);
  ECTransaction::WritePlanObj plan(
    h,
    op,
    sinfo,
    shards,
    shards,
    false,
    8,
    oi,
    std::nullopt,
    0);

  generic_derr << "plan " << plan << dendl;

  // We are growing an option from zero with a hole.
  ASSERT_FALSE(plan.to_read);

  // The writes will cover not cover the zero parts
  ECUtil::shard_extent_set_t ref_write(sinfo.get_k_plus_m());
  ref_write[shard_id_t(1)].insert(4096, 4096);
  ref_write[shard_id_t(2)].insert(4096, 4096);
  ASSERT_EQ(ref_write, plan.will_write);
}

TEST(ectransaction, append_with_large_hole)
{
  hobject_t h;
  PGTransaction::ObjectOperation op;
  bufferlist a;

  // We have a 4k write quite a way after the current limit of a 4k object
  a.append_zero(4096);
  op.buffer_updates.insert(24*4096, a.length(), PGTransaction::ObjectOperation::BufferUpdate::Write{a, 0});

  pg_pool_t pool;
  pool.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
  ECUtil::stripe_info_base_t sinfo_base(2, 1, 8192, &pool, std::vector<shard_id_t>(0));
  ECUtil::stripe_info_t sinfo = sinfo_base.for_default();
  object_info_t oi;
  oi.size = 25*4096;
  shard_id_set shards;
  shards.insert_range(shard_id_t(), 4);
  ECTransaction::WritePlanObj plan(
    h,
    op,
    sinfo,
    shards,
    shards,
    false,
    4096,
    oi,
    std::nullopt,
    0);

  generic_derr << "plan " << plan << dendl;

  // Should not require any reads.
  ASSERT_FALSE(plan.to_read);

  // The writes will cover the new zero parts.
  ECUtil::shard_extent_set_t ref_write(sinfo.get_k_plus_m());
  ref_write[shard_id_t(0)].insert(12*4096, 4096);
  ref_write[shard_id_t(2)].insert(12*4096, 4096);
  ASSERT_EQ(ref_write, plan.will_write);
}

TEST(ectransaction, test_append_not_page_aligned_with_large_hole)
{
  hobject_t h;
  PGTransaction::ObjectOperation op;
  bufferlist a;

  // We have a 4k write quite a way after the current limit of a EC_ALIGN_SIZE object
  a.append_zero(EC_ALIGN_SIZE / 2);
  op.buffer_updates.insert(24 * EC_ALIGN_SIZE + EC_ALIGN_SIZE / 4, a.length(), PGTransaction::ObjectOperation::BufferUpdate::Write{a, 0});

  pg_pool_t pool;
  pool.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
  ECUtil::stripe_info_base_t sinfo_base(2, 1, 2 * EC_ALIGN_SIZE, &pool, std::vector<shard_id_t>(0));
  ECUtil::stripe_info_t sinfo = sinfo_base.for_default();
  object_info_t oi;
  oi.size = 25*EC_ALIGN_SIZE;
  shard_id_set shards;
  shards.insert_range(shard_id_t(), 3);
  ECTransaction::WritePlanObj plan(
    h,
    op,
    sinfo,
    shards,
    shards,
    false,
    EC_ALIGN_SIZE,
    oi,
    std::nullopt,
    0);

  generic_derr << "plan " << plan << dendl;

  // No reads (because not yet written)
  ASSERT_FALSE(plan.to_read);

  // Writes should grow to 4k
  ECUtil::shard_extent_set_t ref_write(sinfo.get_k_plus_m());
  ref_write[shard_id_t(0)].insert(12*EC_ALIGN_SIZE, EC_ALIGN_SIZE);
  ref_write[shard_id_t(2)].insert(12*EC_ALIGN_SIZE, EC_ALIGN_SIZE);
  ASSERT_EQ(ref_write, plan.will_write);
}

TEST(ectransaction, test_overwrite_with_missing)
{
  hobject_t h;
  PGTransaction::ObjectOperation op, op2;
  bufferlist a;

  // We have a 4k write quite a way after the current limit of a 4k object
  a.append_zero(14 * (EC_ALIGN_SIZE / 4));
  op.buffer_updates.insert(0, a.length(), PGTransaction::ObjectOperation::BufferUpdate::Write{a, 0});

  pg_pool_t pool;
  pool.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
  ECUtil::stripe_info_base_t sinfo_base(2, 1, 2 * EC_ALIGN_SIZE, &pool, std::vector<shard_id_t>(0));
  ECUtil::stripe_info_t sinfo = sinfo_base.for_default();
  object_info_t oi;
  oi.size = 42*(EC_ALIGN_SIZE / 4);
  shard_id_set shards;
  shards.insert(shard_id_t(0));
  shards.insert(shard_id_t(1));

  ECTransaction::WritePlanObj plan(
    h,
    op,
    sinfo,
    shards,
    shards,
    false,
    42*(EC_ALIGN_SIZE / 4),
    oi,
    std::nullopt,
    0);

  generic_derr << "plan " << plan << dendl;

  // No reads (because not yet written)
  ASSERT_TRUE(plan.to_read);
  ECUtil::shard_extent_set_t ref_read(sinfo.get_k_plus_m());
  ref_read[shard_id_t(1)].insert(EC_ALIGN_SIZE, EC_ALIGN_SIZE);
  ASSERT_EQ(ref_read, plan.to_read);

  // Writes should grow to 4k
  ECUtil::shard_extent_set_t ref_write(sinfo.get_k_plus_m());
  ref_write[shard_id_t(0)].insert(0, 2 * EC_ALIGN_SIZE);
  ref_write[shard_id_t(1)].insert(0, 2 * EC_ALIGN_SIZE);
  ASSERT_EQ(ref_write, plan.will_write);
}

TEST(ectransaction, truncate_to_bigger_without_write)
{
  hobject_t h;
  PGTransaction::ObjectOperation op;

  op.truncate = std::pair(8192, 8192);

  pg_pool_t pool;
  pool.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
  ECUtil::stripe_info_base_t sinfo_base(2, 2, 8192, &pool);
  ECUtil::stripe_info_t sinfo = sinfo_base.for_default();
  shard_id_set shards;
  shards.insert_range(shard_id_t(), 4);
  ECTransaction::WritePlanObj plan(
    h,
    op,
    sinfo,
    shards,
    shards,
    false,
    4096,
    std::nullopt,
    std::nullopt,
    0);

  generic_derr << "plan " << plan << dendl;

  ASSERT_FALSE(plan.to_read);
  ASSERT_EQ(0u, plan.will_write.shard_count());
}

TEST(ectransaction, truncate_to_smalelr_without_write) {
  hobject_t h;
  PGTransaction::ObjectOperation op;

  op.truncate = std::pair(EC_ALIGN_SIZE/4, EC_ALIGN_SIZE/4);

  pg_pool_t pool;
  pool.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
  ECUtil::stripe_info_base_t sinfo_base(2, 2, EC_ALIGN_SIZE*2, &pool);
  ECUtil::stripe_info_t sinfo = sinfo_base.for_default();
  shard_id_set shards;
  shards.insert_range(shard_id_t(), 4);
  ECTransaction::WritePlanObj plan(
    h,
    op,
    sinfo,
    shards,
    shards,
    false,
    16*EC_ALIGN_SIZE,
    std::nullopt,
    std::nullopt,
    0);

  generic_derr << "plan " << plan << dendl;

  ASSERT_TRUE(plan.to_read);
  ECUtil::shard_extent_set_t ref_read(sinfo.get_k_plus_m());
  ref_read[shard_id_t(0)].insert(0, EC_ALIGN_SIZE);
  ASSERT_EQ(ref_read, plan.to_read);

  // Writes should cover parity only.
  ECUtil::shard_extent_set_t ref_write(sinfo.get_k_plus_m());
  ref_write[shard_id_t(2)].insert(0, EC_ALIGN_SIZE);
  ref_write[shard_id_t(3)].insert(0, EC_ALIGN_SIZE);
  ASSERT_EQ(ref_write, plan.will_write);
}

TEST(ectransaction, delete_and_write_misaligned) {
  hobject_t h;
  PGTransaction::ObjectOperation op;
  bufferlist a;
  uint64_t new_size = 14 * (EC_ALIGN_SIZE / 4);

  // We have a 4k write quite a way after the current limit of a 4k object
  a.append_zero(new_size);
  op.buffer_updates.insert(0, new_size, PGTransaction::ObjectOperation::BufferUpdate::Write{a, 0});
  op.delete_first = true;

  pg_pool_t pool;
  pool.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
  ECUtil::stripe_info_base_t sinfo_base(2, 1, 2 * EC_ALIGN_SIZE, &pool, std::vector<shard_id_t>(0));
  ECUtil::stripe_info_t sinfo = sinfo_base.for_default();
  object_info_t oi;
  oi.size = new_size;
  shard_id_set shards;
  shards.insert_range(shard_id_t(0), 3);

  ECTransaction::WritePlanObj plan(
    h,
    op,
    sinfo,
    shards,
    shards,
    false,
    16*EC_ALIGN_SIZE,
    std::nullopt,
    std::nullopt,
    0);

  generic_derr << "plan " << plan << dendl;

  /* We are going to delete the object before writing it.  Best not write anything
   * from the old object... */
  ASSERT_FALSE(plan.to_read);

  // Writes should cover parity only.
  ECUtil::shard_extent_set_t ref_write(sinfo.get_k_plus_m());
  ref_write[shard_id_t(0)].insert(0, 2*EC_ALIGN_SIZE);
  ref_write[shard_id_t(1)].insert(0, 2*EC_ALIGN_SIZE);
  ref_write[shard_id_t(2)].insert(0, 2*EC_ALIGN_SIZE);
  ASSERT_EQ(ref_write, plan.will_write);
}

TEST(ectransaction, truncate_to_stripe) {
  hobject_t h;
  PGTransaction::ObjectOperation op;
  uint64_t new_size = 2 * EC_ALIGN_SIZE;

  // We have a 4k write quite a way after the current limit of a 4k object
  op.truncate.emplace(new_size, new_size);

  pg_pool_t pool;
  pool.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
  ECUtil::stripe_info_base_t sinfo_base(2, 1, 2 * EC_ALIGN_SIZE, &pool, std::vector<shard_id_t>(0));
  ECUtil::stripe_info_t sinfo = sinfo_base.for_default();
  object_info_t oi;
  oi.size = new_size;
  shard_id_set shards;
  shards.insert_range(shard_id_t(0), 3);

  ECTransaction::WritePlanObj plan(
    h,
    op,
    sinfo,
    shards,
    shards,
    false,
    16*EC_ALIGN_SIZE,
    std::nullopt,
    std::nullopt,
    0);

  generic_derr << "plan " << plan << dendl;

  /* We are going to delete the object before writing it.  Best not write anything
   * from the old object... */
  ASSERT_FALSE(plan.to_read);

  // Truncating to a whole shard - no writes needed.
  ECUtil::shard_extent_set_t ref_write(sinfo.get_k_plus_m());
  ASSERT_EQ(ref_write, plan.will_write);
}

TEST(ectransaction, truncate_then_write_one_shard) {
  hobject_t h;
  PGTransaction::ObjectOperation op;
  bufferlist a, b;

  // Simulate a sparsify operation that overwrites an existing object with data at
  // specific offsets, creating a sparse pattern.
  //
  // Initial object is 20k, with zeros at 0~4k, 8k~4k, 16k~4k
  //
  // The sparsify operation writes at offsets 4k~4k and 12k~4k.
  op.truncate = std::pair(0, 0);
  
  // First write at offset 4096, length 4KB (0~4096)
  a.append_zero(4096);
  op.buffer_updates.insert(4096, a.length(), PGTransaction::ObjectOperation::BufferUpdate::Write{a, 0});
  
  // Second write at offset 12288 (12KB), length 4KB
  b.append_zero(4096);
  op.buffer_updates.insert(12288, b.length(), PGTransaction::ObjectOperation::BufferUpdate::Write{b, 0});

  pg_pool_t pool;
  pool.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
  
  // EC configuration: k=2, m=1, chunk_size=4096 (matching FastEC profile)
  ECUtil::stripe_info_base_t sinfo_base(2, 1, 8192, &pool, std::vector<shard_id_t>(0));
  ECUtil::stripe_info_t sinfo = sinfo_base.for_default();
  
  // Set current object size to 16384 (16KB) - the object exists with this size
  object_info_t oi;
  oi.size = 16384;
  
  shard_id_set shards;
  shards.insert_range(shard_id_t(0), 3);  // k=2 + m=1 = 3 shards

  ECTransaction::WritePlanObj plan(
    h,
    op,
    sinfo,
    shards,
    shards,
    false,
    20480,  // current_size
    oi,
    std::nullopt,
    0);

  generic_derr << "plan " << plan << dendl;

  // With truncate 0, we're starting fresh - no reads should be required
  ASSERT_FALSE(plan.to_read);
  
  // Truncates are handled by the transaction generation. 
  ECUtil::shard_extent_set_t ref_write(sinfo.get_k_plus_m());
  ref_write[shard_id_t(1)].insert(0, 8192);
  ref_write[shard_id_t(2)].insert(0, 8192);
  
  ASSERT_EQ(ref_write, plan.will_write);
}

TEST(ectransaction, write_plan_object_chunk_size_full_object)
{
  hobject_t h;
  PGTransaction::ObjectOperation op;
  const uint64_t object_chunk = 1 << 20;
  bufferlist a;
  a.append_zero(4 * object_chunk);
  op.buffer_updates.insert(0, a.length(), PGTransaction::ObjectOperation::BufferUpdate::Write{a, 0});

  pg_pool_t pool;
  pool.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
  ECUtil::stripe_info_base_t sinfo_base(4, 2, 4 * EC_ALIGN_SIZE, &pool,
                                        std::vector<shard_id_t>(0));
  ECUtil::stripe_info_t sinfo = sinfo_base.for_chunk_size(object_chunk);
  shard_id_set shards;
  shards.insert_range(shard_id_t(), 6);

  ECTransaction::WritePlanObj plan(
    h, op, sinfo, shards, shards, false, 0, std::nullopt, std::nullopt, 0);

  ASSERT_EQ(object_chunk, plan.chunk_size);
  ASSERT_FALSE(plan.to_read);
  // One stripe: every shard receives exactly one chunk.
  ASSERT_EQ(6u, plan.will_write.shard_count());
  for (auto &&[shard, eset] : plan.will_write) {
    extent_set expected;
    expected.insert(0, object_chunk);
    ASSERT_EQ(expected, eset) << "shard " << shard;
  }
}

TEST(ectransaction, write_plan_object_chunk_size_overwrite)
{
  hobject_t h;
  PGTransaction::ObjectOperation op;
  const uint64_t object_chunk = 1 << 20;
  const uint64_t offset = object_chunk + object_chunk / 2;
  bufferlist a;
  a.append_zero(2 * EC_ALIGN_SIZE);
  op.buffer_updates.insert(offset, a.length(), PGTransaction::ObjectOperation::BufferUpdate::Write{a, 0});

  pg_pool_t pool;
  pool.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
  ECUtil::stripe_info_base_t sinfo_base(4, 2, 4 * EC_ALIGN_SIZE, &pool,
                                        std::vector<shard_id_t>(0));
  object_info_t oi;
  oi.size = 4 * object_chunk;
  shard_id_set shards;
  shards.insert_range(shard_id_t(), 6);

  // With the object's chunk size, the overwrite lands on data shard 1 only.
  ECTransaction::WritePlanObj plan(
    h, op, sinfo_base.for_chunk_size(object_chunk), shards, shards, false,
    oi.size, oi, std::nullopt, 0);
  extent_set expected;
  expected.insert(object_chunk / 2, 2 * EC_ALIGN_SIZE);
  ASSERT_TRUE(plan.will_write.contains(shard_id_t(1)));
  ASSERT_EQ(expected, plan.will_write.at(shard_id_t(1)));
  for (shard_id_t shard : {shard_id_t(0), shard_id_t(2), shard_id_t(3)}) {
    ASSERT_FALSE(plan.will_write.contains(shard)) << "shard " << shard;
  }
  for (shard_id_t shard : {shard_id_t(4), shard_id_t(5)}) {
    ASSERT_EQ(expected, plan.will_write.at(shard)) << "shard " << shard;
  }

  // The default geometry puts the same range on two different shards.
  ECTransaction::WritePlanObj default_plan(
    h, op, sinfo_base.for_default(), shards, shards, false, oi.size, oi,
    std::nullopt, 0);
  ASSERT_EQ(EC_ALIGN_SIZE, default_plan.chunk_size);
  ASSERT_EQ(4u, default_plan.will_write.shard_count());
}

TEST(ectransaction, write_plan_object_chunk_size_append)
{
  hobject_t h;
  PGTransaction::ObjectOperation op;
  const uint64_t object_chunk = 16 * EC_ALIGN_SIZE;
  const uint64_t orig_size = object_chunk + object_chunk / 2;
  bufferlist a;
  a.append_zero(2 * EC_ALIGN_SIZE);
  op.buffer_updates.insert(orig_size, a.length(), PGTransaction::ObjectOperation::BufferUpdate::Write{a, 0});

  pg_pool_t pool;
  pool.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
  ECUtil::stripe_info_base_t sinfo_base(4, 2, 4 * EC_ALIGN_SIZE, &pool,
                                        std::vector<shard_id_t>(0));
  const ECUtil::stripe_info_t sinfo = sinfo_base.for_chunk_size(object_chunk);
  object_info_t oi;
  oi.size = orig_size + a.length();
  shard_id_set shards;
  shards.insert_range(shard_id_t(), 6);

  ECTransaction::WritePlanObj plan(
    h, op, sinfo, shards, shards, false, orig_size, oi, std::nullopt, 0);
  generic_derr << "plan " << plan << dendl;

  ASSERT_EQ(oi.size, plan.projected_size);
  // The append continues data shard 1 half way through its chunk.
  const uint64_t shard_offset = object_chunk / 2;
  ECUtil::shard_extent_set_t ref_write(sinfo.get_k_plus_m());
  ref_write[shard_id_t(1)].insert(shard_offset, a.length());
  ref_write[shard_id_t(4)].insert(shard_offset, a.length());
  ref_write[shard_id_t(5)].insert(shard_offset, a.length());
  ASSERT_EQ(ref_write, plan.will_write);
  // The parity needs the data at the same offset of data shard 0; data
  // shards 2 and 3 hold nothing there.
  ASSERT_TRUE(plan.to_read);
  ECUtil::shard_extent_set_t ref_read(sinfo.get_k_plus_m());
  ref_read[shard_id_t(0)].insert(shard_offset, a.length());
  ASSERT_EQ(ref_read, *plan.to_read);
}

TEST(ectransaction, write_plan_object_chunk_size_truncate)
{
  hobject_t h;
  PGTransaction::ObjectOperation op;
  const uint64_t object_chunk = 16 * EC_ALIGN_SIZE;
  const uint64_t orig_size = 4 * object_chunk;
  const uint64_t new_size = object_chunk + EC_ALIGN_SIZE + 100;
  op.truncate = std::pair(new_size, new_size);

  pg_pool_t pool;
  pool.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
  ECUtil::stripe_info_base_t sinfo_base(4, 2, 4 * EC_ALIGN_SIZE, &pool,
                                        std::vector<shard_id_t>(0));
  const ECUtil::stripe_info_t sinfo = sinfo_base.for_chunk_size(object_chunk);
  object_info_t oi;
  oi.size = new_size;
  shard_id_set shards;
  shards.insert_range(shard_id_t(), 6);

  ECTransaction::WritePlanObj plan(
    h, op, sinfo, shards, shards, false, orig_size, oi, std::nullopt, 0);
  generic_derr << "plan " << plan << dendl;

  ASSERT_EQ(new_size, plan.projected_size);
  // Data shard 0 keeps its whole chunk and data shard 1 keeps two pages, so
  // the parity of the whole chunk is rebuilt from them.
  ASSERT_TRUE(plan.to_read);
  ECUtil::shard_extent_set_t ref_read(sinfo.get_k_plus_m());
  ref_read[shard_id_t(0)].insert(0, object_chunk);
  ref_read[shard_id_t(1)].insert(0, 2 * EC_ALIGN_SIZE);
  ASSERT_EQ(ref_read, *plan.to_read);
  ECUtil::shard_extent_set_t ref_write(sinfo.get_k_plus_m());
  ref_write[shard_id_t(4)].insert(0, object_chunk);
  ref_write[shard_id_t(5)].insert(0, object_chunk);
  ASSERT_EQ(ref_write, plan.will_write);
}
