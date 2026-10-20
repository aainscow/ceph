/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2025 IBM
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 */

#include <algorithm>
#include <tuple>

#include "osdc/SplitOp.h"
#include "osdc/Objecter.h"
#include "osd/ECUtil.h"
#include "osd/osd_types.h"
#include "erasure-code/ErasureCodePlugin.h"
#include "global/global_context.h"

using namespace std::literals;

#define dout_subsys ceph_subsys_objecter
#define DBG_LVL 20

namespace {
inline boost::system::error_code osdcode(int r) {
  return (r < 0) ? boost::system::error_code(-r, osd_category()) : boost::system::error_code();
}
}

constexpr static uint64_t kReplicaMinShardReads = 2;

/**
 * A pool's EC plugin and stripe geometry, for client decode. sinfo points at
 * the copy of the pool here, which outlives the OSDMap it came from.
 */
struct ECCodecInfo {
  pg_pool_t pool;
  ceph::ErasureCodeInterfaceRef ec_impl;
  std::optional<ECUtil::stripe_info_t> sinfo;

  explicit ECCodecInfo(const pg_pool_t &pool) : pool(pool) {}
  ECCodecInfo(const ECCodecInfo &) = delete;
  ECCodecInfo &operator=(const ECCodecInfo &) = delete;
};

#undef dout_prefix
#define dout_prefix *_dout << " ECSplitOp::"

/**
 * @brief Initialize reference_sub_read to primary shard.
 *
 * Performs reverse lookup to find which acting index corresponds to the
 * primary shard. Must be called after _calc_target() populates the acting set.
 */
void ECSplitOp::init_reference_sub_read() {
  auto &target = orig_op->target;
  shard_id_t primary_shard = target.actual_pgid.shard;
  
  // Search through the acting set to find which index has the primary shard
  // For EC pools, the acting index directly corresponds to the shard
  for (size_t i = 0; i < target.acting.size(); i++) {
    if (shard_id_t(i) == primary_shard) {
      reference_sub_read = i;
      ldout(cct, DBG_LVL) << __func__ << " set reference_sub_read=" << reference_sub_read
                          << " for primary_shard=" << (int)primary_shard << dendl;
      return;
    }
  }
  
  ldout(cct, DBG_LVL) << __func__ << " WARNING: Could not find primary shard "
                << (int)primary_shard << " in acting set" << dendl;
  abort = true;
}

/**
 * @brief Assemble sparse read results from EC shards into logical object view.
 *
 * Iterates through the EC stripe using ECStripeView, collecting extent maps
 * and data buffers from each shard. The resulting buffer is built up in order,
 * only extending (never inserting into the middle), which maintains buffer
 * efficiency and allows for zero-copy operations where possible.
 *
 * @param ops_index Index of the operation in the operation list
 * @return Pair containing the extent set and assembled buffer list
 */
std::pair<SplitOp::extent_set, bufferlist> ECSplitOp::assemble_buffer_sparse_read(int ops_index) const {
  bufferlist bl_out;
  extent_set extents_out;

  auto &orig_osd_op = orig_op->ops[ops_index].op;
  const pg_pool_t *pi = objecter.osdmap->get_pg_pool(orig_op->target.base_oloc.pool);
  ceph_assert(pi);
  ECStripeView stripe_view(orig_osd_op.extent.offset, orig_osd_op.extent.length, pi);
  ldout(cct, DBG_LVL) << __func__ << " START -->" << " object_id=" << orig_op->target.base_oid << " tid=" << orig_op->tid << " extent="
    << orig_osd_op.extent.offset << "~" << orig_osd_op.extent.length << dendl;

  std::vector<uint64_t> buffer_offset(stripe_view.data_chunk_count);
  mini_flat_map<int, extents_map::const_iterator> map_iterators(stripe_view.data_chunk_count);

  for (auto &&chunk_info : stripe_view) {
    shard_id_t shard = pi->get_shard(chunk_info.raw_shard);
    ceph_assert(shard != shard_id_t::NO_SHARD);
    int shard_index = (int)shard;
    ldout(cct, DBG_LVL) << __func__ << " object_id=" << orig_op->target.base_oid << " tid=" << orig_op->tid << " chunk: " << chunk_info
        << " shard: " << shard << dendl;
    auto &details = sub_reads.at(shard_index).details.at(ops_index);

    if (!map_iterators.contains(shard_index)) {
      map_iterators.emplace(shard_index, details.e->begin());
    }

    extents_map::const_iterator &extent_iter = map_iterators.at(shard_index);

    uint64_t bl_len = 0;
    while (extent_iter != details.e->end() && extent_iter->first < chunk_info.ro_offset + stripe_view.chunk_size) {
      auto [off, len] = *extent_iter;
      ldout(cct, DBG_LVL) << __func__ << " object_id=" << orig_op->target.base_oid << " tid=" << orig_op->tid << " extent=" << off << "~" <<  len << dendl;
      extents_out.insert(off, len);
      bl_len += len;
      ++extent_iter;
    }

    // We try to keep the buffers together where possible.
    if (bl_len != 0) {
      bufferlist bl;
      bl.substr_of(details.bl, buffer_offset[(int)chunk_info.raw_shard], bl_len);
      bl_out.append(bl);
      buffer_offset[(int)chunk_info.raw_shard] += bl_len;
    }
  }

  ldout(cct, DBG_LVL) << __func__ << " END -->" << " object_id=" << orig_op->target.base_oid << " tid=" << orig_op->tid << " extent="
    << orig_osd_op.extent.offset << "~" << orig_osd_op.extent.length << dendl;

  return std::pair(extents_out, bl_out);
}

/**
 * @brief Assemble dense read results from EC shards into contiguous buffer.
 *
 * Iterates through the EC stripe, extracting data from each shard in order.
 * The output buffer is built sequentially by appending data, never inserting
 * into the middle, which maintains buffer efficiency.
 *
 * @param bl_out Output buffer to append assembled data
 * @param ops_index Index of the operation in the operation list
 */
void ECSplitOp::assemble_buffer_read(bufferlist &bl_out, int ops_index) const {
  auto &orig_osd_op = orig_op->ops[ops_index].op;
  if (decoded) {
    uint64_t offset = orig_osd_op.extent.offset;
    if (offset < object_size) {
      uint64_t length = std::min<uint64_t>(orig_osd_op.extent.length,
                                           object_size - offset);
      bl_out.append(decoded->get_ro_buffer(offset, length));
    }
    return;
  }
  const pg_pool_t *pi = objecter.osdmap->get_pg_pool(orig_op->target.base_oloc.pool);
  ceph_assert(pi);
  ECStripeView stripe_view(orig_osd_op.extent.offset, orig_osd_op.extent.length, pi);

  std::vector<uint64_t> buffer_offset(stripe_view.data_chunk_count);
  ldout(cct, DBG_LVL) << __func__ << " object_id=" << orig_op->target.base_oid << " tid=" << orig_op->tid << " extent="
    << orig_osd_op.extent.offset << "~" << orig_osd_op.extent.length << dendl;

  for (auto &&chunk_info : stripe_view) {
    ldout(cct, DBG_LVL) << __func__ << " object_id=" << orig_op->target.base_oid << " tid=" << orig_op->tid << " chunk info " << chunk_info << dendl;
    shard_id_t shard = pi->get_shard(chunk_info.raw_shard);
    ceph_assert(shard != shard_id_t::NO_SHARD);
    int shard_index = (int)shard;
    auto &details = sub_reads.at(shard_index).details.at(ops_index);
    uint64_t src_len = details.bl.length();
    uint64_t buf_off = buffer_offset[(int)chunk_info.raw_shard];
    if (src_len <= buf_off) {
      // Early termination: we've exhausted the available buffer data.
      // This is a normal completion scenario, not an error condition.
      break;
    }
    uint64_t len = std::min(chunk_info.length, src_len - buf_off);
    bufferlist bl;
    bl.substr_of(details.bl, buf_off, len);
    bl_out.append(bl);
    buffer_offset[(int)chunk_info.raw_shard] += len;
  }
}

/**
 * @brief Initialize read sub-operations for erasure-coded pool.
 *
 * Determines which EC shards need to be read based on stripe geometry and
 * creates sub-operations for each required shard. Sets the abort flag if
 * any required OSD is unavailable.
 *
 * Primary shard is required when reading from multiple chunks, when version
 * information is requested, or when non-read operations are present.
 *
 * @param op Operation descriptor containing offset and length
 * @param sparse Whether this is a sparse read operation
 * @param ops_index Index of the operation in the operation list
 * @param single_op True if the whole op will be sent as a single op
 */
void ECSplitOp::init_read(OSDOp &op, bool sparse, int ops_index, bool single_op) {
  auto &target = orig_op->target;
  const pg_pool_t *pi = objecter.osdmap->get_pg_pool(target.base_oloc.pool);
  ceph_assert(pi);

  uint64_t offset = op.op.extent.offset;
  uint64_t length = op.op.extent.length;
  uint64_t data_chunk_count = pi->get_ec_data_shard_count();
  uint32_t chunk_size = pi->get_stripe_width() / data_chunk_count;
  uint64_t start_chunk = offset / chunk_size;
  // This calculation is wrong for length = 0, but such IOs should not have
  // reached here! Zero-length reads are rejected earlier in the validate()
  // function (see lines 551-556), which returns {false, false} for any
  // operation with length == 0, preventing it from being split and processed.
  ceph_assert( op.op.extent.length != 0);
  // No overflow: length >= 1 (asserted above), so offset + length - 1 <= offset + length.
  // Since offset and length are uint64_t, their sum cannot exceed UINT64_MAX in practice
  // due to object size limits and memory constraints. Even if offset + length == UINT64_MAX + 1
  // (impossible in uint64_t), subtracting 1 brings it back to UINT64_MAX (valid).
  uint64_t end_chunk = (offset + op.op.extent.length - 1) / chunk_size;

  unsigned count = std::min(data_chunk_count, end_chunk - start_chunk + 1);
  // Primary is required if:
  // 1. Reading from multiple chunks (count > 1)
  // 2. Version information is needed (orig_op->objver)
  // 3. There are non-read operations that need the primary
  bool has_non_read_ops = false;
  for (const auto &op : orig_op->ops) {
    if (op.op.op != CEPH_OSD_OP_READ && op.op.op != CEPH_OSD_OP_SPARSE_READ) {
      has_non_read_ops = true;
      break;
    }
  }

  bool primary_required = orig_op->objver || has_non_read_ops ||
                          (!single_op &&
                           strategy == InternalVersionStrategy::FromPrimaryShard);

  int first_shard = start_chunk % data_chunk_count;
  // Check all shards are online.
  for (unsigned i = first_shard; i < first_shard + count; i++) {
    raw_shard_id_t raw_shard(i >= data_chunk_count ? i - data_chunk_count : i);
    shard_id_t shard(pi->get_shard(raw_shard));
    if (shard == shard_id_t::NO_SHARD) {
      abort = true;
      return;
    }
    int shard_index = (int)shard;

    int direct_osd = target.acting[shard_index];
    if (!objecter.osdmap->exists(direct_osd)) {
      ldout(cct, DBG_LVL) << __func__ <<" ABORT: Missing OSD" << dendl;
      abort = true;
      return;
    }
    if (!sub_reads.contains(shard_index)) {
      sub_reads.emplace(shard_index, orig_op->ops.size() + 1);
    }
    auto &d = sub_reads.at(shard_index).details[ops_index];
    if (sparse) {
      d.e.emplace();
      sub_reads.at(shard_index).rd.sparse_read(offset, length, &(*d.e), &d.bl, &d.rval);
    } else {
      sub_reads.at(shard_index).rd.read(offset, length, &d.ec, &d.bl);
    }
  }

  // If primary is required and we haven't created a sub_read for it yet, create one
  if (primary_required && !sub_reads.contains(reference_sub_read)) {
    sub_reads.emplace(reference_sub_read, orig_op->ops.size() + 1);
  }
}

shard_id_set ECSplitOp::available_shards(const Objecter::op_target_t &target,
                                         Objecter &objecter) {
  uint64_t excluded = objecter.debug_ec_direct_read_exclude_shards;
  shard_id_set available;
  for (unsigned i = 0; i < target.acting.size(); i++) {
    int osd = target.acting[i];
    if (osd != CRUSH_ITEM_NONE && objecter.osdmap->is_up(osd) &&
        (i >= 64 || !(excluded & (1ull << i)))) {
      available.insert(shard_id_t(i));
    }
  }
  return available;
}

bool ECSplitOp::reads_unavailable_shard(Objecter::Op *op, const pg_pool_t *pi,
                                        const shard_id_set &available) {
  ECUtil::stripe_info_t sinfo(pi);
  ECUtil::shard_extent_set_t want(sinfo.get_k_plus_m());
  for (auto &o : op->ops) {
    if (o.op.op == CEPH_OSD_OP_READ || o.op.op == CEPH_OSD_OP_SPARSE_READ) {
      sinfo.ro_range_to_shard_extent_set(o.op.extent.offset,
                                         o.op.extent.length, want);
    }
  }
  return !available.includes(want.get_shard_id_set());
}

bool ECSplitOp::may_decode(Objecter::Op *op, const pg_pool_t *pi,
                           Objecter &objecter) {
  if (!pi->has_flag(pg_pool_t::FLAG_EC_CLIENT_DECODE) ||
      !objecter.ec_client_decode) {
    return false;
  }
  // set_pool_flags sets the pool flag at any release.
  if (!objecter.osdmap->ec_client_decode_release_ok() &&
      !objecter.debug_ec_client_decode_skip_release_check) {
    return false;
  }
  // Decode loses allocation information, and the size from a STAT does not
  // account for truncates that the client knows of and the OSD does not yet.
  return std::ranges::none_of(op->ops, [](const OSDOp &o) {
    return o.op.op == CEPH_OSD_OP_SPARSE_READ ||
      (o.op.op == CEPH_OSD_OP_READ && o.op.extent.truncate_seq != 0);
  });
}

bool ECSplitOp::single_op_reads_unavailable_shard(Objecter::Op *op,
                                                  Objecter &objecter) {
  const pg_pool_t *pi = objecter.osdmap->get_pg_pool(op->target.base_oloc.pool);
  auto target = op->target;
  target.flags &= ~CEPH_OSD_FLAG_BALANCE_READS;
  objecter._calc_target(&target);
  return reads_unavailable_shard(op, pi, available_shards(target, objecter));
}

std::shared_ptr<const ECCodecInfo> ECSplitOp::get_codec(int64_t pool_id,
                                                        const pg_pool_t *pi) {
  std::shared_ptr<const ECCodecInfo> codec;
  {
    std::lock_guard l{objecter.ec_codecs_lock};
    if (auto it = objecter.ec_codecs.find(pool_id);
        it != objecter.ec_codecs.end()) {
      codec = it->second;
    } else {
      auto info = std::make_shared<ECCodecInfo>(*pi);
      ErasureCodeProfile profile =
        objecter.osdmap->get_erasure_code_profile(pi->erasure_code_profile);
      std::stringstream ss;
      int r = -EINVAL;
      if (!g_ceph_context) {
        ss << "erasure code plugins need the global context, which this "
           << "process has not set up";
      } else if (auto plugin = profile.find("plugin");
                 plugin != profile.end()) {
        r = ceph::ErasureCodePluginRegistry::instance().factory(
          plugin->second, cct->_conf.get_val<std::string>("erasure_code_dir"),
          profile, &info->ec_impl, &ss);
      } else {
        ss << "no plugin in erasure code profile "
           << pi->erasure_code_profile;
      }
      if (r == 0) {
        info->sinfo.emplace(info->ec_impl, &info->pool,
                            pi->get_stripe_width());
        codec = info;
      } else {
        lderr(cct) << "unable to load erasure code plugin for pool "
                   << pool_id << ": " << ss.str()
                   << "; degraded reads will use the primary OSD" << dendl;
      }
      objecter.ec_codecs[pool_id] = codec;
    }
  }
  if (codec && codec->sinfo->supports_sub_chunks()) {
    return nullptr;
  }
  return codec;
}

void ECSplitOp::init_degraded(std::shared_ptr<const ECCodecInfo> _codec,
                              const shard_id_set &available) {
  codec = std::move(_codec);
  const ECUtil::stripe_info_t &sinfo = *codec->sinfo;

  ECUtil::shard_extent_set_t want(sinfo.get_k_plus_m());
  for (auto &o : orig_op->ops) {
    if (o.op.op == CEPH_OSD_OP_READ) {
      sinfo.ro_range_to_shard_extent_set(o.op.extent.offset,
                                         o.op.extent.length, want);
    }
  }
  ECUtil::shard_extent_set_t reads(sinfo.get_k_plus_m());
  ECUtil::shard_extent_set_t zeros(sinfo.get_k_plus_m());
  int r = ECUtil::plan_read(sinfo, codec->ec_impl, want, available, false,
                            std::nullopt, reads, zeros);
  if (r < 0) {
    ldout(cct, DBG_LVL) << __func__ << " ABORT: cannot decode " << want
                        << " from " << available << " r=" << r << dendl;
    abort = true;
    return;
  }
  ldout(cct, DBG_LVL) << __func__ << " object_id=" << orig_op->target.base_oid
                      << " want=" << want << " available=" << available
                      << " reads=" << reads << dendl;

  read_shards = reads.get_shard_id_set();
  for (auto &&[shard, eset] : reads) {
    int index = int(shard);
    if (!sub_reads.contains(index)) {
      sub_reads.emplace(index, orig_op->ops.size() + 1);
    }
    SubRead &sr = sub_reads.at(index);
    for (auto [off, len] : eset) {
      Details &d = sr.shard_extents[off];
      sr.rd.read(off, len, &d.ec, &d.bl);
      sr.rd.set_last_op_flags(CEPH_OSD_OP_FLAG_EC_SHARD_EXTENT);
    }
  }

  for (unsigned i = 0; i < orig_op->ops.size(); ++i) {
    if (orig_op->ops[i].op.op != CEPH_OSD_OP_READ) {
      init(orig_op->ops[i], i, false);
    }
  }

  if (!sub_reads.contains(reference_sub_read)) {
    sub_reads.emplace(reference_sub_read, orig_op->ops.size() + 1);
  }
  sub_reads.at(reference_sub_read).rd.stat(&object_size, nullptr, nullptr);
}

int ECSplitOp::prepare_assembly() {
  if (!codec) {
    return 0;
  }
  const ECUtil::stripe_info_t &sinfo = *codec->sinfo;

  auto buffers = std::make_unique<ECUtil::shard_extent_map_t>(&sinfo);
  for (auto &&[index, sr] : sub_reads) {
    for (auto &&[off, d] : sr.shard_extents) {
      if (d.bl.length() != 0) {
        buffers->insert_in_shard(shard_id_t(index), off, d.bl);
      }
    }
  }

  ECUtil::shard_extent_set_t want(sinfo.get_k_plus_m());
  uint64_t bytes = 0;
  for (auto &o : orig_op->ops) {
    if (o.op.op == CEPH_OSD_OP_READ && o.op.extent.offset < object_size) {
      uint64_t length = std::min<uint64_t>(o.op.extent.length,
                                           object_size - o.op.extent.offset);
      sinfo.ro_range_to_shard_extent_set(o.op.extent.offset, length, want);
      bytes += length;
    }
  }

  auto start = ceph::mono_clock::now();
  int r = ECUtil::complete_read(sinfo, codec->ec_impl, want, read_shards,
                                object_size, *buffers);
  if (r < 0) {
    ldout(cct, 0) << __func__ << " decode failed r=" << r
                  << " pool=" << orig_op->target.base_oloc.pool
                  << " pgid=" << orig_op->target.actual_pgid
                  << " object_id=" << orig_op->target.base_oid
                  << " epoch=" << orig_op->target.epoch
                  << " size=" << object_size << " want=" << want
                  << " read_shards=" << read_shards
                  << "; resending to the primary" << dendl;
    objecter.count_split_op_event(Objecter::SplitOpEvent::decode_error);
    return -EAGAIN;
  }
  objecter.count_split_op_event(Objecter::SplitOpEvent::decode, bytes);
  objecter.count_ec_split_op_decode_latency(ceph::mono_clock::now() - start);
  decoded = std::move(buffers);
  return 0;
}

ECSplitOp::ECSplitOp(Objecter::Op *op, Objecter &objecter, CephContext *cct,
                     int count)
  : SplitOp(op, objecter, cct, count) {}

ECSplitOp::~ECSplitOp() {
  complete();
}

#undef dout_prefix
#define dout_prefix *_dout << " ReplicaSplitOp::"

/**
 * @brief Initialize reference_sub_read to a random valid OSD.
 *
 * Collects the valid acting indices and picks one at random. Must be
 * called after _calc_target() populates the acting set.
 */
void ReplicaSplitOp::init_reference_sub_read() {
  auto &target = orig_op->target;

  for (size_t i = 0; i < target.acting.size(); i++) {
    int direct_osd = target.acting[i];
    if (objecter.osdmap->exists(direct_osd)) {
      valid_indices.push_back(i);
    }
  }

  if (valid_indices.size() < 2) {
    abort = true;
    ldout(cct, DBG_LVL) << __func__ << " ABORT: Not enough valid OSDs" << dendl;
    return;
  }

  reference_valid_index = rand() % valid_indices.size();
  reference_sub_read = valid_indices[reference_valid_index];
}

/**
 * @brief Assemble sparse read results from replicas.
 *
 * Collects extent maps and buffers from each sub-operation. Results are
 * assembled in order by appending, maintaining buffer efficiency.
 *
 * @param ops_index Index of the operation in the operation list
 * @return Pair containing the combined extent set and buffer list
 */
std::pair<SplitOp::extent_set, bufferlist> ReplicaSplitOp::assemble_buffer_sparse_read(int ops_index) const {
  extent_set extents_out;
  bufferlist bl_out;

  for (auto && [acting_index, sr] : sub_reads) {
    // Ops with fewer chunks than sub-reads do not use every sub-read.
    if (!sr.details.contains(ops_index)) {
      continue;
    }
    for (auto [off, len] : *sr.details.at(ops_index).e) {
      extents_out.insert(off, len);
    }
    bl_out.append(sr.details.at(ops_index).bl);
  }

  return std::pair(extents_out, bl_out);
}

/**
 * @brief Assemble dense read results from replicas.
 *
 * Appends buffers from each sub-operation in order.
 *
 * @param bl_out Output buffer to append assembled data
 * @param ops_index Index of the operation in the operation list
 */
void ReplicaSplitOp::assemble_buffer_read(bufferlist &bl_out, int ops_index) const {
  for (auto && [acting_index, sr] : sub_reads) {
    if (!sr.details.contains(ops_index)) {
      continue;
    }
    bl_out.append(sr.details.at(ops_index).bl);
  }
}

/**
 * @brief Initialize read sub-operations for replicated pool.
 *
 * Divides the operation into chunks and distributes them across available
 * replicas for parallel execution. Chunk size is calculated based on the
 * minimum shard read size configuration and number of available OSDs.
 * Chunks are aligned to page boundaries for efficiency.
 *
 * If the operation is too small to benefit from splitting across all replicas,
 * fewer OSDs are used with a random starting offset for load balancing.
 *
 * @param op Operation descriptor containing offset and length
 * @param sparse Whether this is a sparse read operation
 * @param ops_index Index of the operation in the operation list
 * @param single_op True if the whole op will be sent as a single op
 */
void ReplicaSplitOp::init_read(OSDOp &op, bool sparse, int ops_index, bool single_op) {

  auto &target = orig_op->target;
  const pg_pool_t *pi = objecter.osdmap->get_pg_pool(target.base_oloc.pool);
  ceph_assert(pi);

  // init_reference_sub_read() aborts on < 2 valid OSDs; checked by create().
  ceph_assert(valid_indices.size() >= 2);

  uint64_t replica_min_shard_read_size
    = objecter.get_min_split_replica_read_size();

  uint64_t offset = op.op.extent.offset;
  uint64_t length = op.op.extent.length;
  uint64_t slice_count = replica_min_shard_read_size == 0 ? 1 :
                          std::min(length / replica_min_shard_read_size,
                                   static_cast<uint64_t>(valid_indices.size()));
  // A multi-op request may carry a read shorter than the minimum split
  // size; serve it whole from the reference OSD.
  slice_count = std::max<uint64_t>(slice_count, 1);
  // Round up, otherwise chunk_count can exceed slice_count and reuse a
  // sub-read.
  uint64_t chunk_size = p2roundup(div_round_up(length, slice_count),
                                  REPLICA_MIN_SPLIT_SIZE);
  uint64_t chunk_count = div_round_up(length, chunk_size);

  // Window starts at reference_sub_read for load balancing; chunks are
  // assigned in ascending acting-index order to match assemble_buffer_read().
  std::vector<int> chosen_indices;
  for (uint64_t i = 0; i < chunk_count; i++) {
    chosen_indices.push_back(
      valid_indices[(reference_valid_index + i) % valid_indices.size()]);
  }
  std::sort(chosen_indices.begin(), chosen_indices.end());

  for (int acting_index : chosen_indices) {
    if (!sub_reads.contains(acting_index)) {
      sub_reads.emplace(acting_index, orig_op->ops.size() + 1);
    }
    auto &sr = sub_reads.at(acting_index);
    auto bl = &sr.details[ops_index].bl;
    auto rval = &sr.details[ops_index].rval;
    uint64_t len = std::min(length, chunk_size);
    if (sparse) {
      sr.details[ops_index].e.emplace();
      sr.rd.sparse_read(offset, len, &(*sr.details[ops_index].e), bl, rval);
    } else {
      sr.rd.read(offset, len, &sr.details[ops_index].ec, bl);
    }
    offset += len;
    length -= len;
  }
  ceph_assert(length == 0);
}

#undef dout_prefix
#define dout_prefix *_dout << " SplitOp::"

/**
 * @brief Assemble the final return code from all sub-operations.
 *
 * Returns the first non-EAGAIN error encountered, or EAGAIN if any
 * sub-operation returned EAGAIN or if version mismatch is detected.
 * Otherwise returns the reference sub-operation's return code.
 *
 * @return Combined return code for the operation
 */
int SplitOp::assemble_rc() const {
  int rc = 0;
  bool eagain = false;
  bool interval_change = false;
  bool shard_error = false;

  // Sub-reads which use a single op should re-use original op.
  ceph_assert(sub_reads.size() > 1);

  for (auto & [index, sub_read] : sub_reads) {
    if (sub_read.rc == -EAGAIN) {
      // The Objecter fails a sub-read without a reply when its PG changes.
      if (sub_read.reply_epoch) {
        eagain = true;
      } else {
        interval_change = true;
      }
    } else if (index == reference_sub_read) {
      rc = sub_read.rc;
    } else if (sub_read.rc == -EIO || sub_read.rc == -ENOENT ||
               sub_read.rc == -ENODATA) {
      // A race, or a medium error that the primary must record and repair.
      shard_error = true;
    } else if (sub_read.rc < 0) {
      return sub_read.rc;
    }
  }

  // A cancelled or timed out op must not be resent.
  if (rc == -ECANCELED || rc == -ETIMEDOUT) {
    return rc;
  }
  if (eagain) {
    objecter.count_split_op_event(Objecter::SplitOpEvent::fallback_eagain);
    return -EAGAIN;
  }
  if (interval_change) {
    objecter.count_split_op_event(Objecter::SplitOpEvent::fallback_interval);
    return -EAGAIN;
  }
  // The reference decides whether the object exists and runs the ops other
  // than reads, so its errors are those the primary would return.
  if (rc == -EIO) {
    shard_error = true;
  } else if (rc < 0) {
    return rc;
  }
  if (shard_error) {
    objecter.count_split_op_event(Objecter::SplitOpEvent::fallback_shard_error);
    return -EAGAIN;
  }
  if (version_mismatch()) {
    objecter.count_split_op_event(Objecter::SplitOpEvent::fallback_version);
    return -EAGAIN;
  }
  return rc;
}

/**
 * @brief Check for version mismatches across EC shards.
 *
 * Implements torn read detection by comparing internal versions returned
 * by each shard. The reference shard maintains a map of all shard versions,
 * allowing detection of inconsistencies across the EC stripe.
 *
 * @return true if versions mismatch, false if consistent
 */
bool SplitOp::version_mismatch() const {
  ceph_assert(!sub_reads.empty());

  switch(strategy) {
    case InternalVersionStrategy::FromParticipatingShards:
      return version_mismatch_v2();
    case InternalVersionStrategy::FromPrimaryShard:
      return legacy_version_mismatch();
    default:
      ceph_abort_msg("Unknown internal version strategy used");
  }
}

bool ECSplitOp::legacy_version_mismatch() const {
  // First we need to decode the version list from the reference.
  ceph_assert(reference_sub_read != -1);
  ceph_assert(sub_reads.contains(reference_sub_read));
  ceph_assert(sub_reads.at(reference_sub_read).internal_version.has_value());

  std::map<shard_id_t, eversion_t> ref_vers;
  decode(ref_vers, sub_reads.at(reference_sub_read).internal_version->bl);

  for (auto & [shard_index, sub_read] : sub_reads) {
      // Reference shard can't be different to itself.
    if (shard_index == reference_sub_read) {
      continue;
    }

    ceph_assert(sub_read.internal_version.has_value());
    std::map<shard_id_t, eversion_t> shard_vers;
    decode(shard_vers, sub_read.internal_version->bl);

    shard_id_t shard(shard_index);
    if (!ref_vers.contains(shard)) {
      ldout(cct, DBG_LVL) << __func__ << ": "
         << "Reference shard version missing, failing split op." << dendl;
      return true;
    }
    if (!shard_vers.contains(shard)) {
      ldout(cct, DBG_LVL) << __func__ << ": "
         << "Shard version missing, failing split op." << dendl;
      return true;
    }
    if (ref_vers.at(shard) != shard_vers.at(shard)) {
      ldout(cct, DBG_LVL) << __func__ << ": "
               << "Primary version (" << ref_vers.at(shard) << ") != "
               << "shard version (" << shard_vers.at(shard) <<") "
               << "for shard " << shard << dendl;
      return true;
    }
  }
  return false;
}

bool ECSplitOp::version_mismatch_v2() const {
  // Our reference shard needs to be the newest shard
  std::map<shard_id_t, std::map<shard_id_t, eversion_t>> shard_to_shard_vers_map;
  int ref_shard_index = -1;
  std::optional<eversion_t> latest_version;
  for (auto & [shard_index, sub_read] : sub_reads) {
    ceph_assert(sub_read.internal_version.has_value());

    shard_id_t shard_id(shard_index);
    internal_versions_v2_response_t response;
    decode(response, sub_read.internal_version->bl);
    std::map<shard_id_t, eversion_t>& shard_versions
      = shard_to_shard_vers_map[shard_id];
    shard_versions = std::move(response.shard_versions);

    if(!shard_versions.contains(shard_id)) {
      ldout(cct, DBG_LVL) << __func__ << ": "
         << "Shard version missing, failing split op." << dendl;
      return true;
    }

    if (!latest_version || latest_version < shard_versions.at(shard_id)) {
      latest_version = shard_versions.at(shard_id);
      ref_shard_index = shard_index;
    }
  }

  ceph_assert(latest_version);

  const std::map<shard_id_t, eversion_t>& reference_shard_versions
    = shard_to_shard_vers_map.at(shard_id_t(ref_shard_index));

  for (auto & [shard_index, sub_read] : sub_reads) {
    // Reference shard can't be different to itself.
    if (shard_index == ref_shard_index) {
      continue;
    }

    shard_id_t shard_id(shard_index);
    const std::map<shard_id_t, eversion_t>& shard_versions =
      shard_to_shard_vers_map.at(shard_id);

    if (!reference_shard_versions.contains(shard_id)) {
      ldout(cct, DBG_LVL) << __func__ << ": "
         << "Reference shard version missing, failing split op." << dendl;
      return true;
    }
    if (reference_shard_versions.at(shard_id) != shard_versions.at(shard_id)) {
      ldout(cct, DBG_LVL) << __func__ << ": "
               << "Primary version (" << reference_shard_versions.at(shard_id)
               << ") != " << "shard version (" << shard_versions.at(shard_id)
               << ") " << "for shard " << shard_id << dendl;
      return true;
    }
  }
  return false;
}

/**
 * @brief Check for version mismatches across replicas.
 *
 * For replicated pools, checks that all replicas returned the same version.
 * No single replica maintains a reference version list, so the first replica's
 * version becomes the reference for comparison.
 *
 * @return true if versions mismatch, false if consistent
 */
bool ReplicaSplitOp::legacy_version_mismatch() const {
  std::optional<eversion_t> ref_version;

  for (const auto& [acting_index, sub_read] : sub_reads) {
    ceph_assert(sub_read.internal_version.has_value());

    std::map<shard_id_t, eversion_t> shard_vers;
    decode(shard_vers, sub_read.internal_version->bl);

    if (!shard_vers.contains(shard_id_t::NO_SHARD)) {
      ldout(cct, DBG_LVL) << __func__ << ": "
        << "Replica version missing for acting index, failing split op." << dendl;
      return true;
    }

    if (!ref_version) {
      ref_version = shard_vers.at(shard_id_t::NO_SHARD);
    } else if (*ref_version != shard_vers.at(shard_id_t::NO_SHARD)) {
      return true;
    }
  }

  return false;
}

/**
 * @brief Check for version mismatches across replicas.
 *
 * For replicated pools, checks that all replicas returned the same version.
 * No single replica maintains a reference version list, so the first replica's
 * version becomes the reference for comparison.
 *
 * @return true if versions mismatch, false if consistent
 */
bool ReplicaSplitOp::version_mismatch_v2() const {
  std::optional<eversion_t> reference_version;

  for (const auto& [acting_index, sub_read] : sub_reads) {
    ceph_assert(sub_read.internal_version.has_value());

    internal_versions_v2_response_t response;
    decode(response, sub_read.internal_version->bl);
    std::map<shard_id_t, eversion_t> shard_versions = response.shard_versions;

    if (!shard_versions.contains(shard_id_t::NO_SHARD)) {
      ldout(cct, DBG_LVL) << __func__ << ": "
        << "Replica version missing for acting index, failing split op." << dendl;
      return true;
    }

    if (!reference_version) {
      reference_version = shard_versions.at(shard_id_t::NO_SHARD);
    } else if (*reference_version != shard_versions.at(shard_id_t::NO_SHARD)) {
      return true;
    }
  }

  return false;
}

/**
 * @brief Complete the split operation by assembling and returning results.
 *
 * Assembles results from all sub-operations and invokes the original operation's
 * completion handlers. For successful operations, results are assembled based on
 * operation type (SPARSE_READ, READ, or other). For failures or version mismatches,
 * triggers retry through the normal operation path.
 *
 * The buffer handling maintains compatibility with librados expectations, including
 * special handling for pre-allocated buffers used by RadosStriper.
 *
 * @note Only called for successfully sent operations where abort=false
 */
void SplitOp::complete() {
  // STAGE 6: complete() only runs for successfully sent operations.
  // If abort was set during creation, the split op was discarded and
  // complete() is never called. This check handles the edge case where
  // abort might be set after creation but before sending (though this
  // should not happen in the current implementation).
  if (abort) {
    return;
  }
  ldout(cct, DBG_LVL) << __func__ << " object_id=" << orig_op->target.base_oid << " entry this=" << this << dendl;
  boost::system::error_code handler_error;

  int rc = assemble_rc();
  if (rc >= 0) {
    rc = prepare_assembly();
  }
  if (rc >= 0) {

    // In a "normal" completion, out_ops is generated in the MOSDOpReply reply
    // which we do not have here. Here we are going to mimic this behaviour
    // so as to reproduce as much as possible of the IO completion.
    std::vector out_ops(orig_op->ops.begin(), orig_op->ops.end());

    // Copy is necessary: process_op_reply_handlers() reads out_ops while
    // modifying orig_op state. Overhead is acceptable for typical 1-3 ops.
    for (unsigned ops_index=0; ops_index < out_ops.size(); ++ops_index) {
      auto &out_osd_op = out_ops[ops_index];
      switch (out_osd_op.op.op) {
        case CEPH_OSD_OP_SPARSE_READ: {
          auto [extents, bl] = assemble_buffer_sparse_read(ops_index);
          encode(std::move(extents).detach(), out_osd_op.outdata);
          encode_destructively(bl, out_osd_op.outdata);
          break;
        }
        case CEPH_OSD_OP_READ: {
          assemble_buffer_read(out_osd_op.outdata, ops_index);
          break;
        }
        default: {
          out_osd_op.outdata = std::move(sub_reads.at(reference_sub_read).details[ops_index].bl);
          out_osd_op.rval = sub_reads.at(reference_sub_read).details[ops_index].rval;
          break;
        }
      }
    }

    // Copied from Objecter::handle_osd_op_reply() to match correct API behaviour.
    // This is policed in the librados test harness.
    if (orig_op->outbl) {
      bufferlist bl;
      OSDOp::merge_osd_op_vector_out_data(out_ops, bl);
      // Note: Objecter will check for a single buffer here - this will always
      //       be true. Here, there will frequently be multiple buffers due
      //       to the splitting and the original buffer still needs to be
      //       honoured.
      if (orig_op->outbl->length() == bl.length()) {
        // this is here to keep previous users to *relied* on getting data
        // read into existing buffers happy.  Notably,
        // libradosstriper::RadosStriperImpl::aio_read().
        ldout(cct,10) << __func__ << " copying resulting " << bl.length()
                      << " into existing ceph::buffer of length " << orig_op->outbl->length()
                      << dendl;
        // The following seems a little convoluted, but the assumption is that
        // there is a good reason why Sage Weil wrote it this way in Objecter.
        bufferlist t;
        t = std::move(*orig_op->outbl);
        t.invalidate_crc();  // we're overwriting the raw buffers via c_str()
        bl.begin().copy(bl.length(), t.c_str());
        orig_op->outbl->substr_of(t, 0, bl.length());
      } else {
        // librados insists that if it provided a buffer and the client is
        // going to be returning a buffer, that this buffer must be
        // contiguous.
        if (orig_op->outbl->length() != 0) {
          bl.rebuild();
        }
        orig_op->outbl->substr_of(bl, 0, bl.length());
      }
      orig_op->outbl = 0;
    }

    objecter.count_split_op_completed(out_ops);
    handler_error = objecter.process_op_reply_handlers(orig_op, out_ops);
    ldout(cct, DBG_LVL) << __func__ << " object_id=" << orig_op->target.base_oid << " success this=" << this << " rc=" << rc << dendl;
  } else {
    ldout(cct, DBG_LVL) << __func__ << " object_id=" << orig_op->target.base_oid << " retry this=" << this << " rc=" << rc << dendl;
  }
  objecter.op_post_split_op_complete(orig_op, handler_error, rc);
}

/**
 * @brief Add version tracking to sub-operations for torn read protection.
 *
 * Adds internal version queries to all sub-operations to enable detection of
 * version mismatches. If sub-operations read different versions of the object
 * (torn read), the operation will be retried.
 */
void SplitOp::protect_torn_reads() {
  // If multiple reads are emitted from objecter, then it is essential that
  // each read reads the same version. It is not possible to efficiently
  // guarantee this, so instead read the version along with the data and if they
  // are different, then repeat the read to the primary. Such version mismatches
  // should be rare enough that this is not a significant performance impact.

  for (auto&& [index, sr] : sub_reads) {
    auto &internal_version = sr.internal_version;
    internal_version = std::make_optional<InternalVersion>();
    switch(strategy) {
      case InternalVersionStrategy::FromParticipatingShards:
        sr.rd.get_internal_versions_v2(&internal_version->ec, &internal_version->bl);
        break;
      case InternalVersionStrategy::FromPrimaryShard:
        sr.rd.get_internal_versions(&internal_version->ec, &internal_version->bl);
        break;
      default:
        ceph_abort_msg("Unknown internal version strategy used");
    }
  }
}

void SplitOp::init(OSDOp &op, int ops_index, bool single_op) {
  switch (op.op.op) {
  case CEPH_OSD_OP_SPARSE_READ: {
    init_read(op, true, ops_index, single_op);
    break;
  }
  case CEPH_OSD_OP_READ: {
    init_read(op, false, ops_index, single_op);
    break;
  }
  default: {
    // Invalid ops should have been rejected in validate.
    // Ensure the reference sub_read entry exists
    if (!sub_reads.contains(reference_sub_read)) {
      sub_reads.emplace(reference_sub_read, orig_op->ops.size() + 1);
    }
    Details &d = sub_reads.at(reference_sub_read).details[ops_index];
    orig_op->pass_thru_op(sub_reads.at(reference_sub_read).rd, ops_index, &d.bl, &d.rval);
    break;
  }
  }
}

#undef dout_prefix
#define dout_prefix *_dout << " SplitOp::"

namespace {
/**
 * Work out which shard serves a read.
 *
 * @param pi The pool the read is against
 * @param offset Object offset of the read
 * @param len Length of the read
 * @return the raw shard holding the whole read, or nullopt if no single shard
 *         does. A read that crosses a chunk boundary involves more than one
 *         shard, so it has no containing shard and returns nullopt. So does any
 *         read against a pool that is not erasure coded.
 */
std::optional<raw_shard_id_t> containing_shard(const pg_pool_t *pi, uint64_t offset, uint64_t len) {
  if (!pi->is_erasure()) {
    return std::nullopt;
  }

  uint64_t stripe_width = pi->get_stripe_width();

  // Optimization: Use stripe_width / 2 as a threshold to quickly reject requests
  // that cannot fit in a single chunk. Since k (data chunks) is at least 2,
  // chunk_size = stripe_width / k <= stripe_width / 2. This early check avoids
  // the more expensive division operation (stripe_width / data_chunk_count) below.
  if (len > stripe_width / 2) {
    return std::nullopt;
  }
  uint64_t data_chunk_count = pi->get_ec_data_shard_count();
  uint32_t chunk_size = pi->get_stripe_width() / data_chunk_count;

  // Chunk_size should never be zero, so this is paranoia.
  if (len > chunk_size || chunk_size == 0) {
    return std::nullopt;
  }

  uint64_t offset_to_end_of_chunk;

  // Chunk size is normally, but not always a power of 2.
  if (std::has_single_bit(chunk_size)) {
    offset_to_end_of_chunk = chunk_size - (offset & (chunk_size - 1));
  } else {
    offset_to_end_of_chunk = chunk_size - (offset % chunk_size);
  }

  if (len > offset_to_end_of_chunk) {
    return std::nullopt;
  }

  return raw_shard_id_t(offset / chunk_size % data_chunk_count);
}

/**
 * Validate operation flags for split operation eligibility.
 *
 * Checks that the operation has the required BALANCE_READS flag and
 * is not a write operation.
 *
 * @param pi The pool to validate against
 * @param op The operation to validate
 * @param cct CephContext for logging
 * @return true if flags are valid, false otherwise
 */
bool validate_flags(const pg_pool_t *pi, Objecter::Op *op, CephContext *cct) {
  if ((op->target.flags & CEPH_OSD_FLAG_BALANCE_READS) == 0) {
    ldout(cct, DBG_LVL) << __func__ << " REJECT: Client rejects balanced read" << dendl;
    return false;
  }

  // We really should not have a WRITE flagged as balanced read, but out of
  // paranoia ignore it.
  if (op->target.flags & CEPH_OSD_FLAG_WRITE) {
    ldout(cct, DBG_LVL) << __func__ << " REJECT: Flagged as write!" << dendl;
    return false;
  }

  // Only the primary orders reads against writes.
  if (op->target.flags & CEPH_OSD_FLAG_RWORDERED) {
    ldout(cct, DBG_LVL) << __func__ << " REJECT: ordered read" << dendl;
    return false;
  }

  if (pi->has_flag(pg_pool_t::FLAG_CRIMSON)) {
    ldout(cct, DBG_LVL) << __func__ <<" ABORT: Crimson doesn't support"
                                      " direct reads" << dendl;
    return false;
  }

  return true;
}

/**
 * Validate operation types and check read size constraints.
 *
 * Processes all operations in the op list, checking for:
 * - Supported operation types (READ, SPARSE_READ, and various metadata ops)
 * - Valid read sizes (non-zero length, meets minimum size requirements)
 * - Single chunk constraints for erasure coded pools
 *
 * @param op The operation to validate
 * @param pi Pool information
 * @param is_erasure Whether the pool is erasure coded
 * @param replica_min_read_size Minimum read size for replica pools
 * @param cct CephContext for logging
 * @param[out] has_primary_ops Set to true if primary-only ops are found
 * @param[out] single_direct_op Set to true if op can be sent directly to single OSD
 * @param[out] unsupported Set to true if an op or a zero-length read cannot be
 *             split
 * @return true if a suitable read operation was found, false otherwise
 */
bool validate_operations(Objecter::Op *op, const pg_pool_t *pi, bool is_erasure,
                        uint64_t replica_min_read_size, CephContext *cct,
                        bool &has_primary_ops, bool &single_direct_op,
                        bool &unsupported) {
  bool suitable_read_found = false;

  std::optional<raw_shard_id_t> target_shard;

  for (auto &o : op->ops) {
    switch (o.op.op) {
      case CEPH_OSD_OP_READ:
      case CEPH_OSD_OP_SPARSE_READ: {
        uint64_t length = o.op.extent.length;
        if (length == 0) {
          // length of zero actually means "the whole object". This code cannot
          // know the size of the object efficiently, so reject the op.
          ldout(cct, DBG_LVL) << __func__ << " REJECT: Zero length read" << dendl;
          unsupported = true;
          return false;
        }
        if ((is_erasure && length > 0) ||
            (!is_erasure && length >= replica_min_read_size)) {
          suitable_read_found = true;
        }
        if (single_direct_op) {
          std::optional<raw_shard_id_t> read_shard
            = containing_shard(pi, o.op.extent.offset, length);
          if (!target_shard && read_shard) {
            target_shard = read_shard;
          }
          single_direct_op = read_shard.has_value() && read_shard == target_shard;
        }
        break;
      }
      case CEPH_OSD_OP_GETXATTRS:
      case CEPH_OSD_OP_CHECKSUM:
      case CEPH_OSD_OP_GETXATTR:
      case CEPH_OSD_OP_STAT:
      case CEPH_OSD_OP_CMPXATTR:
      case CEPH_OSD_OP_CALL: {
        has_primary_ops = true;
        break; // Do not block validate.
      }
      default: {
        ldout(cct, DBG_LVL) << __func__ << " REJECT: unsupported op" << dendl;
        unsupported = true;
        return false;
      }
    }
  }

  if (single_direct_op && has_primary_ops) {
    single_direct_op = target_shard == raw_shard_id_t(0);
  }

  return suitable_read_found;
}

/**
 * Validate if an operation is eligible for split operation optimization.
 *
 * This function performs a multi-stage validation to determine if an operation
 * can be split across multiple OSDs for improved read performance:
 * 1. Validates operation flags (BALANCE_READS required, no WRITE flag)
 * 2. For replicated pools, rejects if min_split_replica_read_size is 0 (splitting disabled)
 * 3. Validates operation types and read size constraints
 *
 * @param op The operation to validate
 * @param objecter Objecter instance for configuration access
 * @param pi Pool information
 * @param cct CephContext for logging
 * @return A tuple of bools: {suitable_read_found, single_direct_op, has_primary_ops}
 *         - suitable_read_found: true if operation contains valid read(s)
 *         - single_direct_op: true if operation can be sent to single OSD
 *         - has_primary_ops: true if operation holds ops only the primary may run
 */
std::tuple<bool, bool, bool> validate(Objecter::Op *op, Objecter &objecter,
                                      const pg_pool_t *pi, CephContext *cct) {
  bool is_erasure = pi->is_erasure();

  // Validate flags
  if (!validate_flags(pi, op, cct)) {
    return {false, false, false};
  }

  // Initialize state for operation validation
  bool has_primary_ops = nullptr != op->objver;
  bool single_direct_op = is_erasure;

  uint64_t replica_min_shard_read_size = objecter.get_min_split_replica_read_size();
  
  if (!is_erasure && replica_min_shard_read_size == 0) {
    ldout(cct, DBG_LVL) << __func__ << " REJECT: splitting disabled (min_split_replica_read_size=0)" << dendl;
    return {false, false, false};
  }
  
  uint64_t replica_min_read_size = replica_min_shard_read_size * kReplicaMinShardReads;

  // Validate operations and read sizes
  bool unsupported = false;
  bool suitable_read_found = validate_operations(op, pi, is_erasure,
                                                 replica_min_read_size, cct,
                                                 has_primary_ops, single_direct_op,
                                                 unsupported);
  if (unsupported) {
    objecter.count_split_op_event(Objecter::SplitOpEvent::unsupported);
  }

  return {suitable_read_found, single_direct_op, has_primary_ops};
}

void debug_op_summary(const std::string &str, Objecter::Op *op, CephContext *cct) {
  auto &target = op->target;
  ldout(cct, DBG_LVL) << str
    << " object_id=" << target.base_oid
    << " tid=" << op->tid
    << " pool=" << target.base_oloc.pool
    << " pgid=" << target.actual_pgid
    << " osd=" << target.osd
    << " force_osd=" << ((target.flags & CEPH_OSD_FLAG_FORCE_OSD) != 0)
    << " balance_reads=" << ((target.flags & CEPH_OSD_FLAG_BALANCE_READS) != 0)
    << " ops.size()=" << op->ops.size()
    << " needs_version=" << (op->objver?"true":"false");

  for (auto && o : op->ops) {
    *_dout << " op_code=" << ceph_osd_op_name(o.op.op);
    switch (o.op.op) {
      case CEPH_OSD_OP_READ:
      case CEPH_OSD_OP_SPARSE_READ: {
        *_dout << "(" << o.op.extent.offset << "~" << o.op.extent.length << ")";
        break;
      }
      default:
      break;
    }
  }
  *_dout << dendl;
}
}

#ifdef UNIT_TESTS_BUILT
bool test_validate_operations(Objecter::Op *op, const pg_pool_t *pi,
                              bool is_erasure, uint64_t replica_min_read_size,
                              CephContext *cct, bool &has_primary_ops,
                              bool &single_direct_op) {
  bool unsupported = false;
  return validate_operations(op, pi, is_erasure, replica_min_read_size, cct,
                             has_primary_ops, single_direct_op, unsupported);
}
#endif

/**
 * @brief Prepare a single-chunk operation for direct execution.
 *
 * When an operation can be satisfied by reading from a single OSD (e.g.,
 * single chunk in EC pool), configures the operation for direct execution
 * without splitting. Sets EC_DIRECT_READ and FORCE_OSD flags and updates
 * the target OSD and shard ID.
 *
 * This optimization avoids split operation overhead when data can be
 * retrieved from a single shard.
 *
 * An operation that holds ops only the primary may run is left to go to the
 * primary unless the read's shard is the primary's.
 *
 * @param op Operation to prepare
 * @param objecter Objecter instance
 * @param cct CephContext for logging
 * @param has_primary_ops Operation holds ops only the primary may run
 */
void SplitOp::prepare_single_op(Objecter::Op *op, Objecter &objecter,
                                CephContext *cct, bool has_primary_ops) {
  auto &target = op->target;
  const pg_pool_t *pi = objecter.osdmap->get_pg_pool(target.base_oloc.pool);
  ceph_assert(pi);

  objecter._calc_target(&op->target, op);
  uint64_t data_chunk_count = pi->get_ec_data_shard_count();
  uint32_t chunk_size = pi->get_stripe_width() / data_chunk_count;

  // Find the first read to work out where the IO goes.
  for (auto o : op->ops) {
    if (o.op.op == CEPH_OSD_OP_SPARSE_READ ||
        o.op.op == CEPH_OSD_OP_READ) {
      raw_shard_id_t raw_shard((o.op.extent.offset) / chunk_size % data_chunk_count);
      shard_id_t shard = pi->get_shard(raw_shard);
      if (shard != shard_id_t::NO_SHARD &&
          (!has_primary_ops || shard == target.actual_pgid.shard)) {
        int acting_index = (int)shard;
        if (objecter.osdmap->exists(op->target.acting[acting_index])) {
          op->target.flags |= CEPH_OSD_FLAG_EC_DIRECT_READ;
          op->target.flags |= CEPH_OSD_FLAG_FORCE_OSD;
          target.osd = target.acting[acting_index];
          target.actual_pgid.reset_shard(shard);
          target.used_replica = (target.acting_primary != target.osd);
        }
      }
      break;
    }
  }
  debug_op_summary("reuse_op:", op, cct);
}

/**
 * Create and initialize a split operation for parallel reads.
 *
 * This function implements the abort flag pattern to efficiently handle
 * validation and creation failures:
 *
 * STAGE 1: Cheap validation tests (lines 643-666)
 * - Check pool exists
 * - Check split reads are enabled
 * - Validate operation types and parameters
 * - Return false immediately if validation fails (no split op created)
 *
 * STAGE 2: Create split op object (lines 668-674)
 * - Allocate ECSplitOp or ReplicaSplitOp based on pool type
 * - Constructor may detect issues and set abort flag
 *
 * STAGE 3: Check abort after construction (lines 676-679)
 * - If abort is set during construction, discard split op and return false
 * - This catches issues that can only be detected during object creation
 *
 * STAGE 4: Initialize sub-operations (lines 681-690)
 * - Call init() for each operation in the request
 * - init_read() may set abort if OSDs are missing or state is invalid
 * - Break early if abort is detected to avoid unnecessary work
 *
 * STAGE 5: Final abort check (lines 692-695)
 * - If abort was set during initialization, discard split op and return false
 * - Allows fallback to normal (non-split) operation path
 *
 * STAGE 6: Send operations (lines 697-747)
 * - Only reached if abort=false, meaning split op is valid
 * - complete() will only run for these successfully sent operations
 *
 * @return true if split op was created and sent, false to use normal operation
 */
bool SplitOp::create(Objecter::Op *op, Objecter &objecter,
  shunique_lock<ceph::shared_mutex>& sul, CephContext *cct) {

  auto &target = op->target;
  const pg_pool_t *pi = objecter.osdmap->get_pg_pool(target.base_oloc.pool);

  // STAGE 1: Cheap validation tests run first before creating split op
  if (!pi) {
    ldout(cct, DBG_LVL) << __func__ <<" REJECT: No Pool" << dendl;
    return false;
  }

  debug_op_summary("orig_op:", op, cct);

  // Reject if the pool has the EIO flag set, so that the op reaches
  // _op_submit() and is failed there.  Splitting would send sub-ops that
  // _op_submit() fails and frees immediately, and a single-op direct read
  // would skip _op_submit()'s _calc_target(), and with it the EIO check.
  if (pi->has_flag(pg_pool_t::FLAG_EIO)) {
    ldout(cct, DBG_LVL) << __func__ <<" REJECT: pool EIO" << dendl;
    return false;
  }

  // Reject if the operation is a snapshot operation
  if (op->snapid != CEPH_NOSNAP || !op->snapc.empty()) {
    ldout(cct, DBG_LVL) << __func__ <<" REJECT: snapshot operation" << dendl;
    return false;
  }

  // Reject if direct reads not supported by profile.
  if (!pi->has_flag(pg_pool_t::FLAG_CLIENT_SPLIT_READS)) {
    ldout(cct, DBG_LVL) << __func__ <<" REJECT: split reads off" << dendl;
    return false;
  }

  // Reject if pool does not support non-primary reads.
  if (!pi->allows_nonprimary_reads()) {
    ldout(cct, DBG_LVL) << __func__ <<" REJECT: non-primary reads not supported for split ops" << dendl;
    return false;
  }

  auto [validated, single_op, has_primary_ops] = validate(op, objecter, pi, cct);

  if (!validated) {
    return false;
  }

  if (single_op) {
    if (!pi->is_erasure() ||
        !ECSplitOp::single_op_reads_unavailable_shard(op, objecter)) {
      ldout(cct, DBG_LVL) << __func__ <<" reusing original op " << dendl;
      prepare_single_op(op, objecter, cct, has_primary_ops);
      return false;
    }
    ldout(cct, DBG_LVL) << __func__ << " single op chunk unavailable" << dendl;
    single_op = false;
  }

  // STAGE 2: Create split op object (may set abort during construction)
  std::shared_ptr<SplitOp> split_read;

  if (pi->is_erasure()) {
    split_read = std::make_shared<ECSplitOp>(op, objecter, cct, pi->size);
  } else {
    split_read = std::make_shared<ReplicaSplitOp>(op, objecter, cct, pi->size);
  }

  // STAGE 3: Check if abort was set during construction
  if (split_read->abort) {
    ldout(cct, DBG_LVL) << __func__ <<" ABORTED 1" << dendl;
    return false;
  }

  // Populate the target, to extract the acting set from it. _calc_target()
  // short-circuits once pgid is set.
  const auto saved_target = target;
  target.flags &= ~CEPH_OSD_FLAG_BALANCE_READS;
  objecter._calc_target(&op->target, op);

  // Initialize reference_sub_read now that acting set is populated
  split_read->init_reference_sub_read();

  if (split_read->reference_sub_read == -1) {
    split_read->abort = true;
  }

  if (split_read->abort) {
    ldout(cct, DBG_LVL) << __func__ <<" ABORTED after init_reference_sub_read" << dendl;
    target = saved_target;
    return false;
  }

  split_read->strategy
    = (objecter.osdmap->require_osd_release > ceph_release_t::umbrella) ?
    InternalVersionStrategy::FromParticipatingShards :
    InternalVersionStrategy::FromPrimaryShard;

  // STAGE 4: Initialize sub-operations (may set abort if problems detected)
  bool decode = false;
  if (pi->is_erasure()) {
    auto &ec_split_read = static_cast<ECSplitOp&>(*split_read);
    shard_id_set available = ECSplitOp::available_shards(target, objecter);
    if (ECSplitOp::reads_unavailable_shard(op, pi, available)) {
      std::shared_ptr<const ECCodecInfo> codec;
      if (ECSplitOp::may_decode(op, pi, objecter)) {
        codec = ec_split_read.get_codec(target.base_oloc.pool, pi);
      }
      if (codec) {
        ec_split_read.init_degraded(std::move(codec), available);
        decode = true;
      } else {
        split_read->abort = true;
      }
      if (split_read->abort) {
        objecter.count_split_op_event(Objecter::SplitOpEvent::no_decode);
      }
    }
  }
  for (unsigned i = 0; !decode && !split_read->abort && i < op->ops.size();
       ++i) {
    split_read->init(op->ops[i], i, single_op);
  }

  // STAGE 5: Final abort check - discard split op if problems were detected
  if (split_read->abort) {
    ldout(cct, DBG_LVL) << __func__ <<" ABORTED 2" << dendl;
    target = saved_target;
    return false;
  }

  // Ideally, the validate should detect any single-op read. However, if that
  // fails, then this will catch the cases (albeit less efficiently).
  if (split_read->sub_reads.size() <= 1) {
    ldout(cct, DBG_LVL) << __func__ <<" reusing original op - inefficient" << dendl;
    target = saved_target;
    prepare_single_op(op, objecter, cct, has_primary_ops);
    split_read->abort = true; // Required for destructor.
    return false;
  }

  objecter.add_op_to_splitop_session(op);

  split_read->protect_torn_reads();


  op->split_op_tids = std::make_unique<std::vector<ceph_tid_t>>(split_read->sub_reads.size());
  auto &tids = *op->split_op_tids;

  int i=0;

  // We are committed to doing a split read. Any re-attempts should not be either
  // split or balanced.
  for (auto && [index, sub_read] : split_read->sub_reads) {
    auto fin = new Finisher(split_read, sub_read); // Self-destructs when called.

    version_t *objver = nullptr;
    if (index == split_read->reference_sub_read) {
      objver = split_read->orig_op->objver;
    }

    auto sub_op = objecter.prepare_read_op(
      target.base_oid, target.base_oloc, split_read->sub_reads.at(index).rd, op->snapid,
      nullptr, split_read->flags, -1, fin, objver);
    sub_op->reply_epoch = &sub_read.reply_epoch;

    auto &st = sub_op->target;
    st = target; // Target can start off in same state as parent.
    st.flags |= CEPH_OSD_FLAG_FORCE_OSD;
    st.flags |= CEPH_OSD_FLAG_FAIL_ON_EAGAIN;
    if (pi->is_erasure()) {
      st.flags |= CEPH_OSD_FLAG_EC_DIRECT_READ;
      st.actual_pgid.reset_shard(shard_id_t(index));
    } else {
      st.flags |= CEPH_OSD_FLAG_BALANCE_READS;
    }
    st.osd = st.acting[index];

    st.used_replica = (st.acting_primary != st.osd);

    objecter._op_submit(sub_op, sul, &tids[i++]);

    debug_op_summary("sent_op", sub_op, cct);
  }

  ceph_assert(split_read->sub_reads.size() > 0);

  ldout(cct, DBG_LVL) << __func__ << " object_id=" << target.base_oid
    << " assigned parent tid=" << op->tid << dendl;

  return true;
}


#undef dout_prefix
#define dout_prefix *_dout << messenger->get_myname() << ".objecter "