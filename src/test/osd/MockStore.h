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

#include "os/memstore/MemStore.h"
#include <map>
#include <mutex>
#include <memory>
#include <string>

/**
 * MockStore - MemStore wrapper with error injection
 *
 * This class extends MemStore to allow injecting read errors for specific objects,
 * which is useful for testing EC recovery scenarios.
 *
 * Each OSD gets its own independent MockStore instance with its own data directory.
 */
class MockStore : public MemStore {
private:
  /// Map of object -> error code to inject on next read
  std::map<ghobject_t, int> injected_read_errors;


  /// Map of object -> error code to inject on next stat()
  std::map<ghobject_t, int> injected_stat_errors;

  /// Map of object -> error code to inject on next getattrs()
  std::map<ghobject_t, int> injected_getattrs_errors;

  /// Mutex to protect all injected error maps
  std::mutex error_injection_mutex;

public:
  MockStore(CephContext *cct, const std::string& path)
    : MemStore(cct, path) {}
  
  ~MockStore() override = default;

  /**
   * Factory method to create a new MockStore instance in memory-only mode.
   *
   * @param cct CephContext to use for store creation
   * @param osd_id OSD ID (for identification purposes)
   * @return Shared pointer to a new MockStore instance
   */
  static std::shared_ptr<MockStore> create(CephContext *cct, int osd_id);

  /**
   * Inject a read error for a specific object.
   * The error will be returned on the next read() call for this object,
   * then automatically cleared.
   *
   * @param oid The object to inject an error for
   * @param error_code The error code to return (should be negative, e.g., -EIO)
   */
  void inject_read_error(const ghobject_t& oid, int error_code) {
    std::lock_guard<std::mutex> lock(error_injection_mutex);
    injected_read_errors[oid] = error_code;
  }

  /**
   * Clear any injected read error for a specific object.
   *
   * @param oid The object to clear the error for
   */
  void clear_read_error(const ghobject_t& oid) {
    std::lock_guard<std::mutex> lock(error_injection_mutex);
    injected_read_errors.erase(oid);
  }

  /**
   * Clear all injected read errors.
   */
  void clear_all_read_errors() {
    std::lock_guard<std::mutex> lock(error_injection_mutex);
    injected_read_errors.clear();
  }

  /**
   * Inject a stat() error for a specific object (one-time).
   */
  void inject_stat_error(const ghobject_t& oid, int error_code) {
    std::lock_guard<std::mutex> lock(error_injection_mutex);
    injected_stat_errors[oid] = error_code;
  }

  /**
   * Clear any injected stat() error for a specific object.
   */
  void clear_stat_error(const ghobject_t& oid) {
    std::lock_guard<std::mutex> lock(error_injection_mutex);
    injected_stat_errors.erase(oid);
  }

  /**
   * Inject a getattrs() error for a specific object (one-time).
   */
  void inject_getattrs_error(const ghobject_t& oid, int error_code) {
    std::lock_guard<std::mutex> lock(error_injection_mutex);
    injected_getattrs_errors[oid] = error_code;
  }

  /**
   * Clear any injected getattrs() error for a specific object.
   */
  void clear_getattrs_error(const ghobject_t& oid) {
    std::lock_guard<std::mutex> lock(error_injection_mutex);
    injected_getattrs_errors.erase(oid);
  }

  /**
   * Override read() to check for injected errors before calling parent.
   * If an error is injected for this object, return it and clear the injection.
   * Otherwise, call the parent MemStore::read().
   */
  int read(
    CollectionHandle &c,
    const ghobject_t& oid,
    uint64_t offset,
    size_t len,
    ceph::buffer::list& bl,
    uint32_t op_flags = 0) override
  {
    // Check if we should inject an error for this object
    int error_code = 0;
    {
      std::lock_guard<std::mutex> lock(error_injection_mutex);
      auto it = injected_read_errors.find(oid);
      if (it != injected_read_errors.end()) {
        error_code = it->second;
        // Clear the error after using it (one-time injection)
        injected_read_errors.erase(it);
      }
    }

    // If we have an injected error, return it
    if (error_code != 0) {
      return error_code;
    }

    // Otherwise, call the parent implementation
    return MemStore::read(c, oid, offset, len, bl, op_flags);
  }

  /**
   * Override stat() to check for injected errors before calling parent.
   */
  int stat(
    CollectionHandle &c,
    const ghobject_t& oid,
    struct stat *st,
    bool allow_eio = false) override
  {
    int error_code = 0;
    {
      std::lock_guard<std::mutex> lock(error_injection_mutex);
      auto it = injected_stat_errors.find(oid);
      if (it != injected_stat_errors.end()) {
        error_code = it->second;
        injected_stat_errors.erase(it);
      }
    }
    if (error_code != 0) {
      return error_code;
    }
    return MemStore::stat(c, oid, st, allow_eio);
  }

  /**
   * Override getattrs() to check for injected errors before calling parent.
   */
  int getattrs(
    CollectionHandle &c,
    const ghobject_t& oid,
    std::map<std::string, ceph::bufferptr, std::less<>>& aset) override
  {
    int error_code = 0;
    {
      std::lock_guard<std::mutex> lock(error_injection_mutex);
      auto it = injected_getattrs_errors.find(oid);
      if (it != injected_getattrs_errors.end()) {
        error_code = it->second;
        injected_getattrs_errors.erase(it);
      }
    }
    if (error_code != 0) {
      return error_code;
    }
    return MemStore::getattrs(c, oid, aset);
  }
};
