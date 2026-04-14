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

#include "test/osd/ECPeeringTestFixture.h"
#include "test/osd/MockECRecPred.h"
#include "test/osd/MockECReadPred.h"

// ShardDpp implementation
std::ostream& ECPeeringTestFixture::ShardDpp::gen_prefix(std::ostream& out) const {
  out << "shard " << shard << ": ";
  if (fixture->shard_peering_states.contains(shard)) {
    PeeringState *ps = fixture->shard_peering_states[shard].get();
    out << *ps << " ";
  }
  return out;
}

IsPGRecoverablePredicate* ECPeeringTestFixture::get_is_recoverable_predicate() {
  return new MockECRecPred(k, m);
}

IsPGReadablePredicate* ECPeeringTestFixture::get_is_readable_predicate() {
  return new MockECReadPred(k, m);
}

ECPeeringTestFixture::ECPeeringTestFixture()
  : PGBackendTestFixture(PGBackendTestFixture::EC) {
}

void ECPeeringTestFixture::SetUp() {
  PGBackendTestFixture::SetUp();
  for (int i = 0; i < (k + m) * num_zones; i++) {
    create_peering_state(i);
  }
  
  // Override epoch getter to use shard_peering_listeners instead of base class listeners
  // (which are moved into shard_peering_listeners during create_peering_state)
  messenger->set_epoch_getter([this](int osd) -> epoch_t {
    auto it = shard_peering_listeners.find(osd);
    if (it != shard_peering_listeners.end()) {
      return it->second->get_osdmap_epoch();
    }
    // Fallback to test fixture's osdmap
    return osdmap->get_epoch();
  });
  
  // Register handlers for peering messages (MOSDPeeringOp)
  // All peering messages (Query, Notify, Info, Log) use the same handler pattern
  // since they all inherit from MOSDPeeringOp and use get_event()
  auto peering_handler = [this](int from_osd, int to_osd, MOSDPeeringOp* op) -> bool {
    // Message is already correctly typed as MOSDPeeringOp*
    ceph_assert(op);
    
    // Get the peering event from the message
    PGPeeringEventRef evt_ref(op->get_event());
    
    // Handle the event on the destination shard's peering state
    PeeringCtx* ctx = get_peering_ctx(to_osd);
    auto ps = get_peering_state(to_osd);
    ps->handle_event(evt_ref, ctx);

    auto t = ctx->transaction.claim_and_reset();
    int r = queue_transaction_helper(to_osd, std::move(t));
    ceph_assert( r >= 0 );
    return true;  // Message handled
  };
  
  // Register the same handler for all peering message types
  messenger->register_typed_handler<MOSDPeeringOp>(MSG_OSD_PG_QUERY2, peering_handler);
  messenger->register_typed_handler<MOSDPeeringOp>(MSG_OSD_PG_NOTIFY2, peering_handler);
  messenger->register_typed_handler<MOSDPeeringOp>(MSG_OSD_PG_INFO2, peering_handler);
  messenger->register_typed_handler<MOSDPeeringOp>(MSG_OSD_PG_LOG, peering_handler);
  messenger->register_typed_handler<MOSDPeeringOp>(MSG_OSD_PG_LEASE, peering_handler);
  messenger->register_typed_handler<MOSDPeeringOp>(MSG_OSD_PG_LEASE_ACK, peering_handler);
  messenger->register_typed_handler<MOSDPeeringOp>(MSG_OSD_RECOVERY_RESERVE, peering_handler);
  
  // Note: MSG_OSD_PG_PUSH and MSG_OSD_PG_PUSH_REPLY handlers are registered
  // in PGBackendTestFixture::setup_ec_pool() using make_backend_handler
  
  // Register idle callback to check for buffered messages
  event_loop->register_idle_callback([this]() -> bool {
    bool found_messages = false;
    // Check all PeeringCtx objects for buffered messages
    for (auto& [osd, ctx] : shard_peering_ctxs) {
      if (!ctx->message_map.empty()) {
        dispatch_buffered_messages(osd, ctx.get());
        found_messages = true;
      }
    }
    return found_messages;
  });
  
  // Run initial peering cycle to get all shards to active state
  run_first_peering();
}

void ECPeeringTestFixture::TearDown() {
  shard_peering_states.clear();
  shard_peering_ctxs.clear();
  shard_peering_listeners.clear();
  shard_dpps.clear();
  PGBackendTestFixture::TearDown();
}

PeeringState* ECPeeringTestFixture::get_peering_state(int shard) {
  ceph_assert(shard >= 0 && shard < (k + m) * num_zones);
  auto it = shard_peering_states.find(shard);
  ceph_assert(it != shard_peering_states.end());
  ceph_assert(it->second != nullptr);
  return it->second.get();
}

PeeringCtx* ECPeeringTestFixture::get_peering_ctx(int shard) {
  ceph_assert(shard >= 0 && shard < (k + m) * num_zones);
  auto it = shard_peering_ctxs.find(shard);
  ceph_assert(it != shard_peering_ctxs.end());
  ceph_assert(it->second != nullptr);
  return it->second.get();
}

MockPeeringListener* ECPeeringTestFixture::get_peering_listener(int shard) {
  ceph_assert(shard >= 0 && shard < (k + m) * num_zones);
  auto it = shard_peering_listeners.find(shard);
  ceph_assert(it != shard_peering_listeners.end());
  ceph_assert(it->second != nullptr);
  return it->second.get();
}

int ECPeeringTestFixture::get_primary_shard_from_osdmap() const {
  std::vector<int> acting_osds;
  int acting_primary = -1;
  osdmap->pg_to_acting_osds(this->pgid, &acting_osds, &acting_primary);
  return acting_primary;
}

MockPGBackendListener* ECPeeringTestFixture::get_primary_listener() {
  int primary_shard = get_primary_shard_from_osdmap();
  if (primary_shard < 0) {
    return nullptr;
  }
  
  auto it = shard_peering_listeners.find(primary_shard);
  if (it != shard_peering_listeners.end() && it->second &&
      it->second->backend_listener) {
    // Assert that the backend listener agrees it's primary
    ceph_assert(it->second->backend_listener->pgb_is_primary());
    return it->second->backend_listener.get();
  }
  return nullptr;
}

PGBackend* ECPeeringTestFixture::get_primary_backend() {
  int primary_shard = get_primary_shard_from_osdmap();
  if (primary_shard < 0) {
    return nullptr;
  }
  
  auto listener_it = shard_peering_listeners.find(primary_shard);
  if (listener_it != shard_peering_listeners.end() && listener_it->second &&
      listener_it->second->backend_listener) {
    // Assert that the backend listener agrees it's primary
    ceph_assert(listener_it->second->backend_listener->pgb_is_primary());
    
    // Return the backend from the base class's backends map, not from
    // the peering listener, because the base class backend is connected
    // to the event loop and message routers
    auto backend_it = backends.find(primary_shard);
    return (backend_it != backends.end()) ? backend_it->second.get() : nullptr;
  }
  return nullptr;
}

void ECPeeringTestFixture::event_initialize() {
  // Get acting set from OSDMap
  std::vector<int> acting_osds;
  int acting_primary = -1;
  osdmap->pg_to_acting_osds(this->pgid, &acting_osds, &acting_primary);
  
  for (int shard : acting_osds) {
    // Skip failed OSDs (marked as CRUSH_ITEM_NONE)
    if (shard == CRUSH_ITEM_NONE) {
      continue;
    }
    auto evt = std::make_shared<PGPeeringEvent>(
      osdmap->get_epoch(),
      osdmap->get_epoch(),
      PeeringState::Initialize());
    
    get_peering_state(shard)->handle_event(evt, get_peering_ctx(shard));
  }
  event_loop->run_until_idle();
}

void ECPeeringTestFixture::event_advance_map() {
  // Capture the current osdmap and pgid for use in the lambda
  OSDMapRef current_osdmap = osdmap;
  pg_t current_pgid = this->pgid;

  // Schedule advance_map events for each shard instead of running directly
  for (auto& [shard, ctx] : shard_peering_ctxs) {
    PeeringState* ps = shard_peering_states.at(shard).get();
    OSDMapRef lastmap = ps->get_osdmap();
    PeeringCtx* peering_ctx = ctx.get();
    
    event_loop->schedule_peering_event(shard, [ps, current_osdmap, lastmap, current_pgid, peering_ctx]() {
      // Get up/acting sets from OSDMap inside the lambda
      std::vector<int> up_osds, acting_osds;
      int up_primary = -1, acting_primary = -1;
      current_osdmap->pg_to_up_acting_osds(current_pgid, &up_osds, &up_primary, &acting_osds, &acting_primary);
      
      ps->advance_map(
        current_osdmap, lastmap, up_osds, up_primary, acting_osds, acting_primary,
        *peering_ctx);
    });
  }
  event_loop->run_until_idle();
}

void ECPeeringTestFixture::event_activate_map() {
  // Schedule activate_map events for each shard instead of running directly
  for (auto& [shard, ctx] : shard_peering_ctxs) {
    PeeringState* ps = shard_peering_states.at(shard).get();
    PeeringCtx* peering_ctx = ctx.get();
    
    event_loop->schedule_peering_event(shard, [ps, peering_ctx]() {
      ps->activate_map(*peering_ctx);
    });
  }
  event_loop->run_until_idle();
}

void ECPeeringTestFixture::dispatch_buffered_messages(int from_shard, PeeringCtx* ctx) {
  ceph_assert(messenger);
  ceph_assert(ctx);

  // Check if there are any buffered messages in the context
  for (auto& [target_osd, msg_list] : ctx->message_map) {
    for (auto& msg : msg_list) {
      // Route the message through the messenger
      // msg is a MessageRef (boost::intrusive_ptr<Message>), need to get raw pointer
      // MockMessenger will set the connection when it processes the message
      messenger->send_message(from_shard, target_osd, msg.get());
    }
    msg_list.clear();
  }
  ctx->message_map.clear();
}

bool ECPeeringTestFixture::all_shards_active() {
  // Get acting set from OSDMap
  std::vector<int> acting_osds;
  int acting_primary = -1;
  osdmap->pg_to_acting_osds(this->pgid, &acting_osds, &acting_primary);
  
  for (int shard : acting_osds) {
    // Skip failed OSDs (marked as CRUSH_ITEM_NONE)
    if (shard == CRUSH_ITEM_NONE) {
      continue;
    }
    if (!get_peering_state(shard)->is_active()) {
      return false;
    }
  }
  return true;
}

bool ECPeeringTestFixture::all_shards_clean() {
  // Get primary from OSDMap
  std::vector<int> acting_osds;
  int acting_primary = -1;
  osdmap->pg_to_acting_osds(this->pgid, &acting_osds, &acting_primary);
  
  if (acting_primary >= 0 && acting_primary != CRUSH_ITEM_NONE) {
    return get_peering_state(acting_primary)->is_clean();
  }
  return false;
}

std::string ECPeeringTestFixture::get_state_name(int shard) {
  return get_peering_state(shard)->get_current_state();
}

void ECPeeringTestFixture::suspend_osd(int osd) {
  if (event_loop) {
    event_loop->suspend_to_osd(osd);
  }
}

void ECPeeringTestFixture::unsuspend_osd(int osd) {
  if (event_loop) {
    event_loop->unsuspend_to_osd(osd);
  }
}

bool ECPeeringTestFixture::is_osd_suspended(int osd) {
  return event_loop && event_loop->is_to_osd_suspended(osd);
}

void ECPeeringTestFixture::suspend_primary_to_osd(int to_osd) {
  if (event_loop) {
    int primary = get_primary_shard_from_osdmap();
    if (primary >= 0) {
      event_loop->suspend_from_to_osd(primary, to_osd);
    }
  }
}

void ECPeeringTestFixture::unsuspend_primary_to_osd(int to_osd) {
  if (event_loop) {
    int primary = get_primary_shard_from_osdmap();
    if (primary >= 0) {
      event_loop->unsuspend_from_to_osd(primary, to_osd);
    }
  }
}

PeeringState* ECPeeringTestFixture::create_peering_state(int shard)
{
  const pg_pool_t& pi = get_pool();
  pg_shard_t pg_whoami(shard, shard_id_t(shard));
  PGPool pool(osdmap, pool_id, pi, "test_pool");

  shard_dpps[shard] = std::make_unique<ShardDpp>(g_ceph_context, this, shard);

  // Construct MockPeeringListener, transferring ownership of the backend
  // listener created by setup_ec_pool() directly. No throw-away construction.
  shard_peering_listeners[shard] = std::make_unique<MockPeeringListener>(
    osdmap, pool_id, shard_dpps[shard].get(), pg_whoami,
    std::move(listeners[shard]),
    store.get(), colls[shard], chs[shard]);

  auto& pl = shard_peering_listeners[shard];
  pl->current_epoch = osdmap->get_epoch();
  pl->set_messenger(messenger.get());
  pl->set_event_loop(event_loop.get());
  pl->set_fixture(this);
  pl->backend_listener->set_messenger(messenger.get());

  pl->queue_transaction_callback =
    [this, shard](ObjectStore::Transaction&& t) -> int {
      return queue_transaction_helper(shard, std::move(t));
    };

  spg_t spgid(pgid, shard_id_t(shard));
  auto ps = std::make_unique<PeeringState>(
    g_ceph_context,
    pg_whoami,
    spgid,
    pool,
    osdmap,
    PG_FEATURE_CLASSIC_ALL,
    shard_dpps[shard].get(),
    pl.get());

  pl->ps = ps.get();

  ps->set_backend_predicates(
    get_is_readable_predicate(),
    get_is_recoverable_predicate());

  shard_peering_states[shard] = std::move(ps);
  pl->backend_listener->set_peering_state(shard_peering_states[shard].get());
  shard_peering_ctxs[shard] = std::make_unique<PeeringCtx>();

  return shard_peering_states[shard].get();
}

void ECPeeringTestFixture::init_peering(bool dne)
{
  pg_history_t history;
  history.same_interval_since = osdmap->get_epoch();
  history.epoch_pool_created = osdmap->get_epoch();
  history.last_epoch_clean = osdmap->get_epoch();
  if (!dne) {
    history.epoch_created = osdmap->get_epoch();
  }
  PastIntervals past_intervals;

  // Get primary from OSDMap using base class pgid member
  std::vector<int> up_osds, acting_osds;
  int up_primary = -1, acting_primary = -1;
  osdmap->pg_to_up_acting_osds(this->pgid, &up_osds, &up_primary, &acting_osds, &acting_primary);

  for (int shard : acting_osds) {
    ObjectStore::Transaction t;
    get_peering_state(shard)->init(
      (shard == acting_primary) ? 0 : 1,  // role
      up_osds,
      up_primary,
      acting_osds,
      acting_primary,
      history,
      past_intervals,
      t);

    queue_transaction_helper(shard, std::move(t));
  }
}

void ECPeeringTestFixture::update_osdmap_with_peering(
  std::shared_ptr<OSDMap> new_osdmap,
  std::optional<pg_shard_t> new_primary)
{
  OSDMapRef old_osdmap = osdmap;

  update_osdmap(new_osdmap, new_primary);
  new_epoch(false);
  new_epoch_loop();
}

void ECPeeringTestFixture::new_epoch_loop() {
  int max = 10;
  do {
    ceph_assert(--max);
    event_advance_map();
    event_activate_map();
  } while (new_epoch(true));
}

bool ECPeeringTestFixture::new_epoch(bool if_required)
{
  bool did_work = false;
  epoch_t e = osdmap->get_epoch();
  OSDMap::Incremental pending_inc(e + 1);
  pending_inc.fsid = osdmap->get_fsid();

  // Get acting set from OSDMap
  std::vector<int> acting_osds;
  int acting_primary = -1;
  osdmap->pg_to_acting_osds(this->pgid, &acting_osds, &acting_primary);

  for (int shard : acting_osds) {
    // Skip failed OSDs (marked as CRUSH_ITEM_NONE)
    if (shard == CRUSH_ITEM_NONE) {
      continue;
    }
    if (get_peering_state(shard)->get_need_up_thru()) {
      pending_inc.new_up_thru[shard] = e;
      did_work = true;
    }
  }

  if (acting_primary >= 0) {
    auto& listener = shard_peering_listeners[acting_primary];
    if (listener->pg_temp_wanted) {
      std::vector<int> up_osds;
      int up_primary = -1;
      osdmap->pg_to_up_acting_osds(this->pgid, &up_osds, &up_primary, nullptr, nullptr);
      
      std::vector<int> acting_temp = listener->next_acting;
      if (acting_temp.empty()) {
        acting_temp = up_osds;
      }

      pending_inc.new_pg_temp[this->pgid] =
        mempool::osdmap::vector<int>(acting_temp.begin(), acting_temp.end());
      
      listener->pg_temp_wanted = false;
      did_work = true;
    }
  }

  if (!did_work && if_required) {
    return false;
  }

  osdmap->apply_incremental(pending_inc);

  for (auto& [shard, listener] : shard_peering_listeners) {
    listener->current_epoch = osdmap->get_epoch();
  }

  return true;
}

void ECPeeringTestFixture::run_first_peering() {
  init_peering();
  event_initialize();
  new_epoch_loop();
}

int ECPeeringTestFixture::queue_transaction_helper(int shard, ObjectStore::Transaction&& t)
{
  if (t.empty()) {
    return 0;
  }

  // Note: Contexts are stolen by MockPGBackendListener::queue_transaction,
  // so we don't need to call execute_finishers here
  int result = store->queue_transaction(chs[shard], std::move(t));

  return result;
}

void ECPeeringTestFixture::mark_osd_down(int osd_id)
{
  // Create new OSDMap with the OSD marked as down
  // This emulates what the real monitor does: just mark the OSD down,
  // do NOT set pg_temp. Peering will detect the change and request pg_temp.
  auto new_osdmap = std::make_shared<OSDMap>();
  new_osdmap->deepish_copy_from(*osdmap);
  OSDMapTestHelpers::mark_osd_down(new_osdmap, osd_id);
  
  update_osdmap_with_peering(new_osdmap);
}

void ECPeeringTestFixture::mark_osd_up(int osd_id)
{
  // Create new OSDMap with the OSD marked as up using OSDMapTestHelpers
  auto new_osdmap = std::make_shared<OSDMap>();
  new_osdmap->deepish_copy_from(*osdmap);
  OSDMapTestHelpers::mark_osd_up(new_osdmap, osd_id);
  
  update_osdmap_with_peering(new_osdmap);
}

void ECPeeringTestFixture::mark_osds_down(const std::vector<int>& osd_ids)
{
  // Create new OSDMap with all OSDs marked as down using OSDMapTestHelpers
  auto new_osdmap = std::make_shared<OSDMap>();
  new_osdmap->deepish_copy_from(*osdmap);
  OSDMapTestHelpers::mark_osds_down(new_osdmap, osd_ids);
  
  update_osdmap_with_peering(new_osdmap);
}

void ECPeeringTestFixture::set_pool_min_size(unsigned new_min_size)
{
  auto new_osdmap = std::make_shared<OSDMap>();
  new_osdmap->deepish_copy_from(*osdmap);
  OSDMapTestHelpers::set_pool_min_size(new_osdmap, pool_id, new_min_size);
  
  update_osdmap_with_peering(new_osdmap);
}

void ECPeeringTestFixture::advance_epoch()
{
  auto new_osdmap = std::make_shared<OSDMap>();
  new_osdmap->deepish_copy_from(*osdmap);
  OSDMapTestHelpers::advance_epoch(new_osdmap);
  
  update_osdmap_with_peering(new_osdmap);
}

void ECPeeringTestFixture::run_recovery_and_verify_callbacks(
  const std::string& obj_name,
  int removed_osd,
  const std::string& expected_data)
{
  // Get the actual primary from the OSDMap (don't assume it's shard 0)
  int primary_shard = get_primary_shard_from_osdmap();
  auto primary_ps = get_peering_state(primary_shard);
  pg_shard_t removed_shard(removed_osd, shard_id_t(removed_osd));
  
  hobject_t hoid = make_test_object(obj_name);
  pg_missing_item missing_item;
  
  // Check if the removed OSD is the current primary
  // If so, check the primary's own missing set; otherwise check peer_missing
  if (removed_osd == primary_shard) {
    // The removed OSD became primary again after coming back up
    // Check the primary's own missing set
    const pg_missing_t& primary_missing = primary_ps->get_pg_log().get_missing();
    ASSERT_TRUE(primary_missing.have_missing())
      << "Primary OSD " << removed_osd << " should have missing objects after coming back up";
    
    ASSERT_TRUE(primary_missing.is_missing(hoid, &missing_item))
      << "Object " << obj_name << " should be in primary " << removed_osd << "'s missing set";
    
    std::cout << "  OSD " << removed_osd << " is the primary and has the object in its own missing set" << std::endl;
  } else {
    // The removed OSD is a peer, check peer_missing
    const auto& peer_missing_map = primary_ps->get_peer_missing();
    auto peer_missing_it = peer_missing_map.find(removed_shard);
    ASSERT_NE(peer_missing_it, peer_missing_map.end())
      << "Primary should have peer_missing entry for OSD " << removed_osd;

    const pg_missing_t& peer_missing = peer_missing_it->second;
    ASSERT_TRUE(peer_missing.have_missing())
      << "Peer OSD " << removed_osd << " should have missing objects after coming back up";

    ASSERT_TRUE(peer_missing.is_missing(hoid, &missing_item))
      << "Object " << obj_name << " should be in peer " << removed_osd << "'s missing set";
    
    std::cout << "  OSD " << removed_osd << " is a peer and has the object in peer_missing" << std::endl;
  }

  ObjectContextRef obc = get_or_create_obc(hoid, true, expected_data.length());
  ASSERT_FALSE(obc->attr_cache.empty()) << "OBC attr_cache must be populated for recovery";

  // Reset recovery callback tracker before starting recovery
  auto* primary_listener = get_primary_listener();
  primary_listener->recovery_tracker.reset();
  
  std::cout << "  Starting recovery operation for object " << hoid << std::endl;
  PGBackend::RecoveryHandle *h = get_primary_backend()->open_recovery_op();
  int r = get_primary_backend()->recover_object(hoid, missing_item.need, ObjectContextRef(), obc, h);
  ASSERT_EQ(0, r) << "recover_object should successfully queue the recovery operation";
  
  std::cout << "  Running recovery op (this triggers the recovery flow)" << std::endl;
  get_primary_backend()->run_recovery_op(h, 10);  // priority = 10
  event_loop->run_until_idle();
  
  // Verify recovery callbacks were invoked
  std::cout << "\n  === Recovery Callback Verification ===" << std::endl;
  std::cout << "  on_local_recover calls: " << primary_listener->recovery_tracker.on_local_recover_calls << std::endl;
  for (const auto& obj : primary_listener->recovery_tracker.on_local_recover_objects) {
    std::cout << "    - object: " << obj << std::endl;
  }
  
  std::cout << "  on_peer_recover calls: " << primary_listener->recovery_tracker.on_peer_recover_calls.size() << " peers" << std::endl;
  for (const auto& [peer, count] : primary_listener->recovery_tracker.on_peer_recover_calls) {
    std::cout << "    - peer " << peer << ": " << count << " calls" << std::endl;
  }
  for (const auto& [peer, obj] : primary_listener->recovery_tracker.on_peer_recover_objects) {
    std::cout << "      object: " << obj << std::endl;
  }
  
  std::cout << "  on_global_recover calls: " << primary_listener->recovery_tracker.on_global_recover_calls << std::endl;
  for (const auto& obj : primary_listener->recovery_tracker.on_global_recover_objects) {
    std::cout << "    - object: " << obj << std::endl;
  }

  // When the removed OSD is the primary itself, recovery is local (on_local_recover)
  // When the removed OSD is a peer, recovery is remote (on_peer_recover)
  if (removed_osd == primary_shard) {
    // Primary recovering itself - should use on_local_recover
    ASSERT_EQ(1, primary_listener->recovery_tracker.on_local_recover_calls)
      << "on_local_recover should be called once when primary recovers itself";
    ASSERT_EQ(hoid, primary_listener->recovery_tracker.on_local_recover_objects[0])
      << "on_local_recover should be called for the correct object";
    std::cout << "  ✓ Primary shard " << primary_shard << " recovered object locally (on_local_recover called)" << std::endl;
  } else {
    // Peer recovering - should use on_peer_recover
    ASSERT_EQ(1, primary_listener->recovery_tracker.on_peer_recover_calls[removed_shard])
      << "on_peer_recover should be called once on primary for the recovering peer";
    std::cout << "  ✓ Peer shard " << removed_shard << " recovered via on_peer_recover" << std::endl;
  }
  
  ASSERT_EQ(1, primary_listener->recovery_tracker.on_global_recover_calls)
    << "on_global_recover should be called once on primary when all peers complete";
  ASSERT_EQ(hoid, primary_listener->recovery_tracker.on_global_recover_objects[0])
    << "on_global_recover should be called for the correct object";
  
  // Verify that the callbacks updated PeeringState correctly
  std::cout << "\n  === PeeringState Update Verification ===" << std::endl;
  
  // After recovery, the object should no longer be missing
  if (removed_osd == primary_shard) {
    // Primary recovered itself - check primary's own missing set
    const pg_missing_t& updated_primary_missing = primary_ps->get_pg_log().get_missing();
    ASSERT_FALSE(updated_primary_missing.is_missing(hoid))
      << "After on_local_recover, object should no longer be in primary's missing set";
    std::cout << "  ✓ Primary " << primary_shard << " no longer has object " << hoid << " missing" << std::endl;
  } else {
    // Peer recovered - check peer_missing map
    const auto& updated_peer_missing_map = primary_ps->get_peer_missing();
    auto updated_peer_missing_it = updated_peer_missing_map.find(removed_shard);
    
    if (updated_peer_missing_it != updated_peer_missing_map.end()) {
      const pg_missing_t& updated_peer_missing = updated_peer_missing_it->second;
      ASSERT_FALSE(updated_peer_missing.is_missing(hoid))
        << "After on_peer_recover, object should no longer be in peer's missing set";
      std::cout << "  ✓ Peer " << removed_shard << " no longer has object " << hoid << " missing" << std::endl;
    }
  }
  
  // After on_global_recover, the object should be in the missing_loc (available locations)
  const auto& missing_loc = primary_ps->get_missing_loc();
  const auto& missing_locs = missing_loc.get_missing_locs();
  auto loc_it = missing_locs.find(hoid);
  if (loc_it != missing_locs.end()) {
    ASSERT_TRUE(loc_it->second.count(removed_shard) > 0)
      << "After recovery, peer should be listed as having the object";
    std::cout << "  ✓ Object " << hoid << " is now available on peer " << removed_shard << std::endl;
  }
  
  std::cout << "  === All recovery callbacks and PeeringState updates verified successfully ===" << std::endl;
  
  // Verify the object data is correct
  verify_object(obj_name, expected_data, 0, expected_data.length());
}

