/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Meizu Linux mainline project
 *
 * MediaTek CONSYS WLAN driver (MT6735 / MT6755 / MT6797, gen2/gen3 CONSYS).
 *
 * The Wi-Fi firmware runs on the CONNSYS MCU together with the Bluetooth and
 * GPS stacks.  The host talks to it through the same STP transport that
 * carries BT and GPS, on the Wi-Fi channel (CONNSYS_STP_WIFI):
 *
 *   - management/control messages are "H2C" commands (packet type 0x02)
 *     answered by "C2H" events (packet type 0x01);
 *   - 802.3 frames (Ethernet format, no FCS) travel as packet type 0x05.
 *
 * The MCU firmware is loaded by the WMT engine (WIFI_RAM_CODE_*); this
 * driver only sends the init command with the feature bitmap and then
 * services the traffic.
 *
 * Upper layer: a plain cfg80211 driver.  wlan0 is the station interface,
 * ap0/p2p0 is created on demand by /dev/wmtWifi.
 */

#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/random.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>

#include "connsys.h"

#define CONNSYS_WLAN_NAME	"wlan0"
#define CONNSYS_WLAN_AP_NAME	"ap0"

/* Every STP Wi-Fi payload starts with a one byte packet type. */
#define CONNSYS_WLAN_PKT_DATA	0x05	/* 802.3 data frame		 */

#define CONNSYS_WLAN_TX_TIMEOUT	(HZ / 4)
#define CONNSYS_WLAN_RETRIES	32
#define CONNSYS_WLAN_POLL_US	2000

/**
 * struct connsys_wlan - driver state
 *
 * @cd:        CONSYS device
 * @wiphy:     registered wiphy
 * @ndev:      primary (station) interface
 * @ap:        secondary (AP / P2P) interface, NULL when not requested
 * @lock:      protects @ap and the interface mode
 * @dbg:       debug level 0..3
 */
struct connsys_wlan {
	struct connsys_dev *cd;
	struct wiphy *wiphy;
	struct net_device *ndev;
	struct net_device *ap;
	struct mutex lock;
	unsigned int dbg;
};

static struct connsys_wlan wlan;

/* ------------------------------------------------------------------ */
/* Band description                                                   */
/* ------------------------------------------------------------------ */

#define CONNSYS_WLAN_CHAN(_freq, _hw, _flags)			\
	{							\
		.band		= NL80211_BAND_2GHZ,		\
		.center_freq	= (_freq),			\
		.hw_value	= (_hw),			\
		.flags		= (_flags),			\
		.max_power	= 20,				\
	}

static struct ieee80211_channel connsys_wlan_channels[] = {
	CONNSYS_WLAN_CHAN(2412, 1, 0),
	CONNSYS_WLAN_CHAN(2417, 2, 0),
	CONNSYS_WLAN_CHAN(2422, 3, 0),
	CONNSYS_WLAN_CHAN(2427, 4, 0),
	CONNSYS_WLAN_CHAN(2432, 5, 0),
	CONNSYS_WLAN_CHAN(2437, 6, 0),
	CONNSYS_WLAN_CHAN(2442, 7, 0),
	CONNSYS_WLAN_CHAN(2447, 8, 0),
	CONNSYS_WLAN_CHAN(2452, 9, 0),
	CONNSYS_WLAN_CHAN(2457, 10, 0),
	CONNSYS_WLAN_CHAN(2462, 11, 0),
	CONNSYS_WLAN_CHAN(2467, 12, IEEE80211_CHAN_NO_IR),
	CONNSYS_WLAN_CHAN(2472, 13, IEEE80211_CHAN_NO_IR),
	CONNSYS_WLAN_CHAN(2484, 14, IEEE80211_CHAN_NO_IR),
};

static struct ieee80211_rate connsys_wlan_rates[] = {
	{ .bitrate = 10, .hw_value = 0x00 },
	{ .bitrate = 20, .hw_value = 0x01, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 55, .hw_value = 0x02, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 110, .hw_value = 0x03, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 60, .hw_value = 0x04 },
	{ .bitrate = 90, .hw_value = 0x05 },
	{ .bitrate = 120, .hw_value = 0x06 },
	{ .bitrate = 180, .hw_value = 0x07 },
	{ .bitrate = 240, .hw_value = 0x08 },
	{ .bitrate = 360, .hw_value = 0x09 },
	{ .bitrate = 480, .hw_value = 0x0a },
	{ .bitrate = 540, .hw_value = 0x0b },
};

static struct ieee80211_supported_band connsys_wlan_band_2ghz = {
	.band		= NL80211_BAND_2GHZ,
	.channels	= connsys_wlan_channels,
	.n_channels	= ARRAY_SIZE(connsys_wlan_channels),
	.bitrates	= connsys_wlan_rates,
	.n_bitrates	= ARRAY_SIZE(connsys_wlan_rates),
};

/* ------------------------------------------------------------------ */
/* Command / event engine                                             */
/* ------------------------------------------------------------------ */

void connsys_wlan_rx(struct connsys_dev *cd)
{
	struct net_device *ndev = wlan.ndev;
	struct sk_buff *skb;
	u8 *buf;
	size_t len;

	if (!ndev) {
		connsys_stp_flush(cd, CONNSYS_STP_WIFI);
		return;
	}

	buf = kmalloc(CONNSYS_BTIF_MAX_LEN, GFP_KERNEL);
	if (!buf)
		return;

	while ((len = connsys_stp_read(cd, CONNSYS_STP_WIFI, buf,
				       CONNSYS_BTIF_MAX_LEN))) {
		if (buf[0] != CONNSYS_WLAN_PKT_DATA) {
			if (wlan.dbg > 2)
				dev_dbg(cd->dev, "wlan event 0x%02x (%zu bytes)\n",
					buf[0], len);
			continue;
		}
		if (len < 1 + ETH_HLEN)
			continue;

		skb = netdev_alloc_skb(ndev, len + NET_IP_ALIGN);
		if (!skb)
			continue;
		skb_reserve(skb, NET_IP_ALIGN);
		skb_put_data(skb, buf + 1, len - 1);
		skb->dev = ndev;
		skb->protocol = eth_type_trans(skb, ndev);
		skb->ip_summed = CHECKSUM_NONE;
		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += len - 1;
		netif_rx(skb);
	}

	kfree(buf);
}

/* ------------------------------------------------------------------ */
/* net_device operations                                              */
/* ------------------------------------------------------------------ */

static int connsys_wlan_ndo_open(struct net_device *ndev)
{
	netif_carrier_on(ndev);
	netif_start_queue(ndev);
	return 0;
}

static int connsys_wlan_ndo_stop(struct net_device *ndev)
{
	netif_stop_queue(ndev);
	return 0;
}

static netdev_tx_t connsys_wlan_ndo_xmit(struct sk_buff *skb,
					 struct net_device *ndev)
{
	struct connsys_dev *cd = wlan.cd;
	u8 *buf;
	size_t len;
	int ret;

	if (skb->len > CONNSYS_BTIF_MAX_LEN - 1)
		goto drop;

	len = skb->len;
	buf = kmalloc(len + 1, GFP_ATOMIC);
	if (!buf)
		goto drop;

	buf[0] = CONNSYS_WLAN_PKT_DATA;
	skb_copy_bits(skb, 0, buf + 1, len);

	/* The STP window may be full: drop and let the stack retry, as a
	 * wired driver does on a congested TX ring.
	 */
	ret = connsys_stp_send(cd, CONNSYS_STP_WIFI, buf, len + 1);
	kfree(buf);

	if (ret < 0) {
		if (ret == -EAGAIN) {
			netif_stop_queue(ndev);
			ndev->stats.tx_dropped++;
		}
		goto drop;
	}

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += len;
	dev_kfree_skb_any(skb);

	return NETDEV_TX_OK;

drop:
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static void connsys_wlan_ndo_tx_timeout(struct net_device *ndev, unsigned int txqueue)
{
	ndev->stats.tx_errors++;
	netif_wake_queue(ndev);
}

static void connsys_wlan_ndo_get_stats64(struct net_device *ndev,
					 struct rtnl_link_stats64 *stats)
{
	stats->rx_packets = ndev->stats.rx_packets;
	stats->tx_packets = ndev->stats.tx_packets;
	stats->rx_bytes = ndev->stats.rx_bytes;
	stats->tx_bytes = ndev->stats.tx_bytes;
	stats->rx_errors = ndev->stats.rx_errors;
	stats->tx_errors = ndev->stats.tx_errors;
	stats->rx_dropped = ndev->stats.rx_dropped;
	stats->tx_dropped = ndev->stats.tx_dropped;
}

static const struct net_device_ops connsys_wlan_netdev_ops = {
	.ndo_open		= connsys_wlan_ndo_open,
	.ndo_stop		= connsys_wlan_ndo_stop,
	.ndo_start_xmit		= connsys_wlan_ndo_xmit,
	.ndo_tx_timeout		= connsys_wlan_ndo_tx_timeout,
	.ndo_get_stats64	= connsys_wlan_ndo_get_stats64,
	.ndo_validate_addr	= eth_validate_addr,
};

/* ------------------------------------------------------------------ */
/* cfg80211 operations                                                */
/* ------------------------------------------------------------------ */

static int connsys_wlan_op_change_vif(struct wiphy *wiphy,
				      struct net_device *ndev,
				      enum nl80211_iftype type,
				      struct vif_params *params)
{
	switch (type) {
	case NL80211_IFTYPE_STATION:
	case NL80211_IFTYPE_AP:
	case NL80211_IFTYPE_P2P_CLIENT:
	case NL80211_IFTYPE_P2P_GO:
		ndev->ieee80211_ptr->iftype = type;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct cfg80211_ops connsys_wlan_cfg80211_ops = {
	.change_virtual_intf	= connsys_wlan_op_change_vif,
};

/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

static void connsys_wlan_setup(struct net_device *ndev)
{
	ether_setup(ndev);
	ndev->netdev_ops = &connsys_wlan_netdev_ops;
	ndev->needs_free_netdev = true;
	ndev->priv_destructor = NULL;
	ndev->watchdog_timeo = CONNSYS_WLAN_TX_TIMEOUT;
	ndev->priv_flags |= IFF_TX_SKB_SHARING;
}

static struct net_device *connsys_wlan_alloc_netdev(struct wiphy *wiphy,
						    const char *name)
{
	struct wireless_dev *wdev;
	struct net_device *ndev;

	ndev = alloc_netdev_mqs(sizeof(struct wireless_dev), name,
				NET_NAME_ENUM, connsys_wlan_setup, 1, 1);
	if (!ndev)
		return ERR_PTR(-ENOMEM);

	wdev = netdev_priv(ndev);
	wdev->wiphy = wiphy;
	wdev->iftype = NL80211_IFTYPE_STATION;
	ndev->ieee80211_ptr = wdev;

	eth_hw_addr_random(ndev);
	ndev->addr_assign_type = NET_ADDR_RANDOM;
	SET_NETDEV_DEV(ndev, wiphy->dev.parent);

	return ndev;
}

int connsys_wlan_init(struct connsys_dev *cd)
{
	struct net_device *ndev;
	struct wiphy *wiphy;
	int ret;

	memset(&wlan, 0, sizeof(wlan));
	wlan.cd = cd;
	mutex_init(&wlan.lock);

	wiphy = wiphy_new_nm(&connsys_wlan_cfg80211_ops, 0, "mtk-connsys");
	if (!wiphy)
		return -ENOMEM;

	set_wiphy_dev(wiphy, cd->dev);
	wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION) |
				 BIT(NL80211_IFTYPE_AP) |
				 BIT(NL80211_IFTYPE_P2P_CLIENT) |
				 BIT(NL80211_IFTYPE_P2P_GO);
	wiphy->bands[NL80211_BAND_2GHZ] = &connsys_wlan_band_2ghz;
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	wiphy->max_scan_ssids = 1;
	wiphy->max_scan_ie_len = 0;

	ret = wiphy_register(wiphy);
	if (ret) {
		wiphy_free(wiphy);
		return ret;
	}

	ndev = connsys_wlan_alloc_netdev(wiphy, CONNSYS_WLAN_NAME);
	if (IS_ERR(ndev)) {
		ret = PTR_ERR(ndev);
		goto err_wiphy;
	}
	strscpy(ndev->name, CONNSYS_WLAN_NAME, IFNAMSIZ);

	ret = register_netdev(ndev);
	if (ret) {
		free_netdev(ndev);
		goto err_wiphy;
	}

	wlan.wiphy = wiphy;
	wlan.ndev = ndev;
	netif_carrier_off(ndev);

	dev_info(cd->dev, "cfg80211 wlan0 registered\n");

	return 0;

err_wiphy:
	wiphy_unregister(wiphy);
	wiphy_free(wiphy);
	return ret;
}

void connsys_wlan_exit(struct connsys_dev *cd)
{
	struct net_device *ap = wlan.ap;

	mutex_lock(&wlan.lock);
	wlan.ap = NULL;
	mutex_unlock(&wlan.lock);

	if (ap)
		unregister_netdev(ap);

	if (wlan.ndev) {
		unregister_netdev(wlan.ndev);
		wlan.ndev = NULL;
	}

	if (wlan.wiphy) {
		wiphy_unregister(wlan.wiphy);
		wiphy_free(wlan.wiphy);
		wlan.wiphy = NULL;
	}
}

struct net_device *connsys_wlan_netdev(struct connsys_dev *cd)
{
	return wlan.ndev;
}

void connsys_wlan_set_debug_level(unsigned int level)
{
	wlan.dbg = min(level, 3u);
}

int connsys_wlan_set_p2p_mode(struct net_device *ndev, bool enable, bool ap)
{
	struct net_device *sec;
	int ret = 0;

	mutex_lock(&wlan.lock);
	if (!enable) {
		sec = wlan.ap;
		wlan.ap = NULL;
		mutex_unlock(&wlan.lock);
		if (sec)
			unregister_netdev(sec);
		return 0;
	}

	if (!wlan.wiphy) {
		mutex_unlock(&wlan.lock);
		return -ENODEV;
	}

	if (wlan.ap) {
		wlan.ap->ieee80211_ptr->iftype =
			ap ? NL80211_IFTYPE_AP : NL80211_IFTYPE_P2P_CLIENT;
		mutex_unlock(&wlan.lock);
		return 0;
	}

	sec = connsys_wlan_alloc_netdev(wlan.wiphy,
					ap ? CONNSYS_WLAN_AP_NAME : "p2p0");
	if (IS_ERR(sec)) {
		ret = PTR_ERR(sec);
		mutex_unlock(&wlan.lock);
		return ret;
	}

	sec->ieee80211_ptr->iftype =
		ap ? NL80211_IFTYPE_AP : NL80211_IFTYPE_P2P_CLIENT;
	ret = register_netdev(sec);
	if (ret) {
		free_netdev(sec);
		mutex_unlock(&wlan.lock);
		return ret;
	}

	wlan.ap = sec;
	mutex_unlock(&wlan.lock);
	return 0;
}
