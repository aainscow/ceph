// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2024 Contributors
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <gtest/gtest.h>
#include "osd/ObjectPack.h"
#include "os/Transaction.h"
#include "include/buffer.h"
#include <sstream>

using namespace ceph::osd;
using namespace std;

// Helper functions for creating test objects
static hobject_t mk_obj(unsigned id, int64_t pool = 1) {
  hobject_t hoid;
  stringstream ss;
  ss << "obj_" << id;
  hoid.oid = ss.str();
  hoid.set_hash(id);
  hoid.pool = pool;
  hoid.snap = CEPH_NOSNAP;
  return hoid;
}

static ceph::buffer::list mk_data(size_t size, char fill = 'x') {
  ceph::buffer::list bl;
  string data(size, fill);
  bl.append(data);
  return bl;
}

// ============================================================================
// PackedObjectInfo Tests
// ============================================================================

TEST(ObjectPack, PackedObjectInfo_Basic) {
  PackedObjectInfo info(1, 1024, 512);
  
  ASSERT_TRUE(info.is_packed());
  ASSERT_EQ(1u, info.container_index);
  ASSERT_EQ(1024u, info.offset);
  ASSERT_EQ(512u, info.length);
}

TEST(ObjectPack, PackedObjectInfo_NotPacked) {
  PackedObjectInfo info;
  ASSERT_FALSE(info.is_packed());
}

TEST(ObjectPack, PackedObjectInfo_Encoding) {
  PackedObjectInfo info1(1, 2048, 1024);
  
  ceph::buffer::list bl;
  info1.encode(bl);
  
  auto p = bl.cbegin();
  PackedObjectInfo info2;
  info2.decode(p);
  
  ASSERT_EQ(info1.container_index, info2.container_index);
  ASSERT_EQ(info1.offset, info2.offset);
  ASSERT_EQ(info1.length, info2.length);
}

// ============================================================================
// ContainerReverseMapEntry Tests
// ============================================================================

TEST(ObjectPack, ContainerReverseMapEntry_Basic) {
  hobject_t logical = mk_obj(1);
  ContainerReverseMapEntry entry(logical, 512, false);
  
  ASSERT_EQ(logical, entry.logical_object_id);
  ASSERT_EQ(512u, entry.length);
  ASSERT_FALSE(entry.is_garbage);
}

TEST(ObjectPack, ContainerReverseMapEntry_Garbage) {
  hobject_t logical = mk_obj(2);
  ContainerReverseMapEntry entry(logical, 1024, true);
  
  ASSERT_TRUE(entry.is_garbage);
}

TEST(ObjectPack, ContainerReverseMapEntry_Encoding) {
  hobject_t logical = mk_obj(3);
  ContainerReverseMapEntry entry1(logical, 2048, false);
  
  ceph::buffer::list bl;
  entry1.encode(bl);
  
  auto p = bl.cbegin();
  ContainerReverseMapEntry entry2;
  entry2.decode(p);
  
  ASSERT_EQ(entry1.logical_object_id, entry2.logical_object_id);
  ASSERT_EQ(entry1.length, entry2.length);
  ASSERT_EQ(entry1.is_garbage, entry2.is_garbage);
}

// ============================================================================
// ContainerInfo Tests
// ============================================================================

TEST(ObjectPack, ContainerInfo_Basic) {
  ContainerInfo info(1, 4 * 1024 * 1024);
  
  ASSERT_EQ(1u, info.container_index);
  ASSERT_EQ(4 * 1024 * 1024u, info.total_size);
  ASSERT_EQ(0u, info.used_bytes);
  ASSERT_EQ(0u, info.garbage_bytes);
  ASSERT_EQ(0u, info.next_offset);
  ASSERT_FALSE(info.is_sealed);
  ASSERT_EQ(0.0, info.fragmentation_ratio());
  ASSERT_EQ(4 * 1024 * 1024u, info.available_space());
}

TEST(ObjectPack, ContainerInfo_Fragmentation) {
  ContainerInfo info(1, 4 * 1024 * 1024);
  
  info.used_bytes = 1024 * 1024;
  info.garbage_bytes = 256 * 1024;
  
  // Fragmentation = garbage / (used + garbage)
  double expected = 256.0 * 1024 / (1024 * 1024 + 256 * 1024);
  ASSERT_NEAR(expected, info.fragmentation_ratio(), 0.001);
}

TEST(ObjectPack, ContainerInfo_AvailableSpace) {
  ContainerInfo info(1, 1024 * 1024);
  
  info.next_offset = 512 * 1024;
  ASSERT_EQ(512 * 1024u, info.available_space());
  
  info.next_offset = 1024 * 1024;
  ASSERT_EQ(0u, info.available_space());
  
  info.next_offset = 2 * 1024 * 1024; // Overflow case
  ASSERT_EQ(0u, info.available_space());
}

TEST(ObjectPack, ContainerInfo_Encoding) {
  ContainerInfo info1(1, 4 * 1024 * 1024);
  info1.used_bytes = 1024;
  info1.garbage_bytes = 512;
  info1.next_offset = 2048;
  info1.is_sealed = true;
  
  hobject_t obj1 = mk_obj(1);
  info1.reverse_map[0] = ContainerReverseMapEntry(obj1, 1024, false);
  
  ceph::buffer::list bl;
  info1.encode(bl);
  
  auto p = bl.cbegin();
  ContainerInfo info2;
  info2.decode(p);
  
  ASSERT_EQ(info1.container_index, info2.container_index);
  ASSERT_EQ(info1.total_size, info2.total_size);
  ASSERT_EQ(info1.used_bytes, info2.used_bytes);
  ASSERT_EQ(info1.garbage_bytes, info2.garbage_bytes);
  ASSERT_EQ(info1.next_offset, info2.next_offset);
  ASSERT_EQ(info1.is_sealed, info2.is_sealed);
  ASSERT_EQ(info1.reverse_map.size(), info2.reverse_map.size());
}

// ============================================================================
// PackingConfig Tests
// ============================================================================

TEST(ObjectPack, PackingConfig_Defaults) {
  PackingConfig config;
  
  ASSERT_EQ(64 * 1024u, config.small_object_threshold);
  ASSERT_EQ(4 * 1024 * 1024u, config.container_size);
  ASSERT_EQ(64u, config.alignment_small);
  ASSERT_EQ(4096u, config.alignment_large);
  ASSERT_NEAR(0.20, config.gc_fragmentation_threshold, 0.001);
  ASSERT_EQ(256 * 1024u, config.gc_min_garbage_bytes);
}

TEST(ObjectPack, PackingConfig_Alignment) {
  PackingConfig config;
  
  // Small objects use 64-byte alignment
  ASSERT_EQ(64u, config.get_alignment(1024));
  ASSERT_EQ(64u, config.get_alignment(2047));
  
  // Large objects use 4KB alignment
  ASSERT_EQ(4096u, config.get_alignment(2048));
  ASSERT_EQ(4096u, config.get_alignment(32 * 1024));
}

TEST(ObjectPack, PackingConfig_AlignOffset) {
  PackingConfig config;
  
  // 64-byte alignment
  ASSERT_EQ(0u, config.align_offset(0, 64));
  ASSERT_EQ(64u, config.align_offset(1, 64));
  ASSERT_EQ(64u, config.align_offset(63, 64));
  ASSERT_EQ(64u, config.align_offset(64, 64));
  ASSERT_EQ(128u, config.align_offset(65, 64));
  
  // 4KB alignment
  ASSERT_EQ(0u, config.align_offset(0, 4096));
  ASSERT_EQ(4096u, config.align_offset(1, 4096));
  ASSERT_EQ(4096u, config.align_offset(4095, 4096));
  ASSERT_EQ(4096u, config.align_offset(4096, 4096));
  ASSERT_EQ(8192u, config.align_offset(4097, 4096));
}

// ============================================================================
// ObjectPackEngine Tests
// ============================================================================

TEST(ObjectPack, Engine_ShouldPackObject) {
  PackingConfig config;
  config.small_object_threshold = 64 * 1024;
  ObjectPackEngine engine(config);
  
  ASSERT_FALSE(engine.should_pack_object(0));
  ASSERT_TRUE(engine.should_pack_object(1));
  ASSERT_TRUE(engine.should_pack_object(1024));
  ASSERT_TRUE(engine.should_pack_object(64 * 1024));
  ASSERT_FALSE(engine.should_pack_object(64 * 1024 + 1));
  ASSERT_FALSE(engine.should_pack_object(1024 * 1024));
}

TEST(ObjectPack, Engine_PlanWriteRaw_ExistingContainer) {
  ObjectPackEngine engine;
  
  ContainerInfo container_info(1, 4 * 1024 * 1024);
  container_info.next_offset = 1024;
  
  hobject_t obj = mk_obj(1);
  ceph::buffer::list data = mk_data(512);
  
  PackResult result = engine.plan_write_raw(obj, data, container_info);
  
  ASSERT_TRUE(result.success);
  ASSERT_TRUE(result.packed_info.has_value());
  ASSERT_EQ(1u, result.packed_info->container_index);
  // 512 bytes at offset 1024 should stay at 1024 (already 64-byte aligned)
  ASSERT_EQ(1024u, result.packed_info->offset);
  ASSERT_EQ(512u, result.packed_info->length);
}

TEST(ObjectPack, Engine_PlanWriteRaw_NoContainer) {
  ObjectPackEngine engine;
  
  hobject_t obj = mk_obj(1);
  ceph::buffer::list data = mk_data(1024);
  
  PackResult result = engine.plan_write_raw(obj, data, std::nullopt);
  
  // Should fail because no container is provided
  ASSERT_FALSE(result.success);
  ASSERT_FALSE(result.error_message.empty());
}

TEST(ObjectPack, Engine_PlanWriteRaw_TooLarge) {
  PackingConfig config;
  config.small_object_threshold = 1024;
  ObjectPackEngine engine(config);
  
  ContainerInfo container_info(1, 4 * 1024 * 1024);
  hobject_t obj = mk_obj(1);
  ceph::buffer::list data = mk_data(2048);
  
  PackResult result = engine.plan_write_raw(obj, data, container_info);
  
  ASSERT_FALSE(result.success);
  ASSERT_FALSE(result.error_message.empty());
}

TEST(ObjectPack, Engine_PlanWrite_WithOp) {
  ObjectPackEngine engine;
  
  ContainerInfo container_info(1, 4 * 1024 * 1024);
  container_info.next_offset = 0;
  
  hobject_t obj = mk_obj(1);
  ghobject_t gobj(obj);
  coll_t cid;
  ceph::buffer::list data = mk_data(512);
  
  // Create a fake Op for testing
  ceph::os::Transaction::Op op;
  memset(&op, 0, sizeof(op));
  op.op = ceph::os::Transaction::OP_WRITE;
  op.off = 0;
  op.len = 512;
  
  PackResult result = engine.plan_write(op, cid, gobj, data, container_info);
  
  ASSERT_TRUE(result.success);
  ASSERT_TRUE(result.packed_info.has_value());
  ASSERT_EQ(1u, result.packed_info->container_index);
  ASSERT_EQ(512u, result.packed_info->length);
}

TEST(ObjectPack, Engine_PlanWrite_WrongOpType) {
  ObjectPackEngine engine;
  
  ContainerInfo container_info(1, 4 * 1024 * 1024);
  hobject_t obj = mk_obj(1);
  ghobject_t gobj(obj);
  coll_t cid;
  ceph::buffer::list data = mk_data(512);
  
  // Create a non-write Op
  ceph::os::Transaction::Op op;
  memset(&op, 0, sizeof(op));
  op.op = ceph::os::Transaction::OP_TOUCH;  // Not a write
  op.len = 512;
  
  PackResult result = engine.plan_write(op, cid, gobj, data, container_info);
  
  ASSERT_FALSE(result.success);
  ASSERT_TRUE(result.error_message.find("OP_WRITE") != std::string::npos);
}

TEST(ObjectPack, Engine_PlanWrite_DataLengthMismatch) {
  ObjectPackEngine engine;
  
  ContainerInfo container_info(1, 4 * 1024 * 1024);
  hobject_t obj = mk_obj(1);
  ghobject_t gobj(obj);
  coll_t cid;
  ceph::buffer::list data = mk_data(512);
  
  // Op says length is 1024, but data is only 512
  ceph::os::Transaction::Op op;
  memset(&op, 0, sizeof(op));
  op.op = ceph::os::Transaction::OP_WRITE;
  op.off = 0;
  op.len = 1024;  // Mismatch with data.length()
  
  PackResult result = engine.plan_write(op, cid, gobj, data, container_info);
  
  ASSERT_FALSE(result.success);
  ASSERT_TRUE(result.error_message.find("length") != std::string::npos);
}

TEST(ObjectPack, Engine_PlanRead) {
  ObjectPackEngine engine;
  
  hobject_t obj = mk_obj(1);
  PackedObjectInfo packed_info(1, 2048, 1024);
  
  PackResult result = engine.plan_read(obj, packed_info);
  
  ASSERT_TRUE(result.success);
  ASSERT_TRUE(result.read_spec.has_value());
  ASSERT_EQ(1u, result.read_spec->container_index);
  ASSERT_EQ(2048u, result.read_spec->offset);
  ASSERT_EQ(1024u, result.read_spec->length);
}

TEST(ObjectPack, Engine_PlanRead_NotPacked) {
  ObjectPackEngine engine;
  
  hobject_t obj = mk_obj(1);
  PackedObjectInfo packed_info; // Not packed
  
  PackResult result = engine.plan_read(obj, packed_info);
  
  ASSERT_FALSE(result.success);
}

TEST(ObjectPack, Engine_PlanDelete) {
  ObjectPackEngine engine;
  
  hobject_t obj = mk_obj(1);
  PackedObjectInfo packed_info(1, 2048, 1024);
  
  ContainerInfo container_info(1, 4 * 1024 * 1024);
  container_info.used_bytes = 10 * 1024;
  container_info.garbage_bytes = 1024;
  
  PackResult result = engine.plan_delete(obj, packed_info, container_info);
  
  ASSERT_TRUE(result.success);
}

TEST(ObjectPack, Engine_NeedsGC) {
  PackingConfig config;
  config.gc_fragmentation_threshold = 0.25;
  config.gc_min_garbage_bytes = 1024;
  ObjectPackEngine engine(config);
  
  ContainerInfo info(1, 4 * 1024 * 1024);
  
  // No garbage
  info.used_bytes = 10 * 1024;
  info.garbage_bytes = 0;
  ASSERT_FALSE(engine.needs_gc(info));
  
  // High fragmentation but not enough garbage
  info.used_bytes = 1024;
  info.garbage_bytes = 512; // 33% fragmentation but < 1024 bytes
  ASSERT_FALSE(engine.needs_gc(info));
  
  // Enough garbage but low fragmentation
  info.used_bytes = 100 * 1024;
  info.garbage_bytes = 2 * 1024; // Only 2% fragmentation
  ASSERT_FALSE(engine.needs_gc(info));
  
  // Both conditions met
  info.used_bytes = 3 * 1024;
  info.garbage_bytes = 2 * 1024; // 40% fragmentation and > 1024 bytes
  ASSERT_TRUE(engine.needs_gc(info));
}

TEST(ObjectPack, Engine_PlanPromote) {
  PackingConfig config;
  config.small_object_threshold = 1024;
  ObjectPackEngine engine(config);
  
  hobject_t obj = mk_obj(1);
  PackedObjectInfo packed_info(1, 2048, 512);
  
  PackResult result = engine.plan_promote(obj, packed_info, 2048);
  
  ASSERT_TRUE(result.success);
}

TEST(ObjectPack, Engine_PlanPromote_StillSmall) {
  PackingConfig config;
  config.small_object_threshold = 2048;
  ObjectPackEngine engine(config);
  
  hobject_t obj = mk_obj(1);
  PackedObjectInfo packed_info(1, 0, 512);
  
  PackResult result = engine.plan_promote(obj, packed_info, 1024);
  
  ASSERT_FALSE(result.success);
}

TEST(ObjectPack, Engine_ContainerNaming) {
  // Test container name generation
  std::string name = ObjectPackEngine::container_name_from_index(42);
  ASSERT_EQ(".pack_container_42", name);
  
  // Test container hobject generation
  hobject_t hobj = ObjectPackEngine::container_hobject_from_index(42, 1, "ns");
  ASSERT_EQ(".pack_container_42", hobj.oid.name);
  ASSERT_EQ(1, hobj.pool);
  ASSERT_EQ("ns", hobj.nspace);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
