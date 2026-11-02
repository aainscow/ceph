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

/**
 * What the Objecter does to in-flight EC direct reads and split sub-reads
 * when the OSDMap changes or an OSD bounces them, how it completes them,
 * and which split op counters each outcome of a direct read moves.
 *
 * The Objecter is initialised with a session per OSD whose connection
 * records the MOSDOps sent on it.  Maps are delivered through
 * handle_osd_map() and replies through ms_fast_dispatch2().
 *
 * Pool 1 is a 2+1 EC pool with 4K chunks and one PG, 1.0:
 *
 *            osd.0      osd.1     osd.2     osd.3
 *   PG 1.0   shard 0    shard 1   shard 2   not in the PG
 *            primary    data      parity
 *
 * With BALANCE_READS, read 0~4K goes to osd.0, read 4K~4K goes to osd.1,
 * and read 0~8K is split into a sub-read to each of them.
 */

#include <algorithm>

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>

#include "common/Cond.h"
#include "erasure-code/ErasureCodePlugin.h"
#include "global/global_context.h"
#include "include/rados.h"
#include "messages/MOSDMap.h"
#include "messages/MOSDOpReply.h"
#include "mon/MonClient.h"
#include "msg/Messenger.h"
#include "osd/ECUtil.h"
#include "osd/OSDMap.h"
#include "osd/osd_types.h"
#include "osdc/Objecter.h"
#include "test/osd/MockConnection.h"
#include "test/osd/OSDMapTestHelpers.h"
#include "test/osdc/SplitOpCounters.h"

class RecordingConnection : public MockConnection {
public:
  using MockConnection::MockConnection;
  std::vector<MessageRef> sent;

protected:
  int send_msg(MessageRef&& m) override {
    sent.push_back(std::move(m));
    return 0;
  }
};

class TestSplitOpMapChange : public ::testing::Test {
protected:
  static constexpr int num_osds = 4;
  static constexpr int64_t ec_pool_id = 1;
  static constexpr uint64_t chunk_size = 4096;
  static constexpr uint64_t stripe_width = 2 * chunk_size;
  static constexpr int primary = 0;
  static constexpr int shard1_osd = 1;
  static constexpr int parity_osd = 2;
  static constexpr int spare_osd = 3;
  const pg_t pgid{0, ec_pool_id};
  const std::vector<int> acting = {primary, shard1_osd, parity_osd};
  boost::asio::io_context ioc;
  MonClient monc{g_ceph_context, ioc};
  std::unique_ptr<Messenger> msgr{
    Messenger::create_client_messenger(g_ceph_context, "client")};
  std::unique_ptr<Objecter> objecter;
  std::map<int, ceph::ref_t<RecordingConnection>> cons;
  C_SaferCond done;

  void SetUp() override
  {
    objecter = std::make_unique<Objecter>(g_ceph_context, msgr.get(), &monc, ioc);
    objecter->init();

    OSDMap map;
    uuid_d fsid;
    fsid.generate_random();
    ceph_assert(map.build_simple(g_ceph_context, 1, fsid, num_osds) == 0);
    // Up but out, so CRUSH maps PG 1.0 nowhere and only the pg_temp set
    // below gives it an acting set.
    for (int i = 0; i < num_osds; i++) {
      map.set_state(i, CEPH_OSD_EXISTS | CEPH_OSD_UP);
    }

    pg_pool_t ec;
    ec.type = pg_pool_t::TYPE_ERASURE;
    ec.size = 3;
    ec.min_size = 2;
    ec.ec_data_shard_count = 2;
    ec.ec_coding_shard_count = 1;
    ec.set_stripe_width(stripe_width);
    ec.set_flag(pg_pool_t::FLAG_EC_OVERWRITES);
    ec.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
    ec.set_flag(pg_pool_t::FLAG_CLIENT_SPLIT_READS);
    ec.set_pg_num(1);
    ec.set_pgp_num(1);
    OSDMapTestHelpers::add_pool(map, ec_pool_id, ec);
    OSDMapTestHelpers::set_pg_acting(map, pgid, acting);

    objecter->start(&map);

    for (int osd = 0; osd < num_osds; osd++) {
      auto s = new Objecter::OSDSession(g_ceph_context, osd, ioc.get_executor());
      cons[osd] = ceph::make_ref<RecordingConnection>(osd);
      s->con = cons[osd];
      s->con->set_priv(RefCountedPtr{s});
      objecter->osd_sessions[osd] = s;
    }
  }

  void TearDown() override
  {
    objecter->shutdown();
    objecter.reset();
  }

  void poll()
  {
    ioc.restart();
    ioc.poll();
  }

  void advance_map(const std::function<void(OSDMap::Incremental&)>& change)
  {
    auto [epoch, fsid] = objecter->with_osdmap([](const OSDMap& o) {
      return std::make_pair(o.get_epoch(), o.get_fsid());
    });
    OSDMap::Incremental inc(epoch + 1);
    inc.fsid = fsid;
    change(inc);
    auto m = ceph::make_message<MOSDMap>(monc.get_fsid(),
                                         CEPH_FEATURES_SUPPORTED_DEFAULT);
    inc.encode(m->incremental_maps[inc.epoch],
               CEPH_FEATURES_SUPPORTED_DEFAULT | CEPH_FEATURE_RESERVED);
    objecter->handle_osd_map(m.get());
    poll();
  }

  void mark_down(int osd)
  {
    advance_map([osd](OSDMap::Incremental& inc) {
      // XORed onto the current state, so this clears UP.
      inc.new_state[osd] = CEPH_OSD_UP;
    });
  }

  void raise_min_size()
  {
    auto pool = objecter->with_osdmap([](const OSDMap& o) {
      return *o.get_pg_pool(ec_pool_id);
    });
    pool.min_size++;
    advance_map([&pool](OSDMap::Incremental& inc) {
      inc.new_pools[ec_pool_id] = pool;
    });
  }

  Objecter::Op *new_read_op(osdc_opvec&& ops, int flags)
  {
    return new Objecter::Op(object_t("obj"), object_locator_t(ec_pool_id),
                            std::move(ops), flags | CEPH_OSD_FLAG_READ,
                            &done, nullptr);
  }

  Objecter::Op *new_read_op(uint64_t off, uint64_t len, int flags)
  {
    osdc_opvec ops(1);
    ops[0].op.op = CEPH_OSD_OP_READ;
    ops[0].op.extent.offset = off;
    ops[0].op.extent.length = len;
    return new_read_op(std::move(ops), flags);
  }

  // A read of the whole stripe, then another op.
  Objecter::Op *new_read_op_with(int other_op, int flags)
  {
    osdc_opvec ops(2);
    ops[0].op.op = CEPH_OSD_OP_READ;
    ops[0].op.extent.length = stripe_width;
    ops[1].op.op = other_op;
    return new_read_op(std::move(ops), flags);
  }

  ceph_tid_t submit_op(Objecter::Op *op)
  {
    ceph_tid_t tid = 0;
    objecter->op_submit(op, &tid);
    poll();
    return tid;
  }

  ceph_tid_t submit_read(uint64_t off, uint64_t len, int flags)
  {
    return submit_op(new_read_op(off, len, flags));
  }

  shard_id_t shard_of(int osd)
  {
    return shard_id_t(std::ranges::find(acting, osd) - acting.begin());
  }

  std::vector<std::pair<ceph_tid_t, shard_id_t>> sent_to(int osd)
  {
    std::vector<std::pair<ceph_tid_t, shard_id_t>> ops;
    for (auto& m : cons[osd]->sent) {
      ceph_assert(m->get_type() == CEPH_MSG_OSD_OP);
      auto op = boost::static_pointer_cast<_mosdop::MOSDOp<osdc_opvec>>(m);
      ops.emplace_back(op->get_tid(), op->get_spg().shard);
    }
    return ops;
  }

  // What an OSD returns for each op these tests send, for an object of one
  // stripe whose shards are all at the same version.
  static ceph::buffer::list out_data(const OSDOp& o)
  {
    const std::map<shard_id_t, eversion_t> versions = {
      {shard_id_t(0), eversion_t(1, 1)},
      {shard_id_t(1), eversion_t(1, 1)},
      {shard_id_t(2), eversion_t(1, 1)}};
    using ceph::encode;
    ceph::buffer::list bl;
    switch (o.op.op) {
    case CEPH_OSD_OP_READ:
      bl.append_zero(o.op.extent.length);
      break;
    case CEPH_OSD_OP_SPARSE_READ: {
      ceph::buffer::list data;
      data.append_zero(o.op.extent.length);
      encode(std::map<uint64_t, uint64_t>{{o.op.extent.offset,
                                           o.op.extent.length}}, bl);
      encode(data, bl);
      break;
    }
    case CEPH_OSD_OP_STAT:
      encode(stripe_width, bl);
      encode(ceph::real_time(), bl);
      break;
    case CEPH_OSD_OP_GET_INTERNAL_VERSIONS:
      encode(versions, bl);
      break;
    case CEPH_OSD_OP_GET_INTERNAL_VERSIONS_V2:
      encode(internal_versions_v2_response_t{versions}, bl);
      break;
    }
    return bl;
  }

  void reply(int osd, const MessageRef& m, int result = 0,
             std::vector<pg_log_op_return_item_t> returns = {},
             version_t user_version = 0)
  {
    auto op = boost::static_pointer_cast<_mosdop::MOSDOp<osdc_opvec>>(m);
    spg_t pgid = op->get_spg();
    auto req = ceph::make_message<MOSDOp>(0, op->get_tid(), op->get_hobj(),
                                          pgid, op->get_map_epoch(),
                                          op->get_flags(), 0);
    req->ops.assign(op->ops.begin(), op->ops.end());
    req->set_retry_attempt(op->get_retry_attempt());
    epoch_t epoch = objecter->with_osdmap([](const OSDMap& o) {
      return o.get_epoch();
    });
    auto r = ceph::make_message<MOSDOpReply>(
      req.get(), result, epoch, CEPH_OSD_FLAG_ACK | CEPH_OSD_FLAG_ONDISK, false);
    if (returns.empty() && result >= 0) {
      for (auto& o : op->ops) {
        returns.push_back({0, out_data(o)});
      }
    }
    returns.resize(op->ops.size());
    r->set_reply_versions(eversion_t(), user_version);
    r->set_op_returns(returns);
    r->set_connection(cons[osd]);
    objecter->ms_fast_dispatch2(r);
    poll();
  }

  void reply_to_all(int osd, int result = 0)
  {
    auto sent = cons[osd]->sent;
    for (auto& m : sent) {
      reply(osd, m, result);
    }
  }

  // Each test has its own Objecter, whose counters start at zero.
  uint64_t counter(const std::string& name)
  {
    auto counters = split_op_counters(g_ceph_context);
    auto it = counters.find(name);
    EXPECT_NE(counters.end(), it) << "no counter " << name;
    return it == counters.end() ? 0 : it->second;
  }

  // The sub-read of a split read to osd.1 is still outstanding when the PG
  // starts a new interval, so the whole read is redriven to the primary.
  void split_read_redriven_on_new_interval(
    const std::function<void()>& new_interval)
  {
    ceph_tid_t parent = submit_read(0, 2 * chunk_size,
                                    CEPH_OSD_FLAG_BALANCE_READS);
    ASSERT_EQ(1u, sent_to(primary).size());
    ASSERT_EQ(1u, sent_to(shard1_osd).size());
    reply(primary, cons[primary]->sent.back());

    // A map that leaves PG 1.0 alone leaves the sub-read alone.
    mark_down(spare_osd);
    EXPECT_EQ(1u, sent_to(shard1_osd).size());
    EXPECT_EQ(1u, sent_to(primary).size());

    new_interval();
    EXPECT_EQ(1u, sent_to(shard1_osd).size());
    ASSERT_EQ(2u, sent_to(primary).size());
    EXPECT_EQ(parent, sent_to(primary).back().first);
    reply(primary, cons[primary]->sent.back());
    EXPECT_EQ(0, done.wait_for(0));
    EXPECT_EQ(1u, counter("split_op_fallback_interval"));
    EXPECT_EQ(0u, counter("split_op_fallback_eagain"));
    EXPECT_EQ(0u, counter("split_op_completed_ops"));
  }

  // The single-chunk read 4K~4K goes directly to osd.1, which holds shard 1.
  // Once the PG starts a new interval, it is redriven to the primary.
  void direct_read_redriven_on_new_interval(
    const std::function<void()>& new_interval, int new_primary)
  {
    ceph_tid_t tid = submit_read(chunk_size, chunk_size,
                                 CEPH_OSD_FLAG_BALANCE_READS);
    ASSERT_EQ(1u, sent_to(shard1_osd).size());
    EXPECT_EQ(std::make_pair(tid, shard_of(shard1_osd)),
              sent_to(shard1_osd).back());

    new_interval();
    EXPECT_EQ(1u, sent_to(shard1_osd).size());
    ASSERT_EQ(1u, sent_to(new_primary).size());
    EXPECT_EQ(std::make_pair(tid, shard_of(new_primary)),
              sent_to(new_primary).back());
    reply(new_primary, cons[new_primary]->sent.back());
    EXPECT_EQ(0, done.wait_for(0));
    EXPECT_EQ(1u, counter("split_op_fallback_interval"));
    EXPECT_EQ(0u, counter("split_op_fallback_eagain"));
    EXPECT_EQ(0u, counter("split_op_completed_ops"));
  }

  // osd.1 bounces its sub-read of a two-chunk read with -EAGAIN, so the read
  // is resent to the primary, which completes it.
  void split_read_resent_after_eagain()
  {
    const size_t sent = cons[primary]->sent.size();
    ceph_tid_t parent = submit_read(0, stripe_width,
                                    CEPH_OSD_FLAG_BALANCE_READS);
    ASSERT_EQ(sent + 1, sent_to(primary).size());
    reply(primary, cons[primary]->sent.back());
    reply(shard1_osd, cons[shard1_osd]->sent.back(), -EAGAIN);
    ASSERT_EQ(sent + 2, sent_to(primary).size());
    EXPECT_EQ(parent, sent_to(primary).back().first);
    reply(primary, cons[primary]->sent.back());
    EXPECT_EQ(0, done.wait_for(0));
  }
};

TEST_F(TestSplitOpMapChange, ECSplitReadRedrivesWhenMinSizeChanges)
{
  split_read_redriven_on_new_interval([this] { raise_min_size(); });
}

TEST_F(TestSplitOpMapChange, ECSplitReadRedrivesWhenOtherShardOsdDown)
{
  split_read_redriven_on_new_interval([this] { mark_down(parity_osd); });
}

TEST_F(TestSplitOpMapChange, ECSplitReadRedrivesWhenShardOsdDown)
{
  split_read_redriven_on_new_interval([this] { mark_down(shard1_osd); });
}

TEST_F(TestSplitOpMapChange, ECDirectReadRedrivesWhenPrimaryMoves)
{
  direct_read_redriven_on_new_interval([this] {
    advance_map([this](OSDMap::Incremental& inc) {
      inc.new_primary_temp[pgid] = parity_osd;
    });
  }, parity_osd);
}

TEST_F(TestSplitOpMapChange, ECDirectReadRedrivesWhenMinSizeChanges)
{
  direct_read_redriven_on_new_interval([this] { raise_min_size(); }, primary);
}

TEST_F(TestSplitOpMapChange, ECSplitReadCountsCompletion)
{
  submit_read(0, stripe_width, CEPH_OSD_FLAG_BALANCE_READS);
  ASSERT_EQ(1u, sent_to(primary).size());
  ASSERT_EQ(1u, sent_to(acting[1]).size());
  for (int osd : acting) {
    reply_to_all(osd);
  }
  EXPECT_EQ(0, done.wait_for(0));
  EXPECT_EQ(1u, counter("split_op_completed_ops"));
  EXPECT_EQ(stripe_width, counter("split_op_completed_bytes"));
  EXPECT_EQ(0u, counter("split_op_fallback_eagain"));
  EXPECT_EQ(0u, counter("split_op_fallback_interval"));
  EXPECT_EQ(0u, counter("split_op_fallback_version"));
  EXPECT_EQ(0u, counter("split_op_fallback_shard_error"));
  EXPECT_EQ(0u, counter("ec_split_op_decode_ops"));
  EXPECT_EQ(0u, counter("split_op_unsupported"));
}

// An OSD's -EAGAIN, unlike a PG change, comes in a reply.
TEST_F(TestSplitOpMapChange, ECSplitReadCountsOsdEagain)
{
  ceph_tid_t parent = submit_read(0, stripe_width, CEPH_OSD_FLAG_BALANCE_READS);
  ASSERT_EQ(1u, sent_to(acting[1]).size());
  reply_to_all(acting[1], -EAGAIN);
  reply_to_all(primary);
  ASSERT_EQ(2u, sent_to(primary).size());
  EXPECT_EQ(parent, sent_to(primary).back().first);
  EXPECT_EQ(1u, counter("split_op_fallback_eagain"));
  EXPECT_EQ(0u, counter("split_op_fallback_interval"));

  reply(primary, cons[primary]->sent.back());
  EXPECT_EQ(0, done.wait_for(0));
  EXPECT_EQ(0u, counter("split_op_completed_ops"));
}

// The read counts once, in the class of the OSD's -EAGAIN.
TEST_F(TestSplitOpMapChange, ECSplitReadCountsOsdEagainOverPgChange)
{
  ceph_tid_t parent = submit_read(0, stripe_width, CEPH_OSD_FLAG_BALANCE_READS);
  ASSERT_EQ(1u, sent_to(acting[1]).size());
  reply_to_all(primary, -EAGAIN);
  mark_down(acting[1]);
  ASSERT_EQ(2u, sent_to(primary).size());
  EXPECT_EQ(parent, sent_to(primary).back().first);
  EXPECT_EQ(1u, counter("split_op_fallback_eagain"));
  EXPECT_EQ(0u, counter("split_op_fallback_interval"));

  reply(primary, cons[primary]->sent.back());
  EXPECT_EQ(0, done.wait_for(0));
}

TEST_F(TestSplitOpMapChange, ECDirectReadCountsCompletion)
{
  submit_read(chunk_size, chunk_size, CEPH_OSD_FLAG_BALANCE_READS);
  ASSERT_EQ(1u, sent_to(shard1_osd).size());
  reply(1, cons[shard1_osd]->sent.back());
  EXPECT_EQ(0, done.wait_for(0));
  EXPECT_EQ(1u, counter("split_op_completed_ops"));
  EXPECT_EQ(chunk_size, counter("split_op_completed_bytes"));
}

TEST_F(TestSplitOpMapChange, ECFailedDirectReadIsNotCountedCompleted)
{
  submit_read(chunk_size, chunk_size, CEPH_OSD_FLAG_BALANCE_READS);
  ASSERT_EQ(1u, sent_to(shard1_osd).size());
  reply(1, cons[shard1_osd]->sent.back(), -ENOENT);
  EXPECT_EQ(-ENOENT, done.wait_for(0));
  EXPECT_EQ(0u, counter("split_op_completed_ops"));
}

// The STAT returns 16 bytes, which are not read data.
TEST_F(TestSplitOpMapChange, ECSplitReadCountsOnlyReadBytes)
{
  submit_op(new_read_op_with(CEPH_OSD_OP_STAT, CEPH_OSD_FLAG_BALANCE_READS));
  ASSERT_EQ(1u, sent_to(acting[1]).size());
  for (int osd : acting) {
    reply_to_all(osd);
  }
  EXPECT_EQ(0, done.wait_for(0));
  EXPECT_EQ(1u, counter("split_op_completed_ops"));
  EXPECT_EQ(stripe_width, counter("split_op_completed_bytes"));
}

// The out data of a sparse read holds its extent map as well as its data.
TEST_F(TestSplitOpMapChange, ECDirectSparseReadCountsItsOutData)
{
  osdc_opvec ops(1);
  ops[0].op.op = CEPH_OSD_OP_SPARSE_READ;
  ops[0].op.extent.offset = chunk_size;
  ops[0].op.extent.length = chunk_size;
  uint64_t out_length = out_data(ops[0]).length();
  submit_op(new_read_op(std::move(ops), CEPH_OSD_FLAG_BALANCE_READS));
  ASSERT_EQ(1u, sent_to(shard1_osd).size());
  reply(1, cons[shard1_osd]->sent.back());
  EXPECT_EQ(0, done.wait_for(0));
  EXPECT_EQ(1u, counter("split_op_completed_ops"));
  EXPECT_EQ(out_length, counter("split_op_completed_bytes"));
}

// Direct reads do not serve omap reads.
TEST_F(TestSplitOpMapChange, ECReadWithUnsupportedOpCountsUnsupported)
{
  submit_op(new_read_op_with(CEPH_OSD_OP_OMAPGETVALS,
                             CEPH_OSD_FLAG_BALANCE_READS));
  ASSERT_EQ(1u, sent_to(primary).size());
  EXPECT_TRUE(sent_to(acting[1]).empty());
  EXPECT_EQ(1u, counter("split_op_unsupported"));
  EXPECT_EQ(0u, counter("split_op_reads"));
  reply(primary, cons[primary]->sent.back());
  EXPECT_EQ(0, done.wait_for(0));
}

// A zero-length read reads the whole object, whose size the client does
// not know.
TEST_F(TestSplitOpMapChange, ECZeroLengthReadCountsUnsupported)
{
  submit_read(0, 0, CEPH_OSD_FLAG_BALANCE_READS);
  ASSERT_EQ(1u, sent_to(primary).size());
  EXPECT_EQ(1u, counter("split_op_unsupported"));
  reply(primary, cons[primary]->sent.back());
  EXPECT_EQ(0, done.wait_for(0));
}

// A read of metadata only goes to the primary by design.
TEST_F(TestSplitOpMapChange, ECStatIsNotCountedUnsupported)
{
  osdc_opvec ops(1);
  ops[0].op.op = CEPH_OSD_OP_STAT;
  submit_op(new_read_op(std::move(ops), CEPH_OSD_FLAG_BALANCE_READS));
  ASSERT_EQ(1u, sent_to(primary).size());
  EXPECT_EQ(0u, counter("split_op_unsupported"));
  reply(primary, cons[primary]->sent.back());
  EXPECT_EQ(0, done.wait_for(0));
}

TEST_F(TestSplitOpMapChange, ECUnbalancedReadIsNotCountedUnsupported)
{
  submit_op(new_read_op_with(CEPH_OSD_OP_OMAPGETVALS, 0));
  ASSERT_EQ(1u, sent_to(primary).size());
  EXPECT_EQ(0u, counter("split_op_unsupported"));
  reply(primary, cons[primary]->sent.back());
  EXPECT_EQ(0, done.wait_for(0));
}

TEST_F(TestSplitOpMapChange, ECDirectReadCountsOsdEagain)
{
  submit_read(chunk_size, chunk_size, CEPH_OSD_FLAG_BALANCE_READS);
  ASSERT_EQ(1u, sent_to(shard1_osd).size());
  reply(1, cons[shard1_osd]->sent.back(), -EAGAIN);
  ASSERT_EQ(1u, sent_to(primary).size());
  EXPECT_EQ(shard_id_t(primary), sent_to(primary).back().second);
  EXPECT_EQ(1u, counter("split_op_fallback_eagain"));
  EXPECT_EQ(0u, counter("split_op_fallback_interval"));

  // The resent read is not a direct read.
  reply(primary, cons[primary]->sent.back(), -EAGAIN);
  ASSERT_EQ(2u, sent_to(primary).size());
  EXPECT_EQ(1u, counter("split_op_fallback_eagain"));

  reply(primary, cons[primary]->sent.back());
  EXPECT_EQ(0, done.wait_for(0));
  EXPECT_EQ(0u, counter("split_op_completed_ops"));
}

// A read that osd.1 bounces with -EAGAIN is resent to the primary. Once that
// completes, the Objecter has no op left in flight.
TEST_F(TestSplitOpMapChange, ECDirectReadResentAfterEagainLeavesNothingActive)
{
  submit_read(chunk_size, chunk_size, CEPH_OSD_FLAG_BALANCE_READS);
  ASSERT_EQ(1u, sent_to(shard1_osd).size());
  reply(1, cons[shard1_osd]->sent.back(), -EAGAIN);
  ASSERT_EQ(1u, sent_to(primary).size());
  reply(primary, cons[primary]->sent.back());
  EXPECT_EQ(0, done.wait_for(0));
  EXPECT_FALSE(objecter->is_active());
}

TEST_F(TestSplitOpMapChange, ECSplitReadResentAfterEagainLeavesNothingActive)
{
  split_read_resent_after_eagain();
  EXPECT_FALSE(objecter->is_active());
}

// A split read that fails with -ENOENT from its sub-reads, then one that is
// resent to the primary. handle_osd_op_reply() sets op_inflight before it
// uncounts the op it completes, so the last reply sees only its own op.
TEST_F(TestSplitOpMapChange, ECSplitReadsKeepOpInflightCount)
{
  submit_read(0, stripe_width, CEPH_OSD_FLAG_BALANCE_READS);
  reply(primary, cons[primary]->sent.back(), -ENOENT);
  reply(1, cons[shard1_osd]->sent.back(), -ENOENT);
  ASSERT_EQ(-ENOENT, done.wait_for(0));

  split_read_resent_after_eagain();
  EXPECT_EQ(1u, objecter_counters(g_ceph_context, "op_inflight")
                  .at("op_inflight"));
}

// Shard 1 is excluded from direct reads, so a two-chunk read is decoded from
// shard extents read from shard 0, whose sub-read also reads the object size,
// and from the coding shard 2.
class TestSplitOpDecodeMapChange : public TestSplitOpMapChange {
protected:
  ceph::ErasureCodeInterfaceRef ec_impl;

  void SetUp() override
  {
    TestSplitOpMapChange::SetUp();
    const ErasureCodeProfile profile = {
      {"plugin", "jerasure"}, {"technique", "reed_sol_van"},
      {"k", "2"}, {"m", "1"}, {"stripe_unit", "4096"}};
    // Tests run from the build directory, which has the plugins in ./lib.
    // Once loaded from there, the client finds the plugin loaded.
    ErasureCodeProfile loaded = profile;
    std::stringstream ss;
    ASSERT_EQ(0, ceph::ErasureCodePluginRegistry::instance().factory(
      "jerasure", "./lib", loaded, &ec_impl, &ss)) << ss.str();

    set_conf("objecter_debug_ec_direct_read_exclude_shards", "1");
    auto pool = objecter->with_osdmap([](const OSDMap& o) {
      return *o.get_pg_pool(ec_pool_id);
    });
    pool.erasure_code_profile = "ec";
    pool.set_flag(pg_pool_t::FLAG_EC_CLIENT_DECODE);
    advance_map([&](OSDMap::Incremental& inc) {
      inc.new_erasure_code_profiles["ec"] = profile;
      inc.new_pools[ec_pool_id] = pool;
      // Ops are resent on a split only once all OSDs are luminous or later.
      inc.new_require_osd_release = ceph_release();
    });
  }

  void TearDown() override
  {
    set_conf("objecter_debug_ec_direct_read_exclude_shards", "");
    TestSplitOpMapChange::TearDown();
  }

  void set_conf(const std::string& key, const std::string& val)
  {
    g_ceph_context->_conf.set_val_or_die(key, val);
    g_ceph_context->_conf.apply_changes(nullptr);
  }

  const osdc_opvec& first_ops_sent_to(int osd)
  {
    return boost::static_pointer_cast<_mosdop::MOSDOp<osdc_opvec>>(
      cons[osd]->sent.front())->ops;
  }

  void expect_decode_plan(int victim)
  {
    ASSERT_EQ(acting[2], victim);
    EXPECT_TRUE(sent_to(acting[1]).empty());
    EXPECT_EQ(shard_id_t(2), sent_to(victim).front().second);
    EXPECT_TRUE(first_ops_sent_to(victim)[0].op.flags &
                CEPH_OSD_OP_FLAG_EC_SHARD_EXTENT);
    EXPECT_TRUE(first_ops_sent_to(primary)[0].op.flags &
                CEPH_OSD_OP_FLAG_EC_SHARD_EXTENT);
    EXPECT_TRUE(std::ranges::any_of(first_ops_sent_to(primary),
                                    [](const OSDOp& o) {
      return o.op.op == CEPH_OSD_OP_STAT;
    }));
  }

  // The object moves to the child PG 1.1. As pgp_num is unchanged, the child
  // stays on its parent's OSDs, which here come from the parent's pg_temp.
  void split_pg()
  {
    auto pool = objecter->with_osdmap([](const OSDMap& o) {
      return *o.get_pg_pool(ec_pool_id);
    });
    pool.set_pg_num(2);
    advance_map([this, &pool](OSDMap::Incremental& inc) {
      inc.new_pools[ec_pool_id] = pool;
      inc.new_pg_temp[pg_t(1, ec_pool_id)] =
        mempool::osdmap::vector<int32_t>(acting.begin(), acting.end());
    });
  }

  // The coding shard's sub-read is still outstanding when the PG starts a
  // new interval, so the whole read is redriven to the primary.
  void decode_read_redriven_on_new_interval(
    const std::function<void()>& new_interval)
  {
    ceph_tid_t parent = submit_read(0, 2 * chunk_size,
                                    CEPH_OSD_FLAG_BALANCE_READS);
    ASSERT_EQ(1u, sent_to(primary).size());
    ASSERT_EQ(1u, sent_to(parity_osd).size());
    expect_decode_plan(parity_osd);
    reply(primary, cons[primary]->sent.back());

    new_interval();
    EXPECT_EQ(1u, sent_to(parity_osd).size());
    ASSERT_EQ(2u, sent_to(primary).size());
    EXPECT_EQ(parent, sent_to(primary).back().first);
    reply(primary, cons[primary]->sent.back());
    EXPECT_EQ(0, done.wait_for(0));
  }
};

TEST_F(TestSplitOpDecodeMapChange, ECDecodeReadRedrivesWhenPgSplits)
{
  decode_read_redriven_on_new_interval([this] { split_pg(); });
}

TEST_F(TestSplitOpDecodeMapChange, ECDecodeReadRedrivesWhenCodingShardOsdDown)
{
  decode_read_redriven_on_new_interval([this] { mark_down(parity_osd); });
}

TEST_F(TestSplitOpDecodeMapChange, ECDecodeReadCountsDecodeAndCompletion)
{
  submit_read(0, stripe_width, CEPH_OSD_FLAG_BALANCE_READS);
  expect_decode_plan(acting[2]);
  reply_to_all(acting[0]);
  reply_to_all(acting[2]);
  EXPECT_EQ(0, done.wait_for(0));
  EXPECT_EQ(1u, counter("ec_split_op_decode_ops"));
  EXPECT_EQ(stripe_width, counter("ec_split_op_decode_bytes"));
  EXPECT_EQ(1u, counter("ec_split_op_decode_lat"));
  EXPECT_EQ(1u, counter("split_op_completed_ops"));
  EXPECT_EQ(stripe_width, counter("split_op_completed_bytes"));
  EXPECT_EQ(0u, counter("split_op_fallback_eagain"));
  EXPECT_EQ(0u, counter("split_op_fallback_interval"));
  EXPECT_EQ(0u, counter("ec_split_op_decode_error"));
}

// The OSDs answer the sub-reads for an object of 6000 bytes, unless a test
// writes another size, so the client rebuilds shard 1 from shard 0 and the
// coding shard, and trims the reads to the size from the STAT.
class TestSplitOpDecodeComplete : public TestSplitOpDecodeMapChange {
protected:
  static constexpr version_t user_version = 7;
  static constexpr version_t stale_version = user_version - 1;
  const ceph::real_time mtime = ceph::real_clock::from_time_t(1000);
  uint64_t object_size = 0;
  bufferlist data;
  shard_id_map<bufferlist> shards{acting.size()};
  // What the primary records for each shard: the last write did not change
  // shard 1.
  std::map<shard_id_t, eversion_t> versions = {
    {shard_id_t(0), eversion_t(1, user_version)},
    {shard_id_t(1), eversion_t(1, stale_version)},
    {shard_id_t(2), eversion_t(1, user_version)}};
  std::map<std::string, bufferlist> xattrs;
  int cmpxattr_result = 0;
  version_t objver = 0;

  void SetUp() override
  {
    TestSplitOpDecodeMapChange::SetUp();
    write(6000);
    xattrs["user.rgw.etag"].append("etag");
  }

  // The object fits in one stripe. Each shard holds what its OSD stores,
  // which ends with the page holding the end of the object.
  void write(uint64_t size)
  {
    object_size = size;
    data.clear();
    for (uint64_t i = 0; i < size; i++) {
      data.append(char(i % 251));
    }
    bufferlist stripe = data;
    stripe.append_zero(stripe_width - size);
    ECUtil::stripe_info_t sinfo(ec_impl, nullptr, stripe_width);
    shards.clear();
    ASSERT_EQ(0, ec_impl->encode(sinfo.get_all_shards(), stripe, &shards));
    for (auto&& [shard, bl] : shards) {
      bufferlist stored;
      stored.substr_of(bl, 0, sinfo.object_size_to_shard_size(size, shard));
      bl = std::move(stored);
    }
  }

  ceph_tid_t submit(ObjectOperation& rd, bufferlist* outbl = nullptr)
  {
    ceph_tid_t tid = 0;
    objecter->op_submit(objecter->prepare_read_op(
      object_t("obj"), object_locator_t(ec_pool_id), rd, CEPH_NOSNAP, outbl,
      CEPH_OSD_FLAG_BALANCE_READS, -1, &done, &objver), &tid);
    poll();
    return tid;
  }

  // Replies as the OSD would, from a copy of the object that records the
  // given shard versions. Reads of shard extents read the shard the op was
  // sent to, other direct reads read the object within that shard's chunk,
  // and other reads read the object. The other OSDs reply with
  // stale_version as the user version, so a test sees which reply objver
  // came from.
  void answer(int osd, const MessageRef& m,
              const std::map<shard_id_t, eversion_t>& recorded)
  {
    auto op = boost::static_pointer_cast<_mosdop::MOSDOp<osdc_opvec>>(m);
    shard_id_t shard = op->get_spg().shard;
    std::vector<pg_log_op_return_item_t> returns(op->ops.size());
    int result = 0;
    for (unsigned i = 0; i < op->ops.size(); i++) {
      const ceph_osd_op& o = op->ops[i].op;
      bufferlist& out = returns[i].bl;
      switch (o.op) {
      case CEPH_OSD_OP_READ: {
        const bufferlist* bl = &data;
        uint64_t offset = o.extent.offset;
        uint64_t end = offset + o.extent.length;
        if (o.flags & CEPH_OSD_OP_FLAG_EC_SHARD_EXTENT) {
          bl = &shards.at(shard);
        } else if (op->get_flags() & CEPH_OSD_FLAG_EC_DIRECT_READ) {
          uint64_t chunk_start = int(shard) * chunk_size;
          offset = std::clamp(offset, chunk_start, chunk_start + chunk_size);
          end = std::clamp(end, chunk_start, chunk_start + chunk_size);
        }
        end = std::min<uint64_t>(end, bl->length());
        if (offset < end) {
          out.substr_of(*bl, offset, end - offset);
        }
        break;
      }
      case CEPH_OSD_OP_STAT:
        encode(object_size, out);
        encode(mtime, out);
        break;
      case CEPH_OSD_OP_GETXATTR:
        if (auto x = xattrs.find(op->ops[i].indata.to_str());
            x != xattrs.end()) {
          out = x->second;
        } else {
          returns[i].rval = -ENODATA;
          if (!(o.flags & CEPH_OSD_OP_FLAG_FAILOK)) {
            result = -ENODATA;
          }
        }
        break;
      case CEPH_OSD_OP_GETXATTRS:
        encode(xattrs, out);
        break;
      case CEPH_OSD_OP_CMPXATTR:
        returns[i].rval = result = cmpxattr_result;
        break;
      case CEPH_OSD_OP_GET_INTERNAL_VERSIONS: {
        // Only the primary returns the versions of every shard.
        auto sent = recorded;
        if (osd != primary) {
          std::erase_if(sent, [shard](const auto& v) {
            return v.first != shard;
          });
        }
        encode(sent, out);
        break;
      }
      case CEPH_OSD_OP_GET_INTERNAL_VERSIONS_V2:
        encode(internal_versions_v2_response_t{recorded}, out);
        break;
      default:
        ADD_FAILURE() << "unexpected " << ceph_osd_op_name(o.op);
      }
    }
    reply(osd, m, result, std::move(returns),
          osd == primary ? user_version : stale_version);
  }

  void answer(int osd, const MessageRef& m)
  {
    answer(osd, m, versions);
  }

  // The reference sub-read goes to the primary, which holds shard 0.
  void answer_sub_reads()
  {
    answer(primary, cons[primary]->sent.front());
    answer(acting[2], cons[acting[2]]->sent.front());
  }

  void cmpxattr_and_read(bufferlist* bl)
  {
    bufferlist etag;
    etag.append("etag");
    ObjectOperation rd;
    rd.cmpxattr("user.rgw.etag", CEPH_OSD_CMPXATTR_OP_EQ,
                CEPH_OSD_CMPXATTR_MODE_STRING, etag);
    rd.read(0, stripe_width, bl, nullptr, nullptr);
    submit(rd);
    expect_decode_plan(acting[2]);
    answer_sub_reads();
  }

  // reply_to_sub_reads must move exactly the counter fallback.
  void expect_resent_to_the_primary(
    const std::string& fallback,
    const std::function<void()>& reply_to_sub_reads)
  {
    std::map<std::string, bufferlist> attrs;
    uint64_t size = 0;
    bufferlist bl;
    ObjectOperation rd;
    rd.getxattrs(&attrs, nullptr);
    rd.stat(&size, nullptr, nullptr);
    rd.read(0, stripe_width, &bl, nullptr, nullptr);

    ceph_tid_t parent = submit(rd);
    expect_decode_plan(acting[2]);
    auto counters = split_op_counters(g_ceph_context);
    reply_to_sub_reads();
    counters.at(fallback)++;
    EXPECT_EQ(counters, split_op_counters(g_ceph_context));
    EXPECT_TRUE(sent_to(acting[1]).empty());
    EXPECT_EQ(1u, sent_to(acting[2]).size());
    ASSERT_EQ(2u, sent_to(primary).size());
    EXPECT_EQ(std::make_pair(parent, shard_id_t(0)), sent_to(primary).back());
    auto resent = boost::static_pointer_cast<_mosdop::MOSDOp<osdc_opvec>>(
      cons[primary]->sent.back());
    EXPECT_FALSE(resent->get_flags() & (CEPH_OSD_FLAGS_DIRECT_READ |
                                        CEPH_OSD_FLAG_FORCE_OSD |
                                        CEPH_OSD_FLAG_FAIL_ON_EAGAIN));
    ASSERT_EQ(3u, resent->ops.size());
    EXPECT_EQ(CEPH_OSD_OP_GETXATTRS, resent->ops[0].op.op);
    EXPECT_EQ(CEPH_OSD_OP_STAT, resent->ops[1].op.op);
    EXPECT_EQ(CEPH_OSD_OP_READ, resent->ops[2].op.op);
    EXPECT_FALSE(resent->ops[2].op.flags & CEPH_OSD_OP_FLAG_EC_SHARD_EXTENT);

    answer(primary, cons[primary]->sent.back());
    ASSERT_EQ(0, done.wait_for(0));
    EXPECT_EQ(xattrs, attrs);
    EXPECT_EQ(object_size, size);
    EXPECT_TRUE(bl.contents_equal(data)) << bl.length();
    EXPECT_EQ(user_version, objver);
    EXPECT_EQ(counters, split_op_counters(g_ceph_context));
    EXPECT_EQ(2u, sent_to(primary).size());
    EXPECT_EQ(1u, sent_to(acting[2]).size());
  }
};

// RGW reads the xattrs, the size and up to 4 MiB of the head object.
TEST_F(TestSplitOpDecodeComplete, HeadReadIsDecoded)
{
  std::map<std::string, bufferlist> attrs;
  uint64_t size = 0;
  ceph::real_time stat_mtime;
  bufferlist bl;
  int rvals[] = {-1, -1, -1};
  ObjectOperation rd;
  rd.getxattrs(&attrs, &rvals[0]);
  rd.stat(&size, &stat_mtime, &rvals[1]);
  rd.read(0, 4 << 20, &bl, &rvals[2], nullptr);

  submit(rd);
  expect_decode_plan(acting[2]);
  EXPECT_FALSE(std::ranges::any_of(first_ops_sent_to(acting[2]),
                                   [](const OSDOp& o) {
    return o.op.op == CEPH_OSD_OP_GETXATTRS || o.op.op == CEPH_OSD_OP_STAT;
  }));
  auto counters = split_op_counters(g_ceph_context);
  answer_sub_reads();
  ASSERT_EQ(0, done.wait_for(0));
  EXPECT_EQ(xattrs, attrs);
  EXPECT_EQ(object_size, size);
  EXPECT_EQ(mtime, stat_mtime);
  EXPECT_TRUE(bl.contents_equal(data)) << bl.length();
  EXPECT_EQ(user_version, objver);
  for (int rval : rvals) {
    EXPECT_EQ(0, rval);
  }
  EXPECT_EQ(1u, sent_to(primary).size());
  counters.at("ec_split_op_decode_ops")++;
  counters.at("ec_split_op_decode_bytes") += object_size;
  counters.at("ec_split_op_decode_lat")++;
  counters.at("split_op_completed_ops")++;
  counters.at("split_op_completed_bytes") += object_size;
  EXPECT_EQ(counters, split_op_counters(g_ceph_context));
}

TEST_F(TestSplitOpDecodeComplete, ReadsAreTrimmedToTheObjectSize)
{
  bufferlist bls[2];
  ObjectOperation rd;
  rd.read(0, chunk_size, &bls[0], nullptr, nullptr);
  rd.read(chunk_size, chunk_size, &bls[1], nullptr, nullptr);

  submit(rd);
  expect_decode_plan(acting[2]);
  answer_sub_reads();
  ASSERT_EQ(0, done.wait_for(0));
  bufferlist expected;
  expected.substr_of(data, 0, chunk_size);
  EXPECT_TRUE(bls[0].contents_equal(expected)) << bls[0].length();
  expected.substr_of(data, chunk_size, object_size - chunk_size);
  EXPECT_TRUE(bls[1].contents_equal(expected)) << bls[1].length();
}

// Shard 0 holds zeros from the end of the object to the end of its page.
TEST_F(TestSplitOpDecodeComplete, ReadFromPastTheEndReturnsNothing)
{
  write(3000);
  bufferlist bls[2];
  ObjectOperation rd;
  rd.read(0, stripe_width, &bls[0], nullptr, nullptr);
  rd.read(object_size + 500, 500, &bls[1], nullptr, nullptr);

  submit(rd);
  expect_decode_plan(acting[2]);
  answer_sub_reads();
  ASSERT_EQ(0, done.wait_for(0));
  EXPECT_TRUE(bls[0].contents_equal(data)) << bls[0].length();
  EXPECT_EQ(0u, bls[1].length());
}

// Shard 1 holds nothing of an object smaller than a chunk, so the client
// pads it with zeros to rebuild shard 0.
TEST_F(TestSplitOpDecodeComplete, ShardPastTheEndIsPaddedWithZeros)
{
  set_conf("objecter_debug_ec_direct_read_exclude_shards", "0");
  write(3000);
  ASSERT_EQ(0u, shards.at(shard_id_t(1)).length());
  bufferlist bl;
  ObjectOperation rd;
  rd.read(0, object_size, &bl, nullptr, nullptr);

  submit(rd);
  EXPECT_TRUE(std::ranges::none_of(first_ops_sent_to(primary),
                                   [](const OSDOp& o) {
    return o.op.op == CEPH_OSD_OP_READ;
  }));
  auto counters = split_op_counters(g_ceph_context);
  for (int osd : acting) {
    ASSERT_EQ(1u, sent_to(osd).size()) << "osd." << osd;
    answer(osd, cons[osd]->sent.front());
  }
  ASSERT_EQ(0, done.wait_for(0));
  EXPECT_TRUE(bl.contents_equal(data)) << bl.length();
  counters.at("ec_split_op_decode_ops")++;
  counters.at("ec_split_op_decode_bytes") += object_size;
  counters.at("ec_split_op_decode_lat")++;
  counters.at("split_op_completed_ops")++;
  counters.at("split_op_completed_bytes") += object_size;
  EXPECT_EQ(counters, split_op_counters(g_ceph_context));
}

TEST_F(TestSplitOpDecodeComplete, CmpxattrAndReadAreDecoded)
{
  bufferlist bl;
  cmpxattr_and_read(&bl);
  ASSERT_EQ(0, done.wait_for(0));
  EXPECT_TRUE(bl.contents_equal(data)) << bl.length();
}

TEST_F(TestSplitOpDecodeComplete, FailedCmpxattrIsReturned)
{
  cmpxattr_result = -ECANCELED;
  bufferlist bl;
  cmpxattr_and_read(&bl);
  EXPECT_EQ(-ECANCELED, done.wait_for(0));
  EXPECT_EQ(1u, sent_to(primary).size());
  EXPECT_EQ(0u, bl.length());
}

// The op succeeds, but the GETXATTR that may fail returns -ENODATA.
TEST_F(TestSplitOpDecodeComplete, OpReturnCodeIsReturned)
{
  bufferlist val;
  bufferlist bl;
  int rval = 0;
  ObjectOperation rd;
  rd.getxattr("user.missing", &val, &rval);
  rd.set_last_op_flags(CEPH_OSD_OP_FLAG_FAILOK);
  rd.read(0, stripe_width, &bl, nullptr, nullptr);

  submit(rd);
  expect_decode_plan(acting[2]);
  answer_sub_reads();
  ASSERT_EQ(0, done.wait_for(0));
  EXPECT_EQ(-ENODATA, rval);
  EXPECT_TRUE(bl.contents_equal(data)) << bl.length();
}

// The coding shard returns half a chunk, so the decode fails.
TEST_F(TestSplitOpDecodeComplete, ShortShardReadGoesToThePrimary)
{
  bufferlist half;
  half.substr_of(shards.at(shard_id_t(2)), 0, chunk_size / 2);
  shards.at(shard_id_t(2)) = half;
  expect_resent_to_the_primary("ec_split_op_decode_error", [this] {
    answer_sub_reads();
  });
}

TEST_F(TestSplitOpDecodeComplete, ShardEioGoesToThePrimary)
{
  expect_resent_to_the_primary("split_op_fallback_shard_error", [this] {
    answer(primary, cons[primary]->sent.front());
    reply(acting[2], cons[acting[2]]->sent.front(), -EIO);
  });
}

TEST_F(TestSplitOpDecodeComplete, ShardEagainGoesToThePrimary)
{
  expect_resent_to_the_primary("split_op_fallback_eagain", [this] {
    answer(primary, cons[primary]->sent.front());
    reply(acting[2], cons[acting[2]]->sent.front(), -EAGAIN);
  });
}

// The coding shard has not applied the last write that the primary records
// for it.
TEST_F(TestSplitOpDecodeComplete, StaleShardGoesToThePrimary)
{
  auto stale = versions;
  stale[shard_id_t(2)] = eversion_t(1, stale_version);
  expect_resent_to_the_primary("split_op_fallback_version", [&] {
    answer(primary, cons[primary]->sent.front());
    answer(acting[2], cons[acting[2]]->sent.front(), stale);
  });
}

// The read is cancelled once the coding shard has returned -EAGAIN, so it
// completes with -ECANCELED and is not resent.
TEST_F(TestSplitOpDecodeComplete, CancelledReadIsNotResent)
{
  bufferlist bl;
  ObjectOperation rd;
  rd.read(0, stripe_width, &bl, nullptr, nullptr);
  ceph_tid_t parent = submit(rd);
  expect_decode_plan(acting[2]);
  auto counters = split_op_counters(g_ceph_context);
  reply(acting[2], cons[acting[2]]->sent.front(), -EAGAIN);
  ASSERT_EQ(ETIMEDOUT, done.wait_for(0)) << "completed before the cancel";

  EXPECT_EQ(0, objecter->op_cancel(parent, -ECANCELED));
  poll();
  EXPECT_EQ(-ECANCELED, done.wait_for(0));
  EXPECT_EQ(0u, bl.length());
  EXPECT_EQ(counters, split_op_counters(g_ceph_context));
  EXPECT_EQ(1u, sent_to(primary).size());
}

// A read through the primary gets the out data of every op, in op order, as
// the reply data, which the Objecter returns in outbl. A split read, here
// with no shard excluded, must return the same.
TEST_F(TestSplitOpDecodeComplete, SplitReadReturnsTheDataOfEveryOpInOutbl)
{
  set_conf("objecter_debug_ec_direct_read_exclude_shards", "");
  bufferlist outbl;
  ObjectOperation rd;
  rd.stat(nullptr, nullptr, nullptr);
  rd.read(0, stripe_width, nullptr, nullptr, nullptr);

  submit(rd, &outbl);
  for (int osd : {primary, acting[1]}) {
    ASSERT_EQ(1u, sent_to(osd).size()) << "osd." << osd;
    answer(osd, cons[osd]->sent.front());
  }
  ASSERT_EQ(0, done.wait_for(0));
  bufferlist expected;
  encode(object_size, expected);
  encode(mtime, expected);
  expected.append(data);
  EXPECT_TRUE(outbl.contents_equal(expected)) << outbl.length();
}
