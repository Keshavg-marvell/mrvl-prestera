/*******************************************************************************
Copyright (C) Marvell International Ltd. and its affiliates

This software file (the "File") is owned and distributed by Marvell
International Ltd. and/or its affiliates ("Marvell") under the following
alternative licensing terms.  Once you have made an election to distribute the
File under one of the following license alternatives, please (i) delete this
introductory statement regarding license alternatives, (ii) delete the two
license alternatives that you have not elected to use and (iii) preserve the
Marvell copyright notice above.

********************************************************************************
Marvell GPL License Option

If you received this File from Marvell, you may opt to use, redistribute and/or
modify this File in accordance with the terms and conditions of the General
Public License Version 2, June 1991 (the "GPL License"), a copy of which is
available along with the File in the license.txt file or by writing to the Free
Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 or
on the worldwide web at http://www.gnu.org/licenses/gpl.txt.

THE FILE IS DISTRIBUTED AS-IS, WITHOUT WARRANTY OF ANY KIND, AND THE IMPLIED
WARRANTIES OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE ARE EXPRESSLY
DISCLAIMED.  The GPL License provides additional details about this warranty
disclaimer.
*******************************************************************************/

#include <linux/version.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/pci.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/if_ether.h>
#include <linux/ipv6.h>

#include "mvIntDriver.h"

MODULE_LICENSE("GPL");

/* TODO List
- Multi MG
- Complete queue initialization (i.e. when CPSS skip init our queues)
- Queues weights
- Restructure struct ring
- Extend IRQ API by passing pci-id table to it so it will filter only devices
  caller cares about
- sysfs mg_win read only after rx queue setup
- Set MAC in ndo_set_mac_address
- Re-enable all sysfs in stop()
- Update "implementation" sections in the design document
*/

#define MVPPND_DEBUG

/* HW related constants */
#define PCI_DEVICE_ID_FALCON  0x8400
#define PCI_DEVICE_ID_ALDRIN2 0xcc0f
#define NUM_OF_RX_QUEUES 8
#define NUM_OF_TX_QUEUES NUM_OF_RX_QUEUES
#define NUM_OF_ATU_WINDOWS 8
#define NUM_OF_MG_WINDOWS 8
#define ATU_OFFS 0x1200 /* iATU offset in bar0 */
#define DSA_SIZE 16
#define ATU_WIN_SIZE 0xfff00000

/* MG Registers */
#define REG_ADDR_BASE		0x1D000000
#define REG_ADDR_VENDOR		REG_ADDR_BASE + 0x0050
#define REG_ADDR_DEVICE		REG_ADDR_BASE + 0x004C
#define REG_ADDR_RX_FIRST_DESC	REG_ADDR_BASE + 0x260C
#define REG_ADDR_RX_QUEUE_CMD	REG_ADDR_BASE + 0x2680
#define REG_ADDR_TX_FIRST_DESC	REG_ADDR_BASE + 0x26C0
#define REG_ADDR_TX_QUEUE_CMD	REG_ADDR_BASE + 0x2868
#define REG_ADDR_MG_BASE_ADDR	REG_ADDR_BASE + 0x020C
#define REG_ADDR_MG_SIZE	REG_ADDR_BASE + 0x0210
#define REG_ADDR_MG_HA		REG_ADDR_BASE + 0x023C
#define REG_ADDR_MG_CONTROL	REG_ADDR_BASE + 0x0254
#define REG_ADDR_RX_MASK_0	REG_ADDR_BASE + 0x2814
#define REG_ADDR_SDMA_CONF	REG_ADDR_BASE + 0x2800
#define REG_ADDR_TX_MASK_0	REG_ADDR_BASE + 0x2818
#define REG_ADDR_CAUSE_0	REG_ADDR_BASE + 0x0030

/* Array based registers index formulas */
enum {
	REG_ADDR_RX_FIRST_DESC_OFFSET_FORMULA	= 0x10,
	REG_ADDR_TX_FIRST_DESC_OFFSET_FORMULA	= 0x4,
	REG_ADDR_MG_BASE_ADDR_OFFSET_FORMULA	= 0x8,
	REG_ADDR_MG_SIZE_OFFSET_FORMULA		= 0x8,
	REG_ADDR_MG_HA_OFFSET_FORMULA		= 0x4,
	REG_ADDR_MG_CONTROL_OFFSET_FORMULA	= 0x4,
};

/* TX descriptor status/command field bits */
enum {
	TX_CMD_BIT_OWN_SDMA	= (1 << 31),
	TX_CMD_BIT_FIRST	= (1 << 21),
	TX_CMD_BIT_LAST		= (1 << 20),
	TX_CMD_BIT_CRC		= (1 << 12),
};

/* RX descriptor status/command field bits */
enum {
	RX_CMD_BIT_OWN_SDMA	= (1 << 31),
	RX_CMD_BIT_CSUM		= (1 << 30),
	RX_CMD_BIT_LAST		= (1 << 26),
	RX_CMD_BIT_FIRST	= (1 << 27),
	RX_CMD_BIT_RES_ERR	= (1 << 28),
	RX_CMD_BIT_EN_INTR	= (1 << 29),
	RX_CMD_BIT_BUS_ERR	= (1 << 30),
};

/* Some interesting bits in cause register (0x30) */
enum {
	CAUSE_RX_BIT		= (1 << 9),
};

/* Descriptor related macros */
#undef BIT_MASK
#define BIT_MASK(numOfBits) ((1ULL << numOfBits) - 1)
#define FIELD_MASK_NOT(offset, len) (~(BIT_MASK((len)) << (offset)))
#define U32_SET_FIELD(data, offset, length, val) \
	(data) = (((data) & FIELD_MASK_NOT((offset), (length))) | \
		  ((val) << (offset)))

#define TX_DESC_SET_BYTE_CNT(bc, val) \
	(U32_SET_FIELD(bc, 16, 14, val))

#define RX_DESC_GET_BYTE_CNT(bc) \
	((bc >> 16) & 0x3FFF)
#define RX_DESC_SET_BYTE_CNT(bc, val) \
	bc = __builtin_bswap32(bc); \
	U32_SET_FIELD(bc, 0, 14, val); \
	bc = __builtin_bswap32(bc);

/* Configurable constants */
#define DRV_NAME "mvppnd_netdev"
#define MAX_FRAGS 8

/* How long to wait for SDMA to take ownership of a descriptor */
static const unsigned long TX_WAIT_FOR_CPU_OWENERSHIP_USEC = 100000;
static const u32 DEFAULT_RX_QUEUES_WEIGHT = 0x88888888; /* 4 bits for each q */
static const u16 DEFAULT_NAPI_POLL_WEIGHT = NAPI_POLL_WEIGHT;
static const u8 MAX_EMPTY_NAPI_POLL = 20;
static const u16 DEFAULT_ATU_WIN = 5;
static const u8 DEFAULT_MG_WIN = 0xE; /* 3 windows, first for coherent and max 2
					 for streaming, indexes below */
static const u8 MG_WIN_COHERENT_IDX = 0;
static const u8 MG_WIN_STREAMING1_IDX = 1;
static const u8 MG_WIN_STREAMING2_IDX = 2;
static const u8 DEFAULT_TX_DSA[] = {0xC8, 0x00, 0x40, 0x01}; /* Forward */
static const u16 TX_RING_SIZE = roundup_pow_of_two(MAX_FRAGS);
static const u16 MAX_RX_RING_SIZE = roundup_pow_of_two(2048);
static const u16 DEFAULT_RX_RING_SIZE = roundup_pow_of_two(2048);
static const u32 DEFAULT_MTU = 4096;

static const u8 DEFAULT_MAC[] = {0x00, 0x50, 0x43, 0x0, 0x0, 0x0};

/* Defines slot for each statistics attribute in stats array */
enum mvppnd_stats {
	STATS_INTERRUPTS = 0,
	STATS_RX_INTERRUPTS,
	STATS_NAPI_POLL_CALLS,
	STATS_NAPI_BURN_BUDGET,
	STATS_RX_PACKETS,
	STATS_RX_PACKETS_RATE,
	STATS_RX_PACKETS_PER_INTERRUPT,
	STATS_RX_Q0_PACKETS,
	STATS_RX_Q1_PACKETS,
	STATS_RX_Q2_PACKETS,
	STATS_RX_Q3_PACKETS,
	STATS_RX_Q4_PACKETS,
	STATS_RX_Q5_PACKETS,
	STATS_RX_Q6_PACKETS,
	STATS_RX_Q7_PACKETS,
	STATS_TX_PACKETS,
	STATS_LAST = STATS_TX_PACKETS,
};

/* Description of each of the above statistics */
static const char *mvppnd_stats_descs[] = {
	"INTERRUPTS      ",
	"RX_INTERRUPTS   ",
	"NAPI_POLL_CALLS ",
	"NAPI_BURN_BUDGET",
	"RX_PACKETS      ",
	"RX_PACKETS_RATE ",
	"RX_PKT_PER_INTR ",
	"RX_Q0_PACKETS   ",
	"RX_Q1_PACKETS   ",
	"RX_Q2_PACKETS   ",
	"RX_Q3_PACKETS   ",
	"RX_Q4_PACKETS   ",
	"RX_Q5_PACKETS   ",
	"RX_Q6_PACKETS   ",
	"RX_Q7_PACKETS   ",
	"TX_PACKETS      ",
};

struct mvppnd_hw_desc {
	volatile u32 cmd_sts;
	volatile u32 bc;
	volatile u32 buf_addr;
	volatile u32 next_desc_ptr;
};

struct mvppnd_dma_buf {
	void *virt;
	dma_addr_t dma;
	size_t size;
};

struct mvppnd_dma_sg_buf {
	char *virt;
	dma_addr_t mappings[MAX_FRAGS + 1]; /* 1 for head */
	size_t sizes[MAX_FRAGS + 1];
};

struct mvppnd_dma_block {
	struct mvppnd_dma_buf buf;
	size_t mark; /* pointer to free space in the block */
};

struct mvppnd_ring {
	struct mvppnd_hw_desc **descs; /* Array of hw_desc pointers */
	dma_addr_t ring_dma; /* dma address of the first descriptor */
	struct mvppnd_dma_sg_buf **buffs; /* Array of buffers */
	int descs_ptr; /* Index of the next desc  */
	int buffs_ptr; /* Index of the next buff */
};

struct mvppnd_pci_dev {
	struct pci_dev *pdev;
	struct net_device *ndev;

	void __iomem *bar0; /* cnm */
	void __iomem *bar2; /* switching */

	/* For all coherent memory allocations (rings, data pointers etc) */
	struct mvppnd_dma_block coherent;

	int rx_queues[NUM_OF_RX_QUEUES]; /* RX queues indexes to poll from */
	u32 rx_queues_mask; /* Used to set and clear RX interrupt mask */
	int rx_queues_weight[NUM_OF_RX_QUEUES]; /* Weight of RX queues */
	int tx_queue; /* TX queue to post to */
	struct mutex queues_mutex; /* Protect both rx and tx queues */
	struct mvppnd_ring tx_ring;
	struct mvppnd_ring rx_rings[NUM_OF_RX_QUEUES];
	int rx_ring_size;

	int atu_win; /* iATU window number to use */
	int atu_win_addr_base; /* Last used iATU window base address */
	u32 bar2_win_offs; /* Offset in bar2 of our window */

	u8 mg_win[3]; /* 0 for coherent, 1 and 2 for streaming */

	size_t max_pkt_sz; /* Maximum size of frame, set by sysfs */

	u8 config_dsa[DSA_SIZE]; /* allow admin to modify TX dsa tag */
	u8 config_dsa_size; /* To support all kinds of DSA */
	struct mvppnd_dma_buf dsa;
	struct mvppnd_dma_buf mac;
	struct mvppnd_dma_buf tx_buffs;

	struct workqueue_struct *tx_wq;

	struct task_struct *rx_thread;
	struct semaphore interrupts_sema;
	struct napi_struct napi;
	u8 rx_queue_ptr; /* to resume RX NAPI work */
	int napi_poll_weight;

	unsigned long stats[STATS_LAST + 1];
#ifdef MVPPND_DEBUG
	int print_packets_interval;
#endif

	/* sysfs attributes */
	struct kobj_attribute attr_rx_queues_weight;
	struct kobj_attribute attr_napi_poll_weight;
	struct kobj_attribute attr_rx_ring_size;
	struct kobj_attribute attr_max_pkt_sz;
	struct kobj_attribute attr_rx_queues;
	struct kobj_attribute attr_tx_queue;
	struct kobj_attribute attr_atu_win;
	struct kobj_attribute attr_mg_win;
	struct kobj_attribute attr_mac;
	struct kobj_attribute attr_dsa;
#ifdef MVPPND_DEBUG
	struct kobj_attribute attr_reg;
	u32 sysfs_reg_addr;
#endif
	struct kobj_attribute attr_driver_statistics;
};

struct mvppnd_skb_work {
	struct work_struct work;
	struct mvppnd_pci_dev *ppdev;
	struct sk_buff *skb;
};

/*********** Driver statistics functions ***************/
static inline void mvppnd_inc_stat(struct mvppnd_pci_dev *ppdev, u8 stat_idx,
				   unsigned long inc_by)
{
	ppdev->stats[stat_idx] += inc_by;
}
static inline void mvppnd_clear_stat(struct mvppnd_pci_dev *ppdev, u8 stat_idx)
{
	ppdev->stats[stat_idx] = 0;
}

static inline const char *mvppnd_get_stat_desc(u8 stat_idx)
{
	return mvppnd_stats_descs[stat_idx];
}

static inline unsigned long mvppnd_get_stat(struct mvppnd_pci_dev *ppdev,
					    u8 stat_idx,
					    unsigned long last_jiffies)
{
	static unsigned long last_rx_packets = 0;

	/* Some stats needs special care */
	if (stat_idx == STATS_RX_PACKETS_PER_INTERRUPT) {
		ppdev->stats[STATS_RX_PACKETS_PER_INTERRUPT] =
			ppdev->stats[STATS_RX_PACKETS] /
			ppdev->stats[STATS_INTERRUPTS];
	}

	if (stat_idx == STATS_RX_PACKETS_RATE) {
		ppdev->stats[STATS_RX_PACKETS_RATE] =
			(ppdev->stats[STATS_RX_PACKETS] - last_rx_packets) /
			((jiffies - last_jiffies) / HZ);
		last_rx_packets = ppdev->stats[STATS_RX_PACKETS];
	}

	return ppdev->stats[stat_idx];
}

/*********** registers related functions ***************/
static void mvppnd_adjust_bar2_window(struct mvppnd_pci_dev *ppdev,
				      u32 reg_addr)
{
	u32 reg_addr_base, winx_offs, win_size, win0_offs, win0_start,
	    winx_start;

	/* ATU windows are for SPI6 and above devices, skip in case this is not
	   our device */
	if (unlikely(ppdev->atu_win == -1))
		return;

	reg_addr_base = reg_addr & ATU_WIN_SIZE;

	if (ppdev->atu_win_addr_base == reg_addr_base)
		return; /* Already set, we can skip */

	/* Go to our window (0x0100 because we need only in bound) */
	winx_offs = ATU_OFFS + ppdev->atu_win * 0x0200 + 0x0100;

	win0_offs = ATU_OFFS + 0x0100;
	win0_start = ioread32(ppdev->bar0 + win0_offs + 0x08);
	winx_start = ioread32(ppdev->bar0 + winx_offs + 0x08);
	if (unlikely(!winx_start)) { /* If not already set */
		win0_offs = ATU_OFFS + 0x0100;
		win0_start = ioread32(ppdev->bar0 + win0_offs + 0x08);
		win_size = ioread32(ppdev->bar0 + win0_offs + 0x10) - win0_start;
		iowrite32(0, ppdev->bar0 + winx_offs + 0x00);
		iowrite32(0x80000000, ppdev->bar0 + winx_offs + 0x04);
		winx_start = win0_start + ppdev->atu_win * (win_size + 1);
		iowrite32(winx_start, ppdev->bar0 + winx_offs + 0x08);
		iowrite32(winx_start + win_size, ppdev->bar0 + winx_offs + 0x10);
	}

	iowrite32(reg_addr_base, ppdev->bar0 + winx_offs + 0x14);

	ppdev->bar2_win_offs = winx_start - win0_start;

	ppdev->atu_win_addr_base = reg_addr_base;
}

static u32 mvppnd_read_reg(struct mvppnd_pci_dev *ppdev, u32 reg_addr)
{
	u32 reg_addr_offs = reg_addr & 0x00ffffff;
	u32 val;

	mvppnd_adjust_bar2_window(ppdev, reg_addr);

	val = ioread32(ppdev->bar2 + ppdev->bar2_win_offs + reg_addr_offs);

	/*
	dev_dbg(&ppdev->pdev->dev, "read : 0x%x=0x%x\n", reg_addr, val);
	*/

	return val;
}

static void mvppnd_write_reg(struct mvppnd_pci_dev *ppdev, u32 reg_addr,
			     u32 val)
{
	u32 reg_addr_offs = reg_addr & 0x000fffff;

	mvppnd_adjust_bar2_window(ppdev, reg_addr);

	/*
	dev_dbg(&ppdev->pdev->dev, "write: 0x%x=0x%x\n", reg_addr, val);
	*/

	iowrite32(val, ppdev->bar2 + ppdev->bar2_win_offs + reg_addr_offs);
}

static void mvppnd_edit_reg_or(struct mvppnd_pci_dev *ppdev, u32 reg_addr,
			       u32 value)
{
	u32 v;

	v = mvppnd_read_reg(ppdev, reg_addr);
	v |= value;
	mvppnd_write_reg(ppdev, reg_addr, v);
}

/*********** SDMA Registers functions ******************/
static void mvppnd_update_interrupt_mask(struct mvppnd_pci_dev *ppdev,
					 u32 reg, u32 mask, bool set)
{
	u32 reg_mask;

	/*
	dev_dbg(&ppdev->pdev->dev, "%s reg 0x%x mask 0x%x\n",
		set ? "set" : "clear", reg, mask);
	*/

	if (set) {
		mvppnd_edit_reg_or(ppdev, reg, mask);
	} else {
		reg_mask = mvppnd_read_reg(ppdev, reg);
		reg_mask &= ~mask;
		mvppnd_write_reg(ppdev, reg, reg_mask);
	}
}

static inline void mvppnd_disable_rx_interrupts(struct mvppnd_pci_dev *ppdev)
{
	mvppnd_update_interrupt_mask(ppdev, REG_ADDR_RX_MASK_0,
				     /* disable completion events */
				     ppdev->rx_queues_mask << 2 |
				     /* disable resource error events
				        (HW reached to CPU owned descriptor) */
				     ppdev->rx_queues_mask << 11,
				     false);
}

static inline void mvppnd_enable_rx_interrupts(struct mvppnd_pci_dev *ppdev)
{
	/* TODO: We should disable resource error events here, the same as
		 what we are doing in mvppnd_disable_rx_interrupts */
	mvppnd_update_interrupt_mask(ppdev, REG_ADDR_RX_MASK_0,
				     ppdev->rx_queues_mask << 2, true);
}

static inline void mvppnd_disable_tx_interrupts(struct mvppnd_pci_dev *ppdev)
{
	/* Disable Tx Buffer Queue, Tx Error Queue and Tx End Queue */
	mvppnd_update_interrupt_mask(ppdev, REG_ADDR_TX_MASK_0,
				     (1 << (ppdev->tx_queue + 1)) |
				     (1 << (ppdev->tx_queue + 9)) |
				     (1 << (ppdev->tx_queue + 17)), false);
}

/*********** misc functions ****************************/
static void mvppnd_setup_rx_queues_weights(struct mvppnd_pci_dev *ppdev,
					   u32 weights)
{
	u8 *w = (u8 *)&weights;
	int i;

	for (i = 0; i < NUM_OF_RX_QUEUES; i += 2, w++) {
		/* 4 bis for each queue */
		ppdev->rx_queues_weight[i] = *w & 0xF;
		ppdev->rx_queues_weight[i + 1] = (*w & 0xF0) >> 4;
	}
}

/*********** MG windows functions **********************/
static void mvppnd_setup_mg_window(struct mvppnd_pci_dev *ppdev, u8 mg_win,
				   u32 base_addr, u32 size)
{
	u32 target, control;

	/* dev_dbg is not working at probe stage
	dev_info(&ppdev->pdev->dev, "MG window %d: 0x%x\n", mg_win, base_addr);
	*/

	if (ppdev->pdev->device != PCI_DEVICE_ID_ALDRIN2) {
		target = 0xe03;
		control = base_addr | 0x0000000e;
	} else {
		target = 0xe04;
		control = 0x6;
	}

	if (!base_addr) { /* Caller want to clear setting */
		target = 0;
		control = 0;
	}

	mvppnd_write_reg(ppdev, REG_ADDR_MG_BASE_ADDR + mg_win *
			 REG_ADDR_MG_BASE_ADDR_OFFSET_FORMULA,
			 base_addr | target);
	mvppnd_write_reg(ppdev, REG_ADDR_MG_SIZE + mg_win *
			 REG_ADDR_MG_SIZE_OFFSET_FORMULA, size);
	mvppnd_write_reg(ppdev, REG_ADDR_MG_HA + mg_win *
			 REG_ADDR_MG_HA_OFFSET_FORMULA, 0);
	mvppnd_write_reg(ppdev, REG_ADDR_MG_CONTROL + mg_win *
			 REG_ADDR_MG_CONTROL_OFFSET_FORMULA, control);
}

/*********** some debug function ***********************/
#ifdef MVPPND_DEBUG
static void print_skb_hdr(struct mvppnd_pci_dev *ppdev, const char *data)
{
	struct ethhdr *eth = (struct ethhdr *)data;

	return;

	dev_dbg(&ppdev->pdev->dev, "----- mac header -----\n");
	dev_dbg(&ppdev->pdev->dev, "src_mac %pM\n", eth->h_source);
	dev_dbg(&ppdev->pdev->dev, "dst_mac %pM\n", eth->h_dest);

	/* https://en.wikipedia.org/wiki/EtherType */
	dev_dbg(&ppdev->pdev->dev, "proto   0x%x\n", be16_to_cpu(eth->h_proto));

	/* IPv4 */
	if (be16_to_cpu(eth->h_proto) == 0x0800) {
		/* TODO: print IPv4 header */
	}

	/* IPv6 */
	if (be16_to_cpu(eth->h_proto) == 0x86DD) {
		struct ipv6hdr *ipv6 =
			(struct ipv6hdr *)(data + sizeof(struct ethhdr));
		dev_dbg(&ppdev->pdev->dev, "----- ipv6 header -----\n");
		dev_dbg(&ppdev->pdev->dev, "src_ip %pI6\n",
			ipv6->saddr.s6_addr);
		dev_dbg(&ppdev->pdev->dev, "dst_ip %pI6\n",
			ipv6->daddr.s6_addr);
	}
	dev_dbg(&ppdev->pdev->dev, "----------------------\n");
}

static void print_frame(struct mvppnd_pci_dev *ppdev, const char *data,
			size_t len, bool rx)
{
	char *b;
	int i;

	return;

	b = kmalloc(8196, GFP_KERNEL);

	b[0] = 0;
	dev_dbg(&ppdev->pdev->dev, "----------------------------\n");
	dev_dbg(&ppdev->pdev->dev, "%s: %ld bytes\n", rx ? "RX" : "TX", len);
	for (i = 0; i < len; i++) {
		if (rx && ((i == 12) || (i == 12 + 16)))
			sprintf(b, "%s\n", b);
		sprintf(b, "%s 0x%x", b, data[i]);
	}

	dev_dbg(&ppdev->pdev->dev, "%s\n", b);
	dev_dbg(&ppdev->pdev->dev, "----------------------------\n");

	kfree(b);
}

static void print_buff(const char *title, const char *buff, size_t buff_len)
{
	int i;
	char *b;
	b = kmalloc(buff_len * 3 + 1, GFP_KERNEL);

	b[0] = 0;
	for (i = 0; i < buff_len; i++) {
		sprintf(b, "%s%.2x ", b, buff[i]);
	}
	printk("[%s %ld]: %s\n", title, buff_len, b);

	kfree(b);
}
#else
#define print_skb_hdr(ppdev, data)
#define print_frame(ppdev, data, len, from_pp)
#define print_buff(title, buff, buff_len)
#endif


static void print_first_descs(struct mvppnd_pci_dev *ppdev, const char *title,
			      u32 addr, int formula, int cnt)
{
	int i;

	return;

	dev_dbg(&ppdev->pdev->dev, "%s\n", title);

	for (i = 0; i < cnt; i++)
		dev_dbg(&ppdev->pdev->dev, "queue #%d desc ptr: 0x%x\n", i,
			mvppnd_read_reg(ppdev, addr + i * formula));

	dev_dbg(&ppdev->pdev->dev, "end %s\n", title);
}

static void debug_print_some_registers(struct mvppnd_pci_dev *ppdev)
{
	dev_dbg(&ppdev->pdev->dev, "vendor: 0x%x\n",
		mvppnd_read_reg(ppdev, REG_ADDR_VENDOR));
	dev_dbg(&ppdev->pdev->dev, "device: 0x%x\n",
		mvppnd_read_reg(ppdev, REG_ADDR_DEVICE));
	dev_dbg(&ppdev->pdev->dev, "rxdesc: 0x%x\n",
		mvppnd_read_reg(ppdev, REG_ADDR_RX_FIRST_DESC));
	dev_dbg(&ppdev->pdev->dev, "txdesc: 0x%x\n",
		mvppnd_read_reg(ppdev, REG_ADDR_TX_FIRST_DESC));

	print_first_descs(ppdev, "tx_queue", REG_ADDR_TX_FIRST_DESC,
			  REG_ADDR_TX_FIRST_DESC_OFFSET_FORMULA,
			  NUM_OF_TX_QUEUES);
}

/*********** queues related functions ******************/
static int mvppnd_num_of_rx_queues(struct mvppnd_pci_dev *ppdev)
{
	int rc, i;

	for (i = 0, rc = 0; i < NUM_OF_RX_QUEUES; i++)
		rc += (ppdev->rx_queues[i] != -1);

	return rc;
}

static inline int cyclic_idx(int c, size_t s)
{
	if (c < 0)
		return s + c;

	return c % s;
}

static inline void cyclic_inc(int *c, size_t s)
{
	*c = (*c + 1) % s;
}

static int mvppnd_queue_enabled(struct mvppnd_pci_dev *ppdev, u32 cmd_reg_addr,
				int queue)
{
	return (mvppnd_read_reg(ppdev, cmd_reg_addr) & (1 << queue));
}

static void mvppnd_enable_queue(struct mvppnd_pci_dev *ppdev, u32 cmd_reg_addr,
				int queue)
{
	mvppnd_edit_reg_or(ppdev, cmd_reg_addr, 1 << queue);
}

static void mvppnd_disable_queue(struct mvppnd_pci_dev *ppdev, u32 cmd_reg_addr,
				 int queue)
{
	mvppnd_edit_reg_or(ppdev, cmd_reg_addr, 1 << (queue + 8));
}

static u32 mvppnd_read_tx_first_desc(struct mvppnd_pci_dev *ppdev, u8 queue)
{
	return mvppnd_read_reg(ppdev, REG_ADDR_TX_FIRST_DESC + queue *
			       REG_ADDR_TX_FIRST_DESC_OFFSET_FORMULA);
}

static void mvppnd_write_tx_first_desc(struct mvppnd_pci_dev *ppdev,
				       u32 val)
{
	mvppnd_disable_queue(ppdev, REG_ADDR_TX_QUEUE_CMD, ppdev->tx_queue);

	mvppnd_write_reg(ppdev, REG_ADDR_TX_FIRST_DESC + ppdev->tx_queue *
			 REG_ADDR_TX_FIRST_DESC_OFFSET_FORMULA, val);
}

static u32 mvppnd_read_rx_first_desc(struct mvppnd_pci_dev *ppdev,
				     u8 queue)
{
	return mvppnd_read_reg(ppdev, REG_ADDR_RX_FIRST_DESC + queue *
			       REG_ADDR_RX_FIRST_DESC_OFFSET_FORMULA);
}

static void mvppnd_write_rx_first_desc(struct mvppnd_pci_dev *ppdev,
				       u8 queue, u32 val)
{
	/* When using more than one RX queue, after the SDMA places the first
	   packet in some rings, it store the old next-desc-ptr (the one which
	   CPSS puts originally) instead of the one specified as next-desc-ptr
	   of the current descriptor. It is like it has a cache and it fetch
	   the value from there.
	   By writing the first descriptor pointer twice to the register, it is
	   observed that the correct value is used, i.e the next-ptr and not
	   the CPSS value */

	mvppnd_disable_queue(ppdev, REG_ADDR_RX_QUEUE_CMD, queue);

	mvppnd_write_reg(ppdev, REG_ADDR_RX_FIRST_DESC + queue *
			 REG_ADDR_RX_FIRST_DESC_OFFSET_FORMULA, val);

	mvppnd_disable_queue(ppdev, REG_ADDR_RX_QUEUE_CMD, queue);

	mvppnd_write_reg(ppdev, REG_ADDR_RX_FIRST_DESC + queue *
			 REG_ADDR_RX_FIRST_DESC_OFFSET_FORMULA, val);
}

/*********** coherent memory block *********************/
/* We allocate one block for all dma services, then allocate from this block.
 * Allocation from the block cannot be undone, it is too complex and not needed
 * (unless the driver will be required to support run-time configuration
 * changes in the future)
 */
static int mvppnd_alloc_device_coherent(struct mvppnd_pci_dev *ppdev)
{
	dma_addr_t d;
	void *v;
	u32 s;

	ppdev->coherent.buf.size = 0;
	ppdev->coherent.mark = 0;

	/* RX queues */
	ppdev->coherent.buf.size += ppdev->rx_ring_size *
				    mvppnd_num_of_rx_queues(ppdev) *
				    sizeof(struct mvppnd_hw_desc);

	/* One TX queue */
	ppdev->coherent.buf.size += TX_RING_SIZE *
				    sizeof(struct mvppnd_hw_desc);

	/* Temporary buffer for TX packets (1 for head) */
	ppdev->coherent.buf.size += PAGE_SIZE * (MAX_FRAGS + 1);

	/* Space for two MACs (first descriptor) */
	ppdev->coherent.buf.size += ETH_ALEN * 2;

	/* Space for DSA (second descriptor) */
	ppdev->coherent.buf.size += DSA_SIZE;

	ppdev->coherent.buf.size = roundup_pow_of_two(ppdev->coherent.buf.size);

	ppdev->coherent.buf.virt = dma_alloc_coherent(&ppdev->pdev->dev,
						      ppdev->coherent.buf.size,
						      &ppdev->coherent.buf.dma,
						      GFP_DMA32 | GFP_NOFS |
						      GFP_KERNEL);
	if (unlikely(!ppdev->coherent.buf.virt)) {
		dev_err(&ppdev->pdev->dev,
			"Fail to allocate %ld bytes of coherent memory\n",
			ppdev->coherent.buf.size);
		return -ENOMEM;
	}

	/* Make sure address is aligned with the size */
	s = ppdev->coherent.buf.dma % ppdev->coherent.buf.size;
	if (s) {
		dma_free_coherent(&ppdev->pdev->dev, ppdev->coherent.buf.size,
				  ppdev->coherent.buf.virt,
				  ppdev->coherent.buf.dma);

		v = dma_alloc_coherent(&ppdev->pdev->dev, s, &d, GFP_DMA32 |
				       GFP_NOFS | GFP_KERNEL);
		ppdev->coherent.buf.virt =
			dma_alloc_coherent(&ppdev->pdev->dev,
					   ppdev->coherent.buf.size,
					   &ppdev->coherent.buf.dma,
					   GFP_DMA32 | GFP_NOFS | GFP_KERNEL);
		if (unlikely(!ppdev->coherent.buf.virt)) {
			dev_err(&ppdev->pdev->dev,
				"Fail to allocate %ld bytes of coherent memory\n",
				ppdev->coherent.buf.size);
			return -ENOMEM;
		}

		dma_free_coherent(&ppdev->pdev->dev, s, v, d);
	}

	if (ppdev->coherent.buf.dma & (ppdev->coherent.buf.size - 1)) {
		dev_err(&ppdev->pdev->dev,
			"Fail to allocate aligned coherent buffer\n");
		return -ENOMEM;
	}

	mvppnd_setup_mg_window(ppdev, ppdev->mg_win[MG_WIN_COHERENT_IDX],
			       ppdev->coherent.buf.dma,
			       ppdev->coherent.buf.size - 1);

	return 0;
}

static void mvppnd_free_device_coherent(struct mvppnd_pci_dev *ppdev)
{
	if (!ppdev->coherent.buf.virt)
		return;

	dma_free_coherent(&ppdev->pdev->dev, ppdev->coherent.buf.size,
			  ppdev->coherent.buf.virt, ppdev->coherent.buf.dma);

	ppdev->coherent.buf.virt = NULL;
}

static void *mvppnd_alloc_coherent(struct mvppnd_pci_dev *ppdev, size_t size,
				   dma_addr_t *dma)
{
	void *free = ppdev->coherent.buf.virt + ppdev->coherent.mark;

	size = max(size, PAGE_SIZE);

	*dma = ppdev->coherent.buf.dma + ppdev->coherent.mark;

	ppdev->coherent.mark += size;

	return free;
}

/*********** buf wrappers ******************************/
static int mvppnd_copy_skb(struct mvppnd_pci_dev *ppdev, struct sk_buff *skb,
			   struct mvppnd_dma_sg_buf *sgb)
{
	int rc = 0;
	int i;

	if (unlikely((skb_shinfo(skb)->nr_frags > MAX_FRAGS)))
		return -EINVAL;

	skb_pull(skb, ETH_ALEN * 2); /* Skip src and dest macs */

	memcpy(ppdev->tx_buffs.virt, skb->data, skb_headlen(skb));
	/* Clean remaining */
	if (skb_headlen(skb) < PAGE_SIZE)
		memset(ppdev->tx_buffs.virt + skb_headlen(skb), 0,
		       PAGE_SIZE - skb_headlen(skb) + 1);
	sgb->mappings[0] = ppdev->tx_buffs.dma;
	sgb->sizes[0] = max_t(unsigned int, skb_headlen(skb), 64);
	/* Frags starts from index 1 */
	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		const skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0)
		memcpy(ppdev->tx_buffs.virt + i * PAGE_SIZE,
		       page_to_virt(skb_frag_page(frag)),
		       skb_frag_size(frag));
#else
/* TODO: If needed - add support */
#error "No support for this kernel"
#endif
		sgb->mappings[i + 1] = ppdev->tx_buffs.dma + i * PAGE_SIZE;
		sgb->sizes[i + 1] = skb_frag_size(frag);
	}

	skb_push(skb, ETH_ALEN * 2); /* We are done, return to MAC header */
	return rc;
}

static int mvppnd_alloc_buff(struct mvppnd_pci_dev *ppdev,
			     struct mvppnd_dma_sg_buf *sgb, size_t size)
{
	sgb->sizes[0] = size;

	sgb->virt = kmalloc(sgb->sizes[0], GFP_KERNEL);
	if (unlikely(!sgb->virt)) {
		netdev_err(ppdev->ndev, "Fail to allocate buffer\n");
		return -ENOMEM;
	}

	sgb->mappings[0] = dma_map_single(&ppdev->pdev->dev, sgb->virt,
					  sgb->sizes[0], DMA_FROM_DEVICE);
	if (unlikely(dma_mapping_error(&ppdev->pdev->dev, sgb->mappings[0]))) {
		netdev_err(ppdev->ndev,
			   "Fail to map buffer to dma addr (%p, %ld, 0x%llx)\n",
			   sgb->virt, sgb->sizes[0], sgb->mappings[0]);
		goto free_buff;
	}

	return 0;

free_buff:
	kfree(sgb->virt);
	sgb->virt = NULL;

	return -ENOMEM;
}

static void mvppnd_free_buf(struct mvppnd_pci_dev *ppdev,
                           struct mvppnd_dma_sg_buf *sgb)
{
	struct device *dev = &ppdev->pdev->dev;

	dma_unmap_single(dev, sgb->mappings[0], sgb->sizes[0], DMA_FROM_DEVICE);

	kfree(sgb->virt);

	sgb->virt = NULL;
	sgb->sizes[0] = 0;
}

/*********** rings related functions *******************/
static void mvppnd_free_ring_dma(struct mvppnd_pci_dev *ppdev,
				 struct mvppnd_ring *ring, size_t size)
{
	int i;

	if (!ring->buffs)
		return;

	for (i = 0; i < size; i++) {
		/* We are not allocating buffs for TX ring */
		if (ring->buffs[i]->virt)
			mvppnd_free_buf(ppdev, ring->buffs[i]);
		kfree(ring->buffs[i]);
		ring->buffs[i] = NULL;
	}

	kfree(ring->buffs);
	ring->buffs = NULL;

	kfree(ring->descs);
	ring->descs = NULL;
}

static int mvppnd_alloc_ring(struct mvppnd_pci_dev *ppdev,
			     struct mvppnd_ring *ring, size_t size)
{
	int rc = 0;
	int i = 0;

	ring->descs = kmalloc_array(size, sizeof(ring->descs[0]), GFP_KERNEL);
	if (unlikely(!ring->descs))
		return -ENOMEM;

	/* TODO: Why i can't just do the next two allocs together? */
	ring->buffs = kmalloc_array(size, sizeof(ring->buffs[0]),
				    GFP_KERNEL);
	if (unlikely(!ring->buffs))
		goto free_descs;

	for (i = 0; i < size; i++) {
		ring->buffs[i] = kzalloc(sizeof(*ring->buffs[i]), GFP_KERNEL);
		if (unlikely(!ring->buffs[i]))
			goto free_buffs;
	}

	/* Allocate one big coherent and split it to all descs */
	ring->descs[0] = mvppnd_alloc_coherent(ppdev, sizeof(*ring->descs[0]) *
					       size, &ring->ring_dma);
	ring->descs[0]->next_desc_ptr = ring->ring_dma +
					sizeof(*ring->descs[0]);

	for (i = 1; i < size; i++) {
		ring->descs[i] = ring->descs[0] + i;
		ring->descs[i]->next_desc_ptr = ring->ring_dma + (i + 1) *
						sizeof(*ring->descs[0]);
		ring->descs[i]->cmd_sts = 0;
	}

	/* Close the ring */
	ring->descs[size - 1]->next_desc_ptr = ring->ring_dma;

	return 0;

free_buffs:
	for (i = 0; i < size; i++)
		kfree(ring->buffs[i]);
	kfree(ring->buffs);
	ring->buffs = NULL;

free_descs:
	kfree(ring->descs);
	ring->descs = NULL;

	return rc;
}

static int mvppnd_setup_tx_ring(struct mvppnd_pci_dev *ppdev)
{
	int rc = 0;

	if (ppdev->tx_queue == -1)
		return -1;

	rc = mvppnd_alloc_ring(ppdev, &ppdev->tx_ring, TX_RING_SIZE);
	if (rc)
		return rc;

	ppdev->tx_buffs.virt =
		mvppnd_alloc_coherent(ppdev, PAGE_SIZE * (MAX_FRAGS + 1),
				      &ppdev->tx_buffs.dma);

	mvppnd_write_tx_first_desc(ppdev, ppdev->tx_ring.ring_dma);

	return 0;
}

static void mvppnd_destroy_tx_ring(struct mvppnd_pci_dev *ppdev)
{
	if (ppdev->tx_queue == -1)
		return;

	mvppnd_write_tx_first_desc(ppdev, 0);

	mvppnd_free_ring_dma(ppdev, &ppdev->tx_ring, TX_RING_SIZE);
}

static void mvppnd_destroy_rx_rings(struct mvppnd_pci_dev *ppdev)
{
	int i;

	for (i = 0; i < NUM_OF_RX_QUEUES; i++) {
		if (ppdev->rx_queues[i] > -1) {
			mvppnd_write_rx_first_desc(ppdev,
						   ppdev->rx_queues[i], 0);

			mvppnd_free_ring_dma(ppdev, &ppdev->rx_rings[i],
					     ppdev->rx_ring_size);
		}
	}
}

static int mvppnd_setup_rx_rings(struct mvppnd_pci_dev *ppdev)
{
	dma_addr_t min1, min_addr = 0xffffffff;
	dma_addr_t max1, max_addr = 0;
	struct mvppnd_ring *r;
	int i, j, rc = 0;
	u8 mg_win;
	u32 s, d;

	if (!ppdev->mg_win[MG_WIN_STREAMING1_IDX]) {
		dev_err(&ppdev->pdev->dev, "MG window is not set\n");
		return -EFAULT;
	}

	memset(ppdev->rx_rings, 0, sizeof(ppdev->rx_rings));

	for (i = 0; i < NUM_OF_RX_QUEUES; i++) {
		if (ppdev->rx_queues[i] == -1)
			continue;

		rc = mvppnd_alloc_ring(ppdev, &ppdev->rx_rings[i],
				       ppdev->rx_ring_size);
		if (rc)
			goto destroy_rings;

		r = &ppdev->rx_rings[i];

		/* Populate ring with skbs */
		for (j = 0; j < ppdev->rx_ring_size; j++) {
			r->descs[j]->cmd_sts = RX_CMD_BIT_OWN_SDMA |
					       RX_CMD_BIT_EN_INTR;

			rc = mvppnd_alloc_buff(ppdev, r->buffs[j],
					       ppdev->max_pkt_sz);
			if (rc)
				goto destroy_rings;

			r->descs[j]->buf_addr = r->buffs[j]->mappings[0];
			RX_DESC_SET_BYTE_CNT(r->descs[j]->bc,
					     r->buffs[j]->sizes[0]);

			/* Start and end address - needed to setup MG window */
			if (r->buffs[j]->mappings[0] < min_addr)
				min_addr = r->buffs[j]->mappings[0];
			if (r->buffs[j]->mappings[0] > max_addr)
				max_addr = r->buffs[j]->mappings[0];
		}
		r->descs_ptr = 0;
		r->buffs_ptr = 0;
	}

	/* recv boundaries */
	mvppnd_edit_reg_or(ppdev, REG_ADDR_SDMA_CONF, 0x1);

	for (i = 0; i < NUM_OF_RX_QUEUES; i++) {
		if (ppdev->rx_queues[i] == -1)
			continue;

		mvppnd_write_rx_first_desc(ppdev, ppdev->rx_queues[i],
					   ppdev->rx_rings[i].ring_dma);

		mvppnd_enable_queue(ppdev, REG_ADDR_RX_QUEUE_CMD,
				    ppdev->rx_queues[i]);
	}

	max_addr += ppdev->max_pkt_sz;
	s = max_addr - min_addr;
	d = min_addr % s;
	mg_win = MG_WIN_STREAMING1_IDX;
	if (d) {
		/* We need two windows */
		if (!ppdev->mg_win[MG_WIN_STREAMING1_IDX]) {
			dev_err(&ppdev->pdev->dev,
			"MG window is set with one window while two are needed\n");
			rc = -EFAULT;
			goto destroy_rings;
		}
		min1 = min_addr - d;
		max1 = max_addr - d;
		mvppnd_setup_mg_window(ppdev, ppdev->mg_win[mg_win], min1,
				       max1 - min1 - 1);

		/* Configure second window */
		mg_win = MG_WIN_STREAMING2_IDX;
		min_addr = max1;
		max_addr = min_addr + (max1 - min1);
	}
	mvppnd_setup_mg_window(ppdev, ppdev->mg_win[mg_win],
			       min_addr, max_addr - min_addr - 1);

	return 0;

destroy_rings:
	mvppnd_destroy_rx_rings(ppdev);

	return rc;
}

static void mvppnd_destroy_rings(struct mvppnd_pci_dev *ppdev)
{
	mvppnd_destroy_rx_rings(ppdev);
	mvppnd_destroy_tx_ring(ppdev);
}

/*********** rx ****************************************/
static void mvppnd_process_buff(struct mvppnd_pci_dev *ppdev, char *buff,
				u32 bc)
{
	struct sk_buff *skb;
	int rx_bytes;
	int rc;

	print_frame(ppdev, buff, RX_DESC_GET_BYTE_CNT(bc), true);

	/* bytes = recv_bytes - dsa_dag - csum */
	rx_bytes = RX_DESC_GET_BYTE_CNT(bc) - DSA_SIZE - 4;

	skb = netdev_alloc_skb(ppdev->ndev, rx_bytes);

	memcpy(skb->data, buff, ETH_ALEN * 2); /* Copy src and dst MACs */
	memcpy(skb->data + ETH_ALEN * 2, buff + ETH_ALEN * 2 + DSA_SIZE,
	       rx_bytes - ETH_ALEN * 2); /* Rest of the frame */

	skb->len = rx_bytes;

#ifdef MVPPND_DEBUG
	/* Print packet for debug */
	if (ppdev->print_packets_interval && (!(ppdev->ndev->stats.rx_packets %
						ppdev->print_packets_interval)))
		print_buff("rx", skb->data, skb->len);
#endif

	/* Bit 30 - csum validity */
	skb->ip_summed = ((bc & RX_CMD_BIT_CSUM) == RX_CMD_BIT_CSUM) ?
			  CHECKSUM_NONE : CHECKSUM_COMPLETE;
	skb->pkt_type = PACKET_HOST;

	print_skb_hdr(ppdev, skb->data);

	skb->protocol = eth_type_trans(skb, skb->dev);

	rc = netif_receive_skb(skb);
	if (rc == NET_RX_SUCCESS) {
		ppdev->ndev->stats.rx_packets++;
		ppdev->ndev->stats.rx_bytes += rx_bytes;
	} else {
		ppdev->ndev->stats.rx_dropped++;
	}

	print_frame(ppdev, skb->data, skb->len, true);

	dev_dbg(&ppdev->pdev->dev, "netif_receive_skb returns %d\n", rc);
}

static int mvppnd_process_rx_queue(struct mvppnd_pci_dev *ppdev, int queue,
				   int budget)
{
	int local_budget = min(ppdev->rx_queues_weight[ppdev->rx_queues[queue]],
			       budget);
	struct mvppnd_ring *r = &ppdev->rx_rings[queue];
	struct mvppnd_dma_sg_buf *buff;
	int done = 0;

	while (((r->descs[r->descs_ptr]->cmd_sts & RX_CMD_BIT_OWN_SDMA) !=
	       RX_CMD_BIT_OWN_SDMA) && (done < local_budget)) {

		/* TODO: Assumption now that each desc holds only *one* packet
			 so we expect bits last and first to be set.
			 Look for if there are other drivers that need to
			 compose several frags to one skb */

		/* TODO: Check resource error bit (28) */

		buff = r->buffs[r->buffs_ptr];

		dma_sync_single_for_cpu(&ppdev->pdev->dev, buff->mappings[0],
					buff->sizes[0], DMA_FROM_DEVICE);

		/* Populate skb details and pass to network stack */
		mvppnd_process_buff(ppdev, buff->virt,
				    r->descs[r->descs_ptr]->bc);

		r->descs[r->descs_ptr]->bc = 0; /* clean it first */
		RX_DESC_SET_BYTE_CNT(r->descs[r->descs_ptr]->bc,
				     buff->sizes[0]);

		/* Pass ownership back to SDMA */
		r->descs[r->descs_ptr]->cmd_sts = RX_CMD_BIT_OWN_SDMA |
						  RX_CMD_BIT_EN_INTR;

		/* Goto next desc */
		cyclic_inc(&r->descs_ptr, ppdev->rx_ring_size);

		/* Goto next buff */
		cyclic_inc(&r->buffs_ptr, ppdev->rx_ring_size);

		done++;
	}

	/* return the number of processed frames */
	return done;
}

int mvppnd_poll(struct napi_struct *napi, int budget)
{
	struct mvppnd_pci_dev *ppdev = container_of(napi, struct mvppnd_pci_dev,
						    napi);
	int done_queue, done_total = 0;
	u8 empty_polls = 0;

	/*
	dev_dbg(&ppdev->pdev->dev, "budget %d\n", budget);
	*/

	mvppnd_inc_stat(ppdev, STATS_NAPI_POLL_CALLS, 1);

	do {
		done_queue = mvppnd_process_rx_queue(ppdev, ppdev->rx_queue_ptr,
						     budget - done_total);
		if (done_queue)
			empty_polls = 0;
		else
			empty_polls++;

		mvppnd_inc_stat(ppdev, STATS_RX_Q0_PACKETS +
				ppdev->rx_queues[ppdev->rx_queue_ptr],
				done_queue);

		done_total += done_queue;

		do {
			cyclic_inc((int *)&ppdev->rx_queue_ptr,
				   NUM_OF_RX_QUEUES);
		} while (ppdev->rx_queues[ppdev->rx_queue_ptr] == -1);
	} while ((done_total < budget) && (empty_polls < MAX_EMPTY_NAPI_POLL));

	/*
	dev_dbg(&ppdev->pdev->dev, "done %d\n", done_total);
	*/

	if (empty_polls == MAX_EMPTY_NAPI_POLL) { /* No more packets */
		dev_dbg(&ppdev->pdev->dev, "re-enable interrupts\n");
		napi_complete(napi);
		mvppnd_enable_rx_interrupts(ppdev);
	} else {
		mvppnd_inc_stat(ppdev, STATS_NAPI_BURN_BUDGET, 1);
	}

	mvppnd_inc_stat(ppdev, STATS_RX_PACKETS, done_total);

	/*
	dev_dbg(&ppdev->pdev->dev, "done napi poll\n");
	*/

	return done_total;
}

int mvppnd_rx_thread(void *data)
{
	struct mvppnd_pci_dev *ppdev = (struct mvppnd_pci_dev *)data;
	int rc;
	u32 cause;

	do {
		rc = down_interruptible(&ppdev->interrupts_sema);
		cause = mvppnd_read_reg(ppdev, REG_ADDR_CAUSE_0);
		mvppnd_inc_stat(ppdev, STATS_INTERRUPTS, 1);
		/*
		dev_dbg(&ppdev->pdev->dev, "Got interrupt, cause 0x%x\n",
			cause);
		*/

		/* We care only for RX interrupts */
		if ((cause & CAUSE_RX_BIT) == CAUSE_RX_BIT) {
			mvppnd_inc_stat(ppdev, STATS_RX_INTERRUPTS, 1);
			mvppnd_disable_rx_interrupts(ppdev);
			napi_schedule(&ppdev->napi);
		}
	} while (!kthread_should_stop() && !rc);

	dev_dbg(&ppdev->pdev->dev, "Exit RX thread\n");

	return 0;
}

static void mvppnd_stop_rx_thread(struct mvppnd_pci_dev *ppdev)
{
	if (!ppdev->rx_thread)
		return;

	/* Release the RX thread lock */
	up(&ppdev->interrupts_sema);
	kthread_stop(ppdev->rx_thread);
	ppdev->rx_thread = NULL;
}

/*********** sysfs *************************************/
static void mvppnd_sysfs_set_read_only(struct mvppnd_pci_dev *ppdev,
				       struct kobj_attribute *attr)
{
	/* We allow one time configuration for some sysfs entries. Change
	   permissions to read-only */
	if (sysfs_chmod_file(&ppdev->ndev->dev.kobj, &attr->attr, 0444)) {
		dev_err(&ppdev->pdev->dev,
			"Fail to change file attrs, driver is unstable now\n");
	}
}

static void mvppnd_dev_dbg_ring(struct mvppnd_pci_dev *ppdev,
				struct mvppnd_ring *ring, size_t ring_size,
				size_t curr_ptr)
{
	u32 dma;
	int i, j;

	for (j = curr_ptr; j < curr_ptr + 3; j++) {
		i = cyclic_idx(j, ring_size);
		if (i == 0)
			dma = ring->ring_dma;
		else
			dma = ring->descs[i - 1]->next_desc_ptr;

		dev_dbg(&ppdev->pdev->dev,
			"\t[%d, 0x%x]: 0x%x, 0x%x, 0x%x, 0x%x\n",
			i, dma, ring->descs[i]->cmd_sts,
			__builtin_bswap32(ring->descs[i]->bc),
			ring->descs[i]->buf_addr,
			ring->descs[i]->next_desc_ptr);
	}
}

static ssize_t mvppnd_show_rx_queues(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_rx_queues);
	int i;

	strcpy(buf, "");

	for (i = NUM_OF_RX_QUEUES - 1; i >= 0; i--) {
		if (ppdev->rx_queues[i] == -1)
			continue;

		snprintf(buf, PAGE_SIZE,
			 "%squeue %d: status %d, ring-idx %d\n",
			 buf, ppdev->rx_queues[i],
			 mvppnd_queue_enabled(ppdev, REG_ADDR_RX_QUEUE_CMD,
					      ppdev->rx_queues[i]) ? 1 : 0,
			 ppdev->rx_rings[i].descs_ptr);

		if (ppdev->rx_rings[i].descs)
			mvppnd_dev_dbg_ring(ppdev, &ppdev->rx_rings[i],
					    ppdev->rx_ring_size,
					    ppdev->rx_rings[i].descs_ptr);
	}

	return strlen(buf);
}

static ssize_t mvppnd_store_rx_queues(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   const char *buf, size_t count)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
					            attr_rx_queues);
	unsigned long queues_bitmap;
	int rc, i, j = 0;

	if ((ppdev->pdev->device != PCI_DEVICE_ID_ALDRIN2) &&
	    (unlikely(ppdev->atu_win == -1))) {
		dev_err(&ppdev->pdev->dev,
			"Required atu_win configuration is missing\n");
		return -1;
	}

	mvppnd_sysfs_set_read_only(ppdev, attr);
	mvppnd_sysfs_set_read_only(ppdev, &ppdev->attr_rx_ring_size);

	rc = sscanf(buf, "0x%lx", &queues_bitmap);
	if (rc != 1) {
		dev_err(&ppdev->pdev->dev, "Invalid input, expecting 0xlx\n");
		return -EINVAL;
	}

	/* Save for interrupt masking */
	ppdev->rx_queues_mask = queues_bitmap;

	/* Update queue numbers */
	for (i = NUM_OF_RX_QUEUES; i >= 0; i--)
		if (test_bit(i, &queues_bitmap)) {
			ppdev->rx_queues[j] = i;
			j++;
		} else {
			ppdev->rx_queues[j] = -1;
		}
	return count;
}

static ssize_t mvppnd_show_tx_queue(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_tx_queue);
	int i;

	for (i = 0; i < NUM_OF_TX_QUEUES; i++) {
		snprintf(buf, PAGE_SIZE, "%s[%c%d] %d, 0x%x\n", buf,
			 (i == ppdev->tx_queue) ? '*' : ' ', i,
			 mvppnd_queue_enabled(ppdev, REG_ADDR_TX_QUEUE_CMD, i) ?
			 1 : 0, mvppnd_read_tx_first_desc(ppdev, i));
	}

	if (ppdev->tx_ring.ring_dma) /* Ring is initialized? */
		mvppnd_dev_dbg_ring(ppdev, &ppdev->tx_ring, TX_RING_SIZE, 0);

	return strlen(buf);
}

static ssize_t mvppnd_store_tx_queue(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_tx_queue);

	if ((ppdev->pdev->device != PCI_DEVICE_ID_ALDRIN2) &&
	    (unlikely(ppdev->atu_win == -1))) {
		dev_err(&ppdev->pdev->dev,
			"Required atu_win configuration is missing\n");
		return -1;
	}

	mvppnd_sysfs_set_read_only(ppdev, attr);

	if ((sscanf(buf, "%d", &ppdev->tx_queue) != 1) ||
	    (ppdev->tx_queue < -1) || (ppdev->tx_queue >= NUM_OF_TX_QUEUES)) {
		dev_err(&ppdev->pdev->dev,
			"Invalid queue number %d\n", ppdev->tx_queue);
		ppdev->tx_queue = -1;
	}

	return count;
}

static ssize_t mvppnd_show_atu_win(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_atu_win);
	u32 addr;
	int i;

	if (ppdev->pdev->device != PCI_DEVICE_ID_ALDRIN2) {
		for (i = 0; i < NUM_OF_ATU_WINDOWS; i++) {
			addr = ATU_OFFS + i * 0x0200;
			snprintf(buf, PAGE_SIZE,
				"%s[%c%d] (0x%x) out: 0x%-8x\t0x%-8x\t0x%-8x\t0x%-8x\t0x%-8x\n",
				buf, (i == ppdev->atu_win ? '*' : ' '), i, addr,
				ioread32(ppdev->bar0 + addr + 0x0008),
				ioread32(ppdev->bar0 + addr + 0x000c),
				ioread32(ppdev->bar0 + addr + 0x0010),
				ioread32(ppdev->bar0 + addr + 0x0014),
				ioread32(ppdev->bar0 + addr + 0x0018));
			addr += 0x0100; /* Now inbound */
			snprintf(buf, PAGE_SIZE,
				"%s[%c%d] (0x%x) in : 0x%-8x\t0x%-8x\t0x%-8x\t0x%-8x\t0x%-8x\n",
				buf, (i == ppdev->atu_win ? '*' : ' '), i, addr,
				ioread32(ppdev->bar0 + addr + 0x0008),
				ioread32(ppdev->bar0 + addr + 0x000c),
				ioread32(ppdev->bar0 + addr + 0x0010),
				ioread32(ppdev->bar0 + addr + 0x0014),
				ioread32(ppdev->bar0 + addr + 0x0018));
		}
	}

	return strlen(buf);
}

static ssize_t mvppnd_store_atu_win(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_atu_win);

	ppdev->atu_win_addr_base = 0; /* Since we are changing the window */
	if ((sscanf(buf, "%d", &ppdev->atu_win) != 1) ||
	    (ppdev->atu_win < -1) || (ppdev->atu_win >= NUM_OF_ATU_WINDOWS)) {
		dev_err(&ppdev->pdev->dev,
			"Invalid atu_win number %d, disabling\n",
			ppdev->atu_win);
		ppdev->atu_win = -1;
	}

	debug_print_some_registers(ppdev);

	return count;
}

static int mvppnd_is_our_win(struct mvppnd_pci_dev *ppdev, u8 win)
{
	int i;

	for (i = 0; i < sizeof(ppdev->mg_win); i++)
		if (win == ppdev->mg_win[i])
			return true;

	return false;
}

static ssize_t mvppnd_show_mg_win(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_mg_win);
	int i;

	for (i = 0; i < NUM_OF_MG_WINDOWS; i++) {
		snprintf(buf, PAGE_SIZE,
			 "%s[%c%d] 0x%-8x\t0x%-8x\t0x%-8x\t0x%-8x\n", buf,
			 mvppnd_is_our_win(ppdev, i) ?  '*' : ' ', i,
			 mvppnd_read_reg(ppdev, REG_ADDR_MG_BASE_ADDR + i *
					 REG_ADDR_MG_BASE_ADDR_OFFSET_FORMULA),
			 mvppnd_read_reg(ppdev, REG_ADDR_MG_SIZE + i *
					 REG_ADDR_MG_SIZE_OFFSET_FORMULA),
			 mvppnd_read_reg(ppdev, REG_ADDR_MG_HA + i *
					 REG_ADDR_MG_HA_OFFSET_FORMULA),
			 mvppnd_read_reg(ppdev, REG_ADDR_MG_CONTROL + i *
					 REG_ADDR_MG_CONTROL_OFFSET_FORMULA));
	}

	return strlen(buf);
}

static void mvppnd_save_mg_wins(struct mvppnd_pci_dev *ppdev, long bitmask)
{
	int i, j;

	memset(ppdev->mg_win, 0, sizeof(ppdev->mg_win));
	for (i = 0, j = 0; i < NUM_OF_MG_WINDOWS; i++)
		if (test_bit(i, &bitmask))
			ppdev->mg_win[j++] = i;
}

static ssize_t mvppnd_store_mg_win(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_mg_win);
	int win_mask;

	if (mvppnd_num_of_rx_queues(ppdev)) {
		netdev_err(ppdev->ndev,
			   "RX rings already set, cannot modify MG windows\n");
		mvppnd_sysfs_set_read_only(ppdev, attr);
		return -EPERM;
	}

	if (sscanf(buf, "0x%x", &win_mask) != 1) {
		netdev_err(ppdev->ndev, "Invalid input\n");
		goto out;
	}

	mvppnd_save_mg_wins(ppdev, win_mask);
out:
	return count;
}

static ssize_t mvppnd_show_max_pkt_sz(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_max_pkt_sz);

	snprintf(buf, PAGE_SIZE, "%ld\n", ppdev->max_pkt_sz);

	return strlen(buf);
}

static ssize_t mvppnd_store_max_pkt_sz(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_max_pkt_sz);

	sscanf(buf, "%ld", &ppdev->max_pkt_sz);

	mvppnd_sysfs_set_read_only(ppdev, attr);

	return count;
}

static ssize_t mvppnd_show_mac(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_mac);

	snprintf(buf, PAGE_SIZE, "%pM\n", ppdev->ndev->dev_addr);

	return strlen(buf);
}

static ssize_t mvppnd_store_mac(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_mac);
	unsigned int mac[ETH_ALEN];
	int i;

	sscanf(buf, "%x:%x:%x:%x:%x:%x\n", &mac[0], &mac[1], &mac[2], &mac[3],
	       &mac[4], &mac[5]);

	for (i = 0; i < ETH_ALEN; i++)
		ppdev->ndev->dev_addr[i] = mac[i];

	return count;
}

static ssize_t mvppnd_show_dsa(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_dsa);
	unsigned int dsa[DSA_SIZE];
	int i;

	for (i = 0; i < DSA_SIZE; i++)
		dsa[i] = ppdev->config_dsa[i];

	snprintf(buf, PAGE_SIZE,
		 "%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x\n",
		 dsa[0], dsa[1], dsa[2], dsa[3], dsa[4], dsa[5], dsa[6], dsa[7],
		 dsa[8], dsa[9], dsa[10], dsa[11], dsa[12], dsa[13], dsa[14],
		 dsa[15]);
	snprintf(buf, PAGE_SIZE,
		 "%s%.2x%.2x%.2x%.2x %.2x%.2x%.2x%.2x %.2x%.2x%.2x%.2x %.2x%.2x%.2x%.2x\n",
		 buf, dsa[0], dsa[1], dsa[2], dsa[3], dsa[4], dsa[5], dsa[6],
		 dsa[7], dsa[8], dsa[9], dsa[10], dsa[11], dsa[12], dsa[13],
		 dsa[14], dsa[15]);
	snprintf(buf, PAGE_SIZE,
		 "%s%.2x %.2x %.2x %.2x %.2x %.2x %.2x %.2x %.2x %.2x %.2x %.2x %.2x %.2x %.2x %.2x\n",
		 buf, dsa[0], dsa[1], dsa[2], dsa[3], dsa[4], dsa[5], dsa[6],
		 dsa[7], dsa[8], dsa[9], dsa[10], dsa[11], dsa[12], dsa[13],
		 dsa[14], dsa[15]);

	return strlen(buf);
}

static ssize_t mvppnd_store_dsa(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_dsa);
	unsigned int dsa[DSA_SIZE];
	int i;

	ppdev->config_dsa_size =
		sscanf(buf, "%x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x",
		       &dsa[0], &dsa[1], &dsa[2], &dsa[3], &dsa[4], &dsa[5],
		       &dsa[6], &dsa[7], &dsa[8], &dsa[9], &dsa[10], &dsa[11],
		       &dsa[12], &dsa[13], &dsa[14], &dsa[15]);

	memset(ppdev->config_dsa, 0, sizeof(ppdev->config_dsa));
	for (i = 0; i < DSA_SIZE; i++)
		if (i < ppdev->config_dsa_size)
			ppdev->config_dsa[i] = dsa[i];

	return count;
}

static ssize_t mvppnd_show_rx_queues_weight(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *buf)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_rx_queues_weight);
	int i;

	for (i = 0; i < NUM_OF_RX_QUEUES; i++)
		sprintf(buf, "%squeue %d: %d\n", buf, i, ppdev->rx_queues_weight[i]);

	return strlen(buf);
}

static ssize_t mvppnd_store_rx_queues_weight(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     const char *buf, size_t count)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_rx_queues_weight);
	u32 weights;
	int rc;

	rc = sscanf(buf, "0x%x", &weights);
	if (rc != 1) {
		dev_err(&ppdev->pdev->dev, "Invalid input, expecting addr [val]\n");
		return -EINVAL;
	}

	mvppnd_setup_rx_queues_weights(ppdev, weights);

	return count;
}

static ssize_t mvppnd_show_rx_ring_size(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buf)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_rx_ring_size);

	sprintf(buf, "%d\n", ppdev->rx_ring_size);

	return strlen(buf);
}

static ssize_t mvppnd_store_rx_ring_size(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 const char *buf, size_t count)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_rx_ring_size);
	int rc;

	rc = sscanf(buf, "%d", &ppdev->rx_ring_size);
	if ( (rc != 1) || (ppdev->rx_ring_size >= MAX_RX_RING_SIZE) ) {
		dev_err(&ppdev->pdev->dev, "Invalid input, expecting < %d\n",
			MAX_RX_RING_SIZE);
		ppdev->rx_ring_size = DEFAULT_RX_RING_SIZE;
		return -EINVAL;
	}

	ppdev->rx_ring_size = roundup_pow_of_two(ppdev->rx_ring_size);

	return count;
}

static ssize_t mvppnd_show_napi_poll_weight(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *buf)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_napi_poll_weight);

	sprintf(buf, "%d\n", ppdev->napi_poll_weight);

	return strlen(buf);
}

static ssize_t mvppnd_store_napi_poll_weight(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     const char *buf, size_t count)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_napi_poll_weight);
	int rc;

	rc = sscanf(buf, "%d", &ppdev->napi_poll_weight);
	if (rc != 1) {
		dev_err(&ppdev->pdev->dev, "Invalid input\n");
		return -EINVAL;
	}

	return count;
}

static ssize_t mvppnd_show_driver_statistics(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     char *buf)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_driver_statistics);
	static unsigned long last_jiffies;
	int i;

	for (i = 0; i <= STATS_LAST; i++)
		sprintf(buf, "%s%s: %ld\n", buf, mvppnd_get_stat_desc(i),
			mvppnd_get_stat(ppdev, i, last_jiffies));
	last_jiffies = jiffies;

	return strlen(buf);
}

static ssize_t mvppnd_store_driver_statistics(struct kobject *kobj,
					      struct kobj_attribute *attr,
					      const char *buf, size_t count)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_driver_statistics);
	u32 entries_bitmask;
	int rc, i;

	rc = sscanf(buf, "0x%x", &entries_bitmask);
	if (rc != 1) {
		dev_err(&ppdev->pdev->dev,
			"Invalid input, expecting 32 bit hex number\n");
		return -EINVAL;
	}

	for (i = 0; i <= STATS_LAST; i++) {
		if ((entries_bitmask & 0x00000001) == 0x00000001)
			mvppnd_clear_stat(ppdev, i);
		entries_bitmask = entries_bitmask >> 1;
	}

	return count;
}

#ifdef MVPPND_DEBUG
static ssize_t mvppnd_show_reg(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_reg);

	if (!ppdev->sysfs_reg_addr) /* Not yet configured */
		return 0;

	snprintf(buf, PAGE_SIZE, "0x%x\n",
		 mvppnd_read_reg(ppdev, ppdev->sysfs_reg_addr));

	return strlen(buf);
}

static ssize_t mvppnd_store_reg(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	struct mvppnd_pci_dev *ppdev = container_of(attr, struct mvppnd_pci_dev,
						    attr_reg);
	int argc;
	u32 val;
	int i;

	argc = sscanf(buf, "0x%x 0x%x", &ppdev->sysfs_reg_addr, &val);
	if (argc != 1 && argc != 2) {
		dev_err(&ppdev->pdev->dev, "Invalid input, expecting addr [val]\n");
		ppdev->sysfs_reg_addr = 0;
	}

	if (ppdev->sysfs_reg_addr == 0x1) { /* 0x1: clear netdev stat */
		ppdev->ndev->stats.rx_packets = 0;
		ppdev->ndev->stats.rx_bytes = 0;
		ppdev->ndev->stats.rx_dropped = 0;
		ppdev->ndev->stats.tx_packets = 0;
		ppdev->ndev->stats.tx_bytes = 0;
		ppdev->ndev->stats.tx_dropped = 0;
		goto out;
	}

	if (ppdev->sysfs_reg_addr == 0x2) { /* 0x2: enable interrupts (i.e. fake
                                               NAPI) */
		mvppnd_enable_rx_interrupts(ppdev);
		goto out;
	}

	if (ppdev->sysfs_reg_addr == 0x3) { /* 0x3: Interval to print packets */
		ppdev->print_packets_interval = val;
		goto out;
	}

	if (ppdev->sysfs_reg_addr == 0x4) { /* 0x4: print next-desc-ptrs */
		printk("next-desc-ptrs:\n");
		for (i = 0; i < NUM_OF_RX_QUEUES; i++)
			printk("[%d] 0x%x\n", i,
			       mvppnd_read_rx_first_desc(ppdev, i));
	}

	if (argc == 2) /* i.e user wish to set value to register */
		mvppnd_write_reg(ppdev, ppdev->sysfs_reg_addr, val);

out:
	return count;
}
#endif

static int mvppnd_sysfs_create_file(struct mvppnd_pci_dev *ppdev,
				    struct kobj_attribute *attr,
				    const char *name, umode_t mode,
				    ssize_t (*show)(struct kobject *kobj,
						    struct kobj_attribute *attr,
						    char *buf),
				    ssize_t (*store)(struct kobject *kobj,
						     struct kobj_attribute *attr,
						     const char *buf,
						     size_t count))
{
	attr->attr.name = name;
	attr->attr.mode = mode;
	attr->show = show;
	attr->store = store;

	return sysfs_create_file(&ppdev->ndev->dev.kobj, &attr->attr);
}

static int mvppnd_sysfs_create_files(struct mvppnd_pci_dev *ppdev)
{
	int rc;

	rc = mvppnd_sysfs_create_file(ppdev, &ppdev->attr_rx_queues,
				      "rx_queues", 0644, mvppnd_show_rx_queues,
				      mvppnd_store_rx_queues);
	if (rc) {
		netdev_err(ppdev->ndev, "Fail to create rx_queues sysfs file\n");
		return rc;
	}

	rc = mvppnd_sysfs_create_file(ppdev, &ppdev->attr_tx_queue, "tx_queue",
				      0644, mvppnd_show_tx_queue,
		   		      mvppnd_store_tx_queue);
	if (rc) {
		netdev_err(ppdev->ndev, "Fail to create tx_queue sysfs file\n");
		goto remove_rx_queue;
	}

	rc = mvppnd_sysfs_create_file(ppdev, &ppdev->attr_mg_win, "mg_win",
				      0644, mvppnd_show_mg_win,
				      mvppnd_store_mg_win);
	if (rc) {
		netdev_err(ppdev->ndev,
			   "Fail to create mg_win sysfs file\n");
		goto remove_tx_queue;
	}

	rc = mvppnd_sysfs_create_file(ppdev, &ppdev->attr_max_pkt_sz,
				      "max_pkt_sz", 0644,
				      mvppnd_show_max_pkt_sz,
				      mvppnd_store_max_pkt_sz);
	if (rc) {
		netdev_err(ppdev->ndev,
			   "Fail to create max_pkt_sz sysfs file\n");
		goto remove_mg_win;
	}

	rc = mvppnd_sysfs_create_file(ppdev, &ppdev->attr_mac,
				      "mac", 0644, mvppnd_show_mac,
				      mvppnd_store_mac);
	if (rc) {
		netdev_err(ppdev->ndev, "Fail to create mac sysfs file\n");
		goto remove_max_pkt_sz;
	}

	rc = mvppnd_sysfs_create_file(ppdev, &ppdev->attr_dsa,
				      "dsa", 0644, mvppnd_show_dsa,
				      mvppnd_store_dsa);
	if (rc) {
		netdev_err(ppdev->ndev, "Fail to create dsa sysfs file\n");
		goto remove_mac;
	}

	rc = mvppnd_sysfs_create_file(ppdev, &ppdev->attr_rx_queues_weight,
				      "rx_queues_weight", 0644,
				      mvppnd_show_rx_queues_weight,
				      mvppnd_store_rx_queues_weight);
	if (rc) {
		netdev_err(ppdev->ndev,
			   "Fail to create rx_queues_weight sysfs file\n");
		goto remove_dsa;
	}

	rc = mvppnd_sysfs_create_file(ppdev, &ppdev->attr_driver_statistics,
				      "driver_statistics", 0644,
				      mvppnd_show_driver_statistics,
				      mvppnd_store_driver_statistics);
	if (rc) {
		netdev_err(ppdev->ndev,
			   "Fail to create driver_statistics sysfs file\n");
		goto remove_rx_queues_weight;
	}

	rc = mvppnd_sysfs_create_file(ppdev, &ppdev->attr_napi_poll_weight,
				      "napi_poll_weight", 0644,
				      mvppnd_show_napi_poll_weight,
				      mvppnd_store_napi_poll_weight);
	if (rc) {
		netdev_err(ppdev->ndev,
			   "Fail to create napi_poll_weight sysfs file\n");
		goto remove_driver_statistics;
	}

	rc = mvppnd_sysfs_create_file(ppdev, &ppdev->attr_rx_ring_size,
				      "rx_ring_size", 0644,
				      mvppnd_show_rx_ring_size,
				      mvppnd_store_rx_ring_size);
	if (rc) {
		netdev_err(ppdev->ndev,
			   "Fail to create rx_ring_size sysfs file\n");
		goto remove_napi_poll_weight;
	}

	if (ppdev->pdev->device != PCI_DEVICE_ID_ALDRIN2) {
		rc = mvppnd_sysfs_create_file(ppdev, &ppdev->attr_atu_win,
					      "atu_win", 0644,
					      mvppnd_show_atu_win,
					      mvppnd_store_atu_win);
		if (rc) {
			netdev_err(ppdev->ndev,
				   "Fail to create atu_win sysfs file\n");
			goto remove_rx_ring_size;
		}
	}

#ifdef MVPPND_DEBUG
	rc = mvppnd_sysfs_create_file(ppdev, &ppdev->attr_reg, "reg", 0644,
				      mvppnd_show_reg, mvppnd_store_reg);
	if (rc) {
		netdev_err(ppdev->ndev, "Fail to create reg sysfs file\n");
		goto remove_atu_win;
	}

	goto out;

remove_atu_win:
	if (ppdev->pdev->device != PCI_DEVICE_ID_ALDRIN2)
		sysfs_remove_file(&ppdev->ndev->dev.kobj,
				  &ppdev->attr_atu_win.attr);
#else
	goto out;
#endif

remove_rx_ring_size:
	sysfs_remove_file(&ppdev->ndev->dev.kobj,
			  &ppdev->attr_rx_ring_size.attr);

remove_napi_poll_weight:
	sysfs_remove_file(&ppdev->ndev->dev.kobj,
			  &ppdev->attr_napi_poll_weight.attr);

remove_driver_statistics:
	sysfs_remove_file(&ppdev->ndev->dev.kobj,
			  &ppdev->attr_driver_statistics.attr);

remove_rx_queues_weight:
	sysfs_remove_file(&ppdev->ndev->dev.kobj,
			  &ppdev->attr_rx_queues_weight.attr);

remove_dsa:
	sysfs_remove_file(&ppdev->ndev->dev.kobj, &ppdev->attr_dsa.attr);

remove_mac:
	sysfs_remove_file(&ppdev->ndev->dev.kobj, &ppdev->attr_mac.attr);

remove_max_pkt_sz:
	sysfs_remove_file(&ppdev->ndev->dev.kobj, &ppdev->attr_max_pkt_sz.attr);

remove_mg_win:
	sysfs_remove_file(&ppdev->ndev->dev.kobj, &ppdev->attr_mg_win.attr);

remove_tx_queue:
	sysfs_remove_file(&ppdev->ndev->dev.kobj, &ppdev->attr_tx_queue.attr);

remove_rx_queue:
	sysfs_remove_file(&ppdev->ndev->dev.kobj, &ppdev->attr_rx_queues.attr);

out:
	return rc;
}

static void mvppnd_sysfs_remove_files(struct mvppnd_pci_dev *ppdev)
{
#ifdef MVPPND_DEBUG
	sysfs_remove_file(&ppdev->ndev->dev.kobj, &ppdev->attr_reg.attr);
#endif
	sysfs_remove_file(&ppdev->ndev->dev.kobj,
			  &ppdev->attr_rx_ring_size.attr);
	sysfs_remove_file(&ppdev->ndev->dev.kobj,
			  &ppdev->attr_napi_poll_weight.attr);
	sysfs_remove_file(&ppdev->ndev->dev.kobj,
			  &ppdev->attr_driver_statistics.attr);
	sysfs_remove_file(&ppdev->ndev->dev.kobj,
			  &ppdev->attr_rx_queues_weight.attr);
	sysfs_remove_file(&ppdev->ndev->dev.kobj, &ppdev->attr_dsa.attr);
	sysfs_remove_file(&ppdev->ndev->dev.kobj, &ppdev->attr_mac.attr);
	sysfs_remove_file(&ppdev->ndev->dev.kobj, &ppdev->attr_max_pkt_sz.attr);
	sysfs_remove_file(&ppdev->ndev->dev.kobj, &ppdev->attr_mg_win.attr);
	if (ppdev->pdev->device != PCI_DEVICE_ID_ALDRIN2)
		sysfs_remove_file(&ppdev->ndev->dev.kobj,
				  &ppdev->attr_atu_win.attr);
	sysfs_remove_file(&ppdev->ndev->dev.kobj, &ppdev->attr_tx_queue.attr);
	sysfs_remove_file(&ppdev->ndev->dev.kobj, &ppdev->attr_rx_queues.attr);
}

/*********** tx functions ******************************/
static int mvppnd_xmit_buf(struct mvppnd_pci_dev *ppdev, struct sk_buff *skb,
			   struct mvppnd_dma_sg_buf *sgb)
{
	bool sdma_took, wait_too_long;
	int wr_ptr, wr_ptr_first;
	size_t total_bytes = 0;
	u32 tmp_next_desc_ptr; /* TODO: Working in 'list' mode */
	unsigned long jiffs; /* Wait for SDMA to take the desc */
	int data_ptr;

	if (!sgb->mappings[0])
		return -EINVAL;

	wr_ptr = cyclic_idx(ppdev->tx_ring.descs_ptr, TX_RING_SIZE);
	wr_ptr_first = wr_ptr;

	/* MAC */
	memcpy(ppdev->mac.virt, skb->data, ETH_ALEN * 2);
	ppdev->tx_ring.descs[wr_ptr]->buf_addr = ppdev->mac.dma;
	TX_DESC_SET_BYTE_CNT(ppdev->tx_ring.descs[wr_ptr]->bc, ETH_ALEN * 2);
	total_bytes += ETH_ALEN * 2;
	/*
	dev_dbg(&ppdev->pdev->dev, "MACs: desc %d, len %d (%ld), ptr 0x%llx\n",
		wr_ptr, ETH_ALEN * 2, total_bytes, ppdev->mac.dma);
	*/
	cyclic_inc(&wr_ptr, TX_RING_SIZE);

	/* DSA */
	memcpy(ppdev->dsa.virt, ppdev->config_dsa, ppdev->config_dsa_size);
	ppdev->tx_ring.descs[wr_ptr]->buf_addr = ppdev->dsa.dma;
	TX_DESC_SET_BYTE_CNT(ppdev->tx_ring.descs[wr_ptr]->bc,
			     ppdev->config_dsa_size);
	ppdev->tx_ring.descs[wr_ptr]->cmd_sts = TX_CMD_BIT_OWN_SDMA |
						TX_CMD_BIT_CRC;
	/*
	dev_dbg(&ppdev->pdev->dev, "DSA : desc %d, len %d (%ld), ptr 0x%llx\n",
		wr_ptr, DSA_SIZE, total_bytes, ppdev->dsa.dma);
	*/
	total_bytes += ppdev->config_dsa_size;
	cyclic_inc(&wr_ptr, TX_RING_SIZE);

	/* Data */
	data_ptr = 0;
	while (sgb->mappings[data_ptr]) {
		ppdev->tx_ring.descs[wr_ptr]->buf_addr =
			sgb->mappings[data_ptr];
		TX_DESC_SET_BYTE_CNT(ppdev->tx_ring.descs[wr_ptr]->bc,
				     sgb->sizes[data_ptr]);
		total_bytes += sgb->sizes[data_ptr];
		ppdev->tx_ring.descs[wr_ptr]->cmd_sts = TX_CMD_BIT_OWN_SDMA |
							TX_CMD_BIT_CRC;
		/*
		dev_dbg(&ppdev->pdev->dev,
			"data: desc %d, len %ld (%ld), ptr 0x%llx\n",
			wr_ptr, sgb->sizes[data_ptr], total_bytes,
			sgb->mappings[data_ptr]);
		*/

		data_ptr++;
		if (sgb->mappings[data_ptr]) /* We have more? */
			cyclic_inc(&wr_ptr, TX_RING_SIZE);
	}

	/* Last descriptor - add last */
	ppdev->tx_ring.descs[wr_ptr]->cmd_sts |= TX_CMD_BIT_LAST;
	/* TODO: For some reason ring does not work so for now let's use list */
	tmp_next_desc_ptr = ppdev->tx_ring.descs[wr_ptr]->next_desc_ptr;
	ppdev->tx_ring.descs[wr_ptr]->next_desc_ptr = 0;

	mvppnd_write_tx_first_desc(ppdev, ppdev->tx_ring.ring_dma);

	/* We are ready, let's update the first descriptor - ownership, CRC &
	   first */
	ppdev->tx_ring.descs[wr_ptr_first]->cmd_sts = TX_CMD_BIT_OWN_SDMA |
						      TX_CMD_BIT_CRC |
						      TX_CMD_BIT_FIRST;

	/* Flash descriptors before enabling the queue */
	mb();

	mvppnd_enable_queue(ppdev, REG_ADDR_TX_QUEUE_CMD, ppdev->tx_queue);


	jiffs = jiffies;
	do {
		sdma_took = ((ppdev->tx_ring.descs[wr_ptr_first]->cmd_sts &
			     TX_CMD_BIT_OWN_SDMA) != TX_CMD_BIT_OWN_SDMA);
		wait_too_long = (jiffies_to_usecs(jiffies - jiffs) >
				 TX_WAIT_FOR_CPU_OWENERSHIP_USEC);
		mb();
	} while (!sdma_took && !wait_too_long);

	/*
	dev_dbg(&ppdev->pdev->dev,
		"Took %d usec to SDMA to take the descriptor\n",
		jiffies_to_usecs(jiffies - jiffs));
	*/

	/* TODO: For some reason ring does not work so for now let's use list */
	ppdev->tx_ring.descs_ptr = 0;
	ppdev->tx_ring.descs[wr_ptr]->next_desc_ptr = tmp_next_desc_ptr;

	/* TODO: The ring thing */
	/* cyclic_inc(&wr_ptr, TX_RING_SIZE); */
	/* ppdev->tx_ring.descs_ptr = wr_ptr; */

	if (wait_too_long)
		return -EIO;

	return total_bytes - ETH_ALEN * 2 - ppdev->config_dsa_size;
}

static void mvppnd_transmit_skb(struct mvppnd_pci_dev *ppdev,
				struct sk_buff *skb)
{
	struct mvppnd_dma_sg_buf sgb = {};
	int rc;

	/*
	dev_dbg(&ppdev->pdev->dev, "Got packet to transmit, len %d (head %d)\n",
		skb->len, skb_headlen(skb));
	*/
	print_frame(ppdev, skb->data, 100, false);
	print_skb_hdr(ppdev, skb->data);

	rc = mvppnd_copy_skb(ppdev, skb, &sgb);
	if (rc) {
		dev_dbg(&ppdev->pdev->dev, "Fail to map skb %p\n", skb->data);
		return;
	}

	rc = mvppnd_xmit_buf(ppdev, skb, &sgb);
	if (rc > 0) {
		mvppnd_inc_stat(ppdev, STATS_TX_PACKETS, 1);
		ppdev->ndev->stats.tx_packets++;
		ppdev->ndev->stats.tx_bytes += rc;
	} else {
		ppdev->ndev->stats.tx_dropped++;
	}

	kfree_skb(skb);
}

#ifdef TX_WITH_WORKER_THREAD
static void mvppnd_tx_work(struct work_struct *work)
{
	struct mvppnd_skb_work *skb_work = container_of(work,
							struct mvppnd_skb_work,
							work);

	mvppnd_transmit_skb(skb_work->ppdev, skb_work->skb);

	kfree(work);
}
#endif

/*********** netdev ops ********************************/
int mvppnd_open(struct net_device *dev)
{
	struct mvppnd_pci_dev *ppdev = netdev_priv(dev);
	int rc;

	if (ppdev->tx_queue == -1) {
		netdev_err(ppdev->ndev,
			   "Can't open device while tx_queue is not set\n");
		return -EPERM;
	}
	/* TODO: Check other mandatory settings */

	if (mvppnd_num_of_rx_queues(ppdev) == 0) {
		netdev_err(ppdev->ndev,
			   "Can't open device while rx_queues is not set\n");
		return -EPERM;
	}

	rc = mvppnd_alloc_device_coherent(ppdev);
	if (rc < 0) {
		netdev_err(ppdev->ndev, "Fail to allocate coherent mempry\n");
		return -ENOMEM;
	}

	rc = mvppnd_setup_rx_rings(ppdev);
	if (rc) {
		dev_err(&ppdev->pdev->dev, "Fail to create rx rings\n");
		goto free_coherent;
	}

	rc = mvppnd_setup_tx_ring(ppdev);
	if (rc) {
		dev_err(&ppdev->pdev->dev,
			"Fail to create tx ring %d\n", ppdev->tx_queue);
		ppdev->tx_queue = -1;
		goto destroy_rx_rings;
	}

	ppdev->mac.virt = mvppnd_alloc_coherent(ppdev, ETH_ALEN * 2,
						&ppdev->mac.dma);
	ppdev->dsa.virt = mvppnd_alloc_coherent(ppdev, DSA_SIZE,
						&ppdev->dsa.dma);

	/* No more changes to some sysfs entries */
	mvppnd_sysfs_set_read_only(ppdev, &ppdev->attr_napi_poll_weight);

	ppdev->tx_wq = create_workqueue("mvppnd_tx");
	if (!ppdev->tx_wq) {
		netdev_err(ppdev->ndev, "Fail to allocate TX work queue\n");
		goto destroy_tx_rings;
	}

	sema_init(&ppdev->interrupts_sema, 0);
	/* Start RX kernel thread */
	ppdev->rx_thread = kthread_run(mvppnd_rx_thread, ppdev, "mvppnd_rx");
	if (!ppdev->rx_thread) {
		netdev_err(ppdev->ndev, "Fail to create rx thread\n");
		goto destroy_rx_wq;
	}

	netif_napi_add(dev, &ppdev->napi, mvppnd_poll, ppdev->napi_poll_weight);
	napi_enable(&ppdev->napi);

	/* Register with int driver to receive IRQs */
	rc = mvintdrv_register_isr_sema(&ppdev->interrupts_sema);
	if (rc < 0) {
		netdev_err(ppdev->ndev, "Fail to register ISR\n");
		goto destroy_rx_thread;
	}

	mvppnd_disable_tx_interrupts(ppdev);
	mvppnd_enable_rx_interrupts(ppdev);

	debug_print_some_registers(ppdev);

	return 0;

destroy_rx_thread:
	netif_napi_del(&ppdev->napi);
	mvppnd_stop_rx_thread(ppdev);

destroy_rx_wq:
	destroy_workqueue(ppdev->tx_wq);

destroy_tx_rings:
	mvppnd_destroy_tx_ring(ppdev);
	ppdev->tx_wq = NULL;

destroy_rx_rings:
	mvppnd_destroy_rx_rings(ppdev);

free_coherent:
	mvppnd_free_device_coherent(ppdev);

	return -EIO;
}

int mvppnd_stop(struct net_device *dev)
{
	struct mvppnd_pci_dev *ppdev = netdev_priv(dev);
	int i;

	mvppnd_disable_rx_interrupts(ppdev);

	mvintdrv_unregister_isr_sema(&ppdev->interrupts_sema);

	napi_disable(&ppdev->napi);

	netif_napi_del(&ppdev->napi);

	mvppnd_stop_rx_thread(ppdev);

	if (ppdev->tx_wq)
		destroy_workqueue(ppdev->tx_wq);

	mvppnd_destroy_rings(ppdev);

	mvppnd_free_device_coherent(ppdev);

	for (i = 0; i < sizeof(ppdev->mg_win); i++)
		if (ppdev->mg_win[i])
			mvppnd_setup_mg_window(ppdev, ppdev->mg_win[i], 0, 0);

	return 0;
}

netdev_tx_t mvppnd_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct mvppnd_pci_dev *ppdev = netdev_priv(dev);
#ifdef TX_WITH_WORKER_THREAD
	struct mvppnd_skb_work *skb_work = kmalloc(sizeof(*skb_work),
						   GFP_KERNEL);
#endif

	/* This should not happen because we're trying to synch tx_queue with
	 * the interface state */
	if (ppdev->tx_queue == -1) {
		netdev_err(ppdev->ndev, "TX queue not yet initialized\n");
		return NETDEV_TX_OK;
	}

#ifdef TX_WITH_WORKER_THREAD
	INIT_WORK(&skb_work->work, mvppnd_tx_work);
	skb_work->ppdev = ppdev;
	skb_work->skb = skb;

	queue_work(ppdev->tx_wq, &skb_work->work);
#else
	mvppnd_transmit_skb(ppdev, skb);
#endif

	return NETDEV_TX_OK;
}

static const struct net_device_ops mvppnd_ops = {
	.ndo_init		= NULL,
	.ndo_open		= mvppnd_open,
	.ndo_stop		= mvppnd_stop,
	.ndo_uninit		= NULL,
	.ndo_get_stats64	= NULL,
	.ndo_start_xmit		= mvppnd_start_xmit,
	.ndo_tx_timeout		= NULL,
	.ndo_validate_addr	= NULL,
	.ndo_change_mtu		= NULL,
	.ndo_fix_features	= NULL,
	.ndo_set_features	= NULL,
	.ndo_set_mac_address	= NULL,
	.ndo_do_ioctl		= NULL,
	.ndo_set_rx_mode	= NULL,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= NULL,
#endif

};

static void mvppnd_clean_ppdev(struct mvppnd_pci_dev *ppdev)
{
	mutex_destroy(&ppdev->queues_mutex);
}

static struct mvppnd_pci_dev *mvppnd_create_netdev(struct pci_dev *pdev)
{
	struct mvppnd_pci_dev *ppdev;
	struct net_device *ndev;
	int rc;

	ndev = alloc_netdev_mqs(sizeof(*ppdev), "mvpp%d", NET_NAME_UNKNOWN,
				ether_setup, 1, 1);
	if (!ndev)
		return 0;

	ppdev = netdev_priv(ndev);
	memset(ppdev, 0, sizeof(*ppdev));
	ppdev->pdev = pdev;
	ppdev->ndev = ndev;

	SET_NETDEV_DEV(ppdev->ndev, &ppdev->pdev->dev);

	ppdev->ndev->netdev_ops = &mvppnd_ops;

	rc = register_netdev(ppdev->ndev);
	if (rc) {
		netdev_err(ppdev->ndev,
			   "Fail to register net device, aborting.\n");
		goto free_netdev;
	}

	rc = mvppnd_sysfs_create_files(ppdev);
	if (rc) {
		netdev_err(ppdev->ndev,
			   "Fail to create sysfs files, aborting.\n");
		goto unregister_netdev;
	}

	return ppdev;

unregister_netdev:
	unregister_netdev(ppdev->ndev);

free_netdev:
	free_netdev(ndev);

	return 0;
}

static void mvppnd_destroy_netdev(struct mvppnd_pci_dev *ppdev)
{
	if (!ppdev->ndev)
		return;

	mvppnd_sysfs_remove_files(ppdev);

	unregister_netdev(ppdev->ndev);

	mvppnd_clean_ppdev(ppdev);

	free_netdev(ppdev->ndev);
}

/*********** pci ops ***********************************/
static void mvppnd_unmap_bars(struct mvppnd_pci_dev *ppdev)
{
	if (ppdev->bar2)
		iounmap(ppdev->bar2);
	if (ppdev->bar0)
		iounmap(ppdev->bar0);
}

static int mvppnd_map_bars(struct mvppnd_pci_dev *ppdev)
{
	ppdev->bar0 = ioremap(pci_resource_start(ppdev->pdev, 0),
			      pci_resource_len(ppdev->pdev, 0));
	if (!ppdev->bar0) {
		dev_err(&ppdev->pdev->dev,
			"Fail to remap to bar0, aborting.\n");
		return -ENOMEM;
	}
	dev_dbg(&ppdev->pdev->dev, "bar0 size %lld\n",
		pci_resource_len(ppdev->pdev, 0));

	ppdev->bar2 = ioremap(pci_resource_start(ppdev->pdev, 2),
			      pci_resource_len(ppdev->pdev, 2));
	if (!ppdev->bar2) {
		dev_err(&ppdev->pdev->dev,
			"Fail to remap to bar2, aborting.\n");
		goto unmap_bars;
	}
	dev_dbg(&ppdev->pdev->dev, "bar2 size %lld\n",
		pci_resource_len(ppdev->pdev, 2));

	return 0;

unmap_bars:
	mvppnd_unmap_bars(ppdev);

	return -ENOMEM;
}

static void mvppnd_init_ppdev(struct mvppnd_pci_dev *ppdev)
{
	int i;

	mutex_init(&ppdev->queues_mutex);

	ppdev->tx_queue = -1;
	for (i = 0; i < NUM_OF_RX_QUEUES; i++)
		ppdev->rx_queues[i] = -1;
	if (ppdev->pdev->device != PCI_DEVICE_ID_ALDRIN2)
		ppdev->atu_win = DEFAULT_ATU_WIN;
	else
		ppdev->atu_win = -1;
	ppdev->atu_win_addr_base = 0;
	ppdev->max_pkt_sz = DEFAULT_MTU;
	mvppnd_save_mg_wins(ppdev, DEFAULT_MG_WIN);
	ppdev->napi_poll_weight = DEFAULT_NAPI_POLL_WEIGHT;
	ppdev->rx_ring_size = DEFAULT_RX_RING_SIZE;
	mvppnd_setup_rx_queues_weights(ppdev, DEFAULT_RX_QUEUES_WEIGHT);

	memcpy(ppdev->ndev->dev_addr, DEFAULT_MAC, ETH_ALEN);
	ppdev->config_dsa_size = sizeof(DEFAULT_TX_DSA);
	memcpy(ppdev->config_dsa, DEFAULT_TX_DSA, ppdev->config_dsa_size);
}

static int mvppnd_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct mvppnd_pci_dev *ppdev;
	int i, rc;

	dev_dbg(&pdev->dev, "bus: %d, vendor: 0x%04x, device: 0x%04x\n",
		pdev->bus->number, pdev->vendor, pdev->device);

	ppdev = mvppnd_create_netdev(pdev);
	if (!ppdev) {
		dev_err(&pdev->dev, "Fail to create netdev, aborting.\n");
		return -ENOMEM;
	}

	mvppnd_init_ppdev(ppdev);

	pci_set_drvdata(pdev, ppdev);

	rc = pci_enable_device(pdev);
	if (rc) {
		dev_err(&pdev->dev, "Fail to enable PCI device, aborting.\n");
		rc = -ENOMEM;
		goto destroy_netdev;
	}

	for (i = 0; i < PCI_NUM_RESOURCES; i++)
		dev_dbg(&pdev->dev,
			"PCI BAR%d: %#lx %#llx %#llx %#llx\n", i,
			pci_resource_flags(pdev, i),
			(unsigned long long)pci_resource_start(pdev, i),
			(unsigned long long)pci_resource_end(pdev, i),
			(unsigned long long)pci_resource_len(pdev, i));

	/* Make sure that BAR0 and BAR2 are MMIO */
	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM) ||
	    !(pci_resource_flags(pdev, 2) & IORESOURCE_MEM)) {
		dev_err(&pdev->dev, "Invalid PCI resource type, aborting.\n");
		goto disable_pci_device;
	}

	rc = pci_request_regions(pdev, DRV_NAME);
	if (rc) {
		dev_err(&pdev->dev, "Fail to request regions, aborting.\n");
		goto disable_pci_device;
	}

	/* We want 32 bit address space */
	rc = pci_set_dma_mask(pdev, DMA_BIT_MASK(32));
	if (rc) {
		dev_err(&pdev->dev, "Fail to set 32bit DMA mask\n");
		goto free_regions;
	}

	pci_set_master(pdev);

	/* Map to BARs */
	rc = mvppnd_map_bars(ppdev);
	if (rc < 0)
		goto clear_pci_master;

	dev_info(&pdev->dev, "Probed to device\n");

	goto out;

clear_pci_master:
	pci_clear_master(pdev);

free_regions:
	pci_release_regions(pdev);

disable_pci_device:
	pci_disable_device(pdev);

destroy_netdev:
	mvppnd_destroy_netdev(ppdev);

out:
	return rc;
}

static void mvppnd_remove(struct pci_dev *pdev)
{
	struct mvppnd_pci_dev *ppdev = pci_get_drvdata(pdev);

	/* TODO: Cleanup should be sensitive so it will verify that resource was
	 *       initialized in probe, i.e if probe did not failed
         */
	mvppnd_destroy_netdev(ppdev);

	mvppnd_unmap_bars(ppdev);

	pci_clear_master(pdev);
	pci_release_regions(pdev);
	pci_disable_device(pdev);

	dev_info(&pdev->dev, "Detached from device\n");
}

static void mvppnd_shutdown(struct pci_dev *pdev)
{
	dev_dbg(&pdev->dev, "shutdown\n");
}

static const struct pci_device_id mvppnd_pci_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL, PCI_DEVICE_ID_FALCON), 0, 0, 0},
	{ PCI_DEVICE(PCI_VENDOR_ID_MARVELL, PCI_DEVICE_ID_ALDRIN2), 0, 0, 0},
	{ 0, 0, 0, 0, 0, 0, 0 }
};

MODULE_DEVICE_TABLE(pci, mvppnd_pci_tbl);

static struct pci_driver mvppnd_pci_driver = {
	.name		= DRV_NAME,
	.id_table	= mvppnd_pci_tbl,
	.probe		= mvppnd_probe,
	.remove		= mvppnd_remove,
	.shutdown	= mvppnd_shutdown,
	.driver.pm	= NULL,
};

/*********** module ************************************/
int mvppnd_init(void)
{
	int rc;

	rc = pci_register_driver(&mvppnd_pci_driver);
	printk(KERN_INFO "%s loaded\n", DRV_NAME);

	return rc;
}

void mvppnd_exit(void)
{
	pci_unregister_driver(&mvppnd_pci_driver);
	printk(KERN_INFO "%s unloaded\n", DRV_NAME);
}


module_init(mvppnd_init);
module_exit(mvppnd_exit);

MODULE_AUTHOR("Yuval Shaia <yshaia@marvell.com>");
MODULE_DESCRIPTION("Marvell's Prestera network driver");
MODULE_LICENSE("Dual BSD/GPL");
