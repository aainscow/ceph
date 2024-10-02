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

#ifndef ECTRANSACTION_H
#define ECTRANSACTION_H

#include "ECUtil.h"
#include "ExtentCache.h"
#include "erasure-code/ErasureCodeInterface.h"
#include "os/Transaction.h"
#include "PGTransaction.h"

namespace ECTransaction {
  struct WritePlan {
    bool invalidates_cache = false; // Yes, both are possible
    std::map<hobject_t, std::map<int, extent_set>> to_read;
    std::map<hobject_t, std::map<int, extent_set>> will_write;
    std::map<hobject_t, std::map<int, extent_set>> to_rmw;

    std::map<hobject_t,ECUtil::HashInfoRef> hash_infos;
  };

  template <typename F>
  WritePlan get_write_plan(
    const ECUtil::stripe_info_t &sinfo,
    PGTransaction& t,
    F &&get_hinfo,
    DoutPrefixProvider *dpp) {
    WritePlan plan;
    t.safe_create_traverse(
      [&](std::pair<const hobject_t, PGTransaction::ObjectOperation> &i) {
        const auto& [obj, op] = i;
	ECUtil::HashInfoRef hinfo = get_hinfo(obj);
	plan.hash_infos[obj] = hinfo;

	uint64_t projected_size =
	  hinfo->get_projected_total_logical_size(sinfo);

	if (op.deletes_first()) {
	  ldpp_dout(dpp, 20) << __func__ << ": delete, setting projected size"
			     << " to 0" << dendl;
	  projected_size = 0;
	}

	hobject_t source;
	if (op.has_source(&source)) {
	  // typically clone or mv
	  plan.invalidates_cache = true;

	  ECUtil::HashInfoRef shinfo = get_hinfo(source);
	  projected_size = shinfo->get_projected_total_logical_size(sinfo);
	  plan.hash_infos[source] = shinfo;
	}

        extent_set raw_write_set;

        /* If we are truncating, then we need to over-write the new end to
         * the end of that stripe with zeros. Everything after that will get
         * truncated to the shard objects. */
	if (op.truncate &&
	    op.truncate->first < projected_size) {

	  uint64_t new_projected_size = std::min(
	    sinfo.logical_to_next_stripe_offset(op.truncate->first),
	    projected_size);

	  raw_write_set.insert(op.truncate->first, new_projected_size);
	  projected_size = new_projected_size;
	}

	for (auto &&extent: op.buffer_updates) {
	  using BufferUpdate = PGTransaction::ObjectOperation::BufferUpdate;
	  if (boost::get<BufferUpdate::CloneRange>(&(extent.get_val()))) {
	    ceph_assert(
	      0 ==
	      "CloneRange is not allowed, do_op should have returned ENOTSUPP");
	  }
	  raw_write_set.insert(extent.get_off(), extent.get_len());
	}

        extent_set empty;
	for (const auto& [offset, length] : raw_write_set) {
	  extent_set extent_superset;
          auto &will_write = plan.will_write[obj];
          auto &to_read = plan.to_read[obj];
	  auto &to_rmw = plan.to_rmw[obj];
          sinfo.ro_range_to_shard_extent_set(offset, length, will_write, extent_superset);

          for (int raw_shard = 0; raw_shard< sinfo.get_k_plus_m(); raw_shard++) {
            int shard = sinfo.get_shard(raw_shard);
            extent_set _to_read;
            if (raw_shard < sinfo.get_m()) {
              _to_read.insert(extent_superset);
              _to_read.subtract(will_write[shard]);

              if (!_to_read.empty())
                to_read.emplace(shard, _to_read);

            } else {
              will_write[shard].insert(extent_superset);
            }

            to_rmw[shard].union_of(
              to_read.contains(shard)?to_read.at(shard):empty,
              will_write.contains(shard)?will_write.at(shard):empty);
          }
	}

	ldpp_dout(dpp, 20) << __func__ << ": " << obj
			   << " projected size "
			   << projected_size
			   << dendl;
	hinfo->set_projected_total_logical_size(
	  sinfo,
	  projected_size);

	/* validate post conditions:
	 * to_read should have an entry for `obj` if it isn't empty
	 * and if we are reading from `obj`, we can't be renaming or
	 * cloning it */
	ceph_assert(plan.to_read.count(obj) == 0 ||
	       (!plan.to_read.at(obj).empty() &&
		!i.second.has_source()));
      });
    return plan;
  }

  void generate_transactions(
    PGTransaction* _t,
    WritePlan &plan,
    ceph::ErasureCodeInterfaceRef &ecimpl,
    pg_t pgid,
    const ECUtil::stripe_info_t &sinfo,
    const std::map<hobject_t, ECUtil::shard_extent_map_t> &partial_extents,
    std::vector<pg_log_entry_t> &entries,
    std::map<hobject_t,extent_map> *written,
    std::map<shard_id_t, ceph::os::Transaction> *transactions,
    std::set<hobject_t> *temp_added,
    std::set<hobject_t> *temp_removed,
    DoutPrefixProvider *dpp,
    const ceph_release_t require_osd_release = ceph_release_t::unknown);
};

#endif
