  // -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <gtest/gtest.h>
#include "common/hybrid_interval_map.h"
#include "include/buffer.h"
#include <boost/container/flat_map.hpp>

// bl_split_merge for buffer::list operations
struct bl_split_merge {
  ceph::buffer::list split(
      uint64_t offset,
      uint64_t length,
      ceph::buffer::list &bl) const {
    ceph::buffer::list out;
    out.substr_of(bl, offset, length);
    return out;
  }

  bool can_merge(const ceph::buffer::list &left, const ceph::buffer::list &right) const {
    return true;
  }

  ceph::buffer::list merge(ceph::buffer::list &&left, ceph::buffer::list &&right) const {
    ceph::buffer::list bl{std::move(left)};
    bl.claim_append(right);
    return bl;
  }

  uint64_t length(const ceph::buffer::list &b) const { return b.length(); }
};

// Type alias for testing
using test_map = hybrid_interval_map<uint64_t, ceph::buffer::list, bl_split_merge,
                                     boost::container::flat_map, true>;

TEST(HybridExtentMap, EmptyMap) {
  test_map m;
  ASSERT_TRUE(m.empty());
  ASSERT_TRUE(m.is_single()) << "Empty map should be in single mode";
}

TEST(HybridExtentMap, SingleInsert) {
  test_map m;
  ceph::buffer::list bl;
  bl.append("test data", 9);
  
  m.insert(100, 9, std::move(bl));
  
  ASSERT_FALSE(m.empty());
  ASSERT_TRUE(m.is_single()) << "Single insert should stay in single mode";
  ASSERT_EQ(100u, m.get_start_off());
  ASSERT_EQ(109u, m.get_end_off());
}

TEST(HybridExtentMap, SingleInsertMergeAppend) {
  test_map m;
  
  ceph::buffer::list bl1;
  bl1.append("test", 4);
  m.insert(100, 4, std::move(bl1));
  ASSERT_TRUE(m.is_single()) << "First insert should be in single mode";
  
  ceph::buffer::list bl2;
  bl2.append("data", 4);
  m.insert(104, 4, std::move(bl2));  // Adjacent - should merge
  
  ASSERT_FALSE(m.empty());
  ASSERT_TRUE(m.is_single()) << "Merged adjacent inserts should stay in single mode";
  ASSERT_EQ(100u, m.get_start_off());
  ASSERT_EQ(108u, m.get_end_off());
  
  // Should have merged into single extent
  auto it = m.begin();
  ASSERT_NE(it, m.end());
  ASSERT_EQ(100u, it.get_off());
  ASSERT_EQ(8u, it.get_len());
  ASSERT_EQ(8u, it.get_val().length());
  
  ++it;
  ASSERT_EQ(it, m.end());
}

TEST(HybridExtentMap, SingleInsertMergePrepend) {
  test_map m;
  
  ceph::buffer::list bl1;
  bl1.append("data", 4);
  m.insert(104, 4, std::move(bl1));
  ASSERT_TRUE(m.is_single()) << "First insert should be in single mode";
  
  ceph::buffer::list bl2;
  bl2.append("test", 4);
  m.insert(100, 4, std::move(bl2));  // Adjacent before - should merge
  
  ASSERT_FALSE(m.empty());
  ASSERT_TRUE(m.is_single()) << "Merged adjacent inserts should stay in single mode";
  ASSERT_EQ(100u, m.get_start_off());
  ASSERT_EQ(108u, m.get_end_off());
  
  // Should have merged into single extent
  auto it = m.begin();
  ASSERT_NE(it, m.end());
  ASSERT_EQ(100u, it.get_off());
  ASSERT_EQ(8u, it.get_len());
  ASSERT_EQ(8u, it.get_val().length());
  
  ++it;
  ASSERT_EQ(it, m.end());
}

TEST(HybridExtentMap, MultipleNonAdjacent) {
  test_map m;
  
  ceph::buffer::list bl1;
  bl1.append("test", 4);
  m.insert(100, 4, std::move(bl1));
  ASSERT_TRUE(m.is_single()) << "First insert should be in single mode";
  
  ceph::buffer::list bl2;
  bl2.append("data", 4);
  m.insert(200, 4, std::move(bl2));  // Not adjacent - should upgrade to multi
  
  ASSERT_FALSE(m.empty());
  ASSERT_FALSE(m.is_single()) << "Non-adjacent inserts should upgrade to multi mode";
  ASSERT_EQ(100u, m.get_start_off());
  ASSERT_EQ(204u, m.get_end_off());
}

TEST(HybridExtentMap, EraseComplete) {
  test_map m;
  
  ceph::buffer::list bl;
  bl.append("test", 4);
  m.insert(100, 4, std::move(bl));
  ASSERT_TRUE(m.is_single()) << "Single insert should be in single mode";
  
  m.erase(100, 4);
  ASSERT_TRUE(m.empty());
  ASSERT_TRUE(m.is_single()) << "Empty after erase should stay in single mode";
}

TEST(HybridExtentMap, EraseNoOverlap) {
  test_map m;
  
  ceph::buffer::list bl;
  bl.append("test", 4);
  m.insert(100, 4, std::move(bl));
  ASSERT_TRUE(m.is_single()) << "Single insert should be in single mode";
  
  m.erase(200, 4);  // No overlap
  ASSERT_FALSE(m.empty());
  ASSERT_TRUE(m.is_single()) << "Erase with no overlap should stay in single mode";
}

TEST(HybridExtentMap, Iterator) {
  test_map m;
  
  ceph::buffer::list bl;
  bl.append("test data", 9);
  m.insert(100, 9, std::move(bl));
  ASSERT_TRUE(m.is_single()) << "Single insert should be in single mode";
  
  auto it = m.begin();
  ASSERT_NE(it, m.end());
  ASSERT_EQ(100u, it.get_off());
  ASSERT_EQ(9u, it.get_len());
  ASSERT_EQ(9u, it.get_val().length());
  
  ++it;
  ASSERT_EQ(it, m.end());
}

TEST(HybridExtentMap, IteratorMultiple) {
  test_map m;
  
  ceph::buffer::list bl1;
  bl1.append("test", 4);
  m.insert(100, 4, std::move(bl1));
  ASSERT_TRUE(m.is_single()) << "First insert should be in single mode";
  
  ceph::buffer::list bl2;
  bl2.append("data", 4);
  m.insert(200, 4, std::move(bl2));
  ASSERT_FALSE(m.is_single()) << "Non-adjacent insert should upgrade to multi mode";
  
  ceph::buffer::list bl3;
  bl3.append("more", 4);
  m.insert(300, 4, std::move(bl3));
  ASSERT_FALSE(m.is_single()) << "Should stay in multi mode";
  
  std::vector<std::tuple<uint64_t, uint64_t, uint64_t>> extents;
  for (auto it = m.begin(); it != m.end(); ++it) {
    extents.push_back({it.get_off(), it.get_len(), it.get_val().length()});
  }
  
  ASSERT_EQ(3u, extents.size());
  ASSERT_EQ(100u, std::get<0>(extents[0]));
  ASSERT_EQ(4u, std::get<1>(extents[0]));
  ASSERT_EQ(4u, std::get<2>(extents[0]));
}

TEST(HybridExtentMap, CopyConstructor) {
  test_map m1;
  
  ceph::buffer::list bl;
  bl.append("test", 4);
  m1.insert(100, 4, std::move(bl));
  ASSERT_TRUE(m1.is_single()) << "Single insert should be single mode";
  
  test_map m2(m1);
  ASSERT_FALSE(m2.empty());
  ASSERT_TRUE(m2.is_single()) << "Copy of single should be single";
  ASSERT_EQ(100u, m2.get_start_off());
}

TEST(HybridExtentMap, MoveConstructor) {
  test_map m1;
  
  ceph::buffer::list bl;
  bl.append("test", 4);
  m1.insert(100, 4, std::move(bl));
  ASSERT_TRUE(m1.is_single()) << "Single insert should be single mode";
  
  test_map m2(std::move(m1));
  ASSERT_FALSE(m2.empty());
  ASSERT_TRUE(m2.is_single()) << "Move of single should be single";
  ASSERT_EQ(100u, m2.get_start_off());
}

TEST(HybridExtentMap, Clear) {
  test_map m;
  
  ceph::buffer::list bl1;
  bl1.append("test", 4);
  m.insert(100, 4, std::move(bl1));
  ASSERT_TRUE(m.is_single()) << "Single insert should be single mode";
  
  ceph::buffer::list bl2;
  bl2.append("data", 4);
  m.insert(200, 4, std::move(bl2));
  ASSERT_FALSE(m.is_single()) << "Two intervals should be multi mode";
  
  m.clear();
  ASSERT_TRUE(m.empty());
  ASSERT_FALSE(m.is_single()) << "After clear, stays in multi mode (no downgrade)";
}

TEST(HybridExtentMap, IteratorContains) {
  test_map m;
  ceph::buffer::list bl;
  bl.append("data", 4);
  m.insert(10, 4, std::move(bl));
  ASSERT_TRUE(m.is_single()) << "Single insert should be single mode";
  
  auto it = m.begin();
  ASSERT_TRUE(it.contains(10, 4));
  ASSERT_TRUE(it.contains(11, 3));
  ASSERT_TRUE(it.contains(10, 3));
  ASSERT_TRUE(it.contains(11, 2));
  ASSERT_FALSE(it.contains(8, 2));
  ASSERT_FALSE(it.contains(14, 2));
  ASSERT_FALSE(it.contains(8, 3));
  ASSERT_FALSE(it.contains(13, 2));
}

TEST(HybridExtentMap, GetStartEndOff) {
  test_map m;
  
  ceph::buffer::list bl1;
  bl1.append("aaaaa", 5);
  m.insert(0, 5, std::move(bl1));
  ASSERT_TRUE(m.is_single()) << "Single insert should be single mode";
  ASSERT_EQ(0u, m.get_start_off());
  ASSERT_EQ(5u, m.get_end_off());
  
  ceph::buffer::list bl2;
  bl2.append("bbbbb", 5);
  m.insert(5, 5, std::move(bl2));
  ASSERT_TRUE(m.is_single()) << "Adjacent merge should stay single";
  ASSERT_EQ(0u, m.get_start_off());
  ASSERT_EQ(10u, m.get_end_off());
  
  m.erase(0, 5);
  ASSERT_TRUE(m.is_single()) << "Erase leaving one interval should stay single";
  ASSERT_EQ(5u, m.get_start_off());
  ASSERT_EQ(10u, m.get_end_off());
  
  ceph::buffer::list bl3;
  bl3.append("ccccc", 5);
  m.insert(20, 5, std::move(bl3));
  ASSERT_FALSE(m.is_single()) << "Non-adjacent insert should upgrade to multi";
  ASSERT_EQ(5u, m.get_start_off());
  ASSERT_EQ(25u, m.get_end_off());
}

TEST(HybridExtentMap, EraseExact) {
  test_map m;
  
  ceph::buffer::list bl1, bl2, bl3;
  bl1.append("aaaaa", 5);
  bl2.append("bbbbb", 5);
  bl3.append("ccccc", 5);
  
  m.insert(0, 5, std::move(bl1));
  ASSERT_TRUE(m.is_single()) << "Single insert should be single mode";
  
  m.insert(5, 5, std::move(bl2));
  ASSERT_TRUE(m.is_single()) << "Adjacent merge should stay single";
  
  m.insert(10, 5, std::move(bl3));
  ASSERT_TRUE(m.is_single()) << "Adjacent merge should stay single";
  
  m.erase(5, 5);  // Exact erase of middle interval
  ASSERT_FALSE(m.is_single()) << "Erase creating gap should upgrade to multi";
  
  auto it = m.begin();
  ASSERT_EQ(0u, it.get_off());
  ASSERT_EQ(5u, it.get_len());
  ++it;
  
  ASSERT_EQ(10u, it.get_off());
  ASSERT_EQ(5u, it.get_len());
  ++it;
  
  ASSERT_EQ(it, m.end());
}

TEST(HybridExtentMap, EraseTrimLeft) {
  test_map m;
  
  ceph::buffer::list bl;
  bl.append("testdata", 8);
  m.insert(100, 8, std::move(bl));
  ASSERT_TRUE(m.is_single()) << "Single insert should be in single mode";
  
  // Erase from beginning - should stay in single mode
  m.erase(100, 3);
  
  ASSERT_FALSE(m.empty());
  ASSERT_TRUE(m.is_single()) << "Trim from left should stay in single mode";
  auto it = m.begin();
  ASSERT_EQ(103u, it.get_off());
  ASSERT_EQ(5u, it.get_len());
  ASSERT_EQ(5u, it.get_val().length());
  ++it;
  ASSERT_EQ(it, m.end());  // Still single extent
}

TEST(HybridExtentMap, EraseTrimRight) {
  test_map m;
  
  ceph::buffer::list bl;
  bl.append("testdata", 8);
  m.insert(100, 8, std::move(bl));
  ASSERT_TRUE(m.is_single()) << "Single insert should be in single mode";
  
  // Erase from end - should stay in single mode
  m.erase(105, 10);
  
  ASSERT_FALSE(m.empty());
  ASSERT_TRUE(m.is_single()) << "Trim from right should stay in single mode";
  auto it = m.begin();
  ASSERT_EQ(100u, it.get_off());
  ASSERT_EQ(5u, it.get_len());
  ASSERT_EQ(5u, it.get_val().length());
  ++it;
  ASSERT_EQ(it, m.end());  // Still single extent
}

TEST(HybridExtentMap, EraseMiddleSplits) {
  test_map m;
  
  ceph::buffer::list bl;
  bl.append("testdata", 8);
  m.insert(100, 8, std::move(bl));
  ASSERT_TRUE(m.is_single()) << "Single insert should be in single mode";
  
  // Erase from middle - must upgrade to multi
  m.erase(103, 2);
  
  ASSERT_FALSE(m.empty());
  ASSERT_FALSE(m.is_single()) << "Erase from middle should upgrade to multi mode";
  auto it = m.begin();
  ASSERT_EQ(100u, it.get_off());
  ASSERT_EQ(3u, it.get_len());
  ++it;
  
  ASSERT_EQ(105u, it.get_off());
  ASSERT_EQ(3u, it.get_len());
  ++it;
  
  ASSERT_EQ(it, m.end());  // Now has two extents
}



TEST(HybridExtentMap, NoDowngradeAfterUpgrade) {
  test_map m;

  // Start in single mode
  ceph::buffer::list bl1;
  bl1.append(std::string(50, 'a'));
  m.insert(100, 50, bl1);
  ASSERT_FALSE(m.empty());
  ASSERT_EQ(100u, m.get_start_off());
  ASSERT_EQ(150u, m.get_end_off());

  // Force upgrade to multi mode by inserting non-adjacent interval
  ceph::buffer::list bl2;
  bl2.append(std::string(50, 'b'));
  m.insert(200, 50, bl2);
  ASSERT_EQ(100u, m.get_start_off());
  ASSERT_EQ(250u, m.get_end_off());

  // Now erase one interval, leaving only one
  m.erase(200, 50);
  ASSERT_TRUE(m.contains(100, 50));
  ASSERT_EQ(100u, m.get_start_off());
  ASSERT_EQ(150u, m.get_end_off());

  // Verify it still works correctly in multi mode
  ceph::buffer::list bl3;
  bl3.append(std::string(50, 'c'));
  m.insert(150, 50, bl3);  // Adjacent - should merge
  ASSERT_TRUE(m.contains(100, 100));
  ASSERT_EQ(100u, m.get_start_off());
  ASSERT_EQ(200u, m.get_end_off());

  // Erase back down to single interval again
  m.erase(150, 50);
  ASSERT_TRUE(m.contains(100, 50));
  ASSERT_EQ(100u, m.get_start_off());
  ASSERT_EQ(150u, m.get_end_off());

  // Clear and re-insert - should still be in multi mode
  m.clear();
  ASSERT_TRUE(m.empty());
  ceph::buffer::list bl4;
  bl4.append(std::string(50, 'd'));
  m.insert(300, 50, bl4);
  ASSERT_TRUE(m.contains(300, 50));
  ASSERT_EQ(300u, m.get_start_off());
  ASSERT_EQ(350u, m.get_end_off());
}

TEST(HybridExtentMap, IteratorOnEmptyMultiMode) {
  test_map m;
  
  // Start in single mode, upgrade to multi, then clear
  ceph::buffer::list bl1, bl2;
  bl1.append("data1", 5);
  bl2.append("data2", 5);
  
  m.insert(100, 5, std::move(bl1));
  ASSERT_TRUE(m.is_single());
  
  // Upgrade to multi mode by inserting non-adjacent interval
  m.insert(200, 5, std::move(bl2));
  ASSERT_FALSE(m.is_single()) << "Should be in multi mode";
  
  // Clear - stays in multi mode but becomes empty
  m.clear();
  ASSERT_TRUE(m.empty());
  ASSERT_FALSE(m.is_single()) << "Should stay in multi mode after clear";
  
  // Test iterator behavior on empty multi-mode map
  auto begin_it = m.begin();
  auto end_it = m.end();
  
  // begin() should equal end() for empty map
  ASSERT_EQ(begin_it, end_it) << "begin() should equal end() for empty multi-mode map";
  
  // Should not be able to iterate
  int count = 0;
  for (auto it = m.begin(); it != m.end(); ++it) {
    count++;
  }
  ASSERT_EQ(0, count) << "Should not iterate over empty multi-mode map";
}

