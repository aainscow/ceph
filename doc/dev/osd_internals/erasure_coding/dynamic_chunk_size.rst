===================================
Erasure Coding Dynamic Chunk Size
===================================

Summary
=======

An erasure coded pool has a single stripe unit (the *chunk size*), chosen
when the pool is created. Small chunks suit small random I/O; large chunks
suit large objects. A pool that serves both has to pick one.

*Dynamic chunk size* lets an optimized EC pool pick the chunk size per
object. The primary chooses it when the object first receives data, records
it in the object's ``object_info_t`` and uses it for every later operation
on that object. Objects that do not benefit keep the pool's stripe unit.

The feature targets RGW. RGW stores objects as RADOS objects of up to 4 MiB
(``rgw_obj_stripe_size``). With a 4 KiB stripe unit and ``k = 4`` such an
object spans 256 stripes; with a 1 MiB chunk it is one stripe. That removes
the per-stripe overhead on write, lets a whole-object read be served by a
single direct read per data shard, and keeps the pool's small stripe unit
available for small objects.

Terminology
===========

*Default chunk size*
   ``stripe_width / k`` of the pool. Before this feature it was the only
   chunk size.

*Object chunk size*
   The value stored in ``object_info_t::ec_chunk_size``. Zero means "the
   default chunk size".

*Effective chunk size*
   The object chunk size if non-zero, otherwise the default chunk size.
   All geometry for an object uses its effective chunk size.

*Object stripe width*
   ``k`` × the effective chunk size.

Goals
=====

#. Pools without the flag, and objects that keep the default, behave
   exactly as before, bit for bit on disk and on the wire.
#. An object's layout never changes while it holds data.
#. Every component that maps object offsets to shard offsets (write,
   read, direct read, recovery, backfill, scrub, PG log rollback) uses the
   object's own geometry.
#. Clients that do not know an object's chunk size can never read wrong
   data.
#. RGW uses the feature without configuration beyond enabling the flag and
   balanced reads.
#. Recovery of an object does not depend on knowing its size or chunk size
   in advance.

Non-goals
=========

* Changing the chunk size of an object that already holds data.
* Legacy (non-optimized) EC.
* Crimson. The monitor refuses the flag on Crimson pools.
* Plugins that require sub-chunks (CLAY). No sub-chunk plugin supports
  optimized EC, so these pools cannot enable the flag.

Configuration
=============

Two pool properties control the feature::

    ceph osd pool set <pool> allow_ec_dynamic_chunk_size true --yes-i-really-mean-it
    ceph osd pool set <pool> ec_dynamic_chunk_size_max 1M

``allow_ec_dynamic_chunk_size`` (flag ``FLAG_EC_DYNAMIC_CHUNK_SIZE``, shown
as ``ec_dynamic_chunk_size`` in the pool's flags)
   Enables the feature. It can be set but not cleared: once objects with
   a non-default chunk size exist, clearing it would not change how they
   must be read, so the flag only records that such objects may exist.
   The monitor accepts it only if:

   * the pool is erasure coded with ``allow_ec_optimizations`` set,
   * the pool is not a Crimson pool,
   * the plugin does not pad chunk sizes that are multiples of 4 KiB,
   * the command has ``--yes-i-really-mean-it``.

   The last check asks the plugin for the chunk size of stripes of
   ``k × (default + 4 KiB)`` and ``k × (default + 8 KiB)``. A plugin that
   returns both unchanged has an alignment that divides 4 KiB, so it accepts
   every chunk size the pool can choose. Asking about sizes above the default
   keeps the check within what the plugin has already accepted for the pool.

   The feature is experimental and not tied to a release: nothing stops
   an OSD that does not support it from serving the pool, and such an OSD
   would read and write objects with their own chunk size in the pool's
   default geometry. The confirmation makes the administrator take
   responsibility for every OSD serving the pool supporting it.

   The development option ``osd_pool_default_flag_ec_dynamic_chunk_size``
   sets the flag, without confirmation, on new pools that have
   optimizations enabled, and silently skips pools that cannot have it,
   like ``osd_pool_default_flag_ec_optimizations``. The QA suites use it.

``ec_dynamic_chunk_size_max`` (pool option, bytes, default 1 MiB)
   The largest chunk size the primary may choose. It must be a multiple of
   4 KiB, at least the default chunk size and at most 4 MiB, and needs
   ``allow_ec_optimizations``. Setting it to 0 restores the default. It may
   be changed at any time and the change affects only objects created
   afterwards, but clients compute the geometry of a direct read with the
   current value: after a change, direct reads of older objects whose chunk
   size was limited by the old value go to the primary. It is best set
   before the pool holds data.

Existing objects are not modified when the flag is set. They keep an
object chunk size of zero, which is the default chunk size.

Chunk size selection
====================

Policy
------

Given a size hint ``S`` (bytes) the chunk size is::

    per_shard = ceil(S / k)
    cs        = round_up(per_shard, 4 KiB)
    cs        = clamp(cs, default_chunk_size, ec_dynamic_chunk_size_max)

``S = 0`` gives the default. The result is stored as zero when it equals
the default, so an object that does not benefit has an object_info
identical to one written before the feature existed.

The function is ``pg_pool_t::get_ec_chunk_size_for_object_size()``. It
lives on ``pg_pool_t`` because the client needs the same answer (see
`Direct and split reads`_) and the OSDMap is all the two sides share.

Why round to 4 KiB, not to a power of two
-----------------------------------------

For an object that fits in one stripe, the parity shards are as large as
data shard 0, which is ``min(S, cs)``. Space used is therefore
``S + m × min(S, cs)``. Rounding ``cs`` up to a power of two can double
the parity; rounding to 4 KiB wastes at most 4 KiB per shard. Example for
``k = 4, m = 2`` and a 2.1 MiB object:

==================  ===========  ==============  ============
Rounding            Chunk size   Parity (total)  Space / S
==================  ===========  ==============  ============
4 KiB multiple      540 KiB      1080 KiB        1.50
Power of two        1024 KiB     2048 KiB        1.95
==================  ===========  ==============  ============

4 KiB is the optimized EC alignment (``EC_ALIGN_SIZE``); all geometry
helpers use division and modulo, never masks, so any multiple of it works.

Inputs
------

The size hint comes from, in order:

#. ``object_info_t::expected_object_size`` as it stands after the
   operation, which a ``SETALLOCHINT`` op in the same or an earlier
   operation sets;
#. the object's size after the operation.

The second input means an object created by a single write gets a chunk
size that fits it exactly. RGW writes each RADOS object in one operation
in its default configuration, so it needs no hint (see `RGW`_).

When the choice is made
-----------------------

``PrimaryLogPG::finish_ctx()`` chooses the chunk size, before it encodes
the object_info attribute, when all of the following hold:

* the pool has ``FLAG_EC_DYNAMIC_CHUNK_SIZE``;
* the object exists after the operation;
* its object chunk size is zero;
* it holds no data from before the operation: it did not exist, its size
  was zero, or the operation removed it first;
* nothing earlier in the operation set the chunk size explicitly (clone
  rollback and copy-from do, see below).

Because ``finish_ctx()`` applies the new object state to the object context
before the transaction is submitted, a second write pipelined behind the
first sees the chosen value.

An object keeps its chunk size until it is deleted. Deleting an object
resets its chunk size, so a later write chooses again. An operation that
removes the object and writes it again (``remove`` followed by
``write_full``, which is how RGW overwrites a head object) chooses again as
well: the removal discards the old shards, and a PG log rollback of such an
operation restores the whole old object. A ``write_full`` or a truncate to
zero without a removal keeps the chunk size, because rolling it back
restores extents of the old object in its own geometry.

``finish_ctx()`` asserts the invariant: if the object held data before the
operation and the operation did not remove it first, the object chunk
size is unchanged.

Persistent state
================

``object_info_t``
   New field ``uint64_t ec_chunk_size``, encoding version 19 (compat 8).
   Older decoders skip it. It is copied to clones by
   ``object_info_t::copy_user_bits()``, because a clone's data is a byte
   copy of the head's shards.

``pg_pool_t``
   ``FLAG_EC_DYNAMIC_CHUNK_SIZE`` (bit 23), and pool option
   ``EC_DYNAMIC_CHUNK_SIZE_MAX``.

PG log entries
   Unchanged. Rollback takes the chunk size from the object_info that each
   entry already records. See `PG log rollback`_.

Nothing changes in the on-shard data format. A shard of an object with
a 1 MiB chunk is laid out exactly as it would be in a pool whose stripe
unit is 1 MiB.

Geometry in the OSD
===================

``ECUtil::stripe_info_t`` used to hold both the pool's invariant state
(k, m, shard mapping, plugin flags) and the size-dependent geometry. It is
split in two:

``stripe_info_base_t``
   The invariant state and the default chunk size. It has no
   offset-to-shard helpers. The long-lived ``sinfo`` members of
   ``ECBackend``, ``ReadPipeline``, ``RMWPipeline``, ``RecoveryBackend``
   and ``ECExtentCache`` are of this type.

``stripe_info_t``
   A small value type: a pointer to the base plus a chunk size. It carries
   every geometry helper. It is obtained with
   ``base.for_chunk_size(cs)``, ``base.for_default()`` or
   ``base.for_object_chunk_size(oi_value)`` (zero means default).

``shard_extent_map_t`` holds its view by value.

Because the base has no geometry helpers, code that needs geometry cannot
compile without choosing a chunk size, so no call site can silently use
the pool default.

Write path
==========

``ECCommon::get_write_plan()`` takes the chunk size of each object in the
transaction from the object context (``obc->obs.oi``), which
``finish_ctx()`` has already updated. The plan
(``ECTransaction::WritePlanObj::chunk_size``) and
``ECTransaction::Generate`` build their view from it. The EC backend never
writes the object chunk size; it only reads it.

Clones and snapshots
--------------------

``make_writeable()`` creates the clone's object_info with
``copy_user_bits()``, which now includes ``ec_chunk_size``. The clone is a
byte copy of the head's shards, and its write plan takes the chunk size from
the clone's own object context, so both agree.

Rollback to a snapshot
----------------------

``_do_rollback_to()`` removes the head and clones the snapshot onto it.
It sets the head's chunk size to the snapshot's and marks the operation
as having set the chunk size explicitly.

Copy-from
---------

The primary chooses the destination's chunk size from the source object
size reported in the first ``copy_get`` reply, as if the object were
created by one write of that size. A copy that fits in one reply is written
straight to the destination. A larger copy is written to a temporary object
in several operations and renamed onto the destination at the end; the
temporary object's context carries the chosen chunk size (temporary
objects have no persistent object_info), and ``finish_copyfrom()`` gives
the destination the same value. The destination is removed first, so it
may change chunk size. Promotion into a cache tier uses the same path.

Extent cache
------------

The extent cache holds one entry per object. Each cache operation carries
the chunk size of its write plan; an entry has the chunk size of the data
it caches.

The chunk size of an object in the cache can change only when the object
holds no data (a first write after a create or after a delete) or when an
operation removes the object first. In both cases ``get_write_plan()``
marks the operation as invalidating the cache. When the extent cache
performs the invalidation it switches the entry, and its cache lines, to
the operation's chunk size. Lines that an idle object left in the LRU
with its previous chunk size are discarded when an operation next pins
them.

Read path on the primary
========================

Every read issued by the primary passes the object's chunk size:
``objects_read_async()`` (normal client reads and RMW), synchronous reads
(``objects_read_sync()`` for coroutine reads, ``copy_get``, CDC), sparse
reads (``extent_to_shard_extent()`` and ``objects_readv_sync()``). The read
pipeline carries it in ``read_request_t::chunk_size`` and builds a view per
request, including when it assembles sub-read replies.

Direct and split reads
======================

With ``FLAG_CLIENT_SPLIT_READS`` the Objecter sends reads directly to data
shards: either the whole op to the one shard that holds the range
(*single direct read*), or one sub-read per shard plus a reference sub-read
on the primary (*split read*). Both need the object's geometry on the
client, which only the OSD knows.

Client side
-----------

A read operation may carry an *object size hint*: the value that was used
to choose the chunk size, i.e. the object's ``expected_object_size`` if it
was created with one, otherwise its size after the first write. For an
object written in a single operation that is simply its size.

* librados: ``ObjectReadOperation::set_object_size_hint(uint64_t)`` and
  ``rados_read_op_set_object_size_hint()``;
* neorados: ``ReadOp::object_size_hint(uint64_t)``.

The hint is stored in ``ObjectOperation`` and copied to
``Objecter::Op::object_size_hint``. ``SplitOp::create()`` then:

* on pools without the flag, uses the default chunk size, as today;
* on pools with the flag and no hint, does not split or redirect the read:
  it goes to the primary;
* otherwise computes the chunk size with
  ``pg_pool_t::get_ec_chunk_size_for_object_size()`` and uses it for shard
  selection, sub-read planning and reassembly.

On split reads the client declares the chunk size it used: the
``GET_INTERNAL_VERSIONS`` op that every sub-read already carries (to detect
torn reads) gets the chunk size as its input data.

OSD side
--------

Before executing an op flagged ``CEPH_OSD_FLAG_EC_DIRECT_READ`` on a pool
with the flag, ``PrimaryLogPG::do_op()`` checks the op against the
object's effective chunk size, once the object context is loaded:

* if the op has a ``GET_INTERNAL_VERSIONS`` op (a split sub-read), the
  declared chunk size, or the default if there is none, must equal the
  effective chunk size;
* otherwise (a single direct read) every read extent must lie within one
  chunk held by this shard.

On failure the OSD replies ``-EAGAIN``. For single direct reads the
Objecter already redrives ``-EAGAIN`` to the primary; for split reads the
sub-read carries ``FAIL_ON_EAGAIN``, which fails the split op and makes the
Objecter resubmit the original op to the primary. The check makes a wrong
or stale hint, and a client that knows nothing of the feature, cost one
extra round trip instead of returning wrong data.

``objects_read_local()`` maps the extent with the object's chunk size.

Alternatives considered
-----------------------

*Return the chunk size in* ``MOSDOpReply``. Lets the client check after the
fact, but needs a message version bump and still leaves clients without the
check. Declaring the geometry in the request lets the OSD enforce it.

*Speculative split with the default chunk size when there is no hint.* For
RBD-style objects the guess is almost always wrong, so every read would pay
for a failed attempt. Without a hint, reading from the primary is never
slower than today.

*A per-client cache of learned chunk sizes.* Not needed by RGW, which always
knows the size; it can be added later without protocol changes.

Recovery and backfill
=====================

Recovery used to progress in object offsets: each pass recovered
``osd_recovery_max_chunk`` bytes of the object, which needs the object's
geometry to find the shard ranges. When the primary is itself missing the
object, it has no object_info until the first read returns one, so the
first pass had to guess the object's size.

Recovery now progresses in *shard* offsets, for every optimized EC pool.
``ObjectRecoveryProgress::data_recovered_to`` is a shard offset. Each pass
recovers the same range ``[start, start + osd_recovery_max_chunk / k)`` of
every missing shard, and reads the same range from the shards it decodes
from. Encoding works stripe by stripe, and every stripe occupies the same
shard offsets on every shard whatever the chunk size, so a shard range can
always be decoded from the same range of other shards.

* With an object context, the pass is clipped to the size of each missing
  shard, computed from the object's size and chunk size. Recovery ends when
  the progress passes the largest missing shard.
* Without one (the primary is missing the object) the first pass reads the
  whole range from every shard it needs. Shards that are shorter return
  short reads; once the attributes arrive the read switches to the object's
  geometry, treats the missing tails as zeros for decoding and trims what
  it wants to what the missing shards hold. Later passes have the object
  context.

The amount of data a pass moves is the same as before: ``k`` shards of
``osd_recovery_max_chunk / k`` bytes. Pushes carry the object_info, and the
target truncates each shard to the size computed with the object's chunk
size.

Backfill uses the same path with an object context.

The backfill space estimate still allows one default chunk of padding per
object. Objects whose chunk size was chosen from an allocation hint larger
than their size can use more space than that on data shard 0 and the
parity shards.

Scrub
=====

Shallow scrub compares each shard's size with the size computed from the
authoritative object_info, using its chunk size. Deep scrub, on plugins that
support CRC encode/decode, checks parity by decoding whole-shard CRCs, which
does not depend on the chunk size; the zero-fill CRC correction uses the
object's shard size.

The deep scrub read stride is unrelated to the chunk size: it is a running
CRC over the whole shard, so any stride gives the same digest. It keeps its
current alignment.

PG log rollback
===============

Rolling back a divergent append truncates each shard to the size the
object had before the write, and rolling back an overwrite restores
extents that are clipped to that size. Both computations need the chunk
size the object had before the write.

Every log entry that can roll back an append or an overwrite also records,
in a ``SETATTRS`` record, the attributes the write replaced, so that they
can be restored. The object_info is always among them, because every
write updates it, and ``ECTransaction`` takes the recorded value from the
object context's attribute cache, which holds the object's state just
before the write. ``PGBackend::rollback()`` takes the chunk size from that
old object_info (``PGBackend::get_rollback_ec_chunk_size()``, on pools with
the flag) and uses it for every shard size it computes. The ``SETATTRS``
record comes after the ``APPEND`` and ``ROLLBACK_EXTENTS`` records in the
entry, so the entry is scanned for it first.

An entry records no old object_info only for a write that created the
object or removed it first. Such an entry records no append or overwrite
either: it is rolled back by removing the object or by restoring it from
its stash.

The object_info stored on the shard would not do: it describes the newest
state rather than the state before the entry being rolled back, which
differs when several divergent entries span a removal, and a shard that a
partial write skipped keeps an older one.

RGW
===

Writes
------

RGW writes the data of a head object with its metadata in one operation,
and tail stripes and multipart part stripes through ``RadosWriter``. With
the default ``rgw_max_chunk_size`` and ``rgw_obj_stripe_size`` (both 4 MiB)
every RADOS object receives all its data in one write, so the OSD sizes the
chunk from the object's final size without any hint.

If ``rgw_obj_stripe_size`` is larger than the write chunk
(``rgw_max_chunk_size``), a stripe may need several writes. The processor
then tells ``RadosWriter`` the stripe's maximum size when it moves to the
stripe, and every write of the stripe carries it as ``expected_object_size``
in the allocation hint RGW already sends. The last stripe of an object, or
of a multipart part, can end up smaller than that hint.

An overwrite of an existing key removes the head object and writes it again
in one operation, so the new head gets a chunk size for its new size.

Reads
-----

``RGWRados::iterate_obj()`` passes the size of each RADOS object to its
callback: for a manifest stripe, the stripe's size as the manifest
iterator reports it, limited to the end of the object (the iterator keeps
the full stripe size when it moves to the last stripe of an object without
parts); for an object without a manifest, the object size. The read
callbacks (``get_obj_iterate_cb()`` and the D3N cache variant) and
``RGWRados::Object::Read::read()`` set it as the object size hint.

A hint can disagree with the chunk size the OSD chose: an object written
by appends, the last stripe of an object or part in the multi-write
configuration, an object written before the pool had the flag, or an
object written by another client. The OSD check turns these into a read
from the primary.

Configuration
-------------

Direct and split reads are used only for ops with balanced reads, so the
RGW client needs ``rados_replica_read_policy = balance``. The data pool
needs ``allow_ec_optimizations`` and ``allow_ec_dynamic_chunk_size``.

Compatibility and upgrade
=========================

* No ``require_osd_release`` protects a pool with the flag from OSDs that
  do not support it; setting the flag needs ``--yes-i-really-mean-it``
  (see `Configuration`_).
* ``object_info_t`` is always encoded at version 19. OSDs without the
  feature decode it and ignore the field, which is always zero in pools
  without the flag.
* PG log entries do not change.
* ``GET_INTERNAL_VERSIONS`` ignores empty input data, which is what
  clients without the feature send. The OSD takes a split sub-read that
  declares no chunk size as planned with the default chunk size, so such
  clients are redirected to the primary for objects with their own chunk
  size.
* The flag cannot be cleared, so a pool cannot be downgraded to a release
  that does not understand it.

Testing
=======

Unit tests
----------

* ``unittest_osd_types``: selection policy (k, hints, clamps, disabled
  pool); ``object_info_t`` v18/v19 decode; clone copy; pool flag and option.
* ``unittest_ecutil``: the default view equals the old geometry;
  geometry, shard sizes and ro/shard round trips for non-default chunks;
  ``shard_extent_map_t`` contents and read masks in an object's geometry,
  and switching a recovery read result to it; the direct read geometry
  check.
* ``unittest_ec_transaction``: write plans for new objects, partial
  overwrites, appends and truncates at non-default chunk sizes; the chunk
  size rollback takes from a log entry's old object_info.
* ``unittest_extent_cache``: per-object chunk sizes; reads with the
  object's chunk size; an in-flight operation when the first data write of
  an empty object chooses a larger chunk size; an idle object whose cached
  lines have its previous chunk size.
* ``unittest_ecbackend``: reads planned with the object's chunk size.

End-to-end tests on the peering fixture
---------------------------------------

``ECPeeringTestFixture`` runs real ``ECBackend`` and ``PeeringState``
instances for every shard over an in-memory store. Its write helpers give
objects the chunk size ``finish_ctx()`` would, and it can give new objects
an expected size, as an allocation hint would.

* ``unittest_ec_dynamic_chunk_size``: for several plugins, ``k``, ``m`` and
  stripe units, the chunk size and shard sizes of objects of many sizes;
  allocation hints; overwrites, appends and truncates; an object removed
  and written again in one transaction; snapshot rollback, also after the
  head is rewritten with another chunk size;
  recovery of a data shard, a parity shard and the primary (without an
  object context), including in passes smaller than a chunk; rollback of
  divergent appends; scrub; direct reads.
* ``unittest_ecfailover_with_peering``: recovery of objects that take
  several passes, for a data shard, a parity shard and the primary; the
  whole failover suite also runs on pools with the flag, with and without
  an expected object size.

Client tests
------------

* librados ``LibRadosSplitOpECDynamicPP``, with and without balanced
  reads: reads with correct and missing object size hints check the data
  and whether the read was split or sent to the primary; objects removed
  and written again, in one operation or two, get the chunk size of their
  new contents; reads with wrong hints return the right data.
* librados ``CReadOpsTest.ObjectSizeHint`` and neorados
  ``NeoRadosReadOps.ObjectSizeHint``: the API on other pools.

Integration tests
-----------------

* ``qa/standalone/erasure-code/test-erasure-code-dynamic-chunk-size.sh``:
  pool settings, including the confirmation; chunk sizes chosen for
  objects of several sizes and with alloc hints; objects created before
  the flag; overwrite, append,
  truncate; deleted and recreated objects; snapshots and rollback,
  including after the head is recreated; copy-from larger than one
  ``copy_get`` chunk; recovery of a peer and of the primary; backfill; deep
  scrub.
* ``rados/thrash-erasure-code``, ``-overwrites`` and ``-isa``: an
  ``ec_optimizations`` fragment that creates pools with the flag.
* ``rgw/verify``: an EC data pool with the flag and balanced reads.
