// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include "PGTransaction.h"

namespace {
    enum class BufferUpdateTag : uint8_t {
    Write = 0,
    Zero = 1,
    CloneRange = 2,
    };
}

  // Serialise/deserialise op_map for inter-zone replication.
  // obc_map holds live in-memory pointers and is intentionally NOT encoded;
  // the receiver reconstructs OBCs from its local object store.
void PGTransaction::encode(ceph::buffer::list &bl) const {
    using ceph::encode;
    // version 1
    encode(static_cast<uint8_t>(1), bl);
    encode(static_cast<uint32_t>(op_map.size()), bl);
    for (auto &[hoid, op] : op_map) {
        encode(hoid, bl);
        encode_op(op, bl);
    }
}

void PGTransaction::decode(ceph::buffer::list::const_iterator &bl) {
    using ceph::decode;
    uint8_t v;
    uint32_t n;
    decode(v, bl);
    decode(n, bl);
    op_map.clear();
    for (uint32_t i = 0; i < n; ++i) {
        hobject_t hoid; 
        decode(hoid, bl);
        decode_op(op_map[hoid], bl);
    }
}

void PGTransaction::encode_op(const ObjectOperation &op, ceph::buffer::list &bl) {
    using ceph::encode;

    InitTag tag;
    hobject_t src;
    if (std::holds_alternative<ObjectOperation::Init::None>(op.init_type)) {
        tag = InitTag::None;
    } else if (std::holds_alternative<ObjectOperation::Init::Create>(op.init_type)) {
        tag = InitTag::Create;
    } else if (std::holds_alternative<ObjectOperation::Init::Clone>(op.init_type)) {
        tag = InitTag::Clone;
        src = std::get<ObjectOperation::Init::Clone>(op.init_type).source;
    } else {
        tag = InitTag::Rename;
        src = std::get<ObjectOperation::Init::Rename>(op.init_type).source;
    }
    encode(static_cast<uint8_t>(tag), bl);
    if (tag == InitTag::Clone || tag == InitTag::Rename)
        encode(src, bl);

    encode(op.delete_first, bl);
    encode(op.clear_omap, bl);
    encode(op.truncate, bl);
    encode(op.attr_updates, bl);

    // omap_updates
    encode(static_cast<uint32_t>(op.omap_updates.size()), bl);
    for (auto &[utype, ubl] : op.omap_updates) {
        encode(static_cast<uint8_t>(utype), bl);
        encode(ubl, bl);
    }

    encode(op.omap_header, bl);
    encode(op.updated_snaps, bl);

    // alloc_hint
    encode(op.alloc_hint.has_value(), bl);
    if (op.alloc_hint) {
        encode(op.alloc_hint->expected_object_size, bl);
        encode(op.alloc_hint->expected_write_size, bl);
        encode(op.alloc_hint->flags, bl);
    }

    // buffer_updates: flat list of (off, len, type, payload)
    uint32_t nb = op.buffer_updates.size();
    encode(nb, bl);
    for (auto it = op.buffer_updates.begin(); it != op.buffer_updates.end(); ++it) {
        encode(it.get_off(), bl);
        encode(it.get_len(), bl);
        const ObjectOperation::BufferUpdateType &bu = it.get_val();
        if (std::holds_alternative<ObjectOperation::BufferUpdate::Write>(bu)) {
            encode(static_cast<uint8_t>(BufferUpdateTag::Write), bl);
            auto &w = std::get<ObjectOperation::BufferUpdate::Write>(bu);
            encode(w.buffer, bl);
            encode(w.fadvise_flags, bl);
        } else if (std::holds_alternative<ObjectOperation::BufferUpdate::Zero>(bu)) {
            encode(static_cast<uint8_t>(BufferUpdateTag::Zero), bl);
        // len already encoded above; no extra payload
        } else {
            encode(static_cast<uint8_t>(BufferUpdateTag::CloneRange), bl);
            auto &c = std::get<ObjectOperation::BufferUpdate::CloneRange>(bu);
            encode(c.from, bl);
            encode(c.offset, bl);
            encode(c.len, bl);
        }
    }
}

 void PGTransaction::decode_op(ObjectOperation &op, ceph::buffer::list::const_iterator &bl) {
    using ceph::decode;

    uint8_t rawtag; decode(rawtag, bl);
    auto tag = static_cast<InitTag>(rawtag);
    if (tag == InitTag::None) {
      op.init_type = ObjectOperation::Init::None{};
    } else if (tag == InitTag::Create) {
      op.init_type = ObjectOperation::Init::Create{};
    } else if (tag == InitTag::Clone) {
      hobject_t src; decode(src, bl);
      op.init_type = ObjectOperation::Init::Clone{src};
    } else {
      hobject_t src; decode(src, bl);
      op.init_type = ObjectOperation::Init::Rename{src};
    }

    decode(op.delete_first, bl);
    decode(op.clear_omap, bl);
    decode(op.truncate, bl);
    decode(op.attr_updates, bl);

    uint32_t n; 
    decode(n, bl);
    op.omap_updates.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
      uint8_t rawtype; 
      decode(rawtype, bl);
      op.omap_updates[i].first = static_cast<ObjectOperation::OmapUpdateType>(rawtype);
      decode(op.omap_updates[i].second, bl);
    }

    decode(op.omap_header, bl);
    decode(op.updated_snaps, bl);

    bool has_alloc; 
    decode(has_alloc, bl);
    if (has_alloc) {
      ObjectOperation::alloc_hint_t ah;
      decode(ah.expected_object_size, bl);
      decode(ah.expected_write_size, bl);
      decode(ah.flags, bl);
      op.alloc_hint = ah;
    }

    uint32_t nb; 
    decode(nb, bl);
    for (uint32_t i = 0; i < nb; ++i) {
        uint64_t off, len; 
        decode(off, bl); 
        decode(len, bl);
        uint8_t raw_btype;
        decode(raw_btype, bl);
        auto btype = static_cast<BufferUpdateTag>(raw_btype);
        if (btype == BufferUpdateTag::Write) {
            ceph::buffer::list buf;
            decode(buf, bl);
            uint32_t flags;
            decode(flags, bl);
            op.buffer_updates.insert(off, len, ObjectOperation::BufferUpdate::Write{buf, flags});
        } else if (btype == BufferUpdateTag::Zero) {
            op.buffer_updates.insert(off, len, ObjectOperation::BufferUpdate::Zero{len});
        } else {
            hobject_t from;
            decode(from, bl);
            uint64_t src_off, src_len;
            decode(src_off, bl); 
            decode(src_len, bl);
            op.buffer_updates.insert(off, len, ObjectOperation::BufferUpdate::CloneRange{from, src_off, src_len});
        }
    }
}
