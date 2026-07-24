.. This document is inspired by the Linux kernel's equivalent policy at
   Documentation/process/coding-assistants.rst (GPL-2.0). The content
   has been independently written for Ceph.

.. _ai_coding_assistants:

=======================
AI Coding Assistants
=======================

This document provides guidance for AI tools and developers using AI assistance
when contributing to Ceph.

AI tools helping with Ceph development should follow the standard Ceph
development process:

- :doc:`Contributing to Ceph: A Guide for Developers <index>`
- :doc:`Basic Workflow <basic-workflow>`
- `Submitting Patches to Ceph <https://github.com/ceph/ceph/blob/main/SubmittingPatches.rst>`_

Coding Style
------------

All contributions must follow the `Ceph Coding Style
<https://github.com/ceph/ceph/blob/main/CodingStyle>`_ guide (``CodingStyle``
in the root of the repository).

Licensing and Legal Requirements
---------------------------------

All contributions must comply with Ceph's licensing requirements.

Unless stated otherwise, the Ceph source code is distributed under the terms of
the LGPL-2.1 or LGPL-3.0. For full details, see the file `COPYING`_ in the
top-level directory of the source-code tree.

.. _`COPYING`: https://github.com/ceph/ceph/blob/main/COPYING

Signed-off-by and Developer Certificate of Origin
--------------------------------------------------

AI agents **MUST NOT** add ``Signed-off-by`` tags. Only humans can legally
certify the Developer Certificate of Origin (DCO). The human submitter is
responsible for:

- Reviewing all AI-generated code
- Ensuring compliance with licensing requirements
- Adding their own ``Signed-off-by`` tag to certify the DCO
- Taking full responsibility for the contribution

By adding a ``Signed-off-by`` tag, the submitter also certifies that they
have disclosed all AI assistance used in producing the contribution, as
required by this document.

See `Sign your work <https://github.com/ceph/ceph/blob/main/SubmittingPatches.rst#sign-your-work>`_
in ``SubmittingPatches.rst`` (in the root of the source tree) for the full DCO text and
instructions.

Attribution
-----------

When AI tools contribute to Ceph development, proper attribution helps track
the evolving role of AI in the development process. Contributions should
include an ``Assisted-by`` tag in the following format (items in square
brackets are optional)::

    Assisted-by: AGENT_NAME[:MODEL_VERSION] [TOOL1] [TOOL2]

Where:

- ``AGENT_NAME`` is the name of the AI tool or framework
- ``MODEL_VERSION`` is the specific model version used; optional but highly
  encouraged when it can be determined reliably
- ``[TOOL1] [TOOL2]`` are optional specialised analysis tools used
  (e.g., ``clang-tidy``, ``clang-format``, ``cppcheck``, ``mypy``)

Basic development tools (git, gcc, make, editors) should not be listed.

Examples::

    Assisted-by: Claude:claude-sonnet-4-5 clang-tidy
    Assisted-by: AcmeCorp-ModelSwitchingAgent-v2

