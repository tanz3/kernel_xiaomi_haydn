// SPDX-License-Identifier: GPL-2.0-only
/* Atlantic Network Driver
 *
 * Copyright (C) 2017 aQuantia Corporation
 * Copyright (C) 2019-2020 Marvell International Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/ethtool.h>
#include <linux/pm_runtime.h>
#include <linux/ptp_clock_kernel.h>

#include "atl_ethtool.h"
#include "atl_common.h"
#include "atl_mdio.h"
#include "atl_ring.h"
#include "atl_fwdnl.h"
#include "atl_macsec.h"
#include "atl_ptp.h"
#include "atl_fwd.h"

static uint32_t atl_ethtool_get_link(struct net_device *ndev)
{
	return ethtool_op_get_link(ndev);
}

static void atl_link_to_kernel(unsigned int bits, unsigned long *kernel,
	bool legacy)
{
	struct atl_link_type *type;
	int i;

	atl_for_each_rate(i, type) {
		if (legacy && type->ethtool_idx > 31)
			continue;

		if (bits & BIT(i))
			__set_bit(type->ethtool_idx, kernel);
	}
}

#define atl_ethtool_get_common(base, modes, lstate, legacy)		\
do {									\
	struct atl_fc_state *fc = &(lstate)->fc;			\
	(base)->port = PORT_TP;						\
	(base)->autoneg = AUTONEG_DISABLE;				\
	(base)->eth_tp_mdix = ETH_TP_MDI_INVALID;			\
	(base)->eth_tp_mdix_ctrl = ETH_TP_MDI_INVALID;			\
									\
	atl_add_link_supported(modes, Autoneg);				\
	atl_add_link_supported(modes, TP);				\
	atl_add_link_supported(modes, Pause);				\
	atl_add_link_supported(modes, Asym_Pause);			\
	atl_add_link_advertised(modes, TP);				\
	atl_add_link_lpadvertised(modes, Autoneg);			\
									\
	if (lstate->autoneg) {						\
		(base)->autoneg = AUTONEG_ENABLE;			\
		atl_add_link_advertised(modes, Autoneg);		\
	}								\
									\
	if (fc->req & atl_fc_rx)					\
		atl_add_link_advertised(modes, Pause);			\
									\
	if (!!(fc->req & atl_fc_rx) ^ !!(fc->req & atl_fc_tx))		\
		atl_add_link_advertised(modes, Asym_Pause);		\
									\
	if (fc->cur & atl_fc_rx)					\
		atl_add_link_lpadvertised(modes, Pause);		\
									\
	if (!!(fc->cur & atl_fc_rx) ^ !!(fc->cur & atl_fc_tx))		\
		atl_add_link_lpadvertised(modes, Asym_Pause);		\
									\
	atl_link_to_kernel((lstate)->supported,				\
		(unsigned long *)&(modes)->link_modes.supported,	\
		legacy);						\
	atl_link_to_kernel((lstate)->advertized,			\
		(unsigned long *)&(modes)->link_modes.advertising,	\
		legacy);						\
	atl_link_to_kernel((lstate)->lp_advertized,			\
		(unsigned long *)&(modes)->link_modes.lp_advertising,	\
		legacy);						\
} while (0)

#define atl_add_link_supported(ptr, mode) \
	atl_add_link_mode(ptr, SUPPORTED, supported, mode)

#define atl_add_link_advertised(ptr, mode) \
	atl_add_link_mode(ptr, ADVERTISED, advertising, mode)

#define atl_add_link_lpadvertised(ptr, mode) \
	atl_add_link_mode(ptr, ADVERTISED, lp_advertising, mode)

#ifndef ATL_HAVE_ETHTOOL_KSETTINGS

struct atl_ethtool_compat {
	struct {
		unsigned long supported;
		unsigned long advertising;
		unsigned long lp_advertising;
	} link_modes;
};

#define atl_add_link_mode(ptr, nameuc, namelc, mode)	\
	do { \
		(ptr)->link_modes.namelc |= nameuc ## _ ## mode; \
	} while (0)

static int atl_ethtool_get_settings(struct net_device *ndev,
				 struct ethtool_cmd *cmd)
{
	struct atl_nic *nic = netdev_priv(ndev);
	struct atl_link_state *lstate = &nic->hw.link_state;
	struct atl_ethtool_compat cmd_compat;

	memset(&cmd_compat, 0, sizeof(cmd_compat));

	atl_ethtool_get_common(cmd, &cmd_compat, lstate, true);
	cmd->supported = cmd_compat.link_modes.supported;
	cmd->advertising = cmd_compat.link_modes.advertising;
	cmd->lp_advertising = cmd_compat.link_modes.lp_advertising;

	ethtool_cmd_speed_set(cmd, lstate->link ? lstate->link->speed : 0);
	cmd->duplex = (lstate->link) ? lstate->link->duplex : DUPLEX_UNKNOWN;

	return 0;
}

#else

#define atl_add_link_mode(ptr, nameuc, namelc, mode)	\
	ethtool_link_ksettings_add_link_mode(ptr, namelc, mode)

static int atl_ethtool_get_ksettings(struct net_device *ndev,
	struct ethtool_link_ksettings *cmd)
{
	struct atl_nic *nic = netdev_priv(ndev);
	struct atl_link_state *lstate = &nic->hw.link_state;

	ethtool_link_ksettings_zero_link_mode(cmd, supported);
	ethtool_link_ksettings_zero_link_mode(cmd, advertising);
	ethtool_link_ksettings_zero_link_mode(cmd, lp_advertising);

	atl_ethtool_get_common(&cmd->base, cmd, lstate, false);

	cmd->base.speed = lstate->link ? lstate->link->speed : 0;
	cmd->base.duplex = lstate->link ? lstate->link->duplex : DUPLEX_UNKNOWN;

	return 0;
}

#endif

#undef atl_add_link_supported
#undef atl_add_link_advertised
#undef atl_add_link_lpadvertised
#undef atl_add_link_mode

static unsigned int atl_kernel_to_link(const unsigned long int *bits,
	bool legacy)
{
	unsigned int ret = 0;
	int i;
	struct atl_link_type *type;

	atl_for_each_rate(i, type) {
		if (legacy && type->ethtool_idx > 31)
			continue;

		if (test_bit(type->ethtool_idx, bits))
			ret |= BIT(i);
	}

	return ret;
}

static int atl_set_fixed_speed(struct atl_hw *hw, unsigned int speed,
			       unsigned int duplex)
{
	unsigned int dplx = (duplex == DUPLEX_HALF) ? DUPLEX_HALF : DUPLEX_FULL;
	struct atl_link_state *lstate = &hw->link_state;
	__ETHTOOL_DECLARE_LINK_MODE_MASK(link_modes);
	struct atl_link_type *type;
	unsigned long tmp;
	int i;

	lstate->advertized &= ~ATL_EEE_MASK;
	atl_for_each_rate(i, type)
		if (type->speed == speed && type->duplex == dplx) {
			if (!(lstate->supported & BIT(i)))
				return -EINVAL;

			lstate->advertized = BIT(i);
			break;
		}

	if (lstate->eee_enabled) {
		atl_link_to_kernel(lstate->supported >> ATL_EEE_BIT_OFFT,
				   link_modes, false);
		/* advertize the supported links */
		tmp = atl_kernel_to_link(link_modes, false);
		lstate->advertized |= tmp << ATL_EEE_BIT_OFFT;
	}

	lstate->autoneg = false;
	hw->mcp.ops->set_link(hw, false);
	return 0;
}

#define atl_ethtool_set_common(base, lstate, advertise, tmp, legacy, speed) \
do {									\
	struct atl_fc_state *fc = &lstate->fc;				\
									\
	if ((base)->port != PORT_TP)					\
		return -EINVAL;						\
									\
	if ((base)->autoneg != AUTONEG_ENABLE)				\
		return atl_set_fixed_speed(hw, speed, (base)->duplex);	\
									\
	atl_add_link_bit(tmp, Autoneg);					\
	atl_add_link_bit(tmp, TP);					\
	atl_add_link_bit(tmp, Pause);					\
	atl_add_link_bit(tmp, Asym_Pause);				\
	atl_link_to_kernel((lstate)->supported, tmp, legacy);		\
									\
	if (atl_complement_intersect(advertise, tmp)) {			\
		atl_nic_dbg("Unsupported advertising bits from ethtool\n"); \
		return -EINVAL;						\
	}								\
									\
	lstate->autoneg = true;						\
	(lstate)->advertized |= atl_kernel_to_link(advertise, legacy);	\
									\
	fc->req = 0;							\
	if (atl_test_link_bit(advertise, Pause))			\
		fc->req	|= atl_fc_full;					\
									\
	if (atl_test_link_bit(advertise, Asym_Pause))			\
		fc->req ^= atl_fc_tx;					\
									\
} while (0)

#ifndef ATL_HAVE_ETHTOOL_KSETTINGS

#define atl_add_link_bit(ptr, name)		\
	(*(ptr) |= SUPPORTED_ ## name)

#define atl_test_link_bit(ptr, name)		\
	(*(ptr) & SUPPORTED_ ## name)

static inline bool atl_complement_intersect(const unsigned long *advertised,
	unsigned long *supported)
{
	return !!(*(uint32_t *)advertised & ~*(uint32_t *)supported);
}

static int atl_ethtool_set_settings(struct net_device *ndev,
	struct ethtool_cmd *cmd)
{
	struct atl_nic *nic = netdev_priv(ndev);
	struct atl_hw *hw = &nic->hw;
	struct atl_link_state *lstate = &hw->link_state;
	unsigned long tmp = 0;
	uint32_t speed = ethtool_cmd_speed(cmd);

	atl_ethtool_set_common(cmd, lstate,
		(unsigned long *)&cmd->advertising, &tmp, true, speed);
	hw->mcp.ops->set_link(hw, false);
	return 0;
}

#else

#define atl_add_link_bit(ptr, name)				\
	__set_bit(ETHTOOL_LINK_MODE_ ## name ## _BIT, ptr)

#define atl_test_link_bit(ptr, name)				\
	test_bit(ETHTOOL_LINK_MODE_ ## name ## _BIT, ptr)

static inline bool atl_complement_intersect(const unsigned long *advertised,
	unsigned long *supported)
{
	bitmap_complement(supported, supported,
		__ETHTOOL_LINK_MODE_MASK_NBITS);
	return bitmap_intersects(advertised, supported,
		__ETHTOOL_LINK_MODE_MASK_NBITS);
}

static int atl_ethtool_set_ksettings(struct net_device *ndev,
	const struct ethtool_link_ksettings *cmd)
{
	struct atl_nic *nic = netdev_priv(ndev);
	struct atl_hw *hw = &nic->hw;
	struct atl_link_state *lstate = &hw->link_state;
	const struct ethtool_link_settings *base = &cmd->base;
	__ETHTOOL_DECLARE_LINK_MODE_MASK(tmp);

	bitmap_zero(tmp, __ETHTOOL_LINK_MODE_MASK_NBITS);

	atl_ethtool_set_common(base, lstate, cmd->link_modes.advertising, tmp,
		false, cmd->base.speed);
	hw->mcp.ops->set_link(hw, false);
	return 0;
}

#endif

#undef atl_add_link_bit
#undef atl_test_link_bit

static uint32_t atl_rss_tbl_size(struct net_device *ndev)
{
	return ATL_RSS_TBL_SIZE;
}

static uint32_t atl_rss_key_size(struct net_device *ndev)
{
	return ATL_RSS_KEY_SIZE;
}

static int atl_rss_get_rxfh(struct net_device *ndev, uint32_t *tbl,
	uint8_t *key, uint8_t *htype)
{
	struct atl_hw *hw = &((struct atl_nic *)netdev_priv(ndev))->hw;
	int i;

	if (htype)
		*htype = ETH_RSS_HASH_TOP;

	if (key)
		memcpy(key, hw->rss_key, atl_rss_key_size(ndev));

	if (tbl)
		for (i = 0; i < atl_rss_tbl_size(ndev); i++)
			tbl[i] = hw->rss_tbl[i];

	return 0;
}

static int atl_rss_set_rxfh(struct net_device *ndev, const uint32_t *tbl,
	const uint8_t *key, const uint8_t htype)
{
	struct atl_nic *nic = netdev_priv(ndev);
	struct atl_hw *hw = &nic->hw;
	int i;
	uint32_t tbl_size = atl_rss_tbl_size(ndev);

	if (htype && htype != ETH_RSS_HASH_TOP)
		return -EINVAL;

	if (tbl) {
		for (i = 0; i < tbl_size; i++)
			if (tbl[i] >= nic->nvecs)
				return -EINVAL;

		for (i = 0; i < tbl_size; i++)
			hw->rss_tbl[i] = tbl[i];
	}

	if (key) {
		memcpy(hw->rss_key, key, atl_rss_key_size(ndev));
		atl_set_rss_key(hw);
	}

	if (tbl)
		atl_set_rss_tbl(hw);

	return 0;
}

static void atl_get_channels(struct net_device *ndev,
			     struct ethtool_channels *chan)
{
	struct atl_nic *nic = netdev_priv(ndev);
	int max_rings;

	if (nic->flags & ATL_FL_MULTIPLE_VECTORS)
		max_rings = atl_max_queues;
	else
		max_rings = atl_max_queues_non_msi;
	if (max_rings > num_present_cpus())
		max_rings = num_present_cpus();

	chan->max_combined = max_rings;
	chan->combined_count = nic->nvecs;
	if (nic->flags & ATL_FL_MULTIPLE_VECTORS)
		chan->max_other = chan->other_count = ATL_NUM_NON_RING_IRQS;
}

static int atl_set_channels(struct net_device *ndev,
			    struct ethtool_channels *chan)
{
	struct atl_nic *nic = netdev_priv(ndev);
	unsigned int nvecs = chan->combined_count;

	if (!nvecs || chan->rx_count || chan->tx_count)
		return -EINVAL;

	if (nic->flags & ATL_FL_MULTIPLE_VECTORS &&
		chan->other_count != ATL_NUM_NON_RING_IRQS)
		return -EINVAL;

	if (!(nic->flags & ATL_FL_MULTIPLE_VECTORS) &&
		chan->other_count)
		return -EINVAL;

	if (nvecs > atl_max_queues)
		return -EINVAL;

	nic->requested_nvecs = nvecs;

	return atl_reconfigure(nic);
}

static void atl_get_pauseparam(struct net_device *ndev,
	struct ethtool_pauseparam *pause)
{
	struct atl_nic *nic = netdev_priv(ndev);
	struct atl_fc_state *fc = &nic->hw.link_state.fc;

	pause->autoneg = 0;
	pause->rx_pause = !!(fc->req & atl_fc_rx);
	pause->tx_pause = !!(fc->req & atl_fc_tx);
}

static int atl_set_pauseparam(struct net_device *ndev,
	struct ethtool_pauseparam *pause)
{
	struct atl_nic *nic = netdev_priv(ndev);
	struct atl_hw *hw = &nic->hw;
	struct atl_link_state *lstate = &hw->link_state;
	struct atl_fc_state *fc = &lstate->fc;

	if ((hw->chip_id == ATL_ATLANTIC) && (atl_fw_major(hw) < 2))
		return -EOPNOTSUPP;

	if (pause->autoneg)
		return -EINVAL;

	fc->req = (!!pause->rx_pause << atl_fc_rx_shift) |
		(!!pause->tx_pause << atl_fc_tx_shift);

	hw->mcp.ops->set_link(hw, false);
	return 0;
}

static int atl_get_eee(struct net_device *ndev, struct ethtool_eee *eee)
{
	struct atl_nic *nic = netdev_priv(ndev);
	struct atl_link_state *lstate = &nic->hw.link_state;
	int ret = 0;

	eee->supported = eee->advertised = eee->lp_advertised = 0;

	/* Casting to unsigned long is safe, as atl_link_to_kernel()
	 * will only access low 32 bits when called with legacy == true
	 */
	atl_link_to_kernel(lstate->supported >> ATL_EEE_BIT_OFFT,
		(unsigned long *)&eee->supported, true);
	atl_link_to_kernel(lstate->advertized >> ATL_EEE_BIT_OFFT,
		(unsigned long *)&eee->advertised, true);
	atl_link_to_kernel(lstate->lp_advertized >> ATL_EEE_BIT_OFFT,
		(unsigned long *)&eee->lp_advertised, true);

	eee->eee_enabled = eee->tx_lpi_enabled = lstate->eee_enabled;
	eee->eee_active = lstate->eee;

	ret = atl_get_lpi_timer(nic, &nic->hw.lpi_timer);
	if (ret == -ENODATA)
		ret = 0;

	eee->tx_lpi_timer = nic->hw.lpi_timer;
	return ret;
}

static int atl_set_eee(struct net_device *ndev, struct ethtool_eee *eee)
{
	struct atl_nic *nic = netdev_priv(ndev);
	struct atl_hw *hw = &nic->hw;
	struct atl_link_state *lstate = &hw->link_state;
	__ETHTOOL_DECLARE_LINK_MODE_MASK(link_modes);
	unsigned long tmp = 0;

	if ((hw->chip_id == ATL_ATLANTIC) && (atl_fw_major(hw) < 2))
		return -EOPNOTSUPP;

	if (eee->tx_lpi_timer != nic->hw.lpi_timer)
		return -EOPNOTSUPP;

	lstate->eee_enabled = eee->eee_enabled;

	if (lstate->eee_enabled) {
		atl_link_to_kernel(lstate->supported >> ATL_EEE_BIT_OFFT,
				   link_modes, false);
		if (eee->advertised & ~link_modes[0])
			return -EINVAL;

		/* advertize the requested link or all supported */
		if (eee->advertised)
			ethtool_convert_legacy_u32_to_link_mode(link_modes,
								eee->advertised);
		tmp = atl_kernel_to_link(link_modes, false);
	}

	lstate->advertized &= ~ATL_EEE_MASK;
	if (lstate->eee_enabled)
		lstate->advertized |= tmp << ATL_EEE_BIT_OFFT;

	hw->mcp.ops->set_link(hw, false);
	return 0;
}

static void atl_get_drvinfo(struct net_device *ndev,
	struct ethtool_drvinfo *drvinfo)
{
	struct atl_nic *nic = netdev_priv(ndev);
	uint32_t fw_rev = nic->hw.mcp.fw_rev;

	strlcpy(drvinfo->driver, atl_driver_name, sizeof(drvinfo->driver));
	strlcpy(drvinfo->version, ATL_VERSION, sizeof(drvinfo->version));
	snprintf(drvinfo->fw_version, sizeof(drvinfo->fw_version),
		"%d.%d.%d", fw_rev >> 24, fw_rev >> 16 & 0xff,
		fw_rev & 0xffff);
	strlcpy(drvinfo->bus_info, pci_name(nic->hw.pdev),
		sizeof(drvinfo->bus_info));
}

static int atl_nway_reset(struct net_device *ndev)
{
	struct atl_nic *nic = netdev_priv(ndev);
	struct atl_hw *hw = &nic->hw;

	return hw->mcp.ops->restart_aneg(hw);
}

static void atl_get_ringparam(struct net_device *ndev,
	struct ethtool_ringparam *rp)
{
	struct atl_nic *nic = netdev_priv(ndev);

	rp->rx_mini_max_pending = rp->rx_mini_pending = 0;
	rp->rx_jumbo_max_pending = rp->rx_jumbo_pending = 0;

	rp->rx_max_pending = rp->tx_max_pending = ATL_MAX_RING_SIZE;

	rp->rx_pending = nic->requested_rx_size;
	rp->tx_pending = nic->requested_tx_size;
}

static int atl_set_ringparam(struct net_device *ndev,
	struct ethtool_ringparam *rp)
{
	struct atl_nic *nic = netdev_priv(ndev);

	if (rp->rx_mini_pending || rp->rx_jumbo_pending)
		return -EINVAL;

	if (rp->rx_pending < 8 || rp->tx_pending < 8)
		return -EINVAL;

	nic->requested_rx_size = rp->rx_pending & ~7;
	nic->requested_tx_size = rp->tx_pending & ~7;

	return atl_reconfigure(nic);
}

struct atl_stat_desc {
	char stat_name[ETH_GSTRING_LEN];
	int idx;
};

#define ATL_TX_STAT(_name, _field)				\
{								\
	.stat_name = #_name,					\
	.idx = offsetof(struct atl_tx_ring_stats, _field) /	\
		sizeof(uint64_t),				\
}

#define ATL_RX_STAT(_name, _field)				\
{								\
	.stat_name = #_name,					\
	.idx = offsetof(struct atl_rx_ring_stats, _field) /	\
		sizeof(uint64_t),				\
}

#define ATL_RX_FWD_STAT(_name, _field)				\
{								\
	.stat_name = #_name,					\
	.idx = offsetof(struct atl_rx_fwd_ring_stats, _field) /	\
		sizeof(uint64_t),				\
}

#define ATL_ETH_STAT(_name, _field)				\
{								\
	.stat_name = #_name,					\
	.idx = offsetof(struct atl_ether_stats, _field) /	\
		sizeof(uint64_t),				\
}

static const struct atl_stat_desc tx_stat_descs[] = {
	ATL_TX_STAT(tx_packets, packets),
	ATL_TX_STAT(tx_bytes, bytes),
	ATL_TX_STAT(tx_busy, tx_busy),
	ATL_TX_STAT(tx_queue_restart, tx_restart),
	ATL_TX_STAT(tx_dma_map_failed, dma_map_failed),
};

static const struct atl_stat_desc rx_stat_descs[] = {
	ATL_RX_STAT(rx_packets, packets),
	ATL_RX_STAT(rx_bytes, bytes),
	ATL_RX_STAT(rx_multicast_packets, multicast),
	ATL_RX_STAT(rx_lin_skb_overrun, linear_dropped),
	ATL_RX_STAT(rx_skb_alloc_failed, alloc_skb_failed),
	ATL_RX_STAT(rx_head_page_reused, reused_head_page),
	ATL_RX_STAT(rx_data_page_reused, reused_data_page),
	ATL_RX_STAT(rx_head_page_allocated, alloc_head_page),
	ATL_RX_STAT(rx_data_page_allocated, alloc_data_page),
	ATL_RX_STAT(rx_head_page_alloc_failed, alloc_head_page_failed),
	ATL_RX_STAT(rx_data_page_alloc_failed, alloc_data_page_failed),
	ATL_RX_STAT(rx_non_eop_descs, non_eop_descs),
	ATL_RX_STAT(rx_mac_err, mac_err),
	ATL_RX_STAT(rx_checksum_err, csum_err),
};

static const struct atl_stat_desc rx_fwd_stat_descs[] = {
	ATL_RX_FWD_STAT(rx_fwd_packets, packets),
	ATL_RX_FWD_STAT(rx_fwd_bytes, bytes),
};

static const struct atl_stat_desc eth_stat_descs[] = {
	ATL_ETH_STAT(tx_pause, tx_pause),
	ATL_ETH_STAT(tx_ether_pkts, tx_ether_pkts),
	ATL_ETH_STAT(tx_ether_octets, tx_ether_octets),
	ATL_ETH_STAT(tx_errors, tx_errors),
	ATL_ETH_STAT(rx_pause, rx_pause),
	ATL_ETH_STAT(rx_ether_octets, rx_ether_octets),
	ATL_ETH_STAT(rx_ether_pkts, rx_ether_pkts),
	ATL_ETH_STAT(rx_ether_broacasts, rx_ether_broacasts),
	ATL_ETH_STAT(rx_ether_multicasts, rx_ether_multicasts),
	ATL_ETH_STAT(rx_ether_crc_align_errs, rx_ether_crc_align_errs),
	ATL_ETH_STAT(rx_filter_host, rx_filter_host),
	ATL_ETH_STAT(rx_filter_lost, rx_filter_lost),
	ATL_ETH_STAT(rx_errors, rx_errors),
	ATL_ETH_STAT(rx_drops, rx_drops),
	ATL_ETH_STAT(rx_dma_packets, rx_dma_packets),
	ATL_ETH_STAT(rx_dma_octets, rx_dma_octets),
	ATL_ETH_STAT(rx_dma_drops, rx_dma_drops),
	ATL_ETH_STAT(tx_dma_packets, tx_dma_packets),
	ATL_ETH_STAT(tx_dma_octets, tx_dma_octets),
};

#define ATL_PRIV_FLAG(_name, _bit)		\
	[ATL_PF(_bit)] = #_name

static const char atl_priv_flags[][ETH_GSTRING_LEN] = {
	ATL_PRIV_FLAG(PKTSystemLoopback, LPB_SYS_PB),
	ATL_PRIV_FLAG(DMASystemLoopback, LPB_SYS_DMA),
	ATL_PRIV_FLAG(DMANetworkLoopback, LPB_NET_DMA),
	ATL_PRIV_FLAG(PHYInternalLoopback, LPB_INT_PHY),
	ATL_PRIV_FLAG(PHYExternalLoopback, LPB_EXT_PHY),
	ATL_PRIV_FLAG(RX_LPI_MAC, LPI_RX_MAC),
	ATL_PRIV_FLAG(TX_LPI_MAC, LPI_TX_MAC),
	ATL_PRIV_FLAG(RX_LPI_PHY, LPI_RX_PHY),
	ATL_PRIV_FLAG(TX_LPI_PHY, LPI_TX_PHY),
	ATL_PRIV_FLAG(ResetStatistics, STATS_RESET),
	ATL_PRIV_FLAG(StripEtherPadding, STRIP_PAD),
	ATL_PRIV_FLAG(MediaDetect, MEDIA_DETECT),
	ATL_PRIV_FLAG(Downshift, DOWNSHIFT),
};

#if IS_ENABLED(CONFIG_MACSEC) && defined(NETIF_F_HW_MACSEC)

#define ATL_MACSEC_STAT(_name, _field)					\
{									\
	.stat_name = #_name,						\
	.idx = offsetof(struct atl_macsec_common_stats, _field) /	\
		sizeof(uint64_t),					\
}

#define ATL_MACSEC_RX_SA_STAT(_name, _field)				\
{									\
	.stat_name = #_name,						\
	.idx = offsetof(struct atl_macsec_rx_sa_stats, _field) /	\
		sizeof(uint64_t),					\
}

#define ATL_MACSEC_TX_SA_STAT(_name, _field)				\
{									\
	.stat_name = #_name,						\
	.idx = offsetof(struct atl_macsec_tx_sa_stats, _field) /	\
		sizeof(uint64_t),				\
}

#define ATL_MACSEC_TX_SC_STAT(_name, _field)				\
{									\
	.stat_name = #_name,						\
	.idx = offsetof(struct atl_macsec_tx_sc_stats, _field) /	\
		sizeof(uint64_t),					\
}

static const struct atl_stat_desc macsec_stat_descs[] = {
	ATL_MACSEC_STAT(in_ctl_pkts, in.ctl_pkts),
	ATL_MACSEC_STAT(in_tagged_miss_pkts, in.tagged_miss_pkts),
	ATL_MACSEC_STAT(in_untagged_miss_pkts, in.untagged_miss_pkts),
	ATL_MACSEC_STAT(in_notag_pkts, in.notag_pkts),
	ATL_MACSEC_STAT(in_untagged_pkts, in.untagged_pkts),
	ATL_MACSEC_STAT(in_bad_tag_pkts, in.bad_tag_pkts),
	ATL_MACSEC_STAT(in_no_sci_pkts, in.no_sci_pkts),
	ATL_MACSEC_STAT(in_unknown_sci_pkts, in.unknown_sci_pkts),
	ATL_MACSEC_STAT(in_ctrl_prt_pass_pkts, in.ctrl_prt_pass_pkts),
	ATL_MACSEC_STAT(in_unctrl_prt_pass_pkts, in.unctrl_prt_pass_pkts),
	ATL_MACSEC_STAT(in_ctrl_prt_fail_pkts, in.ctrl_prt_fail_pkts),
	ATL_MACSEC_STAT(in_unctrl_prt_fail_pkts, in.unctrl_prt_fail_pkts),
	ATL_MACSEC_STAT(in_too_long_pkts, in.too_long_pkts),
	ATL_MACSEC_STAT(in_igpoc_ctl_pkts, in.igpoc_ctl_pkts),
	ATL_MACSEC_STAT(in_ecc_error_pkts, in.ecc_error_pkts),
	ATL_MACSEC_STAT(in_unctrl_hit_drop_redir, in.unctrl_hit_drop_redir),
	ATL_MACSEC_STAT(out_ctl_pkts, out.ctl_pkts),
	ATL_MACSEC_STAT(out_unknown_sa_pkts, out.unknown_sa_pkts),
	ATL_MACSEC_STAT(out_untagged_pkts, out.untagged_pkts),
	ATL_MACSEC_STAT(out_too_long, out.too_long),
	ATL_MACSEC_STAT(out_ecc_error_pkts, out.ecc_error_pkts),
	ATL_MACSEC_STAT(out_unctrl_hit_drop_redir, out.unctrl_hit_drop_redir),
};

static const struct atl_stat_desc macsec_rx_sa_stat_descs[] = {
	ATL_MACSEC_RX_SA_STAT(untagged_hit_pkts, untagged_hit_pkts),
	ATL_MACSEC_RX_SA_STAT(ctrl_hit_drop_redir_pkts, ctrl_hit_drop_redir_pkts),
	ATL_MACSEC_RX_SA_STAT(not_using_sa, not_using_sa),
	ATL_MACSEC_RX_SA_STAT(unused_sa, unused_sa),
	ATL_MACSEC_RX_SA_STAT(not_valid_pkts, not_valid_pkts),
	ATL_MACSEC_RX_SA_STAT(invalid_pkts, invalid_pkts),
	ATL_MACSEC_RX_SA_STAT(ok_pkts, ok_pkts),
	ATL_MACSEC_RX_SA_STAT(late_pkts, late_pkts),
	ATL_MACSEC_RX_SA_STAT(delayed_pkts, delayed_pkts),
	ATL_MACSEC_RX_SA_STAT(unchecked_pkts, unchecked_pkts),
	ATL_MACSEC_RX_SA_STAT(validated_octets, validated_octets),
	ATL_MACSEC_RX_SA_STAT(decrypted_octets, decrypted_octets),
};

static const struct atl_stat_desc macsec_tx_sa_stat_descs[] = {
	ATL_MACSEC_TX_SA_STAT(hit_drop_redirect, sa_hit_drop_redirect),
	ATL_MACSEC_TX_SA_STAT(protected2_pkts, sa_protected2_pkts),
	ATL_MACSEC_TX_SA_STAT(protected_pkts, sa_protected_pkts),
	ATL_MACSEC_TX_SA_STAT(encrypted_pkts, sa_encrypted_pkts),
};


static const struct atl_stat_desc macsec_tx_sc_stat_descs[] = {
	ATL_MACSEC_TX_SC_STAT(protected_pkts, sc_protected_pkts),
	ATL_MACSEC_TX_SC_STAT(encrypted_pkts, sc_encrypted_pkts),
	ATL_MACSEC_TX_SC_STAT(protected_octets, sc_protected_octets),
	ATL_MACSEC_TX_SC_STAT(encrypted_octets, sc_encrypted_octets),
};
#endif

static int atl_get_sset_count(struct net_device *ndev, int sset)
{
	struct atl_nic *nic = netdev_priv(ndev);

	switch (sset) {
	case ETH_SS_STATS:
		return ARRAY_SIZE(tx_stat_descs) * (nic->nvecs + 1) +
		       ARRAY_SIZE(rx_stat_descs) * (nic->nvecs + 1) +
		       ARRAY_SIZE(eth_stat_descs)
#if IS_ENABLED(CONFIG_ATLFWD_FWD)
		       + ARRAY_SIZE(rx_fwd_stat_descs)
#endif
#if IS_ENABLED(CONFIG_ATLFWD_FWD_NETLINK)
		       + ARRAY_SIZE(tx_stat_descs) *
				 hweight_long(nic->fwd.ring_map[ATL_FWDIR_TX])
		       + ARRAY_SIZE(rx_stat_descs) *
				 hweight_long(nic->fwd.ring_map[ATL_FWDIR_RX])
#endif
#if IS_ENABLED(CONFIG_MACSEC) && defined(NETIF_F_HW_MACSEC)
		       + ARRAY_SIZE(macsec_stat_descs)
		       + ARRAY_SIZE(macsec_tx_sc_stat_descs) *
				 atl_macsec_tx_sc_cnt(&nic->hw)
		       + ARRAY_SIZE(macsec_tx_sa_stat_descs) *
				 atl_macsec_tx_sa_cnt(&nic->hw)
		       + ARRAY_SIZE(macsec_rx_sa_stat_descs) *
				 atl_macsec_rx_sa_cnt(&nic->hw)
#endif
			;

	case ETH_SS_PRIV_FLAGS:
		return ARRAY_SIZE(atl_priv_flags);

	default:
		return -EOPNOTSUPP;
	}
}

static void atl_copy_stats_strings(char **data, char *prefix,
				   const struct atl_stat_desc *descs, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		snprintf(*data, ETH_GSTRING_LEN, "%s%s", prefix,
			 descs[i].stat_name);
		*data += ETH_GSTRING_LEN;
	}
}

static void atl_copy_stats_string_set(char **data, char *prefix)
{
	atl_copy_stats_strings(data, prefix, tx_stat_descs,
			       ARRAY_SIZE(tx_stat_descs));
	atl_copy_stats_strings(data, prefix, rx_stat_descs,
			       ARRAY_SIZE(rx_stat_descs));
}

static void atl_get_strings(struct net_device *ndev, uint32_t sset,
			    uint8_t *data)
{
	struct atl_nic *nic = netdev_priv(ndev);
	int i;
	char prefix[16];
	char *p = data;

	switch (sset) {
	case ETH_SS_STATS:
		atl_copy_stats_string_set(&p, "");

#if IS_ENABLED(CONFIG_ATLFWD_FWD)
		atl_copy_stats_strings(&p, "", rx_fwd_stat_descs,
				       ARRAY_SIZE(rx_fwd_stat_descs));
#endif
		atl_copy_stats_strings(&p, "", eth_stat_descs,
				       ARRAY_SIZE(eth_stat_descs));

		for (i = 0; i < nic->nvecs; i++) {
			snprintf(prefix, sizeof(prefix), "ring_%d_", i);
			atl_copy_stats_string_set(&p, prefix);
		}

#if IS_ENABLED(CONFIG_ATLFWD_FWD_NETLINK)
		for (i = 0; i < ATL_NUM_FWD_RINGS; i++) {
			snprintf(prefix, sizeof(prefix), "fwd_ring_%d_", i);

			if (atlfwd_nl_is_tx_fwd_ring_created(ndev, i))
				atl_copy_stats_strings(
					&p, prefix, tx_stat_descs,
					ARRAY_SIZE(tx_stat_descs));
			if (atlfwd_nl_is_rx_fwd_ring_created(ndev, i))
				atl_copy_stats_strings(
					&p, prefix, rx_stat_descs,
					ARRAY_SIZE(rx_stat_descs));
		}
#endif
#if IS_ENABLED(CONFIG_MACSEC) && defined(NETIF_F_HW_MACSEC)
		atl_copy_stats_strings(&p, "macsec_", macsec_stat_descs,
				       ARRAY_SIZE(macsec_stat_descs));

		for (i = 0; i < ATL_MACSEC_MAX_SC; i++) {
			struct atl_macsec_txsc *atl_txsc =
				&nic->hw.macsec_cfg.atl_txsc[i];
			int assoc_num;

			if (!(test_bit(i, &nic->hw.macsec_cfg.txsc_idx_busy)))
				continue;

			snprintf(prefix, sizeof(prefix), "txsc%d_",
				 atl_txsc->hw_sc_idx);
			atl_copy_stats_strings(
				&p, prefix, macsec_tx_sc_stat_descs,
				ARRAY_SIZE(macsec_tx_sc_stat_descs));
			for (assoc_num = 0; assoc_num < MACSEC_NUM_AN;
			     assoc_num++) {
				if (!test_bit(assoc_num,
					      &atl_txsc->tx_sa_idx_busy))
					continue;
				snprintf(prefix, sizeof(prefix), "txsc%d_sa%d_",
					 atl_txsc->hw_sc_idx, assoc_num);
				atl_copy_stats_strings(
					&p, prefix, macsec_tx_sa_stat_descs,
					ARRAY_SIZE(macsec_tx_sa_stat_descs));
			}
		}
		for (i = 0; i < ATL_MACSEC_MAX_SC; i++) {
			struct atl_macsec_rxsc *atl_rxsc =
				&nic->hw.macsec_cfg.atl_rxsc[i];
			int assoc_num;

			if (!(test_bit(i, &nic->hw.macsec_cfg.rxsc_idx_busy)))
				continue;

			for (assoc_num = 0; assoc_num < MACSEC_NUM_AN;
			     assoc_num++) {
				if (!test_bit(assoc_num,
					      &atl_rxsc->rx_sa_idx_busy))
					continue;
				snprintf(prefix, sizeof(prefix), "rxsc%d_sa%d_",
					 atl_rxsc->hw_sc_idx, assoc_num);
				atl_copy_stats_strings(
					&p, prefix, macsec_rx_sa_stat_descs,
					ARRAY_SIZE(macsec_rx_sa_stat_descs));
			}
		}
#endif
		return;

	case ETH_SS_PRIV_FLAGS:
		memcpy(p, atl_priv_flags, sizeof(atl_priv_flags));
		return;
	}
}

#define atl_write_stats(stats, descs, data, type)	\
do {							\
	type *_stats = (type *)(stats);			\
	int i;						\
							\
	for (i = 0; i < ARRAY_SIZE(descs); i++)		\
		*(data)++ = _stats[descs[i].idx];	\
} while (0)


static void atl_get_ethtool_stats(struct net_device *ndev,
				  struct ethtool_stats *stats, u64 *data)
{
	struct atl_nic *nic = netdev_priv(ndev);
	int i;

	atl_update_eth_stats(nic);
	atl_update_global_stats(nic);
#if IS_ENABLED(CONFIG_MACSEC) && defined(NETIF_F_HW_MACSEC)
	atl_macsec_update_stats(&nic->hw);
#endif
	atl_write_stats(&nic->stats.tx, tx_stat_descs, data, uint64_t);
	atl_write_stats(&nic->stats.rx, rx_stat_descs, data, uint64_t);
#if IS_ENABLED(CONFIG_ATLFWD_FWD)
	atl_write_stats(&nic->stats.rx_fwd, rx_fwd_stat_descs, data, uint64_t);
#endif

	atl_write_stats(&nic->stats.eth, eth_stat_descs, data, uint64_t);

	for (i = 0; i < nic->nvecs; i++) {
		struct atl_queue_vec *qvec = &nic->qvecs[i];
		struct atl_ring_stats tmp;

		atl_get_ring_stats(&qvec->tx, &tmp);
		atl_write_stats(&tmp.tx, tx_stat_descs, data, uint64_t);
		atl_get_ring_stats(&qvec->rx, &tmp);
		atl_write_stats(&tmp.rx, rx_stat_descs, data, uint64_t);
	}

#if IS_ENABLED(CONFIG_ATLFWD_FWD_NETLINK)
	for (i = 0; i < ATL_NUM_FWD_RINGS; i++) {
		struct atl_ring_stats tmp;

		if (atlfwd_nl_is_tx_fwd_ring_created(ndev, i)) {
			atl_fwd_get_ring_stats(nic->fwd.rings[ATL_FWDIR_TX][i],
					       &tmp);
			atl_write_stats(&tmp.tx, tx_stat_descs, data, uint64_t);
		}
		if (atlfwd_nl_is_rx_fwd_ring_created(ndev, i)) {
			atl_fwd_get_ring_stats(nic->fwd.rings[ATL_FWDIR_RX][i],
					       &tmp);
			atl_write_stats(&tmp.rx, rx_stat_descs, data, uint64_t);
		}
	}
#endif
#if IS_ENABLED(CONFIG_MACSEC) && defined(NETIF_F_HW_MACSEC)
	atl_write_stats(&nic->hw.macsec_cfg.stats, macsec_stat_descs, data,
			uint64_t);

	for (i = 0; i < ATL_MACSEC_MAX_SC; i++) {
		struct atl_macsec_txsc *atl_txsc =
			&nic->hw.macsec_cfg.atl_txsc[i];
		int assoc_num;

		if (!(test_bit(i, &nic->hw.macsec_cfg.txsc_idx_busy)))
			continue;

		atl_write_stats(&atl_txsc->stats, macsec_tx_sc_stat_descs, data,
				uint64_t);

		for (assoc_num = 0; assoc_num < MACSEC_NUM_AN; assoc_num++) {
			if (!test_bit(assoc_num, &atl_txsc->tx_sa_idx_busy))
				continue;
			atl_write_stats(&atl_txsc->tx_sa_stats[assoc_num],
					macsec_tx_sa_stat_descs, data,
					uint64_t);
		}
	}
	for (i = 0; i < ATL_MACSEC_MAX_SC; i++) {
		struct atl_macsec_rxsc *atl_rxsc =
			&nic->hw.macsec_cfg.atl_rxsc[i];
		int assoc_num;

		if (!(test_bit(i, &nic->hw.macsec_cfg.rxsc_idx_busy)))
			continue;

		for (assoc_num = 0; assoc_num < MACSEC_NUM_AN; assoc_num++) {
			if (!test_bit(assoc_num, &atl_rxsc->rx_sa_idx_busy))
				continue;
			atl_write_stats(&atl_rxsc->rx_sa_stats[assoc_num],
					macsec_rx_sa_stat_descs, data, uint64_t);
		}
	}
#endif
}

static int atl_update_eee_pflags(struct atl_nic *nic)
{
	int ret = 0;
	uint8_t prtad = 0;
	uint32_t val;
	uint16_t phy_val;
	uint32_t flags = nic->priv_flags;
	struct atl_link_type *link = nic->hw.link_state.link;
	struct atl_hw *hw = &nic->hw;

	flags &= ~ATL_PF_LPI_MASK;

	if (!link || link->speed == 100)
		goto done;

	if (link->speed == 1000) {
		ret = atl_mdio_read(hw, prtad, 3, 1, &phy_val);
		if (ret)
			goto done;

		if (phy_val & BIT(9))
			flags |= ATL_PF_BIT(LPI_TX_PHY);

		if (phy_val & BIT(8))
			flags |= ATL_PF_BIT(LPI_RX_PHY);
	} else {
		ret = atl_mdio_read(hw, prtad, 3, 0xc830, &phy_val);
		if (ret)
			goto done;

		if (phy_val & BIT(0))
			flags |= ATL_PF_BIT(LPI_TX_PHY);

		ret = atl_mdio_read(hw, prtad, 3, 0xe834, &phy_val);
		if (ret)
			goto done;

		if (phy_val & BIT(0))
			flags |= ATL_PF_BIT(LPI_RX_PHY);

	}

	ret = atl_msm_read(&nic->hw, ATL_MSM_GEN_STS, &val);
	if (ret)
		goto done;

	if (val & BIT(8))
		flags |= ATL_PF_BIT(LPI_TX_MAC);
	if (val & BIT(4))
		flags |= ATL_PF_BIT(LPI_RX_MAC);

done:
	nic->priv_flags = flags;
	return ret;
}

void atl_reset_stats(struct atl_nic *nic)
{
	struct atl_queue_vec *qvec;

	/* Fetch current MSM stats */
	atl_update_eth_stats(nic);

	spin_lock(&nic->stats_lock);
	/* Adding current relative values to base makes it equal to
	 * current absolute values, thus zeroing the relative values. */
	atl_adjust_eth_stats(&nic->stats.eth_base, &nic->stats.eth, true);

	atl_for_each_qvec(nic, qvec) {
		memset(&qvec->rx.stats, 0, sizeof(qvec->rx.stats));
		memset(&qvec->tx.stats, 0, sizeof(qvec->tx.stats));
	}
	memset(&nic->stats.rx_fwd, 0, sizeof(nic->stats.rx_fwd));

	spin_unlock(&nic->stats_lock);
}

static int atl_set_pad_stripping(struct atl_nic *nic, bool on)
{
	struct atl_hw *hw = &nic->hw;
	int ret;

	ret = hw->mcp.ops->set_pad_stripping(hw, on);

	return ret;
}

int atl_set_media_detect(struct atl_nic *nic, bool on)
{
	struct atl_hw *hw = &nic->hw;
	int ret;

	ret = hw->mcp.ops->set_mediadetect(hw, on);

	return ret;
}

int atl_set_downshift(struct atl_nic *nic, bool on)
{
	struct atl_hw *hw = &nic->hw;
	int ret;

	ret = hw->mcp.ops->set_downshift(hw, on);

	return ret;
}

static uint32_t atl_get_priv_flags(struct net_device *ndev)
{
	struct atl_nic *nic = netdev_priv(ndev);

	atl_update_eee_pflags(nic);
	return nic->priv_flags;
}

static int atl_set_priv_flags(struct net_device *ndev, uint32_t flags)
{
	struct atl_nic *nic = netdev_priv(ndev);
	uint32_t diff = flags ^ nic->priv_flags;
	uint32_t curr = nic->priv_flags & ATL_PF_LPB_MASK;
	uint32_t lpb = flags & ATL_PF_LPB_MASK;
	int ret;

	if (diff & ATL_PF_RO_MASK)
		return -EINVAL;

	if (diff & ~ATL_PF_RW_MASK)
		return -EOPNOTSUPP;

	if (flags & ATL_PF_BIT(STATS_RESET))
		atl_reset_stats(nic);
	flags &= ~ATL_PF_BIT(STATS_RESET);

	if (diff & ATL_PF_BIT(STRIP_PAD)) {
		ret = atl_set_pad_stripping(nic,
			!!(flags & ATL_PF_BIT(STRIP_PAD)));
		if (ret)
			return ret;
	}

	if (diff & ATL_PF_BIT(MEDIA_DETECT)) {
		ret = atl_set_media_detect(nic,
			!!(flags & ATL_PF_BIT(MEDIA_DETECT)));
		if (ret)
			return ret;
	}

	if (diff & ATL_PF_BIT(DOWNSHIFT)) {
		ret = atl_set_downshift(nic,
			!!(flags & ATL_PF_BIT(DOWNSHIFT)));
		if (ret)
			return ret;
	}

	if (hweight32(lpb) > 1) {
		atl_nic_err("Can't enable more than one loopback simultaneously\n");
		return -EINVAL;
	}

	if (lpb & ATL_PF_BIT(LPB_SYS_DMA) && !atl_rx_linear) {
		atl_nic_err("System DMA loopback suported only in rx_linear mode\n");
		return -EINVAL;
	}

	nic->priv_flags = flags;

	if (curr)
		atl_set_loopback(nic, ffs(curr) - 1, false);

	if (lpb)
		atl_set_loopback(nic, ffs(lpb) - 1, true);

	return 0;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0))
static int atl_get_coalesce(struct net_device *ndev,
			    struct ethtool_coalesce *ec,
			    struct kernel_ethtool_coalesce *kec,
			    struct netlink_ext_ack *extack)
#else
static int atl_get_coalesce(struct net_device *ndev,
			    struct ethtool_coalesce *ec)
#endif
{
	struct atl_nic *nic = netdev_priv(ndev);

	memset(ec, 0, sizeof(*ec));
	ec->rx_coalesce_usecs = nic->rx_intr_delay;
	ec->tx_coalesce_usecs = nic->tx_intr_delay;

	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
static int atl_set_coalesce(struct net_device *ndev,
			    struct ethtool_coalesce *ec,
			    struct kernel_ethtool_coalesce *kec,
			    struct netlink_ext_ack *extack)
#else
static int atl_set_coalesce(struct net_device *ndev,
			    struct ethtool_coalesce *ec)
#endif
{
	struct atl_nic *nic = netdev_priv(ndev);

	if (ec->rx_max_coalesced_frames ||
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
		ec->use_adaptive_rx_coalesce || ec->use_adaptive_tx_coalesce ||
		ec->rx_max_coalesced_frames_irq || ec->rx_coalesce_usecs_irq ||
		ec->tx_max_coalesced_frames_irq || ec->tx_coalesce_usecs_irq ||
#endif
		ec->tx_max_coalesced_frames)
		return -EOPNOTSUPP;

	if (ec->rx_coalesce_usecs < atl_min_intr_delay ||
		ec->tx_coalesce_usecs < atl_min_intr_delay) {
		atl_nic_err("Interrupt coalescing delays less than min_intr_delay (%d uS) not supported\n",
			atl_min_intr_delay);
		return -EINVAL;
	}

	nic->rx_intr_delay = ec->rx_coalesce_usecs;
	nic->tx_intr_delay = ec->tx_coalesce_usecs;

	atl_set_intr_mod(nic);

	return 0;
}

static int atl_get_ts_info(struct net_device *ndev,
			   struct ethtool_ts_info *info)
{
	struct atl_nic *nic = netdev_priv(ndev);
	struct ptp_clock *ptp_clock;

	ethtool_op_get_ts_info(ndev, info);

	if (!nic->ptp)
		return 0;

	info->so_timestamping |=
		SOF_TIMESTAMPING_TX_HARDWARE |
		SOF_TIMESTAMPING_RX_HARDWARE |
		SOF_TIMESTAMPING_RAW_HARDWARE;

	info->tx_types = BIT(HWTSTAMP_TX_OFF) |
			 BIT(HWTSTAMP_TX_ON);

	info->rx_filters = BIT(HWTSTAMP_FILTER_NONE);

	info->rx_filters |= BIT(HWTSTAMP_FILTER_PTP_V2_L4_EVENT) |
			    BIT(HWTSTAMP_FILTER_PTP_V2_L2_EVENT) |
			    BIT(HWTSTAMP_FILTER_PTP_V2_EVENT);

	ptp_clock = atl_ptp_get_ptp_clock(nic);
	if (ptp_clock)
		info->phc_index = ptp_clock_index(ptp_clock);

	return 0;
}

struct atl_rxf_flt_desc {
	int base;
	int max;
	uint32_t rxq_bit;
	int rxq_shift;
	size_t cmd_offt;
	size_t count_offt;
	int (*get_rxf)(const struct atl_rxf_flt_desc *desc,
		struct atl_nic *nic, struct ethtool_rx_flow_spec *fsp);
	int (*set_rxf)(const struct atl_rxf_flt_desc *desc,
		struct atl_nic *nic, struct ethtool_rx_flow_spec *fsp);
	void (*update_rxf)(struct atl_nic *nic, int idx);
	int (*check_rxf)(const struct atl_rxf_flt_desc *desc,
		struct atl_nic *nic, struct ethtool_rx_flow_spec *fsp);
};

#define atl_for_each_rxf_desc(_desc)				\
for (_desc = atl_rxf_descs;					\
	_desc < atl_rxf_descs + ARRAY_SIZE(atl_rxf_descs);	\
	_desc++)

#define atl_for_each_rxf_idx(_desc, _idx)		\
	for (_idx = 0; _idx < _desc->max; _idx++)

static inline int atl_rxf_idx(const struct atl_rxf_flt_desc *desc,
	struct ethtool_rx_flow_spec *fsp)
{
	return fsp->location - desc->base;
}

static inline uint64_t atl_ring_cookie(const struct atl_rxf_flt_desc *desc,
	uint32_t cmd)
{
	if (cmd & desc->rxq_bit)
		return (cmd >> desc->rxq_shift) & ATL_RXF_RXQ_MSK;
	else if (cmd & ATL_RXF_ACT_TOHOST)
		return ATL_RXF_RING_ANY;
	else
		return RX_CLS_FLOW_DISC;
}

static int atl_rxf_get_vlan(const struct atl_rxf_flt_desc *desc,
	struct atl_nic *nic, struct ethtool_rx_flow_spec *fsp)
{
	struct atl_rxf_vlan *vlan = &nic->rxf_vlan;
	int idx = atl_rxf_idx(desc, fsp);
	uint32_t cmd = vlan->cmd[idx];

	if (!(cmd & ATL_RXF_EN))
		return -EINVAL;

	fsp->flow_type = ETHER_FLOW | FLOW_EXT;
	fsp->h_ext.vlan_tci = htons(cmd & ATL_VLAN_VID_MASK);
	fsp->m_ext.vlan_tci = htons(BIT(12) - 1);
	fsp->ring_cookie = atl_ring_cookie(desc, cmd);

	return 0;
}

static int atl_rxf_get_etype(const struct atl_rxf_flt_desc *desc,
	struct atl_nic *nic, struct ethtool_rx_flow_spec *fsp)
{
	struct atl_rxf_etype *etype = &nic->rxf_etype;
	int idx = atl_rxf_idx(desc, fsp);
	uint32_t cmd = etype->cmd[idx];

	if (!(cmd & ATL_RXF_EN))
		return -EINVAL;

	fsp->flow_type = ETHER_FLOW;
	fsp->m_u.ether_spec.h_proto = 0xffff;
	fsp->h_u.ether_spec.h_proto = htons(cmd & ATL_ETYPE_VAL_MASK);
	fsp->ring_cookie = atl_ring_cookie(desc, cmd);

	return 0;
}

static inline void atl_ntuple_swap_v6(__be32 dst[4], __be32 src[4])
{
	int i;

	for (i = 0; i < 4; i++)
		dst[i] = src[3 - i];
}

static int atl_rxf_get_ntuple(const struct atl_rxf_flt_desc *desc,
	struct atl_nic *nic, struct ethtool_rx_flow_spec *fsp)
{
	struct atl_rxf_ntuple *ntuples = &nic->rxf_ntuple;
	uint32_t idx = atl_rxf_idx(desc, fsp);
	uint32_t cmd = ntuples->cmd[idx];

	if (!(cmd & ATL_RXF_EN))
		return -EINVAL;

#ifdef ATL_HAVE_IPV6_NTUPLE
	if (cmd & ATL_NTC_V6) {
		fsp->flow_type = IPV6_USER_FLOW;
	} else
#endif
	{
		fsp->flow_type = IPV4_USER_FLOW;
		fsp->h_u.usr_ip4_spec.ip_ver = ETH_RX_NFC_IP4;
	}

	if (cmd & ATL_NTC_PROTO) {
		switch (cmd & ATL_NTC_L4_MASK) {
		case ATL_NTC_L4_TCP:
			fsp->flow_type = cmd & ATL_NTC_V6 ?
				TCP_V6_FLOW : TCP_V4_FLOW;
			break;

		case ATL_NTC_L4_UDP:
			fsp->flow_type = cmd & ATL_NTC_V6 ?
				UDP_V6_FLOW : UDP_V4_FLOW;
			break;

		case ATL_NTC_L4_SCTP:
			fsp->flow_type = cmd & ATL_NTC_V6 ?
				SCTP_V6_FLOW : SCTP_V4_FLOW;
			break;

		case ATL_NTC_L4_ICMP:
#ifdef ATL_HAVE_IPV6_NTUPLE
			if (cmd & ATL_NTC_V6) {
				fsp->h_u.usr_ip6_spec.l4_proto = IPPROTO_ICMPV6;
				fsp->m_u.usr_ip6_spec.l4_proto = 0xff;
			} else
#endif
			{
				fsp->h_u.usr_ip4_spec.proto = IPPROTO_ICMP;
				fsp->m_u.usr_ip4_spec.proto = 0xff;
			}
			break;

		default:
			return -EINVAL;
		}
	}

#ifdef ATL_HAVE_IPV6_NTUPLE
	if (cmd & ATL_NTC_V6) {
		struct ethtool_tcpip6_spec *rule = &fsp->h_u.tcp_ip6_spec;
		struct ethtool_tcpip6_spec *mask = &fsp->m_u.tcp_ip6_spec;

		if (cmd & ATL_NTC_SA) {
			atl_ntuple_swap_v6(rule->ip6src,
				ntuples->src_ip6[idx]);
			memset(mask->ip6src, 0xff, sizeof(mask->ip6src));
		}

		if (cmd & ATL_NTC_DA) {
			atl_ntuple_swap_v6(rule->ip6dst,
				ntuples->dst_ip6[idx]);
			memset(mask->ip6dst, 0xff, sizeof(mask->ip6dst));
		}

		if (cmd & ATL_NTC_SP) {
			rule->psrc = ntuples->src_port[idx];
			mask->psrc = -1;
		}

		if (cmd & ATL_NTC_DP) {
			rule->pdst = ntuples->dst_port[idx];
			mask->pdst = -1;
		}
	} else
#endif
	{
		struct ethtool_tcpip4_spec *rule = &fsp->h_u.tcp_ip4_spec;
		struct ethtool_tcpip4_spec *mask = &fsp->m_u.tcp_ip4_spec;

		if (cmd & ATL_NTC_SA) {
			rule->ip4src = ntuples->src_ip4[idx];
			mask->ip4src = -1;
		}

		if (cmd & ATL_NTC_DA) {
			rule->ip4dst = ntuples->dst_ip4[idx];
			mask->ip4dst = -1;
		}

		if (cmd & ATL_NTC_SP) {
			rule->psrc = ntuples->src_port[idx];
			mask->psrc = -1;
		}

		if (cmd & ATL_NTC_DP) {
			rule->pdst = ntuples->dst_port[idx];
			mask->pdst = -1;
		}
	}

	fsp->ring_cookie = atl_ring_cookie(desc, cmd);

	return 0;
}

static int atl_rxf_get_flex(const struct atl_rxf_flt_desc *desc,
	struct atl_nic *nic, struct ethtool_rx_flow_spec *fsp)
{
	struct atl_rxf_flex *flex = &nic->rxf_flex;
	int idx = atl_rxf_idx(desc, fsp);
	uint32_t cmd = flex->cmd[idx];

	if (!(cmd & ATL_RXF_EN))
		return -EINVAL;

	fsp->flow_type = ETHER_FLOW;
	fsp->ring_cookie = atl_ring_cookie(desc, cmd);

	return 0;
}

static int atl_check_mask(uint8_t *mask, int len, uint32_t *cmd, uint32_t flag)
{
	uint8_t first = mask[0];
	uint8_t *p;

	if (first != 0 && first != 0xff)
		return -EINVAL;

	for (p = mask; p < &mask[len]; p++)
		if (*p != first)
			return -EINVAL;

	if (first == 0xff) {
		if (cmd)
			*cmd |= flag;
		else
			return -EINVAL;
	}

	return 0;
}

static int atl_rxf_check_ring(struct atl_nic *nic, uint32_t ring)
{
	if (ring > ATL_RXF_RING_ANY)
		return -EINVAL;

	if (ring < nic->nvecs || ring == ATL_RXF_RING_ANY)
		return 0;

#if IS_ENABLED(CONFIG_ATLFWD_FWD)
	if (test_bit(ring, &nic->fwd.ring_map[ATL_FWDIR_RX]))
		return 0;
#endif

	return -EINVAL;
}

static int atl_rxf_set_ring(const struct atl_rxf_flt_desc *desc,
	struct atl_nic *nic, struct ethtool_rx_flow_spec *fsp, uint32_t *cmd)
{
	uint64_t ring_cookie = fsp->ring_cookie;
	uint32_t ring;

	if (ring_cookie == RX_CLS_FLOW_DISC)
		return 0;

	ring = ethtool_get_flow_spec_ring(ring_cookie);
	if (atl_rxf_check_ring(nic, ring)) {
		atl_nic_err("Invalid Rx filter queue %d\n", ring);
		return -EINVAL;
	}

	if (ethtool_get_flow_spec_ring_vf(ring_cookie)) {
		atl_nic_err("Rx filter queue VF must be zero");
		return -EINVAL;
	}

	*cmd |= ATL_RXF_ACT_TOHOST;

	if (ring != ATL_RXF_RING_ANY)
		*cmd |= ring << desc->rxq_shift | desc->rxq_bit;

	return 0;
}

static int atl_rxf_check_vlan_etype_common(struct ethtool_rx_flow_spec *fsp)
{
	int ret;

	ret = atl_check_mask((uint8_t *)&fsp->m_u.ether_spec.h_source,
		sizeof(fsp->m_u.ether_spec.h_source), NULL, 0);
	if (ret)
		return ret;

	ret = atl_check_mask((uint8_t *)&fsp->m_ext.data,
		sizeof(fsp->m_ext.data), NULL, 0);
	if (ret)
		return ret;

	ret = atl_check_mask((uint8_t *)&fsp->m_ext.vlan_etype,
		sizeof(fsp->m_ext.vlan_etype), NULL, 0);

	return ret;
}

static int atl_rxf_check_vlan(const struct atl_rxf_flt_desc *desc,
	struct atl_nic *nic, struct ethtool_rx_flow_spec *fsp)
{
	uint16_t vid, mask;
	int ret;

	if (fsp->flow_type != (ETHER_FLOW | FLOW_EXT)) {
		if (!(fsp->location & RX_CLS_LOC_SPECIAL))
			atl_nic_err("Only ether flow-type supported for VLAN filters\n");
		return -EINVAL;
	}

	ret = atl_rxf_check_vlan_etype_common(fsp);
	if (ret)
		return ret;

	if (fsp->m_u.ether_spec.h_proto)
		return -EINVAL;

	vid = ntohs(fsp->h_ext.vlan_tci);
	mask = ntohs(fsp->m_ext.vlan_tci);

	if (mask & 0xf000 && vid & 0xf000 & mask)
		return -EINVAL;

	if ((mask & 0xfff) != 0xfff)
		return -EINVAL;

	return 0;
}

enum atl_rxf_vlan_idx {
	ATL_VIDX_FOUND = BIT(31),
	ATL_VIDX_FREE = BIT(30),
	ATL_VIDX_REPL = BIT(29),
	ATL_VIDX_NONE = BIT(28),
	ATL_VIDX_MASK = BIT(28) - 1,
};

/* If a filter is enabled for VID, return its index ored with
 * ATL_VIDX_FOUND.  Otherwise find an unused filter index and return
 * it ored with ATL_VIDX_FREE.  If no unused filter exists and
 * try_repl is set, try finding a candidate for replacement and return
 * its index ored with ATL_VIDX_REPL. If all of the above fail,
 * return ATL_VIDX_NONE.
 *
 * A replacement candidate filter must be configured to accept
 * packets, not set to direct to a specific ring and must match a VID
 * from a VLAN subinterface.
 */
static uint32_t atl_rxf_find_vid(struct atl_nic *nic, uint16_t vid,
	bool try_repl)
{
	struct atl_rxf_vlan *vlan = &nic->rxf_vlan;
	int idx, free = vlan->available, repl = vlan->available;

	for (idx = 0; idx < vlan->available; idx++) {
		uint32_t cmd = vlan->cmd[idx];

		if (!(cmd & ATL_RXF_EN)) {
			if (free == vlan->available) {
				free = idx;
				if (vid == 0xffff)
					break;
			}
			continue;
		}

		if ((cmd & ATL_VLAN_VID_MASK) == vid)
			return idx | ATL_VIDX_FOUND;

		if (try_repl && repl == vlan->available &&
			(cmd & ATL_RXF_ACT_TOHOST) &&
			!(cmd & ATL_VLAN_RXQ)) {

			if (!test_bit(cmd & ATL_VLAN_VID_MASK, vlan->map))
				continue;

			repl = idx;
		}
	}

	if (free != vlan->available)
		return free | ATL_VIDX_FREE;

	if (try_repl && repl != vlan->available)
		return repl | ATL_VIDX_REPL;

	return ATL_VIDX_NONE;
}

static uint16_t atl_rxf_vid(struct atl_rxf_vlan *vlan, int idx)
{
	uint32_t cmd = vlan->cmd[idx];

	return cmd & ATL_RXF_EN ? cmd & ATL_VLAN_VID_MASK : 0xffff;
}

static int atl_rxf_dup_vid(struct atl_rxf_vlan *vlan, int idx, uint16_t vid)
{
	int i;

	for (i = 0; i < vlan->available; i++) {
		if (i == idx)
			continue;

		if (atl_rxf_vid(vlan, i) == vid)
			return i;
	}

	return -1;
}

static int atl_rxf_set_vlan(const struct atl_rxf_flt_desc *desc,
	struct atl_nic *nic, struct ethtool_rx_flow_spec *fsp)
{
	struct atl_rxf_vlan *vlan = &nic->rxf_vlan;
	int idx;
	int ret, promisc_delta = 0;
	uint32_t cmd = ATL_RXF_EN;
	int present;
	uint16_t old_vid, vid = ntohs(fsp->h_ext.vlan_tci) & 0xfff;

	if (!(fsp->location & RX_CLS_LOC_SPECIAL)) {
		int dup;

		idx = atl_rxf_idx(desc, fsp);
		if (idx >= vlan->available)
			return -ENOSPC;

		dup = atl_rxf_dup_vid(vlan, idx, vid);
		if (dup >= 0) {
			atl_nic_err("Can't add duplicate VLAN filter @%d (existing @%d)\n",
				idx, dup);
			return -EINVAL;
		}

		old_vid = atl_rxf_vid(vlan, idx);
		if (old_vid != 0xffff && vid != old_vid &&
			test_bit(old_vid, vlan->map)) {
			atl_nic_err("Can't overwrite Linux VLAN filter @%d VID %hd with a different VID %hd\n",
				idx, old_vid, vid);
			return -EINVAL;
		}

		ret = atl_rxf_check_vlan(desc, nic, fsp);
		if (ret)
			return ret;

	} else {
		/* atl_rxf_check_vlan() already succeeded */
		idx = atl_rxf_find_vid(nic, vid, true);

		if (idx == ATL_VIDX_NONE)
			return -EINVAL;

		/* If a filter is being added for a VID without a
		 * corresponding VLAN subdevice, and we're reusing a
		 * filter previously used for a VLAN subdevice-covered
		 * VID, the promisc count needs to be bumped (but
		 * only if filter change succeeds). */
		if ((idx & ATL_VIDX_REPL) && !test_bit(vid, vlan->map))
			promisc_delta++;

		idx &= ATL_VIDX_MASK;
		fsp->location = idx + desc->base;
	}

	cmd |= vid;

	ret = atl_rxf_set_ring(desc, nic, fsp, &cmd);
	if (ret)
		return ret;

	/* If a VLAN subdevice exists, override filter to accept
	 * packets */
	if (test_bit(vid, vlan->map))
		cmd |= ATL_RXF_ACT_TOHOST;

	present = !!(vlan->cmd[idx] & ATL_RXF_EN);
	vlan->cmd[idx] = cmd;
	vlan->promisc_count += promisc_delta;

	return !present;
}

/** Find tag with the same action or new free tag
 *  top - top inclusive tag value
 *  action - action for ActionResolverTable
 */
static inline int atl2_filter_tag_get(struct atl2_tag_policy *tags,
				      int top, u16 action)
{
	int i;

	for (i = 1; i <= top; i++)
		if ((tags[i].usage > 0) && (tags[i].action == action)) {
			tags[i].usage++;
			return i;
		}

	for (i = 1; i <= top; i++)
		if (tags[i].usage == 0) {
			tags[i].usage = 1;
			tags[i].action = action;
			return i;
		}

	return -1;
}

static inline void atl2_filter_tag_put(struct atl2_tag_policy *tags, int tag)
{
	if (tags[tag].usage > 0)
		tags[tag].usage--;
}

static int atl_rxf_set_etype(const struct atl_rxf_flt_desc *desc,
	struct atl_nic *nic, struct ethtool_rx_flow_spec *fsp)
{
	struct atl_rxf_etype *etype = &nic->rxf_etype;
	int idx = atl_rxf_idx(desc, fsp);
	int ret;
	uint32_t cmd = ATL_RXF_EN;
	int present = !!(etype->cmd[idx] & ATL_RXF_EN);

	if (fsp->flow_type != (ETHER_FLOW)) {
		atl_nic_err("Only ether flow-type supported for ethertype filters\n");
		return -EINVAL;
	}

	ret = atl_rxf_check_vlan_etype_common(fsp);
	if (ret)
		return ret;

	if (fsp->m_ext.vlan_tci)
		return -EINVAL;

	if (fsp->m_u.ether_spec.h_proto != 0xffff)
		return -EINVAL;

	if (idx >= etype->available)
		return -ENOSPC;

	cmd |= ntohs(fsp->h_u.ether_spec.h_proto);

	ret = atl_rxf_set_ring(desc, nic, fsp, &cmd);
	if (ret)
		return ret;

	if (nic->hw.new_rpf) {
		uint16_t action;

		if (!(cmd & ATL_RXF_ACT_TOHOST)) {
			action = ATL2_ACTION_DROP;
		} else if (!(cmd & ATL_ETYPE_RXQ)) {
			action = ATL2_ACTION_ASSIGN_TC(0);
		} else {
			int queue = (cmd >> ATL_ETYPE_RXQ_SHIFT) & ATL_RXF_RXQ_MSK;

			action = ATL2_ACTION_ASSIGN_QUEUE(queue);
		}

		etype->tag[idx] = atl2_filter_tag_get(etype->tags_policy,
						      etype->tag_top,
						      action);
		if (etype->tag[idx] < 0)
			return -ENOSPC;
	}

	etype->cmd[idx] = cmd;

	return !present;
}

static int atl2_rxf_l3_is_equal(struct atl2_rxf_l3 *f1, struct atl2_rxf_l3 *f2)
{
	if (f1->cmd != f2->cmd)
		return false;

	if (f1->cmd & ATL2_NTC_L3_IPV4_SA)
		if (f1->src_ip4 != f2->src_ip4)
			return false;

	if (f1->cmd & ATL2_NTC_L3_IPV4_DA)
		if (f1->dst_ip4 != f2->dst_ip4)
			return false;

	if (f1->cmd & (ATL2_NTC_L3_IPV4_PROTO | ATL2_NTC_L3_IPV6_PROTO))
		if (f1->proto != f2->proto)
			return false;

	if (f1->cmd & ATL2_NTC_L3_IPV6_SA)
		if (memcmp(f1->src_ip6, f2->src_ip6, 16))
			return false;

	if (f1->cmd & ATL2_NTC_L3_IPV6_DA)
		if (memcmp(f1->dst_ip6, f2->dst_ip6, 16))
			return false;

	return true;
}

static int atl2_rxf_l4_is_equal(struct atl2_rxf_l4 *f1, struct atl2_rxf_l4 *f2)
{
	if (f1->cmd != f2->cmd)
		return false;

	if (f1->cmd & ATL2_NTC_L4_SP)
		if (f1->src_port != f2->src_port)
			return false;

	if (f1->cmd & ATL2_NTC_L4_DP)
		if (f1->dst_port != f2->dst_port)
			return false;

	return true;
}

static void atl2_rxf_write_l3_cmd(struct atl_hw *hw, int l3_idx, bool is_ipv6,
				  uint32_t cmd)
{
	uint32_t mask = is_ipv6 ? 0xFF7F0000 : 0x0000FFFF;
	uint32_t value = (atl_read(hw, ATL2_RPF_L3_FLT(l3_idx)) & ~mask) | cmd;

	atl_write(hw, ATL2_RPF_L3_FLT(l3_idx), value);
}

static void atl2_rxf_l3_put(struct atl_hw *hw, struct atl2_rxf_l3 *l3, int idx)
{
	if (l3->usage)
		l3->usage--;

	if (!l3->usage) {
		atl2_rxf_write_l3_cmd(hw, idx, l3->cmd & ATL2_NTC_L3_IPV6_EN, 0);
		l3->cmd = 0;
	}
}

static void atl2_rxf_l3_get(struct atl2_rxf_l3 *l3, int idx,
			    const struct atl2_rxf_l3 *_l3)
{
	int i;

	l3->usage++;
	l3->cmd = _l3->cmd;
	for (i = 0; i < 4; i++) {
		l3->src_ip6[i] = _l3->src_ip6[i];
		l3->dst_ip6[i] = _l3->dst_ip6[i];
	}
	l3->proto = _l3->proto;
}

static void atl2_rxf_l4_put(struct atl_hw *hw, struct atl2_rxf_l4 *l4, int idx)
{
	if (l4->usage)
		l4->usage--;

	if (!l4->usage) {
		l4->cmd = 0;
		atl_write(hw, ATL2_RPF_L4_FLT(idx), l4->cmd);
	}
}

static void atl2_rxf_l4_get(struct atl2_rxf_l4 *l4, int idx,
			    const struct atl2_rxf_l4 *_l4)
{
	l4->usage++;
	l4->cmd = _l4->cmd;
	l4->src_port = _l4->src_port;
	l4->dst_port = _l4->dst_port;
}

static void atl2_rxf_configure_l3l4(struct atl_rxf_ntuple *ntuple, int idx,
				    struct atl2_rxf_l3 *l3,
				    struct atl2_rxf_l4 *l4)
{
	if (ntuple->cmd[idx] & ATL_NTC_PROTO)
		l3->cmd |= ntuple->cmd[idx] & ATL_NTC_V6 ?
			  ATL2_NTC_L3_IPV6_PROTO | ATL2_NTC_L3_IPV6_EN :
			  ATL2_NTC_L3_IPV4_PROTO | ATL2_NTC_L3_IPV4_EN;

	switch (ntuple->cmd[idx] & ATL_NTC_L4_MASK) {
	case ATL_NTC_L4_TCP:
		l3->cmd |= ntuple->cmd[idx] & ATL_NTC_V6 ?
			IPPROTO_TCP << ATL2_NTC_L3_IPV6_PROTO_SHIFT :
			IPPROTO_TCP << ATL2_NTC_L3_IPV4_PROTO_SHIFT;
		break;

	case ATL_NTC_L4_UDP:
		l3->cmd |= ntuple->cmd[idx] & ATL_NTC_V6 ?
			IPPROTO_UDP << ATL2_NTC_L3_IPV6_PROTO_SHIFT :
			IPPROTO_UDP << ATL2_NTC_L3_IPV4_PROTO_SHIFT;
		break;

	case ATL_NTC_L4_SCTP:
		l3->cmd |= ntuple->cmd[idx] & ATL_NTC_V6 ?
			IPPROTO_SCTP << ATL2_NTC_L3_IPV6_PROTO_SHIFT :
			IPPROTO_SCTP << ATL2_NTC_L3_IPV4_PROTO_SHIFT;
		break;

	case ATL_NTC_L4_ICMP:
#ifdef ATL_HAVE_IPV6_NTUPLE
		l3->cmd |= ntuple->cmd[idx] & ATL_NTC_V6 ?
			IPPROTO_ICMPV6 << ATL2_NTC_L3_IPV6_PROTO_SHIFT :
			IPPROTO_ICMP << ATL2_NTC_L3_IPV4_PROTO_SHIFT;
#else
		l3->cmd |= IPPROTO_ICMP << ATL2_NTC_L3_IPV4_PROTO_SHIFT;
#endif
		break;

	}

	if (ntuple->cmd[idx] & ATL_NTC_SA) {
		if (ntuple->cmd[idx] & ATL_NTC_V6) {
			l3->cmd |= ATL2_NTC_L3_IPV6_SA | ATL2_NTC_L3_IPV6_EN;
			memcpy(l3->src_ip6, ntuple->src_ip6[idx], 16);
		} else {
			l3->cmd |= ATL2_NTC_L3_IPV4_SA | ATL2_NTC_L3_IPV4_EN;
			l3->src_ip4 = ntuple->src_ip4[idx];
		}
	}
	if (ntuple->cmd[idx] & ATL_NTC_DA) {
		if (ntuple->cmd[idx] & ATL_NTC_V6) {
			l3->cmd |= ATL2_NTC_L3_IPV6_DA | ATL2_NTC_L3_IPV6_EN;
			memcpy(l3->dst_ip6, ntuple->dst_ip6[idx], 16);
		} else {
			l3->cmd |= ATL2_NTC_L3_IPV4_DA | ATL2_NTC_L3_IPV4_EN;
			l3->dst_ip4 = ntuple->dst_ip4[idx];
		}
	}
	if (ntuple->cmd[idx] & ATL_NTC_SP) {
		l4->cmd |= ATL2_NTC_L4_SP | ATL2_NTC_L4_EN;
		l4->src_port = ntuple->src_port[idx];
	}
	if (ntuple->cmd[idx] & ATL_NTC_DP) {
		l4->cmd |= ATL2_NTC_L4_DP | ATL2_NTC_L4_EN;
		l4->dst_port = ntuple->dst_port[idx];
	}
}

static int atl2_rxf_fl3l4_find_l3(struct atl_rxf_ntuple *ntuple,
				  struct atl2_rxf_l3 *l3)
{
	struct atl2_rxf_l3 *nl3 = (l3->cmd & ATL2_NTC_L3_IPV4_EN) ?
					ntuple->l3v4 : ntuple->l3v6;
	int first = (l3->cmd & ATL2_NTC_L3_IPV4_EN) ?
			ntuple->l3_v4_base_index :
			ntuple->l3_v6_base_index;
	int last = first + (l3->cmd & ATL2_NTC_L3_IPV4_EN) ?
				ntuple->l3_v4_available :
				ntuple->l3_v6_available;
	int l3_idx = -1;
	int i;

	for (i = first; i < last; i++) {
		if (atl2_rxf_l3_is_equal(&nl3[i], l3)) {
			l3_idx = i;
			break;
		}
	}
	if (l3_idx < 0)
		for (i = first; i < last; i++)
			if ((nl3[i].cmd & (ATL2_NTC_L3_IPV4_EN |
						ATL2_NTC_L3_IPV6_EN)) == 0) {
				l3_idx = i;
				break;
			}
	if (l3_idx < 0)
		return -ENOSPC;

	return l3_idx;
}

static int atl2_rxf_fl3l4_find_l4(struct atl_rxf_ntuple *ntuple,
				  struct atl2_rxf_l4 *l4)
{
	int l4_idx = -1;
	int i;

	for (i = ntuple->l4_base_index; i < ntuple->l4_available; i++) {
		if (atl2_rxf_l4_is_equal(&ntuple->l4[i], l4))
			l4_idx = i;
	}
	if (l4_idx >= 0)
		return l4_idx;

	for (i = ntuple->l4_base_index; i < ntuple->l4_available; i++) {
		if ((ntuple->l4[i].cmd & ATL2_NTC_L4_EN) == 0) {
			l4_idx = i;
			break;
		}
	}
	if (l4_idx < 0)
		return -ENOSPC;
	return l4_idx;
}

static int atl2_rxf_set_ntuple(struct atl_nic *nic,
				struct atl_rxf_ntuple *ntuple,
				int idx)
{
	struct atl2_rxf_l3 l3;
	struct atl2_rxf_l4 l4;
	struct atl2_rxf_l3 *l3_filters;
	s8 l3_idx = -1;
	s8 l4_idx = -1;

	memset(&l3, 0, sizeof(l3));
	memset(&l4, 0, sizeof(l4));
	atl2_rxf_configure_l3l4(ntuple, idx, &l3, &l4);

	/* find L3 and L4 filters */
	if (l3.cmd & (ATL2_NTC_L3_IPV4_EN | ATL2_NTC_L3_IPV6_EN)) {
		l3_idx = atl2_rxf_fl3l4_find_l3(ntuple, &l3);
		if (l3_idx < 0)
			return l3_idx;
	}

	if (l4.cmd & ATL2_NTC_L4_EN) {
		l4_idx = atl2_rxf_fl3l4_find_l4(ntuple, &l4);
		if (l4_idx < 0)
			return l4_idx;

		if (ntuple->l4_idx[idx] != l4_idx)
			atl2_rxf_l4_get(&ntuple->l4[l4_idx], l4_idx, &l4);
	}

	if (l3.cmd & (ATL2_NTC_L3_IPV4_EN | ATL2_NTC_L3_IPV6_EN)) {
		if (l3.cmd & ATL2_NTC_L3_IPV4_EN)
			l3_filters = ntuple->l3v4;
		else
			l3_filters = ntuple->l3v6;

		if (ntuple->l3_idx[idx] != l3_idx)
			atl2_rxf_l3_get(&l3_filters[l3_idx], l3_idx, &l3);
	}

	/* release old filter */
	if (ntuple->l3_idx[idx] != -1) {
		if (ntuple->is_ipv6[idx])
			l3_filters = ntuple->l3v6;
		else
			l3_filters = ntuple->l3v4;

		if (!(atl2_rxf_l3_is_equal(&l3,
					   &l3_filters[ntuple->l3_idx[idx]]))) {
			atl2_rxf_l3_put(&nic->hw,
					&l3_filters[ntuple->l3_idx[idx]],
					ntuple->l3_idx[idx]);
		}
	}
	ntuple->l3_idx[idx] = l3_idx;

	if (ntuple->l4_idx[idx] != -1)
		if (!(atl2_rxf_l4_is_equal(&l4,
					   &ntuple->l4[ntuple->l4_idx[idx]]))) {
			atl2_rxf_l4_put(&nic->hw,
					&ntuple->l4[ntuple->l4_idx[idx]],
					ntuple->l4_idx[idx]);
		}
	ntuple->l4_idx[idx] = l4_idx;

	ntuple->is_ipv6[idx] = (l3.cmd & ATL2_NTC_L3_IPV4_EN) ? false : true;

	return 0;
}

static int atl_rxf_set_ntuple(const struct atl_rxf_flt_desc *desc,
	struct atl_nic *nic, struct ethtool_rx_flow_spec *fsp)
{
	struct atl_rxf_ntuple *ntuple = &nic->rxf_ntuple;
	int idx = atl_rxf_idx(desc, fsp);
	uint32_t cmd = ATL_NTC_EN;
	int ret;
	__be16 sport, dport;
	int present = !!(ntuple->cmd[idx] & ATL_RXF_EN);

	ret = atl_rxf_set_ring(desc, nic, fsp, &cmd);
	if (ret)
		return ret;

	switch (fsp->flow_type) {
#ifdef ATL_HAVE_IPV6_NTUPLE
	case TCP_V6_FLOW:
	case UDP_V6_FLOW:
	case SCTP_V6_FLOW:
		if (fsp->m_u.tcp_ip6_spec.tclass != 0) {
			atl_nic_err("Unsupported match field\n");
			return -EINVAL;
		}
		cmd |= ATL_NTC_PROTO | ATL_NTC_V6;
		break;

	case IPV6_USER_FLOW:
		if (fsp->m_u.usr_ip6_spec.l4_4_bytes != 0 ||
		    fsp->m_u.usr_ip6_spec.tclass != 0) {
			atl_nic_err("Unsupported match field\n");
			return -EINVAL;
		}

		if (fsp->h_u.usr_ip6_spec.l4_proto == IPPROTO_ICMPV6) {
			cmd |= ATL_NTC_L4_ICMP | ATL_NTC_PROTO;
		} else if (fsp->m_u.usr_ip6_spec.l4_proto != 0) {
			atl_nic_err("Unsupported match field\n");
			return -EINVAL;
		}

		cmd |= ATL_NTC_V6;
		break;
#endif

	case TCP_V4_FLOW:
	case UDP_V4_FLOW:
	case SCTP_V4_FLOW:
		if (fsp->m_u.tcp_ip4_spec.tos != 0) {
			atl_nic_err("Unsupported match field\n");
			return -EINVAL;
		}
		cmd |= ATL_NTC_PROTO;
		break;

	case IPV4_USER_FLOW:
		if (fsp->m_u.usr_ip4_spec.l4_4_bytes != 0 ||
		    fsp->m_u.usr_ip4_spec.tos != 0 ||
		    fsp->h_u.usr_ip4_spec.ip_ver != ETH_RX_NFC_IP4) {
			atl_nic_err("Unsupported match field\n");
			return -EINVAL;
		}

		if (fsp->h_u.usr_ip4_spec.proto == IPPROTO_ICMP) {
			cmd |= ATL_NTC_L4_ICMP | ATL_NTC_PROTO;
		} else if (fsp->m_u.usr_ip4_spec.proto != 0) {
			atl_nic_err("Unsupported match field\n");
			return -EINVAL;
		}
		break;

	default:
		return -EINVAL;
	}

	switch (fsp->flow_type) {
	case TCP_V6_FLOW:
	case TCP_V4_FLOW:
		cmd |= ATL_NTC_L4_TCP;
		break;

	case UDP_V6_FLOW:
	case UDP_V4_FLOW:
		cmd |= ATL_NTC_L4_UDP;
		break;

	case SCTP_V6_FLOW:
	case SCTP_V4_FLOW:
		cmd |= ATL_NTC_L4_SCTP;
		break;
	}

#ifdef ATL_HAVE_IPV6_NTUPLE
	if (cmd & ATL_NTC_V6) {
		int i;

		if (!nic->hw.new_rpf) {
			if (idx & 3) {
				atl_nic_err("IPv6 filters only supported in locations 8 and 12\n");
				return -EINVAL;
			}

			for (i = idx + 1; i < idx + 4; i++)
				if (ntuple->cmd[i] & ATL_NTC_EN) {
					atl_nic_err("IPv6 filter %d overlaps an IPv4 filter %d\n",
						    idx, i);
					return -EINVAL;
				}
		}

		ret = atl_check_mask((uint8_t *)fsp->m_u.tcp_ip6_spec.ip6src,
			sizeof(fsp->m_u.tcp_ip6_spec.ip6src), &cmd, ATL_NTC_SA);
		if (ret)
			return ret;

		ret = atl_check_mask((uint8_t *)fsp->m_u.tcp_ip6_spec.ip6dst,
			sizeof(fsp->m_u.tcp_ip6_spec.ip6dst), &cmd, ATL_NTC_DA);
		if (ret)
			return ret;

		sport = fsp->h_u.tcp_ip6_spec.psrc;
		ret = atl_check_mask((uint8_t *)&fsp->m_u.tcp_ip6_spec.psrc,
			sizeof(fsp->m_u.tcp_ip6_spec.psrc), &cmd, ATL_NTC_SP);
		if (ret)
			return ret;

		dport = fsp->h_u.tcp_ip6_spec.pdst;
		ret = atl_check_mask((uint8_t *)&fsp->m_u.tcp_ip6_spec.pdst,
			sizeof(fsp->m_u.tcp_ip6_spec.pdst), &cmd, ATL_NTC_DP);
		if (ret)
			return ret;
	} else
#endif
	{

		ret = atl_check_mask((uint8_t *)&fsp->m_u.tcp_ip4_spec.ip4src,
			sizeof(fsp->m_u.tcp_ip4_spec.ip4src), &cmd, ATL_NTC_SA);
		if (ret)
			return ret;

		ret = atl_check_mask((uint8_t *)&fsp->m_u.tcp_ip4_spec.ip4dst,
			sizeof(fsp->m_u.tcp_ip4_spec.ip4dst), &cmd, ATL_NTC_DA);
		if (ret)
			return ret;

		sport = fsp->h_u.tcp_ip4_spec.psrc;
		ret = atl_check_mask((uint8_t *)&fsp->m_u.tcp_ip4_spec.psrc,
			sizeof(fsp->m_u.tcp_ip4_spec.psrc), &cmd, ATL_NTC_SP);
		if (ret)
			return ret;

		dport = fsp->h_u.tcp_ip4_spec.pdst;
		ret = atl_check_mask((uint8_t *)&fsp->m_u.tcp_ip4_spec.pdst,
			sizeof(fsp->m_u.tcp_ip4_spec.psrc), &cmd, ATL_NTC_DP);
		if (ret)
			return ret;
	}

#ifdef ATL_HAVE_IPV6_NTUPLE
	if (cmd & ATL_NTC_V6) {
		if (cmd & ATL_NTC_SA)
			atl_ntuple_swap_v6(ntuple->src_ip6[idx],
				fsp->h_u.tcp_ip6_spec.ip6src);

		if (cmd & ATL_NTC_DA)
			atl_ntuple_swap_v6(ntuple->dst_ip6[idx],
				fsp->h_u.tcp_ip6_spec.ip6dst);
	} else
#endif
	{
		if (cmd & ATL_NTC_SA)
			ntuple->src_ip4[idx] = fsp->h_u.tcp_ip4_spec.ip4src;

		if (cmd & ATL_NTC_DA)
			ntuple->dst_ip4[idx] = fsp->h_u.tcp_ip4_spec.ip4dst;
	}


	if (cmd & ATL_NTC_SP)
		ntuple->src_port[idx] = sport;

	if (cmd & ATL_NTC_DP)
		ntuple->dst_port[idx] = dport;

	ntuple->cmd[idx] = cmd;

	if (nic->hw.new_rpf) {
		ret = atl2_rxf_set_ntuple(nic, ntuple, idx);
		if (ret < 0)
			return ret;
	}

	return !present;
}

static int atl_rxf_set_flex(const struct atl_rxf_flt_desc *desc,
			    struct atl_nic *nic, struct ethtool_rx_flow_spec *fsp)
{
	struct atl_rxf_flex *flex = &nic->rxf_flex;
	int idx = atl_rxf_idx(desc, fsp);
	int ret;
	uint32_t cmd = ATL_RXF_EN;
	int present = !!(flex->cmd[idx] & ATL_RXF_EN);;

	ret = atl_rxf_set_ring(desc, nic, fsp, &cmd);
	if (ret)
		return ret;

	flex->cmd[idx] = cmd;

	return !present;
}

static void atl_rxf_update_vlan(struct atl_nic *nic, int idx)
{
	struct atl_rxf_vlan *vlan = &nic->rxf_vlan;
	uint32_t cmd = vlan->cmd[idx];
	struct atl_hw *hw = &nic->hw;
	u16 action;

	atl_write(&nic->hw, ATL_RX_VLAN_FLT(vlan->base_index + idx), cmd);

	if (!nic->hw.new_rpf)
		return;

	if (!(cmd & ATL_RXF_EN)) {
		atl2_act_rslvr_table_set(hw,
			hw->art_base_index + ATL2_RPF_VLAN_USER_INDEX + idx,
			0,
			0,
			ATL2_ACTION_DISABLE);
		return;
	}

	if (!(cmd & ATL_RXF_ACT_TOHOST)) {
		action = ATL2_ACTION_DROP;
	} else if (!(cmd & ATL_VLAN_RXQ)) {
		atl2_rpf_vlan_flr_tag_set(hw, 1, vlan->base_index + idx);
		return;
	} else {
		int queue = (cmd >> ATL_VLAN_RXQ_SHIFT) & ATL_RXF_RXQ_MSK;

		action = ATL2_ACTION_ASSIGN_QUEUE(queue);
	}

	atl2_rpf_vlan_flr_tag_set(hw, idx + 2, vlan->base_index + idx);
	atl2_act_rslvr_table_set(hw,
		hw->art_base_index + ATL2_RPF_VLAN_USER_INDEX + idx,
		(idx + 2) << ATL2_RPF_TAG_VLAN_OFFSET,
		ATL2_RPF_TAG_VLAN_MASK,
		action);
}

static void atl_rxf_update_etype(struct atl_nic *nic, int idx)
{
	struct atl_rxf_etype *etype = &nic->rxf_etype;
	uint32_t cmd = etype->cmd[idx];
	struct atl_hw *hw = &nic->hw;
	u16 action, index;

	atl_write(&nic->hw, ATL_RX_ETYPE_FLT(etype->base_index + idx), cmd);

	if (!nic->hw.new_rpf)
		return;

	if (!(cmd & ATL_RXF_EN)) {
		atl2_filter_tag_put(etype->tags_policy,
				    etype->tag[idx]);
		index = hw->art_base_index +
			ATL2_RPF_ET_PCP_USER_INDEX + idx;
		atl2_act_rslvr_table_set(hw,
			index,
			0,
			0,
			ATL2_ACTION_DISABLE);
		return;
	}

	atl2_rpf_etht_flr_tag_set(hw, etype->tag[idx], etype->base_index + idx);
	action = etype->tags_policy[etype->tag[idx]].action;
	index = hw->art_base_index + ATL2_RPF_ET_PCP_USER_INDEX + idx;
	atl2_act_rslvr_table_set(hw,
		index,
		etype->tag[idx] << ATL2_RPF_TAG_ET_OFFSET,
		ATL2_RPF_TAG_ET_MASK,
		action);
}

static void atl2_update_ntuple_flt(struct atl_nic *nic, int idx)
{
	struct atl_hw *hw = &nic->hw;
	struct atl_rxf_ntuple *ntuple = &nic->rxf_ntuple;
	uint32_t tag = 0, mask = 0, action, cmd;
	struct atl2_rxf_l3 *l3_filters;
	struct atl2_rxf_l3 *l3 = NULL;
	struct atl2_rxf_l4 *l4 = NULL;
	s8 l3_idx = ntuple->l3_idx[idx];
	s8 l4_idx = ntuple->l4_idx[idx];
	bool is_ipv6 = ntuple->is_ipv6[idx];

	l3_filters = ntuple->is_ipv6[idx] ? ntuple->l3v6 : ntuple->l3v4;
	if (!(ntuple->cmd[idx] & ATL_NTC_EN)) {
		if (l3_idx > -1)
			atl2_rxf_l3_put(hw, &l3_filters[l3_idx], l3_idx);

		if (l4_idx > -1)
			atl2_rxf_l4_put(hw, &ntuple->l4[l4_idx], l4_idx);

		ntuple->l4_idx[idx] = -1;
		ntuple->l3_idx[idx] = -1;
		atl2_act_rslvr_table_set(hw,
			hw->art_base_index + ATL2_RPF_L3L4_USER_INDEX + idx,
			0,
			0,
			ATL2_ACTION_DISABLE);

		return;
	}
	if (l3_idx > -1) {
		l3 = &l3_filters[l3_idx];
		cmd = l3->cmd;
		if (l3->cmd & ATL2_NTC_L3_IPV4_EN) {
			tag |= (l3_idx + 1) << ATL2_RPF_TAG_L3_V4_OFFSET;
			mask |= ATL2_RPF_TAG_L3_V4_MASK;
			cmd |= (l3_idx + 1) << 0x4;

			if (l3->cmd & ATL2_NTC_L3_IPV4_SA)
				atl2_rpf_l3_v4_sa_set(hw, l3_idx, l3->src_ip4);
			if (l3->cmd & ATL2_NTC_L3_IPV4_DA)
				atl2_rpf_l3_v4_da_set(hw, l3_idx, l3->dst_ip4);
		} else if (l3->cmd & ATL2_NTC_L3_IPV6_EN) {
			tag |= (l3_idx + 1) << ATL2_RPF_TAG_L3_V6_OFFSET;
			mask |= ATL2_RPF_TAG_L3_V6_MASK;
			cmd |= (l3_idx + 1) << 0x14;

			if (l3->cmd & ATL2_NTC_L3_IPV6_SA)
				atl2_rpf_l3_v6_sa_set(hw, l3_idx, l3->src_ip6);
			if (l3->cmd & ATL2_NTC_L3_IPV6_DA)
				atl2_rpf_l3_v6_da_set(hw, l3_idx, l3->dst_ip6);
		} else {
			WARN(1, "L3 filter invalid");
			return;
		}

		atl2_rxf_write_l3_cmd(hw, l3_idx, is_ipv6,  cmd);
	}

	if (l4_idx > -1) {
		l4 = &ntuple->l4[l4_idx];
		if (l4->cmd & ATL2_NTC_L4_EN) {
			tag |= (l4_idx + 1) << ATL2_RPF_TAG_L4_OFFSET;
			mask |= ATL2_RPF_TAG_L4_MASK;
		} else {
			WARN(1, "L4 filter invalid");
			return;
		}
		cmd = l4->cmd | (l4_idx + 1) << 0x4;
		atl_write(hw, ATL_NTUPLE_SPORT(l4_idx),
			swab16(l4->src_port));
		atl_write(hw, ATL_NTUPLE_DPORT(l4_idx),
			swab16(l4->dst_port));
		atl_write(hw, ATL2_RPF_L4_FLT(l4_idx), cmd);
	}

	if (!(ntuple->cmd[idx] & ATL_RXF_ACT_TOHOST)) {
		action = ATL2_ACTION_DROP;
	} else if (!(ntuple->cmd[idx] & ATL_NTC_RXQ)) {
		action = ATL2_ACTION_ASSIGN_TC(0);
	} else {
		int queue = (ntuple->cmd[idx] >> ATL_NTC_RXQ_SHIFT) & ATL_RXF_RXQ_MSK;

		action = ATL2_ACTION_ASSIGN_QUEUE(queue);
	}

	atl2_act_rslvr_table_set(hw,
			hw->art_base_index + ATL2_RPF_L3L4_USER_INDEX + idx,
			tag,
			mask,
			action);
}

void atl_update_ntuple_flt(struct atl_nic *nic, int idx)
{
	struct atl_hw *hw = &nic->hw;
	struct atl_rxf_ntuple *ntuple = &nic->rxf_ntuple;
	uint32_t cmd = ntuple->cmd[idx];
	int i;

	if (nic->hw.new_rpf)
		return atl2_update_ntuple_flt(nic, idx);

	if (!(cmd & ATL_NTC_EN)) {
		atl_write(hw, ATL_NTUPLE_CTRL(idx), cmd);

		return;
	}

	if (cmd & ATL_NTC_V6) {
		for (i = 0; i < 4; i++) {
			if (cmd & ATL_NTC_SA)
				atl_write(hw, ATL_NTUPLE_SADDR(idx + i),
					swab32(ntuple->src_ip6[idx][i]));

			if (cmd & ATL_NTC_DA)
				atl_write(hw, ATL_NTUPLE_DADDR(idx + i),
					swab32(ntuple->dst_ip6[idx][i]));
		}
	} else {
		if (cmd & ATL_NTC_SA)
			atl_write(hw, ATL_NTUPLE_SADDR(idx),
				swab32(ntuple->src_ip4[idx]));

		if (cmd & ATL_NTC_DA)
			atl_write(hw, ATL_NTUPLE_DADDR(idx),
				swab32(ntuple->dst_ip4[idx]));
	}

	/* ports are used by both new RPF and legacy RPF, but with different
	 * locations
	 */
	if (!nic->hw.new_rpf) {
		if (cmd & ATL_NTC_SP)
			atl_write(hw, ATL_NTUPLE_SPORT(idx),
				swab16(ntuple->src_port[idx]));

		if (cmd & ATL_NTC_DP)
			atl_write(hw, ATL_NTUPLE_DPORT(idx),
				swab16(ntuple->dst_port[idx]));
	}

	if (cmd & ATL_NTC_RXQ)
		cmd |= 1 << ATL_NTC_ACT_SHIFT;

	atl_write(hw, ATL_NTUPLE_CTRL(idx), cmd);
}

static void atl_rxf_update_flex(struct atl_nic *nic, int idx)
{
	atl_write(&nic->hw,
		  ATL_RX_FLEX_FLT_CTRL(nic->rxf_flex.base_index + idx),
		  nic->rxf_flex.cmd[idx]);

	if (nic->hw.new_rpf) {
		uint32_t action;

		atl2_rpf_flex_flr_tag_set(&nic->hw, idx + 1,
					  nic->rxf_flex.base_index + idx);

		if (!(nic->rxf_flex.cmd[idx] & ATL_FLEX_EN)) {
			action = ATL2_ACTION_DISABLE;
		} else if (!(nic->rxf_flex.cmd[idx] & ATL_RXF_ACT_TOHOST)) {
			action = ATL2_ACTION_DROP;
		} else if (!(nic->rxf_flex.cmd[idx] & ATL_FLEX_RXQ)) {
			action = ATL2_ACTION_ASSIGN_TC(0);
		} else {
			int queue = (nic->rxf_flex.cmd[idx] >> ATL_FLEX_RXQ_SHIFT) & ATL_RXF_RXQ_MSK;

			action = ATL2_ACTION_ASSIGN_QUEUE(queue);
		}
		atl2_act_rslvr_table_set(&nic->hw,
			nic->hw.art_base_index + ATL2_RPF_FLEX_USER_INDEX + idx,
			(idx + 1) << ATL2_RPF_TAG_FLEX_OFFSET,
			ATL2_RPF_TAG_FLEX_MASK,
			action);
	}
}

static struct atl_rxf_flt_desc atl_rxf_descs[] = {
	{
		.base = ATL_RXF_VLAN_BASE,
		.max = ATL_RXF_VLAN_MAX,
		.rxq_bit = ATL_VLAN_RXQ,
		.rxq_shift = ATL_VLAN_RXQ_SHIFT,
		.cmd_offt = offsetof(struct atl_nic, rxf_vlan.cmd),
		.count_offt = offsetof(struct atl_nic, rxf_vlan.count),
		.get_rxf = atl_rxf_get_vlan,
		.set_rxf = atl_rxf_set_vlan,
		.update_rxf = atl_rxf_update_vlan,
		.check_rxf = atl_rxf_check_vlan,
	},
	{
		.base = ATL_RXF_ETYPE_BASE,
		.max = ATL_RXF_ETYPE_MAX,
		.rxq_bit = ATL_ETYPE_RXQ,
		.rxq_shift = ATL_ETYPE_RXQ_SHIFT,
		.cmd_offt = offsetof(struct atl_nic, rxf_etype.cmd),
		.count_offt = offsetof(struct atl_nic, rxf_etype.count),
		.get_rxf = atl_rxf_get_etype,
		.set_rxf = atl_rxf_set_etype,
		.update_rxf = atl_rxf_update_etype,
	},
	{
		.base = ATL_RXF_NTUPLE_BASE,
		.max = ATL_RXF_NTUPLE_MAX,
		.rxq_bit = ATL_NTC_RXQ,
		.rxq_shift = ATL_NTC_RXQ_SHIFT,
		.cmd_offt = offsetof(struct atl_nic, rxf_ntuple.cmd),
		.count_offt = offsetof(struct atl_nic, rxf_ntuple.count),
		.get_rxf = atl_rxf_get_ntuple,
		.set_rxf = atl_rxf_set_ntuple,
		.update_rxf = atl_update_ntuple_flt,
	},
	{
		.base = ATL_RXF_FLEX_BASE,
		.max = ATL_RXF_FLEX_MAX,
		.rxq_bit = ATL_FLEX_RXQ,
		.rxq_shift = ATL_FLEX_RXQ_SHIFT,
		.cmd_offt = offsetof(struct atl_nic, rxf_flex.cmd),
		.count_offt = offsetof(struct atl_nic, rxf_flex.count),
		.get_rxf = atl_rxf_get_flex,
		.set_rxf = atl_rxf_set_flex,
		.update_rxf = atl_rxf_update_flex,
	},
};

s8 atl_reserve_filter(enum atl_rxf_type type)
{
	switch (type) {
	case ATL_RXF_ETYPE:
		WARN_ONCE(atl_rxf_descs[type].max != ATL_RXF_ETYPE_MAX,
			  "already reserved");
		atl_rxf_descs[type].max--;
		return atl_rxf_descs[type].max;
	case ATL_RXF_NTUPLE:
		WARN_ONCE(atl_rxf_descs[type].max != ATL_RXF_NTUPLE_MAX,
			  "already reserved");
		atl_rxf_descs[type].max--;
		return atl_rxf_descs[type].max;
	default:
		WARN_ONCE(true, "unexpected type");
		break;
	}

	return -1;
}

void atl_release_filter(enum atl_rxf_type type)
{
	switch (type) {
	case ATL_RXF_ETYPE:
		WARN_ONCE(atl_rxf_descs[type].max == ATL_RXF_ETYPE_MAX,
			  "already released");
		atl_rxf_descs[type].max++;
		break;
	case ATL_RXF_NTUPLE:
		WARN_ONCE(atl_rxf_descs[type].max == ATL_RXF_NTUPLE_MAX,
			  "already released");
		atl_rxf_descs[type].max++;
		break;
	default:
		WARN_ONCE(true, "unexpected type");
		break;
	}
}

static uint32_t *atl_rxf_cmd(const struct atl_rxf_flt_desc *desc,
	struct atl_nic *nic)
{
	return (uint32_t *)((char *)nic + desc->cmd_offt);
}

static int *atl_rxf_count(const struct atl_rxf_flt_desc *desc, struct atl_nic *nic)
{
	return (int *)((char *)nic + desc->count_offt);
}

static const struct atl_rxf_flt_desc *atl_rxf_desc(struct atl_nic *nic,
	struct ethtool_rx_flow_spec *fsp)
{
	uint32_t loc = fsp->location;
	const struct atl_rxf_flt_desc *desc;

	atl_for_each_rxf_desc(desc) {
		if (loc & RX_CLS_LOC_SPECIAL) {
			if (desc->check_rxf && !desc->check_rxf(desc, nic, fsp))
				return desc;

			continue;
		}

		if (loc < desc->base)
			return NULL;

		if (loc < desc->base + desc->max)
			return desc;
	}

	return NULL;
}

static void atl_refresh_rxf_desc(struct atl_nic *nic,
	const struct atl_rxf_flt_desc *desc)
{
	int idx;

	atl_for_each_rxf_idx(desc, idx)
		desc->update_rxf(nic, idx);

	atl_set_vlan_promisc(&nic->hw, atl_vlan_promisc_status(nic->ndev));
}

void atl_refresh_rxfs(struct atl_nic *nic)
{
	const struct atl_rxf_flt_desc *desc;

	atl_for_each_rxf_desc(desc)
		atl_refresh_rxf_desc(nic, desc);

	atl_set_vlan_promisc(&nic->hw, atl_vlan_promisc_status(nic->ndev));
}

static bool atl_vlan_pull_from_promisc(struct atl_nic *nic, uint32_t idx)
{
	struct atl_rxf_vlan *vlan = &nic->rxf_vlan;
	unsigned long *map;
	int i;
	long vid = -1;

	if (!vlan->promisc_count)
		return false;

	map = kcalloc(ATL_VID_MAP_LEN, sizeof(*map), GFP_KERNEL);
	if (!map)
		return false;

	memcpy(map, vlan->map, ATL_VID_MAP_LEN * sizeof(*map));
	for (i = 0; i < vlan->available; i++) {
		uint32_t cmd = vlan->cmd[i];

		if (cmd & ATL_RXF_EN)
			clear_bit(cmd & ATL_VLAN_VID_MASK, map);
	}

	do {
		idx &= ATL_VIDX_MASK;
		vid = find_next_bit(map, BIT(12), vid + 1);
		vlan->cmd[idx] = ATL_RXF_EN | ATL_RXF_ACT_TOHOST | vid;
		atl_rxf_update_vlan(nic, idx);
		__clear_bit(vid, map);
		vlan->promisc_count--;
		vlan->count++;
		if (vlan->promisc_count == 0)
			break;

		idx = atl_rxf_find_vid(nic, -1, false);
	} while (idx & ATL_VIDX_FREE);

	kfree(map);
	atl_set_vlan_promisc(&nic->hw, atl_vlan_promisc_status(nic->ndev));

	return true;
}

static bool atl_rxf_del_vlan_override(const struct atl_rxf_flt_desc *desc,
	struct atl_nic *nic, struct ethtool_rx_flow_spec *fsp)
{
	struct atl_rxf_vlan *vlan = &nic->rxf_vlan;
	uint32_t *cmd = &vlan->cmd[atl_rxf_idx(desc, fsp)];
	uint16_t vid = *cmd & ATL_VLAN_VID_MASK;

	if (!test_bit(vid, vlan->map))
		return false;

	/* Trying to delete filter via ethtool while VLAN subdev still
	 * exists. Just drop queue assignment. */
	*cmd &= ~ATL_VLAN_RXQ;
	return true;
}

static int atl_set_rxf(struct atl_nic *nic,
	struct ethtool_rx_flow_spec *fsp, bool delete)
{
	const struct atl_rxf_flt_desc *desc;
	uint32_t *cmd;
	int *count, ret, idx;

	desc = atl_rxf_desc(nic, fsp);
	if (!desc)
		return -EINVAL;

	count = atl_rxf_count(desc, nic);

	if (delete) {
		idx = atl_rxf_idx(desc, fsp);
		cmd = &atl_rxf_cmd(desc, nic)[idx];

		if (!(*cmd & ATL_RXF_EN))
			/* Attempting to delete non-existent filter */
			return -EINVAL;

		if (desc->base == ATL_RXF_VLAN_BASE &&
			atl_rxf_del_vlan_override(desc, nic, fsp))
			goto done;

		*cmd = 0;
		(*count)--;

		if (desc->base == ATL_RXF_VLAN_BASE &&
			atl_vlan_pull_from_promisc(nic, idx))
			/* Filter already updated by
			 * atl_vlan_pull_from_promisc(), can just
			 * return */
			return 0;
	} else {
		ret = desc->set_rxf(desc, nic, fsp);
		if (ret < 0)
			return ret;

		/* fsp->location may have been set in
		 * ->set_rxf(). Guaranteed to be valid now. */
		idx = atl_rxf_idx(desc, fsp);
		*count += ret;
	}

done:
	desc->update_rxf(nic, idx);
	return 0;
}

static void atl_get_rxf_count(struct atl_nic *nic, struct ethtool_rxnfc *rxnfc)
{
	int count = 0, max = 0;
	const struct atl_rxf_flt_desc *desc;

	atl_for_each_rxf_desc(desc) {
		count += *atl_rxf_count(desc, nic);
		max += desc->max;
	}

	rxnfc->rule_cnt = count;
	rxnfc->data = max | RX_CLS_LOC_SPECIAL;
}

static int atl_get_rxf_locs(struct atl_nic *nic, struct ethtool_rxnfc *rxnfc,
	uint32_t *rule_locs)
{
	int count = 0;
	int i;
	const struct atl_rxf_flt_desc *desc;

	atl_for_each_rxf_desc(desc)
		count += *atl_rxf_count(desc, nic);

	if (rxnfc->rule_cnt < count)
		return -EMSGSIZE;

	atl_for_each_rxf_desc(desc) {
		uint32_t *cmd = atl_rxf_cmd(desc, nic);

		atl_for_each_rxf_idx(desc, i)
			if (cmd[i] & ATL_RXF_EN)
				*rule_locs++ = i + desc->base;

	}

	rxnfc->rule_cnt = count;
	return 0;
}

static int atl_get_rxnfc(struct net_device *ndev, struct ethtool_rxnfc *rxnfc,
	uint32_t *rule_locs)
{
	struct atl_nic *nic = netdev_priv(ndev);
	struct ethtool_rx_flow_spec *fsp = &rxnfc->fs;
	int ret = -ENOTSUPP;
	const struct atl_rxf_flt_desc *desc;

	switch (rxnfc->cmd) {
	case ETHTOOL_GRXRINGS:
		rxnfc->data = nic->nvecs;
		return 0;

	case ETHTOOL_GRXCLSRLCNT:
		atl_get_rxf_count(nic, rxnfc);
		return 0;

	case ETHTOOL_GRXCLSRULE:
		desc = atl_rxf_desc(nic, fsp);
		if (!desc)
			return -EINVAL;

		memset(&fsp->h_u, 0, sizeof(fsp->h_u));
		memset(&fsp->m_u, 0, sizeof(fsp->m_u));
		memset(&fsp->h_ext, 0, sizeof(fsp->h_ext));
		memset(&fsp->m_ext, 0, sizeof(fsp->m_ext));

		ret = desc->get_rxf(desc, nic, fsp);
		break;

	case ETHTOOL_GRXCLSRLALL:
		ret = atl_get_rxf_locs(nic, rxnfc, rule_locs);
		break;

	default:
		break;
	}

	return ret;
}

static int atl_set_rxnfc(struct net_device *ndev, struct ethtool_rxnfc *rxnfc)
{
	struct atl_nic *nic = netdev_priv(ndev);
	struct ethtool_rx_flow_spec *fsp = &rxnfc->fs;

	switch (rxnfc->cmd) {
	case ETHTOOL_SRXCLSRLINS:
		return atl_set_rxf(nic, fsp, false);

	case ETHTOOL_SRXCLSRLDEL:
		return atl_set_rxf(nic, fsp, true);
	}

	return -ENOTSUPP;
}

int atl_vlan_rx_add_vid(struct net_device *ndev, __be16 proto, u16 vid)
{
	struct atl_nic *nic = netdev_priv(ndev);
	struct atl_rxf_vlan *vlan = &nic->rxf_vlan;
	int idx;

	atl_nic_dbg("Add vlan id %hd\n", vid);

	vid &= 0xfff;
	if (__test_and_set_bit(vid, vlan->map))
		/* Already created -- shouldn't happen? */
		return 0;

	vlan->vlans_active++;
	idx = atl_rxf_find_vid(nic, vid, false);

	if (idx == ATL_VIDX_NONE) {
		/* VID not found and no unused filters */
		vlan->promisc_count++;
		if (pm_runtime_active(&nic->hw.pdev->dev))
			atl_set_vlan_promisc(&nic->hw,
					atl_vlan_promisc_status(nic->ndev));
		return 0;
	}

	if (idx & ATL_VIDX_FREE) {
		/* VID not found, program unused filter */
		idx &= ATL_VIDX_MASK;
		vlan->cmd[idx] = ATL_VLAN_EN | ATL_RXF_ACT_TOHOST | vid;
		vlan->count++;
		goto update_vlan;
	}

	idx &= ATL_VIDX_MASK;
	if (vlan->cmd[idx]  & ATL_RXF_ACT_TOHOST)
		/* VID already added via ethtool */
		return 0;

	/* Ethtool filter set to drop. Override. */
	atl_nic_warn("%s: Overriding VLAN filter for VID %hd @%d set to drop\n",
		__func__, vid, idx);

	vlan->cmd[idx] = ATL_RXF_EN | ATL_RXF_ACT_TOHOST | vid;

update_vlan:
	atl_rxf_update_vlan(nic, idx);
	if (pm_runtime_active(&nic->hw.pdev->dev))
		atl_set_vlan_promisc(&nic->hw,
				atl_vlan_promisc_status(nic->ndev));

	return 0;
}

int atl_vlan_rx_kill_vid(struct net_device *ndev, __be16 proto, u16 vid)
{
	struct atl_nic *nic = netdev_priv(ndev);
	struct atl_rxf_vlan *vlan = &nic->rxf_vlan;
	uint32_t cmd;
	int idx;

	atl_nic_dbg("Kill vlan id %hd\n", vid);

	vid &= 0xfff;
	if (!__test_and_clear_bit(vid, vlan->map))
		return -EINVAL;

	vlan->vlans_active--;

	idx = atl_rxf_find_vid(nic, vid, false);
	if (!(idx & ATL_VIDX_FOUND)) {
		/* VID not present in filters, decrease promisc count */
		vlan->promisc_count--;
		goto update_vlan_promisc;
	}

	idx &= ATL_VIDX_MASK;
	cmd = vlan->cmd[idx];
	if (cmd & ATL_VLAN_RXQ)
		/* Queue explicitly set via ethtool, leave the filter
		 * intact */
		return 0;

	/* Delete filter, maybe pull vid from promisc overflow */
	vlan->cmd[idx] = 0;
	vlan->count--;
	if (!atl_vlan_pull_from_promisc(nic, idx))
		atl_rxf_update_vlan(nic, idx);

update_vlan_promisc:
	if (pm_runtime_active(&nic->hw.pdev->dev))
		atl_set_vlan_promisc(&nic->hw,
				atl_vlan_promisc_status(nic->ndev));

	return 0;
}

static void atl_get_wol(struct net_device *ndev, struct ethtool_wolinfo *wol)
{
	struct atl_nic *nic = netdev_priv(ndev);

	wol->supported = ATL_WAKE_SUPPORTED;
	wol->wolopts = nic->hw.wol_mode;
}

static int atl_set_wol(struct net_device *ndev, struct ethtool_wolinfo *wol)
{
	int ret;
	struct atl_nic *nic = netdev_priv(ndev);

	if (wol->wolopts & ~ATL_WAKE_SUPPORTED) {
		atl_nic_err("%s: unsupported WoL mode %x\n", __func__,
			wol->wolopts);
		return -EINVAL;
	}

	if (wol->wolopts)
		nic->flags |= ATL_FL_WOL;
	else
		nic->flags &= ~ATL_FL_WOL;

	nic->hw.wol_mode = wol->wolopts;

	ret = device_set_wakeup_enable(&nic->hw.pdev->dev,
		!!(nic->flags & ATL_FL_WOL));

	if (ret == -EEXIST)
		ret = 0;
	if (ret) {
		atl_nic_err("device_set_wakeup_enable failed: %d\n", -ret);
		nic->flags &= ~ATL_FL_WOL;
		nic->hw.wol_mode = 0;
	}

	return ret;
}

static int atl_ethtool_begin(struct net_device *ndev)
{
	struct atl_nic *nic = netdev_priv(ndev);
	pm_runtime_get_sync(&nic->hw.pdev->dev);
	return 0;
}

static void atl_ethtool_complete(struct net_device *ndev)
{
	struct atl_nic *nic = netdev_priv(ndev);
	pm_runtime_put(&nic->hw.pdev->dev);
}

static int atl_ethool_get_regs_len(struct net_device *ndev)
{
	return atl_get_crash_dump(ndev, NULL);
}

static void atl_ethool_get_regs(struct net_device *ndev, struct ethtool_regs *regs, void *buf)
{
	regs->version = 0;
	memset(buf, 0, regs->len);
	atl_get_crash_dump(ndev, buf);
}

const struct ethtool_ops atl_ethtool_ops = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	.supported_coalesce_params = ETHTOOL_COALESCE_USECS |
				     ETHTOOL_COALESCE_MAX_FRAMES,
#endif
	.get_link = atl_ethtool_get_link,
#ifndef ATL_HAVE_ETHTOOL_KSETTINGS
	.get_settings = atl_ethtool_get_settings,
	.set_settings = atl_ethtool_set_settings,
#else
	.get_link_ksettings = atl_ethtool_get_ksettings,
	.set_link_ksettings = atl_ethtool_set_ksettings,
#endif

	.get_rxfh_indir_size = atl_rss_tbl_size,
	.get_rxfh_key_size = atl_rss_key_size,
	.get_rxfh = atl_rss_get_rxfh,
	.set_rxfh = atl_rss_set_rxfh,
	.get_channels = atl_get_channels,
	.set_channels = atl_set_channels,
	.get_rxnfc = atl_get_rxnfc,
	.set_rxnfc = atl_set_rxnfc,
	.get_pauseparam = atl_get_pauseparam,
	.set_pauseparam = atl_set_pauseparam,
	.get_eee = atl_get_eee,
	.set_eee = atl_set_eee,
	.get_drvinfo = atl_get_drvinfo,
	.nway_reset = atl_nway_reset,
	.get_ringparam = atl_get_ringparam,
	.set_ringparam = atl_set_ringparam,
	.get_sset_count = atl_get_sset_count,
	.get_strings = atl_get_strings,
	.get_ethtool_stats = atl_get_ethtool_stats,
	.get_priv_flags = atl_get_priv_flags,
	.set_priv_flags = atl_set_priv_flags,
	.get_coalesce = atl_get_coalesce,
	.set_coalesce = atl_set_coalesce,
	.get_ts_info = atl_get_ts_info,
	.get_wol = atl_get_wol,
	.set_wol = atl_set_wol,
	.begin = atl_ethtool_begin,
	.complete = atl_ethtool_complete,
	.get_regs_len = atl_ethool_get_regs_len,
	.get_regs = atl_ethool_get_regs,
};
