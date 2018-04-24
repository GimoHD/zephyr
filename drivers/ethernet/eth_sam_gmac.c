/*
 * Copyright (c) 2016 Piotr Mienkowski
 * Copyringt (c) 2018 Antmicro Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file
 * @brief Atmel SAM MCU family Ethernet MAC (GMAC) driver.
 *
 * This is a zero-copy networking implementation of an Ethernet driver. To
 * prepare for the incoming frames the driver will permanently reserve a defined
 * amount of RX data net buffers when the interface is brought up and thus
 * reduce the total amount of RX data net buffers available to the application.
 *
 * Limitations:
 * - one shot PHY setup, no support for PHY disconnect/reconnect
 * - no statistics collection
 * - no support for devices with DCache enabled due to missing non-cacheable
 *   RAM regions in Zephyr.
 */

#define SYS_LOG_DOMAIN "dev/eth_sam"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_ETHERNET_LEVEL
#include <logging/sys_log.h>

#include <kernel.h>
#include <device.h>
#include <misc/__assert.h>
#include <misc/util.h>
#include <errno.h>
#include <stdbool.h>
#include <net/net_pkt.h>
#include <net/net_if.h>
#include <net/ethernet.h>
#include <i2c.h>
#include <soc.h>
#include "phy_sam_gmac.h"
#include "eth_sam_gmac_priv.h"

#if defined(CONFIG_PTP_CLOCK_SAM_GMAC)
#include <ptp_clock.h>
#include <net/gptp.h>
#endif

/*
 * Verify Kconfig configuration
 */
/* No need to verify things for unit tests */
#if !defined(CONFIG_NET_TEST)
#if CONFIG_NET_BUF_DATA_SIZE * CONFIG_ETH_SAM_GMAC_BUF_RX_COUNT \
	< GMAC_FRAME_SIZE_MAX
#error CONFIG_NET_BUF_DATA_SIZE * CONFIG_ETH_SAM_GMAC_BUF_RX_COUNT is \
	not large enough to hold a full frame
#endif

#if CONFIG_NET_BUF_DATA_SIZE * (CONFIG_NET_BUF_RX_COUNT - \
	CONFIG_ETH_SAM_GMAC_BUF_RX_COUNT) < GMAC_FRAME_SIZE_MAX
#error Remaining free RX data buffers (CONFIG_NET_BUF_RX_COUNT -
	CONFIG_ETH_SAM_GMAC_BUF_RX_COUNT) * CONFIG_NET_BUF_DATA_SIZE
	are not large enough to hold a full frame
#endif

#if CONFIG_NET_BUF_DATA_SIZE * CONFIG_NET_BUF_TX_COUNT \
	< GMAC_FRAME_SIZE_MAX
#pragma message "Maximum frame size GMAC driver is able to transmit " \
	"CONFIG_NET_BUF_DATA_SIZE * CONFIG_NET_BUF_TX_COUNT is smaller" \
	"than a full Ethernet frame"
#endif

#if CONFIG_NET_BUF_DATA_SIZE & 0x3F
#pragma message "CONFIG_NET_BUF_DATA_SIZE should be a multiple of 64 bytes " \
	"due to the granularity of RX DMA"
#endif
#endif /* !CONFIG_NET_TEST */

/* RX descriptors list */
static struct gmac_desc rx_desc_que0[MAIN_QUEUE_RX_DESC_COUNT]
	__aligned(GMAC_DESC_ALIGNMENT);
static struct gmac_desc rx_desc_que12[PRIORITY_QUEUE_DESC_COUNT]
	__aligned(GMAC_DESC_ALIGNMENT);
/* TX descriptors list */
static struct gmac_desc tx_desc_que0[MAIN_QUEUE_TX_DESC_COUNT]
	__aligned(GMAC_DESC_ALIGNMENT);
static struct gmac_desc tx_desc_que12[PRIORITY_QUEUE_DESC_COUNT]
	__aligned(GMAC_DESC_ALIGNMENT);

/* RX buffer accounting list */
static struct net_buf *rx_frag_list_que0[MAIN_QUEUE_RX_DESC_COUNT];
/* TX frames accounting list */
static struct net_pkt *tx_frame_list_que0[CONFIG_NET_PKT_TX_COUNT + 1];

#define MODULO_INC(val, max) {val = (++val < max) ? val : 0; }

/*
 * Cache helpers
 */

static bool dcache_enabled;

static inline void dcache_invalidate(u32_t addr, u32_t size)
{
	if (!dcache_enabled) {
		return;
	}

	/* Make sure it is aligned to 32B */
	u32_t start_addr = addr & (u32_t)~(GMAC_DCACHE_ALIGNMENT - 1);
	u32_t size_full = size + addr - start_addr;

	SCB_InvalidateDCache_by_Addr((uint32_t *)start_addr, size_full);
}

static inline void dcache_clean(u32_t addr, u32_t size)
{
	if (!dcache_enabled) {
		return;
	}

	/* Make sure it is aligned to 32B */
	u32_t start_addr = addr & (u32_t)~(GMAC_DCACHE_ALIGNMENT - 1);
	u32_t size_full = size + addr - start_addr;

	SCB_CleanDCache_by_Addr((uint32_t *)start_addr, size_full);
}

/* gmac descriptiors helpers */

/* Get operations */
static inline u32_t gmac_desc_get_w0(struct gmac_desc *desc)
{
	dcache_invalidate((u32_t)desc, sizeof(struct gmac_desc));
	return desc->w0;
}

static inline u32_t gmac_desc_get_w1(struct gmac_desc *desc)
{
	dcache_invalidate((u32_t)desc, sizeof(struct gmac_desc));
	return desc->w1;
}

/* Set operations */
static inline void gmac_desc_set_w0(struct gmac_desc *desc, u32_t value)
{
	desc->w0 = value;
	dcache_clean((u32_t)desc, sizeof(struct gmac_desc));
}

static inline void gmac_desc_set_w1(struct gmac_desc *desc, u32_t value)
{
	desc->w1 = value;
	dcache_clean((u32_t)desc, sizeof(struct gmac_desc));
}

/* Set with 'or' operations */
static inline void gmac_desc_append_w0(struct gmac_desc *desc, u32_t value)
{
	u32_t old_value = gmac_desc_get_w0(desc);

	gmac_desc_set_w0(desc, old_value | value);
}

static inline void gmac_desc_append_w1(struct gmac_desc *desc, u32_t value)
{
	u32_t old_value = gmac_desc_get_w1(desc);

	gmac_desc_set_w1(desc, old_value | value);
}

/*
 * Reset ring buffer
 */
static void ring_buf_reset(struct ring_buf *rb)
{
	rb->head = 0;
	rb->tail = 0;
}

/*
 * Get one 32 bit item from the ring buffer
 */
static u32_t ring_buf_get(struct ring_buf *rb)
{
	u32_t val;

	__ASSERT(rb->tail != rb->head,
		 "retrieving data from empty ring buffer");

	val = rb->buf[rb->tail];
	MODULO_INC(rb->tail, rb->len);

	return val;
}

/*
 * Put one 32 bit item into the ring buffer
 */
static void ring_buf_put(struct ring_buf *rb, u32_t val)
{
	rb->buf[rb->head] = val;
	MODULO_INC(rb->head, rb->len);

	__ASSERT(rb->tail != rb->head,
		 "ring buffer overflow");
}

/*
 * Free pre-reserved RX buffers
 */
static void free_rx_bufs(struct ring_buf *rx_frag_list)
{
	struct net_buf *buf;

	for (int i = 0; i < rx_frag_list->len; i++) {
		buf = (struct net_buf *)rx_frag_list->buf;
		if (buf) {
			net_buf_unref(buf);
		}
	}
}

/*
 * Set MAC Address for frame filtering logic
 */
static void mac_addr_set(Gmac *gmac, u8_t index,
				 u8_t mac_addr[6])
{
	__ASSERT(index < 4, "index has to be in the range 0..3");

	gmac->GMAC_SA[index].GMAC_SAB =   (mac_addr[3] << 24)
					| (mac_addr[2] << 16)
					| (mac_addr[1] <<  8)
					| (mac_addr[0]);
	gmac->GMAC_SA[index].GMAC_SAT =   (mac_addr[5] <<  8)
					| (mac_addr[4]);
}

/*
 * Initialize RX descriptor list
 */
static int rx_descriptors_init(Gmac *gmac, struct gmac_queue *queue)
{
	struct gmac_desc_list *rx_desc_list = &queue->rx_desc_list;
	struct ring_buf *rx_frag_list = &queue->rx_frag_list;
	struct net_buf *rx_buf;
	u8_t *rx_buf_addr;

	__ASSERT_NO_MSG(rx_frag_list->buf);

	rx_desc_list->tail = 0;
	rx_frag_list->tail = 0;

	for (int i = 0; i < rx_desc_list->len; i++) {
		rx_buf = net_pkt_get_reserve_rx_data(0, K_NO_WAIT);
		if (rx_buf == NULL) {
			free_rx_bufs(rx_frag_list);
			SYS_LOG_ERR("Failed to reserve data net buffers");
			return -ENOBUFS;
		}

		rx_frag_list->buf[i] = (u32_t)rx_buf;

		rx_buf_addr = rx_buf->data;
		__ASSERT(!((u32_t)rx_buf_addr & ~GMAC_RXW0_ADDR),
			 "Misaligned RX buffer address");
		__ASSERT(rx_buf->size == CONFIG_NET_BUF_DATA_SIZE,
			 "Incorrect length of RX data buffer");
		/* Give ownership to GMAC and remove the wrap bit */
		gmac_desc_set_w0(&rx_desc_list->buf[i],
				 (u32_t)rx_buf_addr & GMAC_RXW0_ADDR);
		gmac_desc_set_w1(&rx_desc_list->buf[i], 0);
	}

	/* Set the wrap bit on the last descriptor */
	gmac_desc_append_w0(&rx_desc_list->buf[rx_desc_list->len - 1],
			    GMAC_RXW0_WRAP);

	return 0;
}

/*
 * Initialize TX descriptor list
 */
static void tx_descriptors_init(Gmac *gmac, struct gmac_queue *queue)
{
	struct gmac_desc_list *tx_desc_list = &queue->tx_desc_list;

	tx_desc_list->head = 0;
	tx_desc_list->tail = 0;

	for (int i = 0; i < tx_desc_list->len; i++) {
		gmac_desc_set_w0(&tx_desc_list->buf[i], 0);
		gmac_desc_set_w1(&tx_desc_list->buf[i], GMAC_TXW1_USED);
	}

	/* Set the wrap bit on the last descriptor */
	gmac_desc_append_w1(&tx_desc_list->buf[tx_desc_list->len - 1],
			    GMAC_TXW1_WRAP);

	/* Reset TX frame list */
	ring_buf_reset(&queue->tx_frames);
}

#if defined(CONFIG_PTP_CLOCK_SAM_GMAC)
static struct gptp_hdr *check_gptp_msg(struct net_if *iface,
				       struct net_pkt *pkt)
{
	struct ethernet_context *eth_ctx;
	struct gptp_hdr *gptp_hdr;
	u8_t *msg_start;

	if (net_pkt_ll_reserve(pkt)) {
		msg_start = net_pkt_ll(pkt);
	} else {
		msg_start = net_pkt_ip_data(pkt);
	}

#if defined(CONFIG_NET_VLAN)
	eth_ctx = net_if_l2_data(iface);
	if (net_eth_is_vlan_enabled(eth_ctx, iface)) {
		struct net_eth_vlan_hdr *hdr_vlan;

		hdr_vlan = (struct net_eth_vlan_hdr *)msg_start;
		if (ntohs(hdr_vlan->type) != NET_ETH_PTYPE_PTP) {
			return NULL;
		}

		gptp_hdr = (struct gptp_hdr *)(msg_start +
					sizeof(struct net_eth_vlan_hdr));
	} else
#else
	ARG_UNUSED(eth_ctx);
#endif
	{
		struct net_eth_hdr *hdr;

		hdr = (struct net_eth_hdr *)msg_start;
		if (ntohs(hdr->type) != NET_ETH_PTYPE_PTP) {
			return NULL;
		}

		gptp_hdr = (struct gptp_hdr *)(msg_start +
					sizeof(struct net_eth_hdr));
	}

	return gptp_hdr;
}

static bool need_timestamping(struct gptp_hdr *hdr)
{
	switch (hdr->message_type) {
	case GPTP_SYNC_MESSAGE:
	case GPTP_PATH_DELAY_RESP_MESSAGE:
		return true;
	default:
		return false;
	}
}

static void update_pkt_priority(struct gptp_hdr *hdr, struct net_pkt *pkt)
{
	if (GPTP_IS_EVENT_MSG(hdr->message_type)) {
		net_pkt_set_priority(pkt, NET_PRIORITY_CA);
	} else {
		net_pkt_set_priority(pkt, NET_PRIORITY_IC);
	}
}
#endif

static inline struct net_if *get_iface(struct eth_sam_dev_data *ctx,
				       u16_t vlan_tag)
{
#if defined(CONFIG_NET_VLAN)
	struct net_if *iface;

	iface = net_eth_get_vlan_iface(ctx->iface, vlan_tag);
	if (!iface) {
		return ctx->iface;
	}

	return iface;
#else
	ARG_UNUSED(vlan_tag);

	return ctx->iface;
#endif
}

/*
 * Process successfully sent packets
 */
static void tx_completed(Gmac *gmac, struct gmac_queue *queue)
{
	struct gmac_desc_list *tx_desc_list = &queue->tx_desc_list;
	struct gmac_desc *tx_desc;
	struct net_pkt *pkt;
#if defined(CONFIG_PTP_CLOCK_SAM_GMAC)
	u16_t vlan_tag = NET_VLAN_TAG_UNSPEC;
	struct net_ptp_time timestamp;
	struct gptp_hdr *hdr;
	struct eth_sam_dev_data *dev_data =
		CONTAINER_OF(queue, struct eth_sam_dev_data, queue_list);
#endif

	__ASSERT(gmac_desc_get_w1(&tx_desc_list->buf[tx_desc_list->tail])
		 & GMAC_TXW1_USED,
		 "first buffer of a frame is not marked as own by GMAC");

	while (tx_desc_list->tail != tx_desc_list->head) {

		tx_desc = &tx_desc_list->buf[tx_desc_list->tail];
		MODULO_INC(tx_desc_list->tail, tx_desc_list->len);
		k_sem_give(&queue->tx_desc_sem);

		if (gmac_desc_get_w1(tx_desc) & GMAC_TXW1_LASTBUFFER) {
			/* Release net buffer to the buffer pool */
			pkt = UINT_TO_POINTER(ring_buf_get(&queue->tx_frames));
#if defined(CONFIG_PTP_CLOCK_SAM_GMAC)
			timestamp.second =
				((u64_t)(gmac->GMAC_PEFTSH & 0xffff) << 32)
				| gmac->GMAC_PEFTSL;
			timestamp.nanosecond = gmac->GMAC_PEFTN;

			net_pkt_set_timestamp(pkt, &timestamp);

#if defined(CONFIG_NET_VLAN)
			struct net_eth_hdr *eth_hdr = NET_ETH_HDR(pkt);

			if (ntohs(eth_hdr->type) == NET_ETH_PTYPE_VLAN) {
				vlan_tag = net_pkt_vlan_tag(pkt);
			}
#endif
			hdr = check_gptp_msg(get_iface(dev_data, vlan_tag),
					     pkt);
			if (hdr && need_timestamping(hdr)) {
				net_if_add_tx_timestamp(pkt);
			}
#endif
			net_pkt_unref(pkt);
			SYS_LOG_DBG("Dropping pkt %p", pkt);

			break;
		}
	}
}

/*
 * Reset TX queue when errors are detected
 */
static void tx_error_handler(Gmac *gmac, struct gmac_queue *queue)
{
	struct net_pkt *pkt;
	struct ring_buf *tx_frames = &queue->tx_frames;

	queue->err_tx_flushed_count++;

	/* Stop transmission, clean transmit pipeline and control registers */
	gmac->GMAC_NCR &= ~GMAC_NCR_TXEN;

	/* Free all pkt resources in the TX path */
	while (tx_frames->tail != tx_frames->head) {
		/* Release net buffer to the buffer pool */
		pkt = UINT_TO_POINTER(tx_frames->buf[tx_frames->tail]);
		net_pkt_unref(pkt);
		SYS_LOG_DBG("Dropping pkt %p", pkt);
		MODULO_INC(tx_frames->tail, tx_frames->len);
	}

	/* Reinitialize TX descriptor list */
	k_sem_reset(&queue->tx_desc_sem);
	tx_descriptors_init(gmac, queue);
	for (int i = 0; i < queue->tx_desc_list.len - 1; i++) {
		k_sem_give(&queue->tx_desc_sem);
	}

	/* Restart transmission */
	gmac->GMAC_NCR |=  GMAC_NCR_TXEN;
}

/*
 * Clean RX queue, any received data still stored in the buffers is abandoned.
 */
static void rx_error_handler(Gmac *gmac, struct gmac_queue *queue)
{
	queue->err_rx_flushed_count++;

	/* Stop reception */
	gmac->GMAC_NCR &= ~GMAC_NCR_RXEN;

	queue->rx_desc_list.tail = 0;
	queue->rx_frag_list.tail = 0;

	for (int i = 0; i < queue->rx_desc_list.len; i++) {
		gmac_desc_set_w1(&queue->rx_desc_list.buf[i], 0);
		gmac_desc_set_w0(&queue->rx_desc_list.buf[i],
		       gmac_desc_get_w0(&queue->rx_desc_list.buf[i])
		       & ~GMAC_RXW0_OWNERSHIP);
	}

	/* Set Receive Buffer Queue Pointer Register */
	gmac->GMAC_RBQB = (u32_t)queue->rx_desc_list.buf;

	/* Restart reception */
	gmac->GMAC_NCR |=  GMAC_NCR_RXEN;
}

/*
 * Set MCK to MDC clock divisor.
 *
 * According to 802.3 MDC should be less then 2.5 MHz.
 */
static int get_mck_clock_divisor(u32_t mck)
{
	u32_t mck_divisor;

	if (mck <= 20000000) {
		mck_divisor = GMAC_NCFGR_CLK_MCK_8;
	} else if (mck <= 40000000) {
		mck_divisor = GMAC_NCFGR_CLK_MCK_16;
	} else if (mck <= 80000000) {
		mck_divisor = GMAC_NCFGR_CLK_MCK_32;
	} else if (mck <= 120000000) {
		mck_divisor = GMAC_NCFGR_CLK_MCK_48;
	} else if (mck <= 160000000) {
		mck_divisor = GMAC_NCFGR_CLK_MCK_64;
	} else if (mck <= 240000000) {
		mck_divisor = GMAC_NCFGR_CLK_MCK_96;
	} else {
		SYS_LOG_ERR("No valid MDC clock");
		mck_divisor = -ENOTSUP;
	}

	return mck_divisor;
}

static int gmac_init(Gmac *gmac, u32_t gmac_ncfgr_val)
{
	int mck_divisor;

	mck_divisor = get_mck_clock_divisor(SOC_ATMEL_SAM_MCK_FREQ_HZ);
	if (mck_divisor < 0) {
		return mck_divisor;
	}

	/* Set Network Control Register to its default value, clear stats. */
	gmac->GMAC_NCR = GMAC_NCR_CLRSTAT;

	/* Disable all interrupts */
	gmac->GMAC_IDR = UINT32_MAX;
	gmac->GMAC_IDRPQ[GMAC_QUE_1 - 1] = UINT32_MAX;
	gmac->GMAC_IDRPQ[GMAC_QUE_2 - 1] = UINT32_MAX;
	/* Clear all interrupts */
	(void)gmac->GMAC_ISR;
	(void)gmac->GMAC_ISRPQ[GMAC_QUE_1 - 1];
	(void)gmac->GMAC_ISRPQ[GMAC_QUE_2 - 1];
	/* Setup Hash Registers - enable reception of all multicast frames when
	 * GMAC_NCFGR_MTIHEN is set.
	 */
	gmac->GMAC_HRB = UINT32_MAX;
	gmac->GMAC_HRT = UINT32_MAX;
	/* Setup Network Configuration Register */
	gmac->GMAC_NCFGR = gmac_ncfgr_val | mck_divisor;

#ifdef CONFIG_ETH_SAM_GMAC_MII
	/* Setup MII Interface to the Physical Layer, RMII is the default */
	gmac->GMAC_UR = GMAC_UR_RMII; /* setting RMII to 1 selects MII mode */
#endif

#if defined(CONFIG_PTP_CLOCK_SAM_GMAC)
	/* Initialize PTP Clock Registers */
	gmac->GMAC_TI = GMAC_TI_CNS(1);
	gmac->GMAC_TISUBN = 0;
	gmac->GMAC_TN = 0;
	gmac->GMAC_TSH = 0;
	gmac->GMAC_TSL = 0;
#endif

	return 0;
}

static void link_configure(Gmac *gmac, u32_t flags)
{
	u32_t val;

	gmac->GMAC_NCR &= ~(GMAC_NCR_RXEN | GMAC_NCR_TXEN);

	val = gmac->GMAC_NCFGR;

	val &= ~(GMAC_NCFGR_FD | GMAC_NCFGR_SPD);
	val |= flags & (GMAC_NCFGR_FD | GMAC_NCFGR_SPD);

	gmac->GMAC_NCFGR = val;

	gmac->GMAC_UR = 0;  /* Select RMII mode */
	gmac->GMAC_NCR |= (GMAC_NCR_RXEN | GMAC_NCR_TXEN);
}

static int queue_init(Gmac *gmac, struct gmac_queue *queue)
{
	int result;

	__ASSERT_NO_MSG(queue->rx_desc_list.len > 0);
	__ASSERT_NO_MSG(queue->tx_desc_list.len > 0);
	__ASSERT(!((u32_t)queue->rx_desc_list.buf & ~GMAC_RBQB_ADDR_Msk),
		 "RX descriptors have to be word aligned");
	__ASSERT(!((u32_t)queue->tx_desc_list.buf & ~GMAC_TBQB_ADDR_Msk),
		 "TX descriptors have to be word aligned");

	/* Setup descriptor lists */
	result = rx_descriptors_init(gmac, queue);
	if (result < 0) {
		return result;
	}

	tx_descriptors_init(gmac, queue);

	/* Initialize TX descriptors semaphore. The semaphore is required as the
	 * size of the TX descriptor list is limited while the number of TX data
	 * buffers is not.
	 */
	k_sem_init(&queue->tx_desc_sem, queue->tx_desc_list.len - 1,
		   queue->tx_desc_list.len - 1);

	/* Set Receive Buffer Queue Pointer Register */
	gmac->GMAC_RBQB = (u32_t)queue->rx_desc_list.buf;
	/* Set Transmit Buffer Queue Pointer Register */
	gmac->GMAC_TBQB = (u32_t)queue->tx_desc_list.buf;

	/* Configure GMAC DMA transfer */
	gmac->GMAC_DCFGR =
		  /* Receive Buffer Size (defined in multiples of 64 bytes) */
		  GMAC_DCFGR_DRBS(CONFIG_NET_BUF_DATA_SIZE >> 6)
		  /* 4 kB Receiver Packet Buffer Memory Size */
		| GMAC_DCFGR_RXBMS_FULL
		  /* 4 kB Transmitter Packet Buffer Memory Size */
		| GMAC_DCFGR_TXPBMS
		  /* Transmitter Checksum Generation Offload Enable */
		| GMAC_DCFGR_TXCOEN
		  /* Attempt to use INCR4 AHB bursts (Default) */
		| GMAC_DCFGR_FBLDO_INCR4;

	/* Setup RX/TX completion and error interrupts */
	gmac->GMAC_IER = GMAC_INT_EN_FLAGS;

	queue->err_rx_frames_dropped = 0;
	queue->err_rx_flushed_count = 0;
	queue->err_tx_flushed_count = 0;

	SYS_LOG_INF("Queue %d activated", queue->que_idx);

	return 0;
}

static int priority_queue_init_as_idle(Gmac *gmac, struct gmac_queue *queue)
{
	struct gmac_desc_list *rx_desc_list = &queue->rx_desc_list;
	struct gmac_desc_list *tx_desc_list = &queue->tx_desc_list;

	__ASSERT(!((u32_t)rx_desc_list->buf & ~GMAC_RBQB_ADDR_Msk),
		 "RX descriptors have to be word aligned");
	__ASSERT(!((u32_t)tx_desc_list->buf & ~GMAC_TBQB_ADDR_Msk),
		 "TX descriptors have to be word aligned");
	__ASSERT((rx_desc_list->len == 1) && (tx_desc_list->len == 1),
		 "Priority queues are currently not supported, descriptor "
		 "list has to have a single entry");

	/* Setup RX descriptor lists */
	/* Take ownership from GMAC and set the wrap bit */
	gmac_desc_set_w0(&rx_desc_list->buf[0], GMAC_RXW0_WRAP);
	gmac_desc_set_w1(&rx_desc_list->buf[0], 0);
	/* Setup TX descriptor lists */
	gmac_desc_set_w0(&tx_desc_list->buf[0], 0);
	/* Take ownership from GMAC and set the wrap bit */
	gmac_desc_set_w1(&tx_desc_list->buf[0],
			 GMAC_TXW1_USED | GMAC_TXW1_WRAP);

	/* Set Receive Buffer Queue Pointer Register */
	gmac->GMAC_RBQBAPQ[queue->que_idx - 1] = (u32_t)rx_desc_list->buf;
	/* Set Transmit Buffer Queue Pointer Register */
	gmac->GMAC_TBQBAPQ[queue->que_idx - 1] = (u32_t)tx_desc_list->buf;

	return 0;
}

static struct net_pkt *frame_get(struct gmac_queue *queue)
{
	struct gmac_desc_list *rx_desc_list = &queue->rx_desc_list;
	struct gmac_desc *rx_desc;
	struct ring_buf *rx_frag_list = &queue->rx_frag_list;
	struct net_pkt *rx_frame;
	bool frame_is_complete;
	struct net_buf *frag;
	struct net_buf *new_frag;
	struct net_buf *last_frag = NULL;
	u8_t *frag_data;
	u32_t frag_len;
	u32_t frame_len = 0;
	u16_t tail;
	u8_t wrap;

	/* Check if there exists a complete frame in RX descriptor list */
	tail = rx_desc_list->tail;
	rx_desc = &rx_desc_list->buf[tail];
	frame_is_complete = false;
	while ((gmac_desc_get_w0(rx_desc) & GMAC_RXW0_OWNERSHIP)
		&& !frame_is_complete) {
		frame_is_complete = (bool)(gmac_desc_get_w1(rx_desc)
					   & GMAC_RXW1_EOF);
		MODULO_INC(tail, rx_desc_list->len);
		rx_desc = &rx_desc_list->buf[tail];
	}
	/* Frame which is not complete can be dropped by GMAC. Do not process
	 * it, even partially.
	 */
	if (!frame_is_complete) {
		return NULL;
	}

	rx_frame = net_pkt_get_reserve_rx(0, K_NO_WAIT);

	/* Process a frame */
	tail = rx_desc_list->tail;
	rx_desc = &rx_desc_list->buf[tail];
	frame_is_complete = false;

	/* TODO: Don't assume first RX fragment will have SOF (Start of frame)
	 * bit set. If SOF bit is missing recover gracefully by dropping
	 * invalid frame.
	 */
	__ASSERT(gmac_desc_get_w1(rx_desc) & GMAC_RXW1_SOF,
		 "First RX fragment is missing SOF bit");

	/* TODO: We know already tail and head indexes of fragments containing
	 * complete frame. Loop over those indexes, don't search for them
	 * again.
	 */
	while ((gmac_desc_get_w0(rx_desc) & GMAC_RXW0_OWNERSHIP)
	       && !frame_is_complete) {
		frag = (struct net_buf *)rx_frag_list->buf[tail];
		frag_data =
			(u8_t *)(gmac_desc_get_w0(rx_desc) & GMAC_RXW0_ADDR);
		__ASSERT(frag->data == frag_data,
			 "RX descriptor and buffer list desynchronized");
		frame_is_complete =
			(bool)(gmac_desc_get_w1(rx_desc) & GMAC_RXW1_EOF);
		if (frame_is_complete) {
			frag_len = (gmac_desc_get_w1(rx_desc) & GMAC_RXW1_LEN)
				   - frame_len;
		} else {
			frag_len = CONFIG_NET_BUF_DATA_SIZE;
		}

		frame_len += frag_len;

		/* Link frame fragments only if RX net buffer is valid */
		if (rx_frame != NULL) {
			/* Assure cache coherency after DMA write operation */
			dcache_invalidate((u32_t)frag_data, frag->size);

			/* Get a new data net buffer from the buffer pool */
			new_frag = net_pkt_get_frag(rx_frame, K_NO_WAIT);
			if (new_frag == NULL) {
				queue->err_rx_frames_dropped++;
				net_pkt_unref(rx_frame);
				rx_frame = NULL;
			} else {
				net_buf_add(frag, frag_len);
				if (!last_frag) {
					net_pkt_frag_insert(rx_frame, frag);
				} else {
					net_buf_frag_insert(last_frag, frag);
				}
				last_frag = frag;
				frag = new_frag;
				rx_frag_list->buf[tail] = (u32_t)frag;
				dcache_clean((u32_t)&rx_frag_list->buf[tail],
					     sizeof(u32_t));
			}
		}

		/* Update buffer descriptor status word */
		gmac_desc_set_w1(rx_desc, 0);
		/* Guarantee that status word is written before the address
		 * word to avoid race condition.
		 */
		__DMB();  /* data memory barrier */
		/* Update buffer descriptor address word */
		wrap = (tail == rx_desc_list->len-1 ? GMAC_RXW0_WRAP : 0);
		gmac_desc_set_w0(rx_desc,
				 ((u32_t)frag->data & GMAC_RXW0_ADDR) | wrap);

		MODULO_INC(tail, rx_desc_list->len);
		rx_desc = &rx_desc_list->buf[tail];
	}

	rx_desc_list->tail = tail;
	SYS_LOG_DBG("Frame complete: rx=%p, tail=%d", rx_frame, tail);
	__ASSERT_NO_MSG(frame_is_complete);

	return rx_frame;
}

static void eth_rx(struct gmac_queue *queue)
{
	struct eth_sam_dev_data *dev_data =
		CONTAINER_OF(queue, struct eth_sam_dev_data, queue_list);
	u16_t vlan_tag = NET_VLAN_TAG_UNSPEC;
	struct net_pkt *rx_frame;
#if defined(CONFIG_PTP_CLOCK_SAM_GMAC)
	struct device *const dev = net_if_get_device(dev_data->iface);
	const struct eth_sam_dev_cfg *const cfg = DEV_CFG(dev);
	Gmac *gmac = cfg->regs;
#endif

	/* More than one frame could have been received by GMAC, get all
	 * complete frames stored in the GMAC RX descriptor list.
	 */
	rx_frame = frame_get(queue);
	while (rx_frame) {
		SYS_LOG_DBG("ETH rx");

#if defined(CONFIG_NET_VLAN)
		/* FIXME: Instead of this, use the GMAC register to get
		 * the used VLAN tag.
		 */
		{
			struct net_eth_hdr *hdr = NET_ETH_HDR(rx_frame);

			if (ntohs(hdr->type) == NET_ETH_PTYPE_VLAN) {
				struct net_eth_vlan_hdr *hdr_vlan =
					(struct net_eth_vlan_hdr *)
					NET_ETH_HDR(rx_frame);

				net_pkt_set_vlan_tci(rx_frame,
						    ntohs(hdr_vlan->vlan.tci));
				vlan_tag = net_pkt_vlan_tag(rx_frame);

#if CONFIG_NET_TC_RX_COUNT > 1
				{
					enum net_priority prio;

					prio = net_vlan2priority(
					      net_pkt_vlan_priority(rx_frame));
					net_pkt_set_priority(rx_frame, prio);
				}
#endif
			}
		}
#endif
#if defined(CONFIG_PTP_CLOCK_SAM_GMAC)
		struct gptp_hdr *hdr;
		struct net_ptp_time timestamp;

		timestamp.second = ((u64_t)(gmac->GMAC_PEFRSH & 0xffff) << 32)
			| gmac->GMAC_PEFRSL;
		timestamp.nanosecond = gmac->GMAC_PEFRN;

		net_pkt_set_timestamp(rx_frame, &timestamp);

		hdr = check_gptp_msg(get_iface(dev_data, vlan_tag), rx_frame);
		if (hdr) {
			update_pkt_priority(hdr, rx_frame);
		}
#endif /* CONFIG_PTP_CLOCK_SAM_GMAC */

		if (net_recv_data(get_iface(dev_data, vlan_tag),
				  rx_frame) < 0) {
			net_pkt_unref(rx_frame);
		}

		rx_frame = frame_get(queue);
	}
}

static int eth_tx(struct net_if *iface, struct net_pkt *pkt)
{
	struct device *const dev = net_if_get_device(iface);
	const struct eth_sam_dev_cfg *const cfg = DEV_CFG(dev);
	struct eth_sam_dev_data *const dev_data = DEV_DATA(dev);
	Gmac *gmac = cfg->regs;
	struct gmac_queue *queue = &dev_data->queue_list[0];
	struct gmac_desc_list *tx_desc_list = &queue->tx_desc_list;
	struct gmac_desc *tx_desc;
	struct net_buf *frag;
	u8_t *frag_data, *frag_orig;
	u16_t frag_len;
	u32_t err_tx_flushed_count_at_entry = queue->err_tx_flushed_count;
	unsigned int key;

	__ASSERT(pkt, "buf pointer is NULL");
	__ASSERT(pkt->frags, "Frame data missing");

	SYS_LOG_DBG("ETH tx");

	/* Store the original frag data pointer */
	frag_orig = pkt->frags->data;

	/* First fragment is special - it contains link layer (Ethernet
	 * in our case) header. Modify the data pointer to account for more data
	 * in the beginning of the buffer.
	 */
	net_buf_push(pkt->frags, net_pkt_ll_reserve(pkt));

	frag = pkt->frags;
	while (frag) {
		frag_data = frag->data;
		frag_len = frag->len;

		/* Assure cache coherency before DMA read operation */
		dcache_clean((u32_t)frag_data, frag->size);

		k_sem_take(&queue->tx_desc_sem, K_FOREVER);

		/* The following section becomes critical and requires IRQ lock
		 * / unlock protection only due to the possibility of executing
		 * tx_error_handler() function.
		 */
		key = irq_lock();

		/* Check if tx_error_handler() function was executed */
		if (queue->err_tx_flushed_count != err_tx_flushed_count_at_entry) {
			irq_unlock(key);
			return -EIO;
		}

		tx_desc = &tx_desc_list->buf[tx_desc_list->head];

		/* Update buffer descriptor address word */
		gmac_desc_set_w0(tx_desc, (u32_t)frag_data);

		/* Guarantee that address word is written before the status
		 * word to avoid race condition.
		 */
		__DMB();  /* data memory barrier */
		/* Update buffer descriptor status word (clear used bit) */
		gmac_desc_set_w1(tx_desc,
			  (frag_len & GMAC_TXW1_LEN)
			| (!frag->frags ? GMAC_TXW1_LASTBUFFER : 0)
			| (tx_desc_list->head == tx_desc_list->len - 1
			   ? GMAC_TXW1_WRAP : 0));

		/* Update descriptor position */
		MODULO_INC(tx_desc_list->head, tx_desc_list->len);

		__ASSERT(tx_desc_list->head != tx_desc_list->tail,
			 "tx_desc_list overflow");

		irq_unlock(key);

		/* Continue with the rest of fragments (only data) */
		frag = frag->frags;
	}

	/* Restore the original frag data pointer */
	pkt->frags->data = frag_orig;

	key = irq_lock();

	/* Check if tx_error_handler() function was executed */
	if (queue->err_tx_flushed_count != err_tx_flushed_count_at_entry) {
		irq_unlock(key);
		return -EIO;
	}

	/* Ensure the descriptor following the last one is marked as used */
	tx_desc = &tx_desc_list->buf[tx_desc_list->head];
	gmac_desc_append_w1(tx_desc, GMAC_TXW1_USED);

	/* Account for a sent frame */
	ring_buf_put(&queue->tx_frames, POINTER_TO_UINT(pkt));

	irq_unlock(key);

	/* Start transmission */
	gmac->GMAC_NCR |= GMAC_NCR_TSTART;

	return 0;
}

static void queue0_isr(void *arg)
{
	struct device *const dev = (struct device *const)arg;
	const struct eth_sam_dev_cfg *const cfg = DEV_CFG(dev);
	struct eth_sam_dev_data *const dev_data = DEV_DATA(dev);
	Gmac *gmac = cfg->regs;
	struct gmac_queue *queue = &dev_data->queue_list[0];
	struct gmac_desc *tail_desc;
	u32_t isr;

	/* Interrupt Status Register is cleared on read */
	isr = gmac->GMAC_ISR;
	SYS_LOG_DBG("GMAC_ISR=0x%08x", isr);

	/* RX packet */
	if (isr & GMAC_INT_RX_ERR_BITS) {
		rx_error_handler(gmac, queue);
	} else if (isr & GMAC_ISR_RCOMP) {
		tail_desc = &queue->rx_desc_list.buf[queue->rx_desc_list.tail];
		SYS_LOG_DBG("rx.w1=0x%08x, tail=%d",
			    gmac_desc_get_w1(tail_desc),
			    queue->rx_desc_list.tail);
		eth_rx(queue);
	}

	/* TX packet */
	if (isr & GMAC_INT_TX_ERR_BITS) {
		tx_error_handler(gmac, queue);
	} else if (isr & GMAC_ISR_TCOMP) {
		tail_desc = &queue->tx_desc_list.buf[queue->tx_desc_list.tail];
		SYS_LOG_DBG("tx.w1=0x%08x, tail=%d",
			    gmac_desc_get_w1(tail_desc),
			    queue->tx_desc_list.tail);
		tx_completed(gmac, queue);
	}

	if (isr & GMAC_IER_HRESP) {
		SYS_LOG_DBG("HRESP");
	}
}

static int eth_initialize(struct device *dev)
{
	const struct eth_sam_dev_cfg *const cfg = DEV_CFG(dev);

	cfg->config_func();

	/* Enable GMAC module's clock */
	soc_pmc_peripheral_enable(cfg->periph_id);

	/* Connect pins to the peripheral */
	soc_gpio_list_configure(cfg->pin_list, cfg->pin_list_size);

	return 0;
}

#ifdef CONFIG_ETH_SAM_GMAC_MAC_I2C_EEPROM
void get_mac_addr_from_i2c_eeprom(u8_t mac_addr[6])
{
	struct device *dev;
	u32_t iaddr = CONFIG_ETH_SAM_GMAC_MAC_I2C_INT_ADDRESS;

	dev = device_get_binding(CONFIG_ETH_SAM_GMAC_MAC_I2C_DEV_NAME);
	if (!dev) {
		SYS_LOG_ERR("I2C: Device not found");
		return;
	}

	i2c_burst_read_addr(dev, CONFIG_ETH_SAM_GMAC_MAC_I2C_SLAVE_ADDRESS,
			    (u8_t *)&iaddr,
			    CONFIG_ETH_SAM_GMAC_MAC_I2C_INT_ADDRESS_SIZE,
			    mac_addr, 6);
}
#endif

static void eth0_iface_init(struct net_if *iface)
{
	struct device *const dev = net_if_get_device(iface);
	struct eth_sam_dev_data *const dev_data = DEV_DATA(dev);
	const struct eth_sam_dev_cfg *const cfg = DEV_CFG(dev);
	static bool init_done;
	u32_t gmac_ncfgr_val;
	u32_t link_status;
	int result;

	/* For VLAN, this value is only used to get the correct L2 driver */
	dev_data->iface = iface;

	ethernet_init(iface);

	/* The rest of initialization should only be done once */
	if (init_done) {
		return;
	}

	/* Check the status of data caches */
	dcache_enabled = (SCB->CCR & SCB_CCR_DC_Msk);

	/* Initialize GMAC driver, maximum frame length is 1518 bytes */
	gmac_ncfgr_val =
		  GMAC_NCFGR_MTIHEN  /* Multicast Hash Enable */
		| GMAC_NCFGR_LFERD   /* Length Field Error Frame Discard */
		| GMAC_NCFGR_RFCS    /* Remove Frame Check Sequence */
		| GMAC_NCFGR_RXCOEN; /* Receive Checksum Offload Enable */
	result = gmac_init(cfg->regs, gmac_ncfgr_val);
	if (result < 0) {
		SYS_LOG_ERR("Unable to initialize ETH driver");
		return;
	}

#ifdef CONFIG_ETH_SAM_GMAC_MAC_I2C_EEPROM
	/* Read MAC address from an external EEPROM */
	get_mac_addr_from_i2c_eeprom(dev_data->mac_addr);
#endif

	SYS_LOG_INF("MAC: %x:%x:%x:%x:%x:%x",
		    dev_data->mac_addr[0], dev_data->mac_addr[1],
		    dev_data->mac_addr[2], dev_data->mac_addr[3],
		    dev_data->mac_addr[4], dev_data->mac_addr[5]);

	/* Set MAC Address for frame filtering logic */
	mac_addr_set(cfg->regs, 0, dev_data->mac_addr);

	/* Register Ethernet MAC Address with the upper layer */
	net_if_set_link_addr(iface, dev_data->mac_addr,
			     sizeof(dev_data->mac_addr),
			     NET_LINK_ETHERNET);

	/* Initialize GMAC queues */
	/* Note: Queues 1 and 2 are not used, configured to stay idle */
	priority_queue_init_as_idle(cfg->regs, &dev_data->queue_list[2]);
	priority_queue_init_as_idle(cfg->regs, &dev_data->queue_list[1]);
	result = queue_init(cfg->regs, &dev_data->queue_list[0]);
	if (result < 0) {
		SYS_LOG_ERR("Unable to initialize ETH queue");
		return;
	}

	/* PHY initialize */
	result = phy_sam_gmac_init(&cfg->phy);
	if (result < 0) {
		SYS_LOG_ERR("ETH PHY Initialization Error");
		return;
	}
	/* PHY auto-negotiate link parameters */
	result = phy_sam_gmac_auto_negotiate(&cfg->phy, &link_status);
	if (result < 0) {
		SYS_LOG_ERR("ETH PHY auto-negotiate sequence failed");
		return;
	}

	/* Set up link parameters */
	link_configure(cfg->regs, link_status);

	init_done = true;
}

static enum ethernet_hw_caps eth_sam_gmac_get_capabilities(struct device *dev)
{
	ARG_UNUSED(dev);

	return ETHERNET_HW_VLAN | ETHERNET_LINK_10BASE_T |
#if defined(CONFIG_PTP_CLOCK_SAM_GMAC)
		ETHERNET_PTP |
#endif
		ETHERNET_LINK_100BASE_T;
}

#if defined(CONFIG_PTP_CLOCK_SAM_GMAC)
static struct device *eth_sam_gmac_get_ptp_clock(struct device *dev)
{
	struct eth_sam_dev_data *const dev_data = DEV_DATA(dev);

	return dev_data->ptp_clock;
}
#endif

static const struct ethernet_api eth_api = {
	.iface_api.init = eth0_iface_init,
	.iface_api.send = eth_tx,

	.get_capabilities = eth_sam_gmac_get_capabilities,

#if defined(CONFIG_PTP_CLOCK_SAM_GMAC)
	.get_ptp_clock = eth_sam_gmac_get_ptp_clock,
#endif
};

static struct device DEVICE_NAME_GET(eth0_sam_gmac);

static void eth0_irq_config(void)
{
	IRQ_CONNECT(GMAC_IRQn, CONFIG_ETH_SAM_GMAC_IRQ_PRI, queue0_isr,
		    DEVICE_GET(eth0_sam_gmac), 0);
	irq_enable(GMAC_IRQn);
}

static const struct soc_gpio_pin pins_eth0[] = PINS_GMAC0;

static const struct eth_sam_dev_cfg eth0_config = {
	.regs = GMAC,
	.periph_id = ID_GMAC,
	.pin_list = pins_eth0,
	.pin_list_size = ARRAY_SIZE(pins_eth0),
	.config_func = eth0_irq_config,
	.phy = {GMAC, CONFIG_ETH_SAM_GMAC_PHY_ADDR},
};

static struct eth_sam_dev_data eth0_data = {
#ifdef CONFIG_ETH_SAM_GMAC_MAC_MANUAL
	.mac_addr = {
		CONFIG_ETH_SAM_GMAC_MAC0,
		CONFIG_ETH_SAM_GMAC_MAC1,
		CONFIG_ETH_SAM_GMAC_MAC2,
		CONFIG_ETH_SAM_GMAC_MAC3,
		CONFIG_ETH_SAM_GMAC_MAC4,
		CONFIG_ETH_SAM_GMAC_MAC5,
	},
#endif
	.queue_list = {{
			.que_idx = GMAC_QUE_0,
			.rx_desc_list = {
				.buf = rx_desc_que0,
				.len = ARRAY_SIZE(rx_desc_que0),
			},
			.tx_desc_list = {
				.buf = tx_desc_que0,
				.len = ARRAY_SIZE(tx_desc_que0),
			},
			.rx_frag_list = {
				.buf = (u32_t *)rx_frag_list_que0,
				.len = ARRAY_SIZE(rx_frag_list_que0),
			},
			.tx_frames = {
				.buf = (u32_t *)tx_frame_list_que0,
				.len = ARRAY_SIZE(tx_frame_list_que0),
			},
		}, {
			.que_idx = GMAC_QUE_1,
			.rx_desc_list = {
				.buf = rx_desc_que12,
				.len = ARRAY_SIZE(rx_desc_que12),
			},
			.tx_desc_list = {
				.buf = tx_desc_que12,
				.len = ARRAY_SIZE(tx_desc_que12),
			},
		}, {
			.que_idx = GMAC_QUE_2,
			.rx_desc_list = {
				.buf = rx_desc_que12,
				.len = ARRAY_SIZE(rx_desc_que12),
			},
			.tx_desc_list = {
				.buf = tx_desc_que12,
				.len = ARRAY_SIZE(tx_desc_que12),
			},
		}
	},
};

ETH_NET_DEVICE_INIT(eth0_sam_gmac, CONFIG_ETH_SAM_GMAC_NAME, eth_initialize,
		    &eth0_data, &eth0_config, CONFIG_ETH_INIT_PRIORITY,
		    &eth_api, GMAC_MTU);

#if defined(CONFIG_PTP_CLOCK_SAM_GMAC)
struct ptp_context {
	struct device *eth_dev;
};

static struct ptp_context ptp_gmac_0_context;

static int ptp_clock_sam_gmac_set(struct device *dev,
				  struct net_ptp_time *tm)
{
	struct ptp_context *ptp_context = dev->driver_data;
	const struct eth_sam_dev_cfg *const cfg = DEV_CFG(ptp_context->eth_dev);
	Gmac *gmac = cfg->regs;

	gmac->GMAC_TSH = tm->_sec.high & 0xffff;
	gmac->GMAC_TSL = tm->_sec.low & 0xffffffff;
	gmac->GMAC_TN = tm->nanosecond & 0xffffffff;

	return 0;
}

static int ptp_clock_sam_gmac_get(struct device *dev,
				  struct net_ptp_time *tm)
{
	struct ptp_context *ptp_context = dev->driver_data;
	const struct eth_sam_dev_cfg *const cfg = DEV_CFG(ptp_context->eth_dev);
	Gmac *gmac = cfg->regs;

	tm->second = ((u64_t)(gmac->GMAC_TSH & 0xffff) << 32) | gmac->GMAC_TSL;
	tm->nanosecond = gmac->GMAC_TN;

	return 0;
}

static int ptp_clock_sam_gmac_adjust(struct device *dev, int increment)
{
	struct ptp_context *ptp_context = dev->driver_data;
	const struct eth_sam_dev_cfg *const cfg = DEV_CFG(ptp_context->eth_dev);
	Gmac *gmac = cfg->regs;
	GMAC_TA_Type gmac_ta;

	if ((increment <= -NSEC_PER_SEC) || (increment >= NSEC_PER_SEC)) {
		return -EINVAL;
	}

	if (increment < 0) {
		gmac_ta.bit.ADJ = 1;
		gmac_ta.bit.ITDT = -increment;
	} else {
		gmac_ta.bit.ADJ = 0;
		gmac_ta.bit.ITDT = increment;
	}

	gmac->GMAC_TA = gmac_ta.reg;

	return 0;
}

static int ptp_clock_sam_gmac_rate_adjust(struct device *dev, float ratio)
{
	struct ptp_context *ptp_context = dev->driver_data;
	const struct eth_sam_dev_cfg *const cfg = DEV_CFG(ptp_context->eth_dev);
	Gmac *gmac = cfg->regs;
	u8_t nanos;
	u16_t subnanos;
	float increment;

	/* No change needed. */
	if (ratio == 1.0) {
		return 0;
	}

	if (ratio < 0) {
		return -EINVAL;
	}

	/* Do not allow drastic rate changes */
	if (ratio < 0.5) {
		ratio = 0.5;
	} else if (ratio > 2.0) {
		ratio = 2.0;
	}

	/* Get current increment values */
	nanos = gmac->GMAC_TI & GMAC_TI_CNS_Msk;
	subnanos = gmac->GMAC_TISUBN & GMAC_TISUBN_Msk;

	/* Convert to a single float */
	increment = (nanos + (subnanos / UINT16_MAX));
	increment *= ratio;

	/* Calculate new increment values */
	nanos = (uint8_t)increment;
	subnanos = (uint16_t)((increment - nanos) * UINT16_MAX);

	/* Validate, not validating subnanos, 1 nano is the least we accept */
	if (nanos == 0) {
		return -EINVAL;
	}

	/* Write the registers (clears ACNS and NIT fields on purpose) */
	gmac->GMAC_TI = GMAC_TI_CNS(nanos);
	gmac->GMAC_TISUBN = GMAC_TISUBN_LSBTIR(subnanos);

	return 0;
}

static const struct ptp_clock_driver_api ptp_api = {
	.set = ptp_clock_sam_gmac_set,
	.get = ptp_clock_sam_gmac_get,
	.adjust = ptp_clock_sam_gmac_adjust,
	.rate_adjust = ptp_clock_sam_gmac_rate_adjust,
};

static int ptp_gmac_init(struct device *port)
{
	struct device *eth_dev = DEVICE_GET(eth0_sam_gmac);
	struct eth_sam_dev_data *dev_data = eth_dev->driver_data;
	struct ptp_context *ptp_context = port->driver_data;

	dev_data->ptp_clock = port;
	ptp_context->eth_dev = eth_dev;

	return 0;
}

DEVICE_AND_API_INIT(gmac_ptp_clock_0, PTP_CLOCK_NAME, ptp_gmac_init,
		    &ptp_gmac_0_context, NULL, POST_KERNEL,
		    CONFIG_APPLICATION_INIT_PRIORITY, &ptp_api);

#endif /* CONFIG_PTP_CLOCK_SAM_GMAC */
