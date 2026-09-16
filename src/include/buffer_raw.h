// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2017 Red Hat, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_BUFFER_RAW_H
#define CEPH_BUFFER_RAW_H

#include <map>
#include <mutex>
#include <utility>
#include <type_traits>
#include <atomic>
#include <iostream>
#include "common/ceph_atomic.h"
#include "include/buffer.h"
#include "include/mempool.h"
#include "include/spinlock.h"
#include "common/BackTrace.h"

namespace ceph::buffer {
inline namespace v15_2_0 {

// ---------------------------------------------------------------------------
// Allocation tracker for buffer_anon leak hunting.
//
// When g_buffer_anon_tracker.enabled is true every buffer::raw constructed
// in the buffer_anon pool is assigned a monotonically increasing ID and its
// birth stack is stored in the live_allocs map.  The entry is removed when
// the raw is destroyed OR when it is reassigned out of buffer_anon.
//
// After a fully-quiesced operation, any entries still in live_allocs are
// retained (leaked) buffers.  Call dump_live() to print them.
//
// All state is in a single inline struct so the test only needs to touch one
// global; it is zero-cost when disabled (single relaxed load in the hot path).
// ---------------------------------------------------------------------------
struct BufferAnonTracker {
  std::atomic<bool>    enabled{false};
  std::atomic<uint64_t> next_id{1};

  struct Entry {
    unsigned            len;
    ceph::ClibBackTrace bt;
    explicit Entry(unsigned l) : len(l), bt(1) {}  // skip=1: drops _register()
  };

  std::mutex                        mtx;
  std::map<uint64_t, Entry>         live_allocs;  // guarded by mtx

  // Register a new allocation; returns the assigned ID (or 0 if disabled).
  uint64_t reg(unsigned len) {
    if (!enabled.load(std::memory_order_relaxed))
      return 0;
    uint64_t id = next_id.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(mtx);
    live_allocs.emplace(std::piecewise_construct,
                        std::forward_as_tuple(id),
                        std::forward_as_tuple(len));
    return id;
  }

  // Remove an allocation (on free or reassignment away from buffer_anon).
  void unreg(uint64_t id) {
    if (id == 0) return;
    std::lock_guard<std::mutex> lk(mtx);
    live_allocs.erase(id);
  }

  // Print all surviving entries to stderr.  Call after the event loop is idle.
  void dump_live() const {
    std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(mtx));
    if (live_allocs.empty()) {
      std::cerr << "[buffer_anon tracker] No live allocations — no leak.\n";
      return;
    }
    std::cerr << "[buffer_anon tracker] " << live_allocs.size()
              << " live allocation(s) still retained:\n";
    for (const auto& [id, e] : live_allocs) {
      std::cerr << "  #" << id << "  " << e.len << " bytes\n" << e.bt << "\n";
    }
  }
};

inline BufferAnonTracker g_buffer_anon_tracker;

  class raw {
  public:
    // In the future we might want to have a slab allocator here with few
    // embedded slots. This would allow to avoid the "if" in dtor of ptr_node.
    struct alignas(ptr_node) {
      unsigned char data[sizeof(ptr_node)];
    } bptr_storage;
  protected:
    char *data;
    unsigned len;
  public:
    ceph::atomic<unsigned> nref { 0 };
    int mempool;

    std::pair<size_t, size_t> last_crc_offset {std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max()};
    std::pair<uint32_t, uint32_t> last_crc_val;

    mutable ceph::spinlock crc_spinlock;

    explicit raw(unsigned l, int mempool=mempool::mempool_buffer_anon)
      : data(nullptr), len(l), nref(0), mempool(mempool), _alloc_id(0) {
      mempool::get_pool(mempool::pool_index_t(mempool)).adjust_count(1, len);
      if (mempool == mempool::mempool_buffer_anon)
        _alloc_id = g_buffer_anon_tracker.reg(l);
    }
    raw(char *c, unsigned l, int mempool=mempool::mempool_buffer_anon)
      : data(c), len(l), nref(0), mempool(mempool), _alloc_id(0) {
      mempool::get_pool(mempool::pool_index_t(mempool)).adjust_count(1, len);
      if (mempool == mempool::mempool_buffer_anon)
        _alloc_id = g_buffer_anon_tracker.reg(l);
    }
    virtual ~raw() {
      mempool::get_pool(mempool::pool_index_t(mempool)).adjust_count(
 -1, -(int)len);
      g_buffer_anon_tracker.unreg(_alloc_id);
    }

    void _set_len(unsigned l) {
      mempool::get_pool(mempool::pool_index_t(mempool)).adjust_count(
	-1, -(int)len);
      len = l;
      mempool::get_pool(mempool::pool_index_t(mempool)).adjust_count(1, len);
    }

    void reassign_to_mempool(int pool) {
      if (pool == mempool) {
 return;
      }
      mempool::get_pool(mempool::pool_index_t(mempool)).adjust_count(
 -1, -(int)len);
      // Moving away from buffer_anon: remove tracking entry (not a leak).
      if (mempool == mempool::mempool_buffer_anon) {
        g_buffer_anon_tracker.unreg(_alloc_id);
        _alloc_id = 0;
      }
      mempool = pool;
      mempool::get_pool(mempool::pool_index_t(pool)).adjust_count(1, len);
    }

    void try_assign_to_mempool(int pool) {
      if (mempool == mempool::mempool_buffer_anon) {
	reassign_to_mempool(pool);
      }
    }

    uint64_t _alloc_id;  // 0 = not tracked

    // no copying.
    // cppcheck-suppress noExplicitConstructor
    raw(const raw &other) = delete;
    const raw& operator=(const raw &other) = delete;
public:
    char *get_data() const {
      return data;
    }
    unsigned get_len() const {
      return len;
    }
    bool get_crc(const std::pair<size_t, size_t> &fromto,
		 std::pair<uint32_t, uint32_t> *crc) const {
      std::lock_guard lg(crc_spinlock);
      if (last_crc_offset == fromto) {
        *crc = last_crc_val;
        return true;
      }
      return false;
    }
    void set_crc(const std::pair<size_t, size_t> &fromto,
		 const std::pair<uint32_t, uint32_t> &crc) {
      std::lock_guard lg(crc_spinlock);
      last_crc_offset = fromto;
      last_crc_val = crc;
    }
    void invalidate_crc() {
      std::lock_guard lg(crc_spinlock);
      last_crc_offset.first = std::numeric_limits<size_t>::max();
      last_crc_offset.second = std::numeric_limits<size_t>::max();
    }
  };

} // inline namespace v15_2_0
} // namespace ceph::buffer

#endif // CEPH_BUFFER_RAW_H
