// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#ifndef MOSDECZONEREPLICATE_H
#define MOSDECZONEREPLICATE_H

#include "MOSDFastDispatchOp.h"
#include "osd/ECMsgTypes.h"

// Sent by the cluster Primary to a remote Zone Primary.
// Carries the raw (pre-encoding) PGBackend transaction so the Zone Primary
// can encode and fan out shard writes locally within its zone.
class MOSDECZoneReplicate : public MOSDFastDispatchOp {
private:
  static constexpr int HEAD_VERSION = 1;
  static constexpr int COMPAT_VERSION = 1;

public:
  spg_t pgid;
  epoch_t map_epoch = 0, min_epoch = 0;
  ECZoneReplicateOp op;

  int get_cost() const override {
    return 0;
  }
  epoch_t get_map_epoch() const override {
    return map_epoch;
  }
  epoch_t get_min_epoch() const override {
    return min_epoch;
  }
  spg_t get_spg() const override {
    return pgid;
  }

  MOSDECZoneReplicate()
    : MOSDFastDispatchOp{MSG_OSD_EC_ZONE_REPLICATE, HEAD_VERSION, COMPAT_VERSION}
    {}

  void decode_payload() override {
    using ceph::decode;
    auto p = payload.cbegin();
    decode(pgid, p);
    decode(map_epoch, p);
    decode(min_epoch, p);
    decode(op, p);
    decode_trace(p);
  }

  void encode_payload(uint64_t features) override {
    using ceph::encode;
    encode(pgid, payload);
    encode(map_epoch, payload);
    encode(min_epoch, payload);
    op.encode(payload);
    encode_trace(payload, features);
  }

  std::string_view get_type_name() const override {
    return "MOSDECZoneReplicate";
  }

  void print(std::ostream& out) const override {
    out << "MOSDECZoneReplicate(" << pgid
        << " " << map_epoch << "/" << min_epoch
        << " " << op
        << ")";
  }

  void clear_buffers() override {
    op.t.reset();
    op.log_entries.clear();
  }

private:
  template<class T, typename... Args>
  friend boost::intrusive_ptr<T> ceph::make_message(Args&&... args);
};

#endif
