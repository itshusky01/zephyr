.. _ipc_service_static_vrings:

OpenAMP static VRINGs backend
#############################

The ``zephyr,ipc-openamp-static-vrings`` backend implements the IPC service API
on top of OpenAMP RPMsg and two statically allocated VRINGs. Each instance uses
one mailbox channel for notifications and one shared memory region for the
virtio status area, VRINGs, and RPMsg buffers.

The host and remote sides must use the same VRING layout, buffer size,
alignment, and descriptor count.

Derived layout
==============

By default, the backend derives the layout from the size of the
``memory-region`` and the ``zephyr,buffer-size`` property. The generated layout
places the virtio status area at the start of the region and derives the number
of descriptors from the remaining space.

This layout is suitable when both sides use this Zephyr backend. It is not
compatible with firmware that has a pre-existing OpenAMP layout or resource
table.

Fixed layout
============

The backend can instead use an explicit layout for interoperability with
existing remote firmware. Provide all of the following properties on the IPC
instance:

``zephyr,tx-vring-offset``
    Offset of the TX VRING from the start of ``memory-region``.

``zephyr,rx-vring-offset``
    Offset of the RX VRING from the start of ``memory-region``.

``zephyr,buffer-pool-offset``
    Offset of the RPMsg buffer pool from the start of ``memory-region``.

``zephyr,buffer-pool-size``
    Size of the RPMsg buffer pool in bytes.

``zephyr,num-descriptors``
    Number of descriptors in each VRING.

Example:

.. code-block:: devicetree

   ipc0: ipc0 {
           compatible = "zephyr,ipc-openamp-static-vrings";
           memory-region = <&ipc_shm>;
           mboxes = <&mbox0>, <&mbox0>;
           mbox-names = "tx", "rx";
           role = "host";
           zephyr,buffer-size = <512>;
           zephyr,tx-vring-offset = <0x1000>;
           zephyr,rx-vring-offset = <0x2000>;
           zephyr,buffer-pool-offset = <0x10000>;
           zephyr,buffer-pool-size = <0x70000>;
           zephyr,num-descriptors = <16>;
   };

The virtio status byte is always located at the start of the selected
``memory-region``.

Constraints
===========

The fixed layout is validated when the instance is opened:

* All fixed-layout properties must be provided together.
* ``zephyr,num-descriptors`` must be a nonzero power of two and fit in
  ``uint16_t``.
* The shared-memory base, TX and RX VRING offsets, buffer-pool offset, and
  buffer-pool size must be aligned to
  :kconfig:option:`CONFIG_IPC_SERVICE_STATIC_VRINGS_MEM_ALIGNMENT`.
* The status area, TX VRING, RX VRING, and buffer pool must fit completely
  inside ``memory-region``.
* The status area, TX VRING, RX VRING, and buffer pool must not overlap.
* The buffer pool must contain at least ``zephyr,num-descriptors`` buffers of
  ``zephyr,buffer-size`` bytes.

The alignment must match the remote OpenAMP implementation. Shared memory
should be mapped consistently on both sides, for example as non-cacheable
memory when the two cores do not maintain hardware cache coherency.
