.. _radosgw-pools:

=====
Pools
=====

The Ceph Object Gateway uses several pools for its various storage needs,
which are listed in the Zone object (see ``radosgw-admin zone get``). A
single zone named ``default`` is created automatically with pool names
starting with ``default.rgw.``, but a :ref:`Multisite Configuration <multisite>`
will have multiple zones.

Tuning
======

When ``radosgw`` first tries to operate on a zone pool that does not exist, it
will create that pool with the default values from ``osd pool default pg num``
and ``osd pool default pgp num``. These defaults are sufficient for some pools,
but others (especially those listed in ``placement_pools`` for the bucket index
and data) will require additional tuning. See :ref:`rados_pools` for details on
pool creation.

Erasure-Coded Data Pools
------------------------

A data pool may be erasure coded. With :ref:`dynamic chunk sizes
<rados_ops_erasure_coding_dynamic_chunk_size>` enabled on the pool, each RADOS
object that ``radosgw`` writes gets a chunk size that fits it in a single
stripe, while the pool keeps its small stripe unit for small objects:

.. prompt:: bash $

   ceph osd pool set default.rgw.buckets.data allow_ec_optimizations true
   ceph osd pool set default.rgw.buckets.data allow_ec_dynamic_chunk_size true --yes-i-really-mean-it

``radosgw`` passes the size of each RADOS object with its reads, so if
:confval:`rados_replica_read_policy` is set to ``balance`` for ``radosgw``,
reads go directly to the OSDs that hold the data.

.. _radosgw-pool-namespaces:

Pool Namespaces
===============

Pool names particular to a zone follow the naming convention
``{zone-name}.pool-name``. For example, a zone named ``us-east`` will
have the following pools:

-  ``.rgw.root``

-  ``us-east.rgw.control``

-  ``us-east.rgw.meta``

-  ``us-east.rgw.log``

-  ``us-east.rgw.buckets.index``

-  ``us-east.rgw.buckets.data``

The zone definitions list several more pools than that, but many of those
are consolidated through the use of rados namespaces. For example, all of
the following pool entries use namespaces of the ``us-east.rgw.meta`` pool::

    "user_keys_pool": "us-east.rgw.meta:users.keys",
    "user_email_pool": "us-east.rgw.meta:users.email",
    "user_swift_pool": "us-east.rgw.meta:users.swift",
    "user_uid_pool": "us-east.rgw.meta:users.uid",

