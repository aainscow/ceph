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

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "common/ceph_context.h"
#include "common/perf_counters_collection.h"

inline std::map<std::string, uint64_t> objecter_counters(CephContext* cct,
                                                         std::string_view part)
{
  const std::string prefix = "objecter.";
  std::map<std::string, uint64_t> counters;
  cct->get_perfcounters_collection()->with_counters(
    [&](const PerfCountersCollectionImpl::CounterMap& by_path) {
      for (auto& [path, counter] : by_path) {
        if (path.starts_with(prefix) &&
            path.find(part) != std::string::npos) {
          // A time average counts its samples.
          auto& data = *counter.data;
          bool average = data.type & PERFCOUNTER_LONGRUNAVG;
          counters[path.substr(prefix.size())] =
            average ? data.avgcount : data.u64;
        }
      }
    });
  return counters;
}

inline std::map<std::string, uint64_t> split_op_counters(CephContext* cct)
{
  return objecter_counters(cct, "split_op_");
}
