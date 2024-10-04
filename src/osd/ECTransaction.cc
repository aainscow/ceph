// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2013 Inktank Storage, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <iostream>
#include <vector>
#include <sstream>

#include "ECTransaction.h"
#include "ECUtil.h"
#include "os/ObjectStore.h"
#include "common/inline_variant.h"

using std::less;
using std::make_pair;
using std::map;
using std::pair;
using std::set;
using std::string;
using std::vector;

using ceph::bufferlist;
using ceph::decode;
using ceph::encode;
using ceph::ErasureCodeInterfaceRef;

static void encode_and_write(
  pg_t pgid,
  const hobject_t &oid,
  const ECUtil::stripe_info_t &sinfo,
  ErasureCodeInterfaceRef &ecimpl,
  map<int, extent_set> &want_to_write,
  ECUtil::shard_extent_map_t &shard_extent_map,
  uint32_t flags,
  ECUtil::HashInfoRef hinfo,
  map<shard_id_t, ObjectStore::Transaction> *transactions,
  DoutPrefixProvider *dpp)
{
  const uint64_t before_size = hinfo->get_total_logical_size(sinfo);

  int r = shard_extent_map.encode(ecimpl, hinfo, before_size);
  ceph_assert(r == 0);

  ldpp_dout(dpp, 20) << __func__ << ": " << oid
	             << " want_to_write "
	             << want_to_write
	             << " shard_extent_map "
	             << shard_extent_map
	             << dendl;


  for (auto &&[shard_id, t]: *transactions) {
    if (want_to_write.contains(shard_id)) {
      extent_map emap = shard_extent_map.get_extent_map(shard_id);
      extent_set to_write_eset = want_to_write[shard_id];
      if (to_write_eset.begin().get_start() >= before_size) {
	t.set_alloc_hint(
	  coll_t(spg_t(pgid, shard_id)),
	  ghobject_t(oid, ghobject_t::NO_GEN, shard_id),
	  0, 0,
	  CEPH_OSD_ALLOC_HINT_FLAG_SEQUENTIAL_WRITE |
	  CEPH_OSD_ALLOC_HINT_FLAG_APPEND_ONLY);
      }

      for (auto &&[offset, len]: to_write_eset) {
	buffer::list bl;
	shard_extent_map.get_buffer(shard_id, offset, len, bl, false);
	t.write(
	  coll_t(spg_t(pgid, shard_id)),
	  ghobject_t(oid, ghobject_t::NO_GEN, shard_id),
	  sinfo.logical_to_prev_chunk_offset(offset),
	  bl.length(),
	  bl,
	  flags);
      }
    }
  }
}

uint64_t ECTransaction::WritePlan::generate(
  const hobject_t obj,
  uint64_t projected_size,
  const PGTransaction::ObjectOperation &op,
  const ECUtil::stripe_info_t &sinfo,
  DoutPrefixProvider *dpp)
{
  extent_set ro_writes;

  /* If we are truncating, then we need to over-write the new end to
   * the end of that page with zeros. Everything after that will get
   * truncated to the shard objects. */
  if (op.truncate &&
      op.truncate->first < projected_size) {

    uint64_t new_projected_size = std::min(
      ECUtil::align_page_next(op.truncate->first),
      projected_size);

    ro_writes.insert(op.truncate->first, new_projected_size);
    projected_size = new_projected_size;
  }

  for (auto &&extent: op.buffer_updates) {
    using BufferUpdate = PGTransaction::ObjectOperation::BufferUpdate;
    if (boost::get<BufferUpdate::CloneRange>(&(extent.get_val()))) {
      ceph_assert(
	0 ==
	"CloneRange is not allowed, do_op should have returned ENOTSUPP");
    }
    uint64_t start = extent.get_off();
    uint64_t end = start + extent.get_len();

    if (end > projected_size) {
      // This is an append. round up to a full page.
      end = sinfo.logical_to_next_stripe_offset(end);
    }

    if(start > projected_size) {
      start = projected_size;
    }

    ro_writes.insert(start, end - start);
  }

  auto &write = will_write[obj];
  extent_set outter_extent_superset;

  std::optional<std::map<int, extent_set>> inner;
  for (const auto& [ro_off, ro_len] : ro_writes) {
    /* Here, we calculate the "inner" and "outer" extent sets. The inner
     * represents the complete pages read. The outer represents the rounded
     * up/down pages. Clearly if the IO is entirely aligned, then the inner
     * and outer sets are the same and we optimise this by avoiding
     * calculating the inner in this case.
     *
     * This is useful because partially written pages must be fully read
     * from the backend as part of the RMW.
     */
    uint64_t raw_end = ro_off + ro_len;
    uint64_t outter_off = ECUtil::align_page_prev(ro_off);
    uint64_t outter_len = ECUtil::align_page_next(raw_end) - outter_off;
    uint64_t inner_off = ECUtil::align_page_next(ro_off);
    uint64_t inner_len = ECUtil::align_page_prev(raw_end) - inner_off;

    if (inner || outter_off != inner_off || outter_len != inner_len) {
      if (!inner) inner = std::map(write);
      sinfo.ro_range_to_shard_extent_set(inner_off,inner_len, *inner);
    }

    // Will write is expanded to page offsets.
    sinfo.ro_range_to_shard_extent_set(outter_off,outter_len,
      write, outter_extent_superset);

    projected_size = std::max(outter_off + outter_len, projected_size);
  }
  std::map<int, extent_set> &small_set = inner?*inner:write;

  // std::map<int, extent_set> object_limit;
  // sinfo.ro_range_to_shard_extent_set(0,projected_size, *inner);

  /* Construct the to read on the stack, to avoid having to insert and
   * erase into maps */
  std::map<int, extent_set> reads;
  for (int raw_shard = 0; raw_shard< sinfo.get_k_plus_m(); raw_shard++) {
    int shard = sinfo.get_shard(raw_shard);
    extent_set _to_read;

    if (raw_shard < sinfo.get_k()) {
      _to_read.insert(outter_extent_superset);

      if (write.contains(shard)) {
        _to_read.insert(write.at(shard));
      }

      if (small_set.contains(shard)) {
        extent_set intersection;
        // FIXME: A subtraction should be sufficient on its own. However,
        //        there is a limitation in interval_set.erase when
        //        erasing a larger extent from a smaller one.
        intersection.intersection_of(_to_read,small_set.at(shard));
        _to_read.subtract(intersection);
      }

      if (!_to_read.empty()) {
        reads.emplace(shard, std::move(_to_read));
      }
    } else {
      write[shard].insert(outter_extent_superset);
    }
  }

  // Do not do a read if there is nothing to read!
  if (!reads.empty()) {
     to_read.emplace(obj, std::move(reads));
  }

  ldpp_dout(dpp, 20) << __func__ << ": " << obj
		     << " projected_size="
		     << projected_size
                     << " plan=" << this
		     << dendl;

  /* validate post conditions:
   * to_read should have an entry for `obj` if it isn't empty
   * and if we are reading from `obj`, we can't be renaming or
   * cloning it */
  if (to_read.contains(obj)) {
    ceph_assert(!to_read.at(obj).empty());
    ceph_assert(!op.has_source());
  }

  return projected_size;
}

void ECTransaction::generate_transactions(
  PGTransaction *_t,
  WritePlan &plan,
  ErasureCodeInterfaceRef &ecimpl,
  pg_t pgid,
  const ECUtil::stripe_info_t &sinfo,
  const map<hobject_t, ECUtil::shard_extent_map_t> &partial_extents,
  vector<pg_log_entry_t> &entries,
  map<hobject_t, extent_map> *written_map,
  map<shard_id_t, ObjectStore::Transaction> *transactions,
  set<hobject_t> *temp_added,
  set<hobject_t> *temp_removed,
  DoutPrefixProvider *dpp,
  const ceph_release_t require_osd_release)
{
  ceph_assert(written_map);
  ceph_assert(transactions);
  ceph_assert(temp_added);
  ceph_assert(temp_removed);
  ceph_assert(_t);
  auto &t = *_t;

  auto &hash_infos = plan.hash_infos;

  map<hobject_t, pg_log_entry_t*> obj_to_log;
  for (auto &&i: entries) {
    obj_to_log.insert(make_pair(i.soid, &i));
  }

  map<hobject_t, extent_set> write_plan_validation;

  t.safe_create_traverse(
    [&](pair<const hobject_t, PGTransaction::ObjectOperation> &opair)
    {
      const hobject_t &oid = opair.first;
      auto &op = opair.second;
      auto &obc_map = t.obc_map;
      //FIXME: Currently unused
      //auto& written = (*written_map)[oid];

      auto iter = obj_to_log.find(oid);
      pg_log_entry_t *entry = iter != obj_to_log.end() ? iter->second : nullptr;

      ObjectContextRef obc;
      auto obiter = t.obc_map.find(oid);
      if (obiter != t.obc_map.end()) {
	obc = obiter->second;
      }
      if (entry) {
	ceph_assert(obc);
      } else {
	ceph_assert(oid.is_temp());
      }

      write_plan_validation[oid];

      ECUtil::HashInfoRef hinfo;
      {
	auto iter = hash_infos.find(oid);
	ceph_assert(iter != hash_infos.end());
	hinfo = iter->second;
      }

      if (oid.is_temp()) {
	if (op.is_fresh_object()) {
	  temp_added->insert(oid);
	} else if (op.is_delete()) {
	  temp_removed->insert(oid);
	}
      }

      if (entry &&
	  entry->is_modify() &&
	  op.updated_snaps) {
	bufferlist bl(op.updated_snaps->second.size() * 8 + 8);
	encode(op.updated_snaps->second, bl);
	entry->snaps.swap(bl);
	entry->snaps.reassign_to_mempool(mempool::mempool_osd_pglog);
      }

      ldpp_dout(dpp, 20) << "generate_transactions: "
			 << opair.first
			 << ", current size is "
			 << hinfo->get_total_logical_size(sinfo)
			 << " buffers are "
			 << op.buffer_updates
			 << dendl;
      if (op.truncate) {
	ldpp_dout(dpp, 20) << "generate_transactions: "
			   << " truncate is "
			   << *(op.truncate)
			   << dendl;
      }

      if (entry && op.updated_snaps) {
	entry->mod_desc.update_snaps(op.updated_snaps->first);
      }

      map<string, std::optional<bufferlist> > xattr_rollback;
      ceph_assert(hinfo);
      bufferlist old_hinfo;
      encode(*hinfo, old_hinfo);
      xattr_rollback[ECUtil::get_hinfo_key()] = old_hinfo;

      if (op.is_none() && op.truncate && op.truncate->first == 0) {
	ceph_assert(entry);
	ceph_assert(obc);

	if (op.truncate->first != op.truncate->second) {
	  op.truncate->first = op.truncate->second;
	} else {
	  op.truncate = std::nullopt;
	}

	op.delete_first = true;
	op.init_type = PGTransaction::ObjectOperation::Init::Create();

	if (obc) {
	  /* We need to reapply all of the cached xattrs.
	     * std::map insert fortunately only writes keys
	     * which don't already exist, so this should do
	     * the right thing. */
	  op.attr_updates.insert(
	    obc->attr_cache.begin(),
	    obc->attr_cache.end());
	}
      }

      if (op.delete_first) {
	/* We also want to remove the std::nullopt entries since
	   * the keys already won't exist */
	for (auto j = op.attr_updates.begin();
	     j != op.attr_updates.end();
	  ) {
	  if (j->second) {
	    ++j;
	  } else {
	    op.attr_updates.erase(j++);
	  }
	}
	/* Fill in all current entries for xattr rollback */
	if (obc) {
	  xattr_rollback.insert(
	    obc->attr_cache.begin(),
	    obc->attr_cache.end());
	  obc->attr_cache.clear();
	}
	if (entry) {
	  entry->mod_desc.rmobject(entry->version.version);
	  for (auto &&st: *transactions) {
	    st.second.collection_move_rename(
	      coll_t(spg_t(pgid, st.first)),
	      ghobject_t(oid, ghobject_t::NO_GEN, st.first),
	      coll_t(spg_t(pgid, st.first)),
	      ghobject_t(oid, entry->version.version, st.first));
	  }
	} else {
	  for (auto &&st: *transactions) {
	    st.second.remove(
	      coll_t(spg_t(pgid, st.first)),
	      ghobject_t(oid, ghobject_t::NO_GEN, st.first));
	  }
	}
	hinfo->clear();
      }

      if (op.is_fresh_object() && entry) {
	entry->mod_desc.create();
      }

      match(
	op.init_type,
	[&](const PGTransaction::ObjectOperation::Init::None &) {},
	[&](const PGTransaction::ObjectOperation::Init::Create &op) {
	  for (auto &&st: *transactions) {
	    if (require_osd_release >= ceph_release_t::octopus) {
	      st.second.create(
		coll_t(spg_t(pgid, st.first)),
		ghobject_t(oid, ghobject_t::NO_GEN, st.first));
	    } else {
	      st.second.touch(
		coll_t(spg_t(pgid, st.first)),
		ghobject_t(oid, ghobject_t::NO_GEN, st.first));
	    }
	  }
	},
	[&](const PGTransaction::ObjectOperation::Init::Clone &op) {
	  for (auto &&st: *transactions) {
	    st.second.clone(
	      coll_t(spg_t(pgid, st.first)),
	      ghobject_t(op.source, ghobject_t::NO_GEN, st.first),
	      ghobject_t(oid, ghobject_t::NO_GEN, st.first));
	  }

	  auto siter = hash_infos.find(op.source);
	  ceph_assert(siter != hash_infos.end());
	  hinfo->update_to(*(siter->second));

	  if (obc) {
	    auto cobciter = obc_map.find(op.source);
	    ceph_assert(cobciter != obc_map.end());
	    obc->attr_cache = cobciter->second->attr_cache;
	  }
	},
	[&](const PGTransaction::ObjectOperation::Init::Rename &op) {
	  ceph_assert(op.source.is_temp());
	  for (auto &&st: *transactions) {
	    st.second.collection_move_rename(
	      coll_t(spg_t(pgid, st.first)),
	      ghobject_t(op.source, ghobject_t::NO_GEN, st.first),
	      coll_t(spg_t(pgid, st.first)),
	      ghobject_t(oid, ghobject_t::NO_GEN, st.first));
	  }
	  auto siter = hash_infos.find(op.source);
	  ceph_assert(siter != hash_infos.end());
	  hinfo->update_to(*(siter->second));
	  if (obc) {
	    auto cobciter = obc_map.find(op.source);
	    ceph_assert(cobciter == obc_map.end());
	    obc->attr_cache.clear();
	  }
	});

      // omap not supported (except 0, handled above)
      ceph_assert(!(op.clear_omap));
      ceph_assert(!(op.omap_header));
      ceph_assert(op.omap_updates.empty());

      if (!op.attr_updates.empty()) {
	map<string, bufferlist, less<>> to_set;
	for (auto &&j: op.attr_updates) {
	  if (j.second) {
	    to_set[j.first] = *(j.second);
	  } else {
	    for (auto &&st : *transactions) {
	      st.second.rmattr(
		coll_t(spg_t(pgid, st.first)),
		ghobject_t(oid, ghobject_t::NO_GEN, st.first),
		j.first);
	    }
	  }
	  if (obc) {
	    auto citer = obc->attr_cache.find(j.first);
	    if (entry) {
	      if (citer != obc->attr_cache.end()) {
		// won't overwrite anything we put in earlier
		xattr_rollback.insert(
		  make_pair(
		    j.first,
		    std::optional<bufferlist>(citer->second)));
	      } else {
		// won't overwrite anything we put in earlier
		xattr_rollback.insert(
		  make_pair(
		    j.first,
		    std::nullopt));
	      }
	    }
	    if (j.second) {
	      obc->attr_cache[j.first] = *(j.second);
	    } else if (citer != obc->attr_cache.end()) {
	      obc->attr_cache.erase(citer);
	    }
	  } else {
	    ceph_assert(!entry);
	  }
	}
	for (auto &&st : *transactions) {
	  st.second.setattrs(
	    coll_t(spg_t(pgid, st.first)),
	    ghobject_t(oid, ghobject_t::NO_GEN, st.first),
	    to_set);
	}
	ceph_assert(!xattr_rollback.empty());
      }
      if (entry && !xattr_rollback.empty()) {
	entry->mod_desc.setattrs(xattr_rollback);
      }

      if (op.alloc_hint) {
	/* logical_to_next_chunk_offset() scales down both aligned and
	   * unaligned offsets
	   
	   * we don't bother to roll this back at this time for two reasons:
	   * 1) it's advisory
	   * 2) we don't track the old value */
	uint64_t object_size = sinfo.logical_to_next_chunk_offset(
	  op.alloc_hint->expected_object_size);
	uint64_t write_size = sinfo.logical_to_next_chunk_offset(
	  op.alloc_hint->expected_write_size);
	
	for (auto &&st : *transactions) {
	  st.second.set_alloc_hint(
	    coll_t(spg_t(pgid, st.first)),
	    ghobject_t(oid, ghobject_t::NO_GEN, st.first),
	    object_size,
	    write_size,
	    op.alloc_hint->flags);
	}
      }

      ECUtil::shard_extent_map_t to_write(&sinfo);
      auto pextiter = partial_extents.find(oid);
      if (pextiter != partial_extents.end()) {
	to_write = pextiter->second;
      }

      vector<pair<uint64_t, uint64_t>> rollback_extents;
      const uint64_t orig_size = hinfo->get_total_logical_size(sinfo);

      uint64_t new_size = orig_size;
      uint64_t append_after = new_size;
      ldpp_dout(dpp, 20) << "generate_transactions: new_size start "
	<< new_size << dendl;
      if (op.truncate && op.truncate->first < new_size) {
	ceph_assert(!op.is_fresh_object());
	new_size = sinfo.logical_to_next_stripe_offset(
	  op.truncate->first);
	ldpp_dout(dpp, 20) << "generate_transactions: new_size truncate down "
	                   << new_size << dendl;
	if (new_size != op.truncate->first) {
	  // 0 the unaligned part
	  to_write.append_zeros_to_ro_offset(new_size);
	  append_after = sinfo.logical_to_prev_stripe_offset(
	    op.truncate->first);
	}
	else {
	  append_after = new_size;
	}
	to_write.erase_after_ro_offset(new_size);

	if (entry && !op.is_fresh_object()) {
	  uint64_t restore_from = sinfo.logical_to_prev_chunk_offset(
	    op.truncate->first);
	  uint64_t restore_len = sinfo.aligned_logical_offset_to_chunk_offset(
	    orig_size -
	    sinfo.logical_to_prev_stripe_offset(op.truncate->first));
	  ceph_assert(rollback_extents.empty());

	  ldpp_dout(dpp, 20) << "generate_transactions: saving extent "
			     << make_pair(restore_from, restore_len)
			     << dendl;
	  ldpp_dout(dpp, 20) << "generate_transactions: truncating to "
			     << new_size
			     << dendl;
	  rollback_extents.emplace_back(
	    make_pair(restore_from, restore_len));
	  for (auto &&st: *transactions) {
	    st.second.touch(
	      coll_t(spg_t(pgid, st.first)),
	      ghobject_t(oid, entry->version.version, st.first));
	    st.second.clone_range(
	      coll_t(spg_t(pgid, st.first)),
	      ghobject_t(oid, ghobject_t::NO_GEN, st.first),
	      ghobject_t(oid, entry->version.version, st.first),
	      restore_from,
	      restore_len,
	      restore_from);
	    
	  }
	} else {
	  ldpp_dout(dpp, 20) << "generate_transactions: not saving extents"
                                ", fresh object" << dendl;
	}
	for (auto &&st : *transactions) {
	  st.second.truncate(
	    coll_t(spg_t(pgid, st.first)),
	    ghobject_t(oid, ghobject_t::NO_GEN, st.first),
	    sinfo.aligned_logical_offset_to_chunk_offset(new_size));
	}
      }

      uint32_t fadvise_flags = 0;
      for (auto &&extent: op.buffer_updates) {
	using BufferUpdate = PGTransaction::ObjectOperation::BufferUpdate;
	bufferlist bl;
	match(
	  extent.get_val(),
	  [&](const BufferUpdate::Write &op) {
	    bl = op.buffer;
	    fadvise_flags |= op.fadvise_flags;
	  },
	  [&](const BufferUpdate::Zero &) {
	    bl.append_zero(extent.get_len());
	  },
	  [&](const BufferUpdate::CloneRange &) {
	    ceph_assert(
	      0 ==
	      "CloneRange is not allowed, do_op should have returned ENOTSUPP");
	  });

	uint64_t off = extent.get_off();
	uint64_t len = extent.get_len();
	uint64_t end = off + len;
	ldpp_dout(dpp, 20) << "generate_transactions: adding buffer_update "
			   << make_pair(off, len)
			   << dendl;
	ceph_assert(len > 0);
	if (off > new_size) {
	  ceph_assert(off > append_after);
	  bl.prepend_zero(off - new_size);
	  len += off - new_size;
	  ldpp_dout(dpp, 20) << "generate_transactions: prepending zeroes to align "
			     << off << "->" << new_size
			     << dendl;
	  off = new_size;
	}
	if (!sinfo.logical_offset_is_stripe_aligned(end) && (end > append_after)) {
	  uint64_t aligned_end = sinfo.logical_to_next_stripe_offset(end);
	  uint64_t tail = aligned_end - end;
	  bl.append_zero(tail);
	  ldpp_dout(dpp, 20) << "generate_transactions: appending zeroes to align end "
			     << end << "->" << end+tail
			     << ", len: " << len << "->" << len+tail
			     << dendl;
	  end += tail;
	  len += tail;
	}

	if (end > new_size)
	  new_size = end;

        sinfo.ro_range_to_shard_extent_map(off, len, bl, to_write);
      }

      if (op.truncate &&
	  op.truncate->second > new_size) {
	ceph_assert(op.truncate->second > append_after);
	uint64_t truncate_to =
	  sinfo.logical_to_next_stripe_offset(
	    op.truncate->second);
	uint64_t zeroes = truncate_to - new_size;
	bufferlist bl;
	bl.append_zero(zeroes);
	sinfo.ro_range_to_shard_extent_map(new_size, zeroes, bl, to_write);
	new_size = truncate_to;
	ldpp_dout(dpp, 20) << "generate_transactions: truncating out to "
			   << truncate_to
			   << dendl;
      }

      set<int> want;
      for (unsigned i = 0; i < ecimpl->get_chunk_count(); ++i) {
	want.insert(i);
      }

      auto to_overwrite = to_write.intersect_ro_range(0, append_after);
      ldpp_dout(dpp, 20) << "generate_transactions: to_overwrite: "
	                 << to_overwrite
	                 << dendl;

      if (!to_overwrite.empty()) {
	// Depending on the write, we may or may not have the parity buffers.
	// Here we invent some buffers.
	to_overwrite.insert_parity_buffers();

	// For an overwirte, we can restrict ourselves to the overwrite itself
	// and parity updates.
	auto &want_to_write = plan.will_write[oid];

	/* Generate the clone transactions for every shard. These are the same
	 * for each shard and cover complete chunks.
	 *
	 * This could probably be more efficient...
	 */
	auto clone_region = to_overwrite.get_extent_superset();
	clone_region.align(sinfo.get_chunk_size());

	int shard_count = sinfo.get_k_plus_m();

	uint64_t restore_from = clone_region.range_start();
	uint64_t restore_len = clone_region.range_end() - restore_from;
	for (int raw_shard = 0; raw_shard < shard_count; ++raw_shard) {
	  int shard = sinfo.get_shard(raw_shard);
	  shard_id_t shard_id(shard);
	  auto &&st = (*transactions)[shard_id];
	  st.clone_range(
	    coll_t(spg_t(pgid, shard_id)),
	    ghobject_t(oid, ghobject_t::NO_GEN, shard_id),
	    ghobject_t(oid, entry->version.version, shard_id),
	    restore_from,
	    restore_len,
	    restore_from);
	}

	encode_and_write(
	  pgid,
	  oid,
	  sinfo,
	  ecimpl,
	  want_to_write,
	  to_overwrite,
	  fadvise_flags,
	  hinfo,
	  transactions,
	  dpp);
      }

      auto to_append = to_write.intersect_ro_range(
	append_after,
	std::numeric_limits<uint64_t>::max() - append_after);
      ldpp_dout(dpp, 20) << "generate_transactions: to_append: "
	                 << to_append
	                 << dendl;

      if (!to_append.empty()) {
	// The above would not have buffers for parity, so add them now
	to_append.insert_parity_buffers();

	// For appends, we need to write everything (even if it is zeros)
	map<int, extent_set> want_to_write = to_append.get_extent_set_map();

	encode_and_write(
	  pgid,
	  oid,
	  sinfo,
	  ecimpl,
	  want_to_write,
	  to_append,
	  fadvise_flags,
	  hinfo,
	  transactions,
	  dpp);
      }

      ldpp_dout(dpp, 20) << "generate_transactions: " << oid
			 << " resetting hinfo to logical size "
			 << new_size
			 << dendl;
      if (!rollback_extents.empty() && entry) {
	if (entry) {
	  ldpp_dout(dpp, 20) << "generate_transactions: " << oid
			     << " marking rollback extents "
			     << rollback_extents
			     << dendl;
	  entry->mod_desc.rollback_extents(
	    entry->version.version, rollback_extents);
	}
	hinfo->set_total_chunk_size_clear_hash(
	  sinfo.aligned_logical_offset_to_chunk_offset(new_size));
      } else {
	ceph_assert(hinfo->get_total_logical_size(sinfo) == new_size);
      }

      if (entry && !to_append.empty()) {
	ldpp_dout(dpp, 20) << "generate_transactions: marking append "
			   << append_after
			   << dendl;
	entry->mod_desc.append(append_after);
      }

      if (!op.is_delete()) {
	bufferlist hbuf;
	encode(*hinfo, hbuf);
	for (auto &&i : *transactions) {
	  i.second.setattr(
	    coll_t(spg_t(pgid, i.first)),
	    ghobject_t(oid, ghobject_t::NO_GEN, i.first),
	    ECUtil::get_hinfo_key(),
	    hbuf);
	}
      }

      /* FIXME: FAIL REVIEW
      Need to put something in here to update written.  I think written is only
      used as an update the cache, so is actually just the plan.will_write
      populated with buffers.
      */
    });
}
