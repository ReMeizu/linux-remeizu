/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek MD1 cellular modem (ECCCI) - private definitions.
 *
 * Shared between the ECCCI sources only. The userspace ABI lives in
 * <uapi/linux/mtk_ccci.h>, the channel numbers and wire format in
 * ccci_channel.h, and the public kernel interface in <linux/mtk_ccci.h>.
 */
#ifndef __CCCI_PRIV_H__
#define __CCCI_PRIV_H__

#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <linux/mtk_ccci.h>
#include "ccci_channel.h"

#define ccci_err(md, fmt, ...)	dev_err((md)->dev, fmt, ##__VA_ARGS__)
#define ccci_warn(md, fmt, ...)	dev_warn((md)->dev, fmt, ##__VA_ARGS__)
#define ccci_info(md, fmt, ...)	dev_info((md)->dev, fmt, ##__VA_ARGS__)
#define ccci_dbg(md, fmt, ...)	dev_dbg((md)->dev, fmt, ##__VA_ARGS__)

/* ---------------------------------------------------------------------- */
/* Register windows and interrupts, in the fixed device tree order        */
/* ---------------------------------------------------------------------- */
enum ccci_reg_index {
	CCCI_REG_CLDMA_AP_AO = 0,
	CCCI_REG_CLDMA_MD_AO,
	CCCI_REG_CLDMA_AP_PDN,
	CCCI_REG_CLDMA_MD_PDN,
	CCCI_REG_CCIF_AP,
	CCCI_REG_CCIF_MD,
	CCCI_REG_MAX,
};

enum ccci_irq_index {
	CCCI_IRQ_CLDMA = 0,
	CCCI_IRQ_CCIF,
	CCCI_IRQ_MDWDT,
	CCCI_IRQ_MAX,
};

/* CLDMA ring kinds, also the index into ccci_cldma::q[] */
enum ccci_cldma_queue_kind {
	CCCI_CLDMA_AP_TX = 0,	/* AP -> MD GPD ring, the AP produces */
	CCCI_CLDMA_AP_RX = 1,	/* MD -> AP GPD ring, the AP consumes */
	CCCI_CLDMA_QUEUE_KINDS,
};

/*
 * CLDMA queue -> logical channel mapping. Queues 0..5 carry the non network
 * channels (control, system, FS, AT/META, AT2/MUX, log), queues 6 and 7
 * carry CCMNI frames, exactly like the vendor driver splits them.
 */
#define CCCI_CLDMA_QUEUE_NUM		8
#define CCCI_CLDMA_QUEUE_GPD		64
#define CCCI_CLDMA_NET_FIRST_Q		6
#define CCCI_CLDMA_NET_NUM_Q		2
#define CCCI_CLDMA_BUDGET		64
#define CCCI_CLDMA_PKT_BUDGET		128
#define CCCI_CLDMA_TX_TIMEOUT		(2 * HZ)
#define CCCI_CLDMA_TX_CHUNK		0x40

/* Modem boot handshake stages */
#define CCCI_MD_BOOT_STAGE_0		0
#define CCCI_MD_BOOT_STAGE_1		1
#define CCCI_MD_BOOT_STAGE_2		2
#define CCCI_MD_BOOT_STAGE_3		3

/* Modem watchdog: MD must say hello within these bounds after boot. */
#define CCCI_MD_BOOT_TIMEOUT_MS		20000
#define CCCI_MD_WDT_POLL_MS		1000

/* Channels whose open() keeps the modem from being reset. */
#define CCCI_CRITICAL_USERS		4

/* MTU is defined once for both the char and the net side. */
#define CCCI_CCMNI_MTU			1500

/* ---------------------------------------------------------------------- */
/* GPD descriptor                                                         */
/* ---------------------------------------------------------------------- */
/**
 * struct cldma_gpd - CLDMA buffer descriptor, used in both directions
 * @flags: bit7 IOC (interrupt on completion), bit1 BDP (buffer points to a
 *	   funnel BD), bit0 HWO (hardware owns this descriptor)
 * @checksum: 0xff - (flags + mailbox low byte + mailbox high byte)
 * @mailbox: allow length (RX) or debug id (TX), written by the producer
 * @next: physical address of the next GPD in the ring
 * @buffer: physical address of the data buffer
 * @length: data buffer length (TX) or number of bytes written (RX)
 * @ext_len: descriptor extension length, always 0 on these SoCs
 * @reserved: unused, kept for the fixed 16 byte size
 */
struct cldma_gpd {
	u8 flags;
	u8 checksum;
	__le16 mailbox;
	__le32 next;
	__le32 buffer;
	__le16 length;
	u8 ext_len;
	u8 reserved;
} __packed;

#define CLDMA_GPD_HWO		BIT(0)
#define CLDMA_GPD_BDP		BIT(1)
#define CLDMA_GPD_IOC		BIT(7)

static inline u8 cldma_gpd_checksum(u8 flags, __le16 mailbox)
{
	u8 sum = flags + ((u8 *)&mailbox)[0] + ((u8 *)&mailbox)[1];

	return (u8)(0xff - sum);
}

/* Number of GPDs the peer still owns in a ring, indices kept < 256. */
static inline unsigned int cldma_ring_busy(unsigned int read, unsigned int write)
{
	return (write - read) & (CCCI_CLDMA_QUEUE_GPD - 1);
}

static inline unsigned int cldma_ring_free(unsigned int read, unsigned int write)
{
	return CCCI_CLDMA_QUEUE_GPD - 1 - cldma_ring_busy(read, write);
}

/**
 * struct cldma_desc - a ring slot
 * @gpd: descriptor, DMA coherent
 * @gpd_dma: DMA address of @gpd
 * @skb: data buffer, owned by the hardware while HWO is set
 * @dma: DMA address of skb->data
 * @dma_len: length of the DMA mapping
 * @skb_len: logical size of the packet
 */
struct cldma_desc {
	struct cldma_gpd *gpd;
	dma_addr_t gpd_dma;
	struct sk_buff *skb;
	dma_addr_t dma;
	size_t dma_len;
	unsigned int skb_len;
};

/**
 * struct cldma_ring - one GPD ring
 * @rings: DMA coherent descriptor array, CCCI_CLDMA_QUEUE_GPD entries
 * @rings_dma: DMA address of @rings
 * @dsc: per slot bookkeeping
 * @read: first slot the peer owns, advanced by the consumer
 * @write: first free slot, advanced by the producer
 */
struct cldma_ring {
	struct cldma_gpd *rings;
	dma_addr_t rings_dma;
	struct cldma_desc dsc[CCCI_CLDMA_QUEUE_GPD];
	unsigned int read;
	unsigned int write;
};

/**
 * struct cldma_queue - one queue slot, TX and RX halves
 * @md: owning modem
 * @index: 0..7
 * @tx: AP -> MD ring
 * @rx: MD -> AP ring
 * @tx_busy: the engine cannot take more descriptors until it reports done
 * @txq: this queue carries AP -> MD traffic
 * @rxq: this queue carries MD -> AP traffic
 * @wq: TX completion wait queue, used by the synchronous senders
 * @tx_pkts: packets queued
 * @tx_bytes: bytes queued
 * @rx_pkts: packets received
 * @rx_bytes: bytes received
 * @rx_full: events where the receive ring had no free descriptor
 */
struct cldma_queue {
	struct ccci_modem *md;
	unsigned int index;
	struct cldma_ring tx;
	struct cldma_ring rx;
	bool tx_busy;
	bool txq;
	bool rxq;
	wait_queue_head_t wq;
	u64 tx_pkts;
	u64 tx_bytes;
	u64 rx_pkts;
	u64 rx_bytes;
	u64 rx_full;
};

/**
 * struct cldma_hw - invariant properties of a CLDMA implementation
 * @name: SoC name used in log messages
 * @queue_mask: hardware queues usable by the AP
 * @net_queue_mask: queues used for network traffic
 * @capability: setting of "mediatek,cldma_capability"
 * @pd_ignore_busy: the PD clock domain reports busy while gated
 */
struct cldma_hw {
	const char *name;
	u32 queue_mask;
	u32 net_queue_mask;
	u32 capability;
	bool pd_ignore_busy;
};

/**
 * struct ccci_cldma - CLDMA + CCIF transport state
 * @md: owning modem
 * @pdata: SoC description
 * @hw: resolved hardware properties
 * @napi: receive NAPI context
 * @q: queue slots
 * @irq_lock: guards the RING indices against the hard IRQ handler
 * @tx_lock: serialises the transmit path
 * @wq: serialises the receive completion work
 * @napi_enabled: NAPI is registered and enabled
 * @started: rings are live and the engine was started
 * @bus_error: a fatal DMA bus error was detected
 * @irq: interrupt numbers, CCCI_IRQ_* order
 * @irq_name: allocated IRQ names
 * @wdt_work: polls the MD watchdog status register
 */
struct ccci_cldma {
	struct ccci_modem *md;
	const struct cldma_hw *hw;

	struct napi_struct napi;
	struct cldma_queue q[CCCI_CLDMA_QUEUE_NUM];

	spinlock_t irq_lock;
	struct mutex tx_lock;
	struct workqueue_struct *wq;
	bool napi_enabled;
	bool started;
	bool bus_error;

	int irq[CCCI_IRQ_MAX];
	bool irq_owned[CCCI_IRQ_MAX];
	char irq_name[CCCI_IRQ_MAX][24];
	struct delayed_work wdt_work;
};

/* ---------------------------------------------------------------------- */
/* Ports                                                                  */
/* ---------------------------------------------------------------------- */
enum ccci_port_type {
	CCCI_PORT_TYPE_CHAR = 0,
	CCCI_PORT_TYPE_NET,
	CCCI_PORT_TYPE_SMEM,
};

#define CCCI_PORT_QUEUE_MAX	1024

/**
 * struct ccci_port - one logical channel endpoint
 * @md: owning modem
 * @rx_ch: logical channel received from
 * @tx_ch: logical channel sent to
 * @type: CCCI_PORT_TYPE_*
 * @name: device node name, without the /dev/ prefix
 * @cdev: character device
 * @dev: device registered in the ccci class
 * @rx_queue: packets waiting for read()
 * @rx_lock: protects @rx_queue and @busy
 * @queue_max: maximum number of queued packets
 * @rx_wait: read()/poll() wait queue
 * @busy: the peer asked us to stop reading this channel
 * @is_open: a file descriptor currently uses the port
 * @open_lock: serialises open/close against unregister
 * @tx_bytes: bytes handed to the transport
 * @rx_bytes: bytes handed to userspace
 * @tx_pkts: packets handed to the transport
 * @rx_pkts: packets handed to userspace
 * @dropped: packets discarded because the consumer was too slow
 * @last_seq: last sequence number received
 * @critical: this port is a critical user, held in critical_user_active[]
 * @critical_index: slot in critical_user_active[]
 */
struct ccci_port {
	struct ccci_modem *md;
	int rx_ch;
	int tx_ch;
	enum ccci_port_type type;

	char name[24];
	struct cdev cdev;
	struct device *dev;

	struct sk_buff_head rx_queue;
	spinlock_t rx_lock;
	unsigned int queue_max;
	wait_queue_head_t rx_wait;
	bool busy;
	bool is_open;
	struct mutex open_lock;

	u64 tx_bytes;
	u64 rx_bytes;
	u64 tx_pkts;
	u64 rx_pkts;
	u64 dropped;
	u16 last_seq;

	int critical_index;	/* slot in critical_user_active[], -1 if none */
};

/* ---------------------------------------------------------------------- */
/* Hardware description, filled from the device tree                      */
/* ---------------------------------------------------------------------- */
struct ccci_hw_info {
	phys_addr_t cldma_ap_ao;
	phys_addr_t cldma_md_ao;
	phys_addr_t cldma_ap_pdn;
	phys_addr_t cldma_md_pdn;
	phys_addr_t ccif_ap;
	phys_addr_t ccif_md;
	resource_size_t cldma_len;
	resource_size_t ccif_len;

	u32 md_id;
	u32 capability;
	size_t smem_size;

	/* modem DRAM carved out through /reserved-memory */
	phys_addr_t md_base;
	resource_size_t md_size;
	phys_addr_t smem_base;
	resource_size_t smem_len;

	/* CCIF mailbox, only valid while the modem is powered */
	void __iomem *ccif_sram;
};

/**
 * struct ccci_platform_data - per SoC glue
 * @name: SoC name
 * @cldma: CLDMA properties
 * @slave_boot: the MD core is released through a boot vector handshake
 * @boot_vector_key: boot vector key register offset inside the MD region
 * @boot_vector: boot vector register offset
 * @boot_vector_en: boot vector enable register offset
 * @boot_vector_key_value: value written to the key register
 * @boot_vector_value: value written to the vector register
 * @boot_vector_en_value: value written to the enable register
 * @infra_rst_mask: CLDMA reset bits in the INFRA reset registers
 * @md_clock: SCP/MD clock name, NULL if the MD domain has no dedicated clock
 */
struct ccci_platform_data {
	const char *name;
	const struct cldma_hw cldma;

	bool slave_boot;
	u32 boot_vector_key;
	u32 boot_vector;
	u32 boot_vector_en;
	u32 boot_vector_key_value;
	u32 boot_vector_value;
	u32 boot_vector_en_value;

	u32 cldma_ao_rst_mask;
	u32 cldma_pd_rst_mask;
};

/* Per SoC descriptions, defined in ccci_platform.c. */
extern const struct ccci_platform_data ccci_mt6735_data;
extern const struct ccci_platform_data ccci_mt6755_data;
extern const struct ccci_platform_data ccci_mt6797_data;

/**
 * struct ccci_modem - one MD instance (only MD1 exists on these SoCs)
 * @index: 0 based modem index
 * @dev: backing platform device
 * @pdata: SoC description
 * @hw: hardware resources
 * @base: remapped register windows
 * @irq: interrupts, CCCI_IRQ_* order
 * @clk: MD subsystem clocks
 * @cldma: transport state
 * @net: network (ccmni) state
 * @fw: modem firmware, "modem_1_wg_n.img"
 * @fw_dsp: DSP image, "dsp_1_wg_n.bin"
 * @fw_dsp2: second DSP image, "dsp_1_wg_n.bin" companion, optional
 * @state: modem state, protected by @state_lock
 * @boot_stage: handshake progress, 0..3
 * @capability: MODEM_CAP_* bits
 * @boot_count: number of successful boots
 * @ports: port table indexed by logical channel
 * @port_lock: protects @ports
 * @state_lock: protects @state, @boot_stage and the handshake
 * @wq: ordered workqueue serialising the boot state machine
 * @boot_work: powers the modem on and loads the firmware
 * @stop_work: powers the modem off
 * @poll_work: MD boot progress poller
 * @mdtimeout_timer: fires when the modem failed to boot
 * @assert_timer: debounces modem assert storms
 * @handshake_wait: completed by the CCCI control channel handshake
 * @debug_root: debugfs directory, /sys/kernel/debug/ccci/md1
 */
struct ccci_modem {
	int index;
	struct device *dev;
	const struct ccci_platform_data *pdata;

	struct ccci_hw_info hw;
	void __iomem *base[CCCI_REG_MAX];
	int irq[CCCI_IRQ_MAX];

	struct clk_bulk_data *clks;
	int nr_clks;

	struct ccci_cldma *cldma;
	struct ccci_net *net;
	struct ccci_char *char_dev;

	const struct firmware *fw;
	const struct firmware *fw_dsp;
	const struct firmware *fw_dsp2;

	enum ccci_md_state state;
	unsigned int boot_stage;
	unsigned int ex_stage;
	enum ccci_md_state reset_target;
	unsigned int capability;
	unsigned int boot_count;
	bool powered;
	bool flown;		/* in flight mode */

	struct ccci_port *ports[CCCI_MAX_CH_NUM];
	struct mutex port_lock;

	struct mutex state_lock;
	struct workqueue_struct *wq;
	struct work_struct boot_work;
	struct work_struct stop_work;
	struct delayed_work poll_work;
	struct timer_list mdtimeout_timer;
	struct completion handshake_wait;

	struct dentry *debug_root;

	atomic_t critical_user_active[CCCI_CRITICAL_USERS];
	atomic_t traffic_cnt;

	/* per channel 15 bit sequence numbers */
	u16 seq_num[CCCI_MAX_CH_NUM];
	u64 ch_pkt_cnt[CCCI_MAX_CH_NUM];

	struct list_head modem_entry;
};

struct ccci_net;
struct ccci_char;

/* ---------------------------------------------------------------------- */
/* Cross file helpers                                                     */
/* ---------------------------------------------------------------------- */
int ccci_cldma_init(struct ccci_modem *md);
void ccci_cldma_deinit(struct ccci_modem *md);
int ccci_cldma_start(struct ccci_modem *md);
void ccci_cldma_stop(struct ccci_modem *md);
int ccci_cldma_send_packet(struct ccci_modem *md, int ch,
			   struct sk_buff *skb);
int ccci_cldma_napi_poll(struct ccci_modem *md, struct napi_struct *napi,
			 int budget);
bool ccci_cldma_queue_of_ch(int ch, int *queue);
void ccci_cldma_dump(struct ccci_modem *md, const char *reason);

int ccci_ccmni_init(struct ccci_modem *md);
void ccci_ccmni_deinit(struct ccci_modem *md);
bool ccci_ccmni_rx(struct ccci_modem *md, struct sk_buff *skb);
void ccci_ccmni_xoff_hint(struct ccci_modem *md, int ch);
void ccci_ccmni_set_carrier(struct ccci_modem *md, bool up);

/* ccci_core.c services used by the other files */
int ccci_send_packet(struct ccci_modem *md, int ch, struct sk_buff *skb);
void ccci_recv_packet(struct ccci_modem *md, struct sk_buff *skb);
void ccci_broadcast_state(struct ccci_modem *md, enum ccci_md_state state);
struct ccci_modem *ccci_modem_get(int md_id);
int ccci_send_control_msg(struct ccci_modem *md, int ch, const void *data,
			  size_t len);

/* ccci_char.c entry points, called by the core once the modem is probed */
int ccci_char_init(struct ccci_modem *md);
void ccci_char_deinit(struct ccci_modem *md);
int ccci_port_register(struct ccci_modem *md, struct ccci_port *port);
void ccci_port_unregister(struct ccci_modem *md, struct ccci_port *port);
void ccci_port_broadcast_state(struct ccci_modem *md, enum ccci_md_state state);
void ccci_port_recv(struct ccci_port *port, struct sk_buff *skb);

static inline struct sk_buff *ccci_alloc_skb(unsigned int size, gfp_t flags)
{
	struct sk_buff *skb = alloc_skb(size, flags);

	if (skb)
		skb_put(skb, size);
	return skb;
}

static inline void ccci_header_init(struct ccci_msg *hdr, int ch, int len,
				    int dlen, u16 seq)
{
	memset(hdr, 0, sizeof(*hdr));
	CCCI_HDR_SET_CHANNEL(hdr, ch);
	CCCI_HDR_SET_LEN(hdr, len);
	hdr->seq_num = cpu_to_le16(seq);
	CCCI_HDR_SET_DATA_LEN(hdr, dlen);
}

#endif /* __CCCI_PRIV_H__ */
