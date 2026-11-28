.. SPDX-License-Identifier: GPL-2.0-only

=============================
MediaTek MD1 modem (CCCI) ABI
=============================

The ECCCI driver suite (``drivers/misc/mediatek/eccci/``) drives the MD1
cellular modem found in the MediaTek MT6735/MT6737M, MT6750/MT6755 and
MT6797 SoCs.  The Android baseband stack (``md_init``, ``rild``,
``ccci_fsd``, ``mdlogger``, the META and audio HALs) talks to it through a
mix of character devices, network interfaces and ioctls.  The numeric
values below are the vendor ABI and must never change.

Character devices
=================

``/dev/ccci_fs``
    Modem file system server.  RX channel ``CCCI_FS_RX`` (14), TX channel
    ``CCCI_FS_TX`` (15).  Packets are raw CCCI frames: a 16 byte
    ``struct ccci_msg`` header followed by the payload.  ``read()``
    returns exactly one frame, ``write()`` accepts one or several
    concatenated frames (the length is ``data[1]`` of each header).

``/dev/ttyC0``
    AT command channel (RX 1, TX 1).  ``rild`` and ``mdlogger`` use it
    for the QMI/AT signalling.

``/dev/ttyC1``
    Multiplexed IMS/AT channel (RX 6, TX 6).

``/dev/ccci_monitor``
    Modem monitor: the AP feeds it modem state and receives the log bursts
    (RX 3, TX 3).

``/dev/ccci_aud``
    Voice speech IPC between the modem DSP and the audio HAL (RX 10, TX 10).

``/dev/ccci_ipc_<n>``
    IPC channels between the modem and other AP subsystems.

``/dev/ccci_ioctl0`` .. ``/dev/ccci_ioctl4``
    Modem wide control surface, see below.

Network interfaces
==================

``ccmni0`` .. ``ccmni7`` carry cellular IP bearers as point-to-point
devices (no Ethernet header).  Each bearer uses its own channel quartet:
RX ``20 + 4n``, RX ACK ``21 + 4n`` (flow control), TX ``22 + 4n`` and TX
ACK ``23 + 4n``.  The interfaces are only usable once the modem reached
the ``CCCI_STATE_READY`` state; ``ifconfig ccmni0 up`` fails with
``ENODEV`` before that.

ioctls
======

All ioctls use the ``CCCI_IOC_MAGIC`` (``'C'``) except the IPC ones
(``CCCI_IPC_MAGIC``, ``'P'``); the generic definitions live in
``include/uapi/linux/mtk_ccci.h``.

Modem life cycle
----------------

``CCCI_IOC_POWER_ON_MD`` / ``CCCI_IOC_POWER_ON_MD_REQUEST``
    ``md_init`` asks for the modem to be powered; the firmware image is
    loaded and the MD released from reset.  Returns 0 when the boot has
    been queued.

``CCCI_IOC_POWER_OFF_MD`` / ``CCCI_IOC_POWER_OFF_MD_REQUEST``
    Power the modem off and hold it in reset.

``CCCI_IOC_MD_RESET`` / ``CCCI_IOC_DO_MD_RST``
    Assert the modem reset; the firmware is reloaded.

``CCCI_IOC_FORCE_MD_ASSERT``
    Force an assert, used by ``mdlogger`` when the modem stops responding.

``CCCI_IOC_ENTER_DEEP_FLIGHT`` / ``CCCI_IOC_LEAVE_DEEP_FLIGHT``
    Deep flight mode.  ``ENTER`` is only accepted when the modem is not
    ready.

State and information
---------------------

``CCCI_IOC_GET_MD_STATE``
    Current ``enum ccci_md_state``.

``CCCI_IOC_GET_MD_INFO``
    ``(boot_stage << 8) | state``.

``CCCI_IOC_GET_MD_TYPE`` / ``CCCI_IOC_GET_MD_EX_TYPE``
    Baseband type; the World phone LTE modem is 5.

``CCCI_IOC_GET_MD_MEM_SIZE``
    Size of the modem shared memory window.

Shared memory
-------------

``CCCI_IOC_SMEM_BASE`` / ``CCCI_IOC_SMEM_LEN``
    Physical address and length of the ``modem-share`` reserved memory, so
    ``ccci_fsd`` can ``mmap()`` it via ``/dev/mem`` and skip the CLDMA
    round trip.  Both return 0 when the board has no reserved window, in
    which case the daemon falls back to ``/dev/ccci_fs``.

``CCCI_IOC_SMEM_TX_NOTIFY`` / ``CCCI_IOC_SMEM_SET_STATE``
    Polled shared memory protocol helpers; the current ``ccci_fsd`` does
    not use them and the driver answers ``-EOPNOTSUPP``.

IPC
---

``CCCI_IPC_WAIT_MD_READY``
    Returns 0 when the modem is ready, ``-EAGAIN`` otherwise.

``CCCI_IPC_RESET_RECV`` / ``CCCI_IPC_RESET_SEND``
    Flush the port receive queue.

Packet framing
==============

Every frame starts with a 16 byte header::

	struct ccci_msg {
		__le32 data[2];		/* data[1] is the total frame length */
		__le16 channel;
		__le16 seq_num;
		__le32 reserved;	/* the modem stores the payload length */
	};

The driver validates the channel and length on every write() before the
frame reaches CLDMA; malformed frames are rejected with ``-EINVAL``.
