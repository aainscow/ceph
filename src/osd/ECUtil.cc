// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-

#include <errno.h>
#include "common/ceph_context.h"
#include "global/global_context.h"
#include "include/encoding.h"
#include "ECUtil.h"
#include "common/debug.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_osd
#undef dout_prefix
#define dout_prefix _prefix(_dout)

using namespace std;
using ceph::bufferlist;
using ceph::ErasureCodeInterfaceRef;
using ceph::Formatter;

// FIXME: We want this to be the same as the constant in ErasureCode.h, which
//        cannot be included here, for reasons I have not figured out yet.
static const unsigned SIMD_ALIGN = 32;

static ostream& _prefix(std::ostream* _dout)
{
  return *_dout << "ECUtil: ";
}

std::pair<uint64_t, uint64_t> ECUtil::stripe_info_t::chunk_aligned_offset_len_to_chunk(
  uint64_t _off, uint64_t _len) const {
  auto [off, len] = offset_len_to_stripe_bounds(_off, _len);
  return std::make_pair(
    chunk_aligned_logical_offset_to_chunk_offset(off),
    chunk_aligned_logical_size_to_chunk_size(len));
}

/*
ASCII Art describing the various variables in the following function:
                    start    end
                      |       |
                      |       |
                      |       |
           - - - - - -v- -+---+-----------+ - - - - - -
                 start_adj|   |           |      ^
to_read.offset - ->-------+   |           | chunk_size
                  |           |           |      v
           +------+ - - - - - + - - - - - + - - - - - -
           |                  |           |
           |                  v           |
           |              - - - - +-------+
           |               end_adj|
           |              +-------+
           |              |       |
           +--------------+       |
                          |       |
                          | shard |

Given an offset and size, this adds to a vector of extents describing the
minimal IO ranges on each shard.  If passed, this method will also populate
a superset of all extents required.
 */
void ECUtil::stripe_info_t::ro_range_to_shards(
    uint64_t ro_offset,
    uint64_t ro_size,
    shard_extent_set_t *shard_extent_set,
    extent_set *extent_superset,
    buffer::list *bl,
    shard_extent_map_t *shard_extent_map) const {

  // Some of the maths below assumes size not zero.
  if (ro_size == 0) return;

  uint64_t k  = get_data_chunk_count();

  // Aim is to minimise non-^2 divs (chunk_size is assumed to be a power of 2).
  // These should be the only non ^2 divs.
  uint64_t begin_div = ro_offset / stripe_width;
  uint64_t end_div = (ro_offset+ro_size+stripe_width-1)/stripe_width - 1;
  uint64_t start = begin_div*chunk_size;
  uint64_t end = end_div*chunk_size;

  uint64_t start_shard = (ro_offset - begin_div*stripe_width) / chunk_size;
  uint64_t chunk_count = (ro_offset + ro_size + chunk_size - 1)/chunk_size - ro_offset/chunk_size;;

  // The end_shard needs a modulus to calculate the actual shard, however
  // it is convenient to store it like this for the loop.
  auto end_shard = start_shard + std::min(chunk_count, k );

  // The last shard is the raw shard index which contains the last chunk.
  // Is it possible to calculate this without th e +%?
  uint64_t last_shard = (start_shard + chunk_count - 1) % k ;

  uint64_t buffer_shard_start_offset = 0;

  for (auto i = start_shard; i < end_shard; i++) {
    auto raw_shard = i >= k  ? i - k  : i;

    // Adjust the start and end blocks if needed.
    uint64_t start_adj = 0;
    uint64_t end_adj = 0;

    if (raw_shard<start_shard) {
      // Shards before the start, must start on the next chunk.
      start_adj = chunk_size;
    } else if (raw_shard == start_shard) {
      // The start shard itself needs to be moved a partial-chunk forward.
      start_adj = ro_offset % chunk_size;
    }

    // The end is similar to the start, but the end must be rounded up.
    if (raw_shard < last_shard) {
      end_adj = chunk_size;
    } else if (raw_shard == last_shard) {
      end_adj = (ro_offset + ro_size - 1) % chunk_size + 1;
    }

    auto shard = chunk_mapping.size() > raw_shard ?
             chunk_mapping[raw_shard] : static_cast<int>(raw_shard);

    uint64_t off = start + start_adj;
    uint64_t len =  end + end_adj - start - start_adj;
    if (shard_extent_set) {
      (*shard_extent_set)[shard].insert(off, len);
    }

    if (extent_superset) {
      extent_superset->insert(off, len);
    }

    if (shard_extent_map) {
      ceph_assert(bl);
      buffer::list shard_bl;

      uint64_t bl_offset = buffer_shard_start_offset;

      // Start with any partial chunks.
      if(chunk_size != start_adj) {
        shard_bl.substr_of(*bl, bl_offset, min((uint64_t)bl->length() - bl_offset, chunk_size-start_adj));
        buffer_shard_start_offset += chunk_size - start_adj;
        bl_offset += chunk_size - start_adj + (k - 1) * chunk_size;
      } else {
        buffer_shard_start_offset += chunk_size;
      }
      while (bl_offset < bl->length()) {
        buffer::list tmp;
        tmp.substr_of(*bl, bl_offset, min(chunk_size, bl->length() - bl_offset));
        shard_bl.append(tmp);
        bl_offset += k * chunk_size;
      }
      (*shard_extent_map).insert_in_shard(shard, off, shard_bl, ro_offset, ro_offset + ro_size);
    }
  }
}

void ECUtil::stripe_info_t::trim_shard_extent_set_for_ro_offset (uint64_t ro_offset,
  shard_extent_set_t &shard_extent_set) const {

  /* If the offset is within the first shard, then the remaining shards are
   * not written and we don't need to generated zeros for either */
  int ro_offset_shard = (ro_offset / chunk_size) % k;
  if (ro_offset_shard == 0) {
    uint64_t shard_offset = ro_offset_to_shard_offset(ro_offset, 0);
    for (auto &&iter = shard_extent_set.begin(); iter != shard_extent_set.end();) {
      iter->second.erase_after(align_page_next(shard_offset));
      if (iter->second.empty()) iter = shard_extent_set.erase(iter);
      else ++iter;
    }
  }
}

void ECUtil::stripe_info_t::ro_size_to_stripe_aligned_read_mask(
  uint64_t ro_size,
  shard_extent_set_t &shard_extent_set) const {
  ro_range_to_shard_extent_set_with_parity(0, logical_to_next_stripe_offset(ro_size), shard_extent_set);
  trim_shard_extent_set_for_ro_offset(ro_size, shard_extent_set);
}

void ECUtil::stripe_info_t::ro_size_to_read_mask(
  uint64_t ro_size,
  shard_extent_set_t &shard_extent_set) const {
  ro_range_to_shard_extent_set_with_parity(0, align_page_next(ro_size), shard_extent_set);
}

void ECUtil::stripe_info_t::ro_size_to_zero_mask(
  uint64_t ro_size,
  shard_extent_set_t &shard_extent_set) const {
  // There should never be any zero padding on the parity.
  ro_range_to_shard_extent_set(align_page_next(ro_size),
    logical_to_next_stripe_offset(ro_size) - align_page_next(ro_size),
    shard_extent_set);
  trim_shard_extent_set_for_ro_offset(ro_size, shard_extent_set);
}

namespace ECUtil {
  void shard_extent_map_t::erase_after_ro_offset(uint64_t ro_offset)
  {
    /* Ignore the null case */
    if (ro_offset >= ro_end)
      return;

    shard_extent_set_t ro_to_erase;
    sinfo->ro_range_to_shard_extent_set(ro_offset, ro_end - ro_start,
                              ro_to_erase);
    for (auto && [shard, eset] : ro_to_erase) {
      if (extent_maps.contains(shard)) {
        extent_maps[shard].erase(eset.range_start(), eset.range_end());
      }

      // If the result is empty, delete the extent map.
      if (extent_maps[shard].empty()) {
          extent_maps.erase(shard);
      }
    }

    compute_ro_range();
  }

  shard_extent_map_t shard_extent_map_t::intersect_ro_range(uint64_t ro_offset,
    uint64_t ro_length) const
  {    // Optimise (common) use case where the overlap is everything
    if (ro_offset <= ro_start &&
        ro_offset + ro_length >= ro_end) {
      return *this;
    }

    // Optimise (common) use cases where the overlap is nothing
    if (ro_offset >= ro_end ||
        ro_offset + ro_length <= ro_start) {
      return shard_extent_map_t(sinfo);
    }

    shard_extent_set_t ro_to_intersect;
    sinfo->ro_range_to_shard_extent_set(ro_offset, ro_length, ro_to_intersect);

    return intersect(ro_to_intersect);
  }

  shard_extent_map_t shard_extent_map_t::intersect(optional<shard_extent_set_t> const &other) const
  {
    if (!other)
      return shard_extent_map_t(sinfo);

    return intersect(*other);
  }

  shard_extent_map_t shard_extent_map_t::intersect(shard_extent_set_t const &other) const
  {
    shard_extent_map_t out(sinfo);
    out.ro_end = 0;
    out.end_offset = 0;

    for (auto && [shard, this_eset] : other) {
      if (extent_maps.contains(shard)) {
        extent_map tmp;
        extent_set eset;
        eset.intersection_of(extent_maps.at(shard).get_interval_set(), this_eset);

        for (auto [offset, len] : eset) {
          bufferlist bl;
          get_buffer(shard, offset, len, bl);
          tmp.insert(offset, len, bl);
        }
        if (!tmp.empty()) {
          uint64_t range_start = tmp.get_start_off();
          uint64_t range_end = tmp.get_end_off();

          out.start_offset = min(out.start_offset, range_start);
          out.end_offset = max(out.end_offset, range_end);

          int raw_shard = sinfo->get_raw_shard(shard);
          if (raw_shard < sinfo->get_k()) {
            out.ro_start = std::min(out.ro_start, calc_ro_offset(raw_shard, range_start));
            out.ro_end = std::max(out.ro_end, calc_ro_end(raw_shard, range_end));
          }

          out.extent_maps.emplace(shard, std::move(tmp));
        }
      }
    }

    if (out.ro_start == invalid_offset) {
      out.ro_end = out.end_offset = invalid_offset;
    }

    return out;
  }

  void shard_extent_map_t::insert(shard_extent_map_t const &other)
  {
    for (auto && [shard, emap] : other.extent_maps)
    {
      if (!extent_maps.contains(shard))
        extent_maps.emplace(shard, emap);
      else
        extent_maps[shard].insert(emap);
    }

    if (ro_start == invalid_offset || other.ro_start < ro_start)
      ro_start = other.ro_start;
    if (ro_end == invalid_offset || other.ro_end > ro_end)
      ro_end = other.ro_end;
    if (start_offset == invalid_offset || other.start_offset < start_offset)
      start_offset = other.start_offset;
    if (end_offset == invalid_offset || other.end_offset > end_offset)
      end_offset = other.end_offset;
  }

  uint64_t shard_extent_map_t::size()
  {
    uint64_t size = 0;
    for (auto &i : extent_maps)
    {
      for (auto &j : i.second ) size += j.get_len();
    }

    return size;
  }

  void shard_extent_map_t::clear() {
    ro_start = ro_end = start_offset = end_offset = invalid_offset;
    extent_maps.clear();
  }


  /* Insert a buffer for a particular shard.
   * NOTE: DO NOT CALL sinfo->get_min_want_shards()
   */
  void shard_extent_map_t::insert_in_shard(int shard, uint64_t off,
    const buffer::list &bl)
  {
    if (bl.length() == 0)
      return;

    extent_maps[shard].insert(off, bl.length(), bl);
    uint64_t new_start = calc_ro_offset(sinfo->get_raw_shard(shard), off);
    uint64_t new_end = calc_ro_end(sinfo->get_raw_shard(shard), off + bl.length());
    if (empty()) {
      ro_start = new_start;
      ro_end = new_end;
      start_offset = off;
      end_offset = off + bl.length();
    } else {
      ro_start = min(ro_start, new_start);
      ro_end = max(ro_end, new_end);
      start_offset = min(start_offset, off);
      end_offset = max(end_offset, off + bl.length());
    }
  }

  /* Insert a buffer for a particular shard.
   * If the client knows the new start and end, use this interface to improve
   * performance.
   */
  void shard_extent_map_t::insert_in_shard(int shard, uint64_t off,
    const buffer::list &bl, uint64_t new_start, uint64_t new_end)
  {
    if (bl.length() == 0)
      return;

    extent_maps[shard].insert(off, bl.length(), bl);
    if (empty()) {
      ro_start = new_start;
      ro_end = new_end;
      start_offset = off;
      end_offset = off + bl.length();
    } else {
      ro_start = min(ro_start, new_start);
      ro_end = max(ro_end, new_end);
      start_offset = min(start_offset, off);
      end_offset = max(end_offset, off + bl.length());
    }
  }

  /* Insert a region of zeros in rados object address space..
   */
  void shard_extent_map_t::insert_ro_zero_buffer( uint64_t ro_offset,
    uint64_t ro_length )
  {
    buffer::list zero_buffer;
    zero_buffer.append_zero(ro_length);
    sinfo->ro_range_to_shard_extent_map(ro_offset, ro_length, zero_buffer, *this);
  }

  /* Append zeros to the extent maps, such that all bytes from the current end
   * of the rados object range to the specified offset are zero.  Note that the
   * byte at ro_offset does NOT get populated, so that this works as an
   * addition to length.
   */
  void shard_extent_map_t::append_zeros_to_ro_offset( uint64_t ro_offset )
  {
    uint64_t _ro_end = ro_end == invalid_offset ? 0 : ro_end;
    if (ro_offset <= _ro_end)
      return;
    uint64_t append_offset = _ro_end;
    uint64_t append_length = ro_offset - _ro_end;
    insert_ro_zero_buffer(append_offset, append_length);
  }

  /* This method rearranges buffers from a rados object extent map into a shard
   * extent map.  Note that it is a simple transformation, it does NOT perform
   * any encoding of parity shards.
   */
  void shard_extent_map_t::insert_ro_extent_map(const extent_map &host_extent_map)
  {
    for (auto &&range = host_extent_map.begin();
        range != host_extent_map.end();
        ++range) {
      buffer::list bl = range.get_val();
      sinfo->ro_range_to_shard_extent_map(
        range.get_off(),
        range.get_len(),
        bl,
        *this);
    }
  }

  extent_set shard_extent_map_t::get_extent_superset() const {
    extent_set eset;

    for (auto &&[shard, emap] : extent_maps) {
      eset.union_of(emap.get_interval_set());
    }

    return eset;
  }

  void shard_extent_map_t::insert_parity_buffers()
  {
    extent_set encode_set = get_extent_superset();

    /* Invent buffers for the parity coding, if they were not provided.
     * e.g. appends will not provide parity buffers.
     * We should EITHER have no buffers, or have the right buffers.
     */
    for (unsigned int i=sinfo->get_k(); i<sinfo->get_k_plus_m(); i++) {
      int shard = sinfo->get_shard(i);
      for (auto &&[offset, length] : encode_set) {
        /* No need to recreate buffers we already have */
        if (extent_maps.contains(shard)) {
          extent_map emap = extent_maps.at(shard);
          if (emap.contains(offset, length))
            continue;
        }
        std::set<int> shards;
        std::map<int, buffer::list> chunk_buffers;
        bufferlist bl;
        bl.push_back(buffer::create_aligned(length, SIMD_ALIGN));
        extent_maps[shard].insert(offset, length, bl);
      }
    }
  }

  shard_extent_map_t::slice_iterator::slice_iterator(shard_extent_map_t &sem) : sem(sem)
  {
    for (auto &&[shard, emap] : sem.extent_maps) {
      auto emap_iter = emap.begin();
      auto bl_iter = emap_iter.get_val().begin();
      auto p = make_pair(std::move(emap_iter), std::move(bl_iter));
      iters.emplace(shard, std::move(p));

      if (emap_iter.get_off() < start) {
        start = emap_iter.get_off();
      }
    }

    advance();
  }

  void shard_extent_map_t::slice_iterator::advance()
  {
    slice.clear();
    offset = start;
    end =  (uint64_t)-1;

    if (iters.empty()) return;

    // First we find the last buffer in the list
    for (auto &&[shard, iters] : iters) {
      auto &&[emap_iter, bl_iter] = iters;
      uint64_t iter_offset = emap_iter.get_off() + bl_iter.get_off();
      ceph_assert(iter_offset >= start);
      // If this iterator is after the current offset, then we will ignore
      // it for this buffer ptr. The end must move to or before this point.
      if (iter_offset > start && iter_offset < end) {
        end = iter_offset;
        continue;
      }

      uint64_t iter_end = iter_offset + bl_iter.get_current_ptr().length();
      if (iter_end < end) {
        end = iter_end;
      }
    }

    for (auto &&iter = iters.begin(); iter != iters.end();) {
      auto shard = iter->first;
      auto &&[emap_iter, bl_iter] = iter->second;
      uint64_t iter_offset = emap_iter.get_off() + bl_iter.get_off();
      bool erase = false;

      // Ignore any blank buffers.
      if (iter_offset == start) {
        ceph_assert(iter_offset == start);

        // Create a new buffer pointer for the result. We don't want the client
        // manipulating the ptr.
        slice.emplace(
          shard, bufferptr(bl_iter.get_current_ptr(), 0,end - start));

        // Now we need to move on the iterators.
        bl_iter += end - start;

        // If we have reached the end of the extent, we need to move that on too.
        if (bl_iter == emap_iter.get_val().end()) {
          ++emap_iter;
          if (emap_iter == sem.get_extent_map(shard).end()) {
            erase = true;
          } else {
            iters.at(shard).second = emap_iter.get_val().begin();
          }
        }
      } else ceph_assert(iter_offset > start);

      if (erase) iter = iters.erase(iter);
      else ++iter;
    }

    // We can now move the offset on.
    length = end - start;
    start = end;

    // The above can sometimes create an empty buffer out of a gap. This is
    // not desirable, so we run again in such an occasion.
    if (slice.empty()) {
      advance();
    }
  }

  shard_extent_map_t::slice_iterator shard_extent_map_t::begin_slice_iterator()
  {
    return slice_iterator(*this);
  }

  bool shard_extent_map_t::slice_iterator::is_page_aligned()
  {
    for (auto &&[_, ptr] : slice) {
      uintptr_t p = (uintptr_t)ptr.c_str();
      if (p & ~CEPH_PAGE_MASK) return false;
      if ((p + ptr.length()) & ~CEPH_PAGE_MASK) return false;
    }

    return true;
  }


  /* Encode parity chunks, using the encode_chunks interface into the
   * erasure coding.  This generates all parity.
   */
  int shard_extent_map_t::encode(ErasureCodeInterfaceRef& ec_impl,
                                 const HashInfoRef &hinfo,
                                 uint64_t before_ro_size) 
  {
    bool rebuild_req = false;

    for (auto iter = begin_slice_iterator(); !iter.is_end(); ++iter) {
      if (!iter.is_page_aligned()) {
        rebuild_req = true;
        break;
      }

      std::map<int, bufferptr> all_shards = iter.get_bufferptrs();
      std::map<int, bufferptr> in;
      std::map<int, bufferptr> out;

      auto in_start = all_shards.begin();
      auto in_end = all_shards.lower_bound(ec_impl->get_data_chunk_count());
      in.insert(in_start, in_end);

      auto out_start = all_shards.lower_bound(ec_impl->get_data_chunk_count());
      auto out_end = all_shards.lower_bound(ec_impl->get_data_chunk_count() + ec_impl->get_coding_chunk_count());
      out.insert(out_start, out_end);      

      int ret = ec_impl->encode_chunks(in, out);
      if (ret) return ret;
    }

    if (rebuild_req) {
      pad_and_rebuild_to_page_align();
      return encode(ec_impl, hinfo, before_ro_size);
    }

    if (hinfo && ro_start >= before_ro_size) {
      /* NEEDS REVIEW:  The following calculates the new hinfo CRCs. This is
       *                 currently considering ALL the buffers, including the
       *                 parity buffers.  Is this really right?
       *                 Also, does this really belong here? Its convenient
       *                 because have just built the buf§fer list...
       */
      for (auto iter = begin_slice_iterator(); !iter.is_end(); ++iter) {
        ceph_assert(ro_start == before_ro_size);
        hinfo->append(iter.get_offset(),  iter.get_bufferptrs());
      }
    }

    return 0;
  }

  int shard_extent_map_t::decode(ErasureCodeInterfaceRef& ec_impl,
    shard_extent_set_t want)
  {
    bool did_decode = false;
    int r = 0;
    for (auto &&[shard, eset]: want) {
      /* We are assuming here that a shard that has been read does not need
       * to be decoded. The ECBackend::handle_sub_read_reply code will erase
       * buffers for any shards with missing reads, so this should be safe.
       */
      if (extent_maps.contains(shard))
        continue;

      /* Use encode to encode the parity */
      if (sinfo->get_raw_shard(shard) >= sinfo->get_k())
        continue;

      did_decode = true;

      for (auto [offset, length]: eset) {
        /* Here we recover each missing shard independently. There may be
         * multiple missing shards and we could collect together all the
         * recoveries at one time. There may be some performance gains in
         * * that scenario if found necessary.
         */
        std::set<int> want_to_read;
        std::map<int, bufferlist> decoded;

        want_to_read.insert(shard);
        auto s = slice(offset, length);

        for (auto &&[_, bl] : s) {
          bl.rebuild_aligned_size_and_memory(sinfo->get_chunk_size(), SIMD_ALIGN);
        }

        /* Call the decode function.  This is not particularly efficient, as it
         * creates buffers for every shard, even if they are not needed.
         *
         * Currently, some plugins rely on this behaviour.
         *
         * The chunk size passed in is only used in the clay encoding. It is
         * NOT the size of the decode.
         */
        int r_i = ec_impl->decode(want_to_read, s, &decoded, sinfo->get_chunk_size());
        if (r_i) {
          r = r_i;
          break;
        }

        ceph_assert(decoded[shard].length() == length);
        insert_in_shard(shard, offset, decoded[shard], ro_start, ro_end);
      }
    }

    if (did_decode) compute_ro_range();

    return r;
  }

  void shard_extent_map_t::pad_and_rebuild_to_page_align()
  {
    bool resized = false;
    for (auto &&[shard, emap]: extent_maps) {
      for ( auto i = emap.begin(); i != emap.end(); ++i) {
        bool resized_i = false;
        bufferlist bl = i.get_val();
        uint64_t start = i.get_off();
        uint64_t end = start + i.get_len();

        if ((start & ~CEPH_PAGE_MASK) != 0) {
          bl.prepend_zero(start - (start & CEPH_PAGE_MASK));
          start = start & CEPH_PAGE_MASK;
          resized_i = true;
        }
        if ((end & ~CEPH_PAGE_MASK) != 0) {
          bl.append_zero((end & CEPH_PAGE_MASK) + CEPH_PAGE_SIZE - end);
          end = (end & CEPH_PAGE_MASK) + CEPH_PAGE_SIZE;
          resized_i = true;
        }

        // Perhaps we can get away without page aligning here and only SIMD
        // align. However, typical workloads are actually page aligned already,
        // so this should not cause problems on any sensible workload.
        if (bl.rebuild_aligned_size_and_memory(bl.length(), CEPH_PAGE_SIZE) || resized_i) {
          emap.insert(start, end - start, bl);
        }
        if (resized_i) resized = true;
      }
    }

    if (resized) compute_ro_range();
  }

  std::map<int, bufferlist> shard_extent_map_t::slice(int offset, int length) const
  {
    std::map<int, bufferlist> slice;

    for (auto &&[shard, emap]: extent_maps) {
      bufferlist shard_bl;
      get_buffer(shard, offset, length, shard_bl);
      if (shard_bl.length()) {
        shard_bl.rebuild_aligned_size_and_memory(length, SIMD_ALIGN);
        slice.emplace(shard, std::move(shard_bl));
      }
    }

    return slice;
  }

  shard_extent_map_t shard_extent_map_t::slice_map(int offset, int length) const
  {
    // Range entirely contains offset - this will be common for small IO.
    if (offset <= start_offset && offset + length >= end_offset) return *this;

    shard_extent_map_t slice(sinfo);

    // Null cases just generate an empty map.
    if (offset >= end_offset) return slice;
    if (offset + length <= start_offset) return slice;

    slice.end_offset = slice.ro_end = 0;

    for (auto &&[shard, emap]: extent_maps) {
      extent_map iemap = emap.intersect(offset, length);

      if (!iemap.empty()) {
        slice.start_offset = min(slice.start_offset, iemap.get_start_off());
        slice.end_offset = max(slice.start_offset, iemap.get_end_off());
        slice.ro_start = min(slice.start_offset, calc_ro_offset(sinfo->get_raw_shard(shard), iemap.get_start_off()));
        slice.ro_end = min(slice.ro_end, calc_ro_end(sinfo->get_raw_shard(shard), iemap.get_end_off()));
        slice.extent_maps.emplace(shard, iemap);
      }
    }

    if (slice.end_offset == 0) {
      slice.end_offset = slice.ro_end = invalid_offset;
    }

    return slice;
  }

  void shard_extent_map_t::get_buffer(int shard, uint64_t offset, uint64_t length,
                                      buffer::list &append_to) const
  {
    const extent_map &emap = extent_maps.at(shard);
    auto &&range = emap.get_lower_range(offset, length);

    if(range == emap.end() || !emap.contains(offset, length))
      return;

    if (range.get_len() == length) {
      buffer::list bl = range.get_val();
      // This should be asserted on extent map insertion.
      ceph_assert(bl.length() == length);
      append_to.append(bl);
    } else {
      buffer::list bl;
      bl.substr_of(range.get_val(), offset - range.get_off(), length);
      append_to.append(bl);
    }
  }

  void shard_extent_map_t::get_shard_first_buffer(int shard, buffer::list &append_to) const {

    if (!extent_maps.contains(shard))
      return;
    const extent_map &emap = extent_maps.at(shard);
    auto range = emap.begin();
    if (range == emap.end())
      return;

    append_to.append(range.get_val());
  }

  uint64_t shard_extent_map_t::get_shard_first_offset(int shard) const {

    if (!extent_maps.contains(shard))
      return invalid_offset;
    const extent_map &emap = extent_maps.at(shard);
    auto range = emap.begin();
    if (range == emap.end())
      return invalid_offset;

    return range.get_off();
  }

  void shard_extent_map_t::zero_pad(int shard, uint64_t offset, uint64_t length)
  {
    const extent_map &emap = extent_maps[shard];
    if (emap.contains(offset, length)) return;

    extent_set required;
    required.insert(offset, length);
    required.subtract(emap.get_interval_set());

    for (auto [z_off, z_len] : required) {
      bufferlist zeros;
      zeros.append_zero(z_len);
      insert_in_shard(shard, z_off, zeros);
    }
  }

  ECUtil::shard_extent_set_t shard_extent_map_t::get_extent_set()
  {
    shard_extent_set_t shard_eset;
    for (auto &&[shard, emap] : extent_maps) {
      shard_eset.emplace(shard, emap.get_interval_set());
    }

    return shard_eset;
  }

  void shard_extent_map_t::erase_shard(int shard)
  {
    if (extent_maps.erase(shard)) {
      compute_ro_range();
    }
  }

  bufferlist shard_extent_map_t::get_ro_buffer(
    uint64_t ro_offset,
    uint64_t ro_length)
  {
    bufferlist bl;
    uint64_t chunk_size = sinfo->get_chunk_size();
    uint64_t stripe_size = sinfo->get_stripe_width();
    int data_chunk_count = sinfo->get_data_chunk_count();

    pair read_pair(ro_offset, ro_length);
    auto chunk_aligned_read = sinfo->offset_len_to_chunk_bounds(read_pair);

    int raw_shard = (ro_offset / chunk_size) % data_chunk_count;

    for (uint64_t chunk_offset = chunk_aligned_read.first;
        chunk_offset < chunk_aligned_read.first + chunk_aligned_read.second;
        chunk_offset += chunk_size, raw_shard++) {

      if (raw_shard == data_chunk_count) raw_shard = 0;

      uint64_t sub_chunk_offset = std::max(chunk_offset, ro_offset);
      uint64_t sub_chunk_shard_offset = (chunk_offset / stripe_size) * chunk_size + sub_chunk_offset - chunk_offset;
      uint64_t sub_chunk_len = std::min(ro_offset + ro_length, chunk_offset + chunk_size) - sub_chunk_offset;

      get_buffer(sinfo->get_shard(raw_shard), sub_chunk_shard_offset, sub_chunk_len, bl);
    }
    return bl;
  }

  bufferlist shard_extent_map_t::get_ro_buffer() {
    return get_ro_buffer(ro_start, ro_end - ro_start);
  }

  std::string shard_extent_map_t::debug_string(uint64_t interval, uint64_t offset) const
  {
    std::stringstream str;
    str << "shard_extent_map_t: " << *this << " bufs: [";

    bool s_comma = false;
    for ( auto &&[shard, emap]: get_extent_maps()) {
      if (s_comma) str << ", ";
      s_comma = true;
      str << shard << ": [";

      bool comma = false;
      for ( auto &&extent: emap ) {
        bufferlist bl = extent.get_val();
        char *buf = bl.c_str();
        for (uint64_t i=0; i < extent.get_len(); i += interval) {
          int *seed = (int*)&buf[i + offset];
          if (comma) str << ", ";
          str << (i + extent.get_off()) << ":" << std::to_string(*seed);
          comma = true;
        }
      }
      str << "]";
    }
    str << "]";
    return str.str();
  }

  void shard_extent_map_t::erase_stripe(uint64_t offset, uint64_t length)
  {
    for ( auto iter = extent_maps.begin(); iter != extent_maps.end(); ) {
      auto && [shard, emap] = *iter;
      emap.erase(offset, length);
      if (emap.empty()) {
        iter = extent_maps.erase(iter);
      } else {
        ++iter;
      }
    }
    compute_ro_range();
  }

  bool shard_extent_map_t::contains(int shard) const
  {
    return extent_maps.contains(shard);
  }

  bool shard_extent_map_t::contains(optional<shard_extent_set_t> const &other) const
  {
    if (!other)
      return true;

    return contains(*other);
  }

  bool shard_extent_map_t::contains(shard_extent_set_t const &other) const
  {
    for ( auto &&[shard, other_eset]: other)
    {
      if (!extent_maps.contains(shard))
        return false;

      extent_set eset = extent_maps.at(shard).get_interval_set();
      if (!eset.contains(other_eset)) return false;
    }

    return true;
  }

  void shard_extent_set_t::subtract(const shard_extent_set_t &other) {
    for (auto && [shard, eset] : other) {
      if (!contains(shard) || !other.contains(shard))
        continue;

      at(shard).subtract(other.at(shard));
      if(at(shard).empty()) erase(shard);
    }
  }

  void shard_extent_set_t::insert(const shard_extent_set_t &other) {
    for (auto && [shard, eset] : other)
      map[shard].insert(other.at(shard));
  }

}

void ECUtil::HashInfo::append(uint64_t old_size,
			      map<int, bufferptr> &to_append) {
  ceph_assert(old_size == total_chunk_size);
  uint64_t size_to_append = to_append.begin()->second.length();
  if (has_chunk_hash()) {
    ceph_assert(to_append.size() == cumulative_shard_hashes.size());
    for (auto &&[shard, ptr] : to_append) {
      ceph_assert(size_to_append == ptr.length());
      ceph_assert(shard < static_cast<int>(cumulative_shard_hashes.size()));
      cumulative_shard_hashes[shard] =
        ceph_crc32c(cumulative_shard_hashes[shard],
          (unsigned char*)ptr.c_str(), ptr.length());
    }
  }
  total_chunk_size += size_to_append;
}

void ECUtil::HashInfo::encode(bufferlist &bl) const
{
  ENCODE_START(1, 1, bl);
  encode(total_chunk_size, bl);
  encode(cumulative_shard_hashes, bl);
  ENCODE_FINISH(bl);
}

void ECUtil::HashInfo::decode(bufferlist::const_iterator &bl)
{
  DECODE_START(1, bl);
  decode(total_chunk_size, bl);
  decode(cumulative_shard_hashes, bl);
  DECODE_FINISH(bl);
}

void ECUtil::HashInfo::dump(Formatter *f) const
{
  f->dump_unsigned("total_chunk_size", total_chunk_size);
  f->open_array_section("cumulative_shard_hashes");
  for (unsigned i = 0; i != cumulative_shard_hashes.size(); ++i) {
    f->open_object_section("hash");
    f->dump_unsigned("shard", i);
    f->dump_unsigned("hash", cumulative_shard_hashes[i]);
    f->close_section();
  }
  f->close_section();
}

namespace ECUtil {
std::ostream& operator<<(std::ostream& out, const HashInfo& hi)
{
  ostringstream hashes;
  for (auto hash: hi.cumulative_shard_hashes)
    hashes << " " << hex << hash;
  return out << "tcs=" << hi.total_chunk_size << hashes.str();
}
std::ostream& operator<<(std::ostream& out, const shard_extent_map_t& rhs)
{
  // sinfo not thought to be needed for debug, as it is constant.
  return out << "shard_extent_map: ({" << rhs.ro_start << "~"
    << rhs.ro_end << "}, maps=" << rhs.extent_maps << ")";
}

std::ostream& operator<<(std::ostream& out, const log_entry_t& rhs)
{
  switch(rhs.event) {
  case READ_REQUEST: out << "READ_REQUEST"; break;
  case ZERO_REQUEST: out << "ZERO_REQUEST"; break;
  case READ_DONE: out << "READ_DONE"; break;
  case ZERO_DONE: out << "ZERO_DONE"; break;
  case INJECT_EIO: out << "INJECT_EIO"; break;
  case CANCELLED: out << "CANCELLED"; break;
  case ERROR: out << "ERROR"; break;
  case REQUEST_MISSING: out << "REQUEST_MISSING"; break;
  case COMPLETE_ERROR: out << "COMPLETE_ERROR"; break;
  case ERROR_CLEAR: out << "ERROR_CLEAR"; break;
  case COMPLETE: out << "COMPLETE"; break;
  default:
    ceph_assert(false);
  }
  return out << "[" << rhs.shard << "]->" << rhs.io << "\n";
}
}

void ECUtil::HashInfo::generate_test_instances(list<HashInfo*>& o)
{
  o.push_back(new HashInfo(3));
  {
    bufferptr bp;
    bp.append_zeros(20);
    map<int, bufferptr> buffers;
    buffers[0] = bp;
    buffers[1] = bp;
    buffers[2] = bp;
    o.back()->append(0, buffers);
    o.back()->append(20, buffers);
  }
  o.push_back(new HashInfo(4));
}

const string HINFO_KEY = "hinfo_key";

bool ECUtil::is_hinfo_key_string(const string &key)
{
  return key == HINFO_KEY;
}

const string &ECUtil::get_hinfo_key()
{
  return HINFO_KEY;
}
