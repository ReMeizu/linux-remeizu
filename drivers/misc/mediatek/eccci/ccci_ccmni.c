// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MD1 cellular modem (ECCCI) - CCMNI network interfaces.
 *
 * ccmni0..ccmni7 are the IP bearers of the modem. Each one carries IPv4 or
 * IPv6 packets (never Ethernet frames) on its own CCCI channel quartet:
 *
 *   RX 20 + 4n   MD -> AP, the modem produced a packet for this bearer
 *   RX ACK       flow control, the modem consumed one of our packets
 *   TX 22 + 4n   AP -> MD, the stack wants to send
 *   TX ACK       not used by the AP in this direction
 *
 * The 16 byte CCCI header is prepended on transmit and stripped on receive;
 * its "reserved" word also carries the payload length, which is what the
 * modem uses to find the end of a frame inside the 4K DMA buffers.
 */
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/if_arp.h>
#include <linux/ip.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>

#include "ccci_priv.h"

#define CCMNI_NAME			"ccmni%d"
#define CCMNI_TX_QUEUE_LEN		1000
#define CCMNI_MAX_HEADERS		(40)	/* IPv6 header, worst case */

/**
 * struct ccmni_priv - per netdev state
 * @md: owning modem
 * @index: 0..7, also the channel group
 * @rx_ch: receive channel
 * @tx_ch: transmit channel
 * @netdev: back pointer used by the receive path and the TX path
 * @xoff: the modem asked us to stop sending
 */
struct ccmni_priv {
	struct ccci_modem *md;
	unsigned int index;
	int rx_ch;
	int tx_ch;
	struct net_device *netdev;
	bool xoff;
};

/**
 * struct ccci_net - the CCMNI block of one modem
 * @md: owning modem
 * @devs: one netdev per bearer
 * @nr: number of registered netdevs
 */
struct ccci_net {
	struct ccci_modem *md;
	struct net_device *devs[CCCI_CCMNI_NUM];
	unsigned int nr;
};

static int ccmni_open(struct net_device *dev)
{
	struct ccmni_priv *priv = netdev_priv(dev);

	if (READ_ONCE(priv->md->state) != CCCI_STATE_READY) {
		netdev_warn(dev, "modem is not ready, interface stays down\n");
		return -ENODEV;
	}

	netif_carrier_on(dev);
	netif_start_queue(dev);

	return 0;
}

static int ccmni_stop(struct net_device *dev)
{
	struct ccmni_priv *priv = netdev_priv(dev);

	netif_tx_disable(dev);
	netif_carrier_off(dev);
	priv->xoff = false;

	return 0;
}

static netdev_tx_t ccmni_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct ccmni_priv *priv = netdev_priv(dev);
	struct ccci_modem *md = priv->md;
	struct ccci_msg *hdr;
	unsigned int len = skb->len;
	int ret;

	if (unlikely(!netif_running(dev) || priv->xoff ||
		     READ_ONCE(md->state) != CCCI_STATE_READY)) {
		dev->stats.tx_dropped++;
		goto drop;
	}

	if (unlikely(len > CCCI_NET_MTU)) {
		dev->stats.tx_dropped++;
		goto drop;
	}

	if (unlikely(skb_headroom(skb) < CCCI_HDR_SIZE)) {
		struct sk_buff *n;

		/* The stack builds skbs without room for our header. */
		n = skb_realloc_headroom(skb, CCCI_HDR_SIZE);
		if (!n) {
			dev->stats.tx_dropped++;
			goto drop;
		}
		dev_kfree_skb(skb);
		skb = n;
	}

	hdr = skb_push(skb, CCCI_HDR_SIZE);
	ccci_header_init(hdr, priv->tx_ch, skb->len, len, 0);

	ret = ccci_send_packet(md, priv->tx_ch, skb);
	if (ret == -EBUSY) {
		/*
		 * The ring is full: let the stack retry instead of dropping.
		 * CLDMA restarts the queue from its TX completion IRQ.
		 */
		netif_stop_queue(dev);
		return NETDEV_TX_BUSY;
	}
	if (ret) {
		dev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	dev->stats.tx_packets++;
	dev->stats.tx_bytes += len;

	return NETDEV_TX_OK;

drop:
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static void ccmni_tx_timeout(struct net_device *dev, unsigned int txqueue)
{
	netdev_warn(dev, "transmit queue %u timed out\n", txqueue);
	netif_trans_update(dev);
	netif_wake_queue(dev);
}

static int ccmni_change_mtu(struct net_device *dev, int new_mtu)
{
	if (new_mtu < ETH_MIN_MTU || new_mtu > CCCI_NET_MTU)
		return -EINVAL;

	WRITE_ONCE(dev->mtu, new_mtu);
	return 0;
}

static const struct net_device_ops ccmni_netdev_ops = {
	.ndo_open = ccmni_open,
	.ndo_stop = ccmni_stop,
	.ndo_start_xmit = ccmni_start_xmit,
	.ndo_change_mtu = ccmni_change_mtu,
	.ndo_tx_timeout = ccmni_tx_timeout,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

static void ccmni_init_netdev(struct net_device *dev)
{
	ether_setup(dev);
	dev->netdev_ops = &ccmni_netdev_ops;
	dev->header_ops = NULL;
	dev->type = ARPHRD_PPP;
	dev->hard_header_len = 0;
	dev->addr_len = 0;
	dev->mtu = CCCI_CCMNI_MTU;
	dev->min_mtu = ETH_MIN_MTU;
	dev->max_mtu = CCCI_NET_MTU;
	dev->tx_queue_len = CCMNI_TX_QUEUE_LEN;
	dev->flags = IFF_POINTOPOINT | IFF_NOARP | IFF_MULTICAST;
	dev->priv_flags |= IFF_NO_QUEUE;
}

/* ---------------------------------------------------------------------- */
/* Registration                                                           */
/* ---------------------------------------------------------------------- */

static int ccmni_create(struct ccci_net *net, unsigned int index)
{
	struct ccci_modem *md = net->md;
	struct net_device *dev;
	struct ccmni_priv *priv;
	int ret;

	dev = alloc_netdev(sizeof(*priv), CCMNI_NAME, NET_NAME_UNKNOWN,
			   ccmni_init_netdev);
	if (!dev)
		return -ENOMEM;

	priv = netdev_priv(dev);
	priv->md = md;
	priv->index = index;
	priv->rx_ch = CCCI_CCMNI1_RX + 4 * index;
	priv->tx_ch = CCCI_CCMNI1_TX + 4 * index;
	priv->netdev = dev;

	dev->dev.parent = md->dev;
	SET_NETDEV_DEV(dev, md->dev);

	ret = register_netdev(dev);
	if (ret) {
		free_netdev(dev);
		return ret;
	}

	if (READ_ONCE(md->state) == CCCI_STATE_READY) {
		netif_carrier_on(dev);
	} else {
		netif_carrier_off(dev);
		/* Keep the queue closed until ndo_open(). */
		netif_tx_stop_all_queues(dev);
	}

	net->devs[index] = dev;
	net->nr++;

	dev_info(&dev->dev, "registered on CCCI channels %d/%d\n",
		 priv->rx_ch, priv->tx_ch);
	return 0;
}

int ccci_ccmni_init(struct ccci_modem *md)
{
	struct ccci_net *net;
	unsigned int i;
	int ret;

	if (md->capability & MODEM_CAP_CCMNI_DISABLE) {
		ccci_info(md, "CCMNI interfaces disabled by the capability mask\n");
		return 0;
	}

	net = devm_kzalloc(md->dev, sizeof(*net), GFP_KERNEL);
	if (!net)
		return -ENOMEM;
	net->md = md;
	md->net = net;

	for (i = 0; i < CCCI_CCMNI_NUM; i++) {
		ret = ccmni_create(net, i);
		if (ret) {
			ccci_err(md, "cannot create ccmni%u (%d)\n", i, ret);
			ccci_ccmni_deinit(md);
			return ret;
		}
	}

	return 0;
}

void ccci_ccmni_deinit(struct ccci_modem *md)
{
	struct ccci_net *net = md->net;
	unsigned int i;

	if (!net)
		return;

	for (i = 0; i < CCCI_CCMNI_NUM; i++) {
		struct net_device *dev = net->devs[i];

		if (!dev)
			continue;
		unregister_netdev(dev);
		net->devs[i] = NULL;
	}
	net->nr = 0;
	md->net = NULL;
}

/* ---------------------------------------------------------------------- */
/* Data path                                                              */
/* ---------------------------------------------------------------------- */

static struct ccmni_priv *ccmni_of_ch(struct ccci_net *net, int ch)
{
	unsigned int idx;

	if (ch < CCCI_CCMNI1_RX || ch > CCCI_CCMNI8_DLACK_RX)
		return NULL;

	idx = (ch - CCCI_CCMNI1_RX) / 4;
	if (idx >= CCCI_CCMNI_NUM || !net->devs[idx])
		return NULL;

	return netdev_priv(net->devs[idx]);
}

/**
 * ccci_ccmni_rx() - hand one received bearer frame to the stack
 * @md: modem
 * @skb: packet with the CCCI header still in place
 *
 * Returns true when the channel belongs to a network interface (the packet
 * has then been consumed), false when another port must handle it.
 */
bool ccci_ccmni_rx(struct ccci_modem *md, struct sk_buff *skb)
{
	struct ccci_msg *hdr = (struct ccci_msg *)skb->data;
	struct ccmni_priv *priv;
	unsigned int ch = CCCI_HDR_CHANNEL(hdr);
	unsigned int dlen;
	struct net_device *dev;

	if (!md->net)
		return false;

	priv = ccmni_of_ch(md->net, ch);
	if (!priv)
		return false;

	dev = priv->netdev;

	/*
	 * The ACK channels only carry flow control: the modem reports that
	 * it consumed a packet, so the transmit queue may be restarted.
	 */
	if ((ch - CCCI_CCMNI1_RX) % 4 != 0) {
		priv->xoff = false;
		if (netif_queue_stopped(dev))
			netif_wake_queue(dev);
		kfree_skb(skb);
		return true;
	}

	if (!netif_running(dev)) {
		dev->stats.rx_dropped++;
		kfree_skb(skb);
		return true;
	}

	dlen = CCCI_HDR_DATA_LEN(hdr);
	if (skb->len <= CCCI_HDR_SIZE) {
		dev->stats.rx_errors++;
		kfree_skb(skb);
		return true;
	}

	skb_pull(skb, CCCI_HDR_SIZE);
	if (dlen && dlen <= skb->len)
		__skb_trim(skb, dlen);

	if (skb->len < sizeof(struct iphdr) ||
	    skb->len > dev->mtu + CCMNI_MAX_HEADERS) {
		dev->stats.rx_length_errors++;
		kfree_skb(skb);
		return true;
	}

	skb->dev = dev;
	skb_reset_network_header(skb);
	skb->protocol = (skb->data[0] >> 4) == 6 ? htons(ETH_P_IPV6) :
						  htons(ETH_P_IP);

	dev->stats.rx_packets++;
	dev->stats.rx_bytes += skb->len;

	netif_receive_skb(skb);
	return true;
}


void ccci_ccmni_xoff_hint(struct ccci_modem *md, int ch)
{
	struct ccmni_priv *priv;

	if (!md->net)
		return;
	priv = ccmni_of_ch(md->net, ch);
	if (!priv)
		return;

	priv->xoff = true;
	netif_stop_queue(priv->netdev);
}

void ccci_ccmni_set_carrier(struct ccci_modem *md, bool up)
{
	unsigned int i;

	if (!md->net)
		return;

	for (i = 0; i < CCCI_CCMNI_NUM; i++) {
		struct net_device *dev = md->net->devs[i];

		if (!dev)
			continue;
		if (up)
			netif_carrier_on(dev);
		else
			netif_carrier_off(dev);
	}
}
