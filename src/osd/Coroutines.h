// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2012 New Dream Network/Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 */

#pragma once
#include <boost/coroutine2/all.hpp>

using yield_token_t = boost::coroutines2::coroutine<void>::pull_type;
using resume_token_t = boost::coroutines2::coroutine<void>::push_type;

struct CoroHandles {
  yield_token_t& yield;
  resume_token_t& resume;
};