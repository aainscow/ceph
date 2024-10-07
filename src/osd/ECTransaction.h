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

    std::map<hobject_t,ECUtil::HashInfoRef> hash_infos;

    uint64_t generate(
      hobject_t obj,
      uint64_t projected_size,
      const PGTransaction::ObjectOperation &op,
      const ECUtil::stripe_info_t &sinfo,
      DoutPrefixProvider *dpp);
  };
  std::ostream& operator<<(std::ostream& lhs, const ECTransaction::WritePlan& rhs);

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

        projected_size = plan.generate(obj, projected_size, op, sinfo, dpp);

        hinfo->set_projected_total_logical_size(
          sinfo,
          projected_size);

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
