/*
 * Copyright (C) 2011 Samsung Electronics.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <stdarg.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <net/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/rtc.h>
#include <linux/time.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/wait.h>
#include <linux/time.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/wakelock.h>
#include <trace/events/modem_if.h>
#include <linux/module.h>

#include "modem_prj.h"
#include "modem_utils.h"

#define CMD_SUSPEND	((u16)(0x00CA))
#define CMD_RESUME	((u16)(0x00CB))

#define TX_SEPARATOR	"mif: >>>>>>>>>> Outgoing packet "
#define RX_SEPARATOR	"mif: Incoming packet <<<<<<<<<<"
#define LINE_SEPARATOR	\
	"mif: ------------------------------------------------------------"
#define LINE_BUFF_SIZE	80

enum bit_debug_flags {
	DEBUG_FLAG_FMT,
	DEBUG_FLAG_RFS,
	DEBUG_FLAG_PS,
	DEBUG_FLAG_IOD,
	DEBUG_FLAG_CSVT,
	DEBUG_FLAG_BOOT,
	DEBUG_FLAG_DUMP,
	DEBUG_FLAG_LOG,
	DEBUG_FLAG_UNKNOWN
};

static unsigned long dflags = (1 << DEBUG_FLAG_FMT);
module_param(dflags, ulong, S_IRUGO | S_IWUSR | S_IWGRP);
MODULE_PARM_DESC(dflags, "modem_v1 debug flags");

/* ipc_log_level: 0 is the highest level */
static unsigned long ipc_log_level;
module_param(ipc_log_level, ulong, S_IRUGO | S_IWUSR | S_IWGRP);
MODULE_PARM_DESC(ipc_log_level, "modem_v1 log level for IPC message");

static const char *hex = "0123456789abcdef";

static struct raw_notifier_head cp_crash_notifier;

static inline void ts2utc(struct timespec *ts, struct utc_time *utc)
{
	struct tm tm;

	time_to_tm((ts->tv_sec - (sys_tz.tz_minuteswest * 60)), 0, &tm);
	utc->year = 1900 + tm.tm_year;
	utc->mon = 1 + tm.tm_mon;
	utc->day = tm.tm_mday;
	utc->hour = tm.tm_hour;
	utc->min = tm.tm_min;
	utc->sec = tm.tm_sec;
	utc->us = ns2us(ts->tv_nsec);
}

void get_utc_time(struct utc_time *utc)
{
	struct timespec ts;
	getnstimeofday(&ts);
	ts2utc(&ts, utc);
}

int mif_dump_log(struct modem_shared *msd, struct io_device *iod)
{
	unsigned long read_len = 0;
	unsigned long int flags;

	spin_lock_irqsave(&msd->lock, flags);
	while (read_len < MAX_MIF_BUFF_SIZE) {
		struct sk_buff *skb;

		skb = alloc_skb(MAX_IPC_SKB_SIZE, GFP_ATOMIC);
		if (!skb) {
			mif_err("ERR! alloc_skb fail\n");
			spin_unlock_irqrestore(&msd->lock, flags);
			return -ENOMEM;
		}
		memcpy(skb_put(skb, MAX_IPC_SKB_SIZE),
			msd->storage.addr + read_len, MAX_IPC_SKB_SIZE);
		skb_queue_tail(&iod->sk_rx_q, skb);
		read_len += MAX_IPC_SKB_SIZE;
		wake_up(&iod->wq);
	}
	spin_unlock_irqrestore(&msd->lock, flags);
	return 0;
}

static unsigned long long get_kernel_time(void)
{
	int this_cpu;
	unsigned long flags;
	unsigned long long time;

	preempt_disable();
	raw_local_irq_save(flags);

	this_cpu = smp_processor_id();
	time = cpu_clock(this_cpu);

	preempt_enable();
	raw_local_irq_restore(flags);

	return time;
}

void mif_ipc_log(enum mif_log_id id,
	struct modem_shared *msd, const char *data, size_t len)
{
	struct mif_ipc_block *block;
	unsigned long int flags;

	spin_lock_irqsave(&msd->lock, flags);

	block = (struct mif_ipc_block *)
		(msd->storage.addr + (MAX_LOG_SIZE * msd->storage.cnt));
	msd->storage.cnt = ((msd->storage.cnt + 1) < MAX_LOG_CNT) ?
		msd->storage.cnt + 1 : 0;

	spin_unlock_irqrestore(&msd->lock, flags);

	block->id = id;
	block->time = get_kernel_time();
	block->len = (len > MAX_IPC_LOG_SIZE) ? MAX_IPC_LOG_SIZE : len;
	memcpy(block->buff, data, block->len);
}

void _mif_irq_log(enum mif_log_id id, struct modem_shared *msd,
	struct mif_irq_map map, const char *data, size_t len)
{
	struct mif_irq_block *block;
	unsigned long int flags;

	spin_lock_irqsave(&msd->lock, flags);

	block = (struct mif_irq_block *)
		(msd->storage.addr + (MAX_LOG_SIZE * msd->storage.cnt));
	msd->storage.cnt = ((msd->storage.cnt + 1) < MAX_LOG_CNT) ?
		msd->storage.cnt + 1 : 0;

	spin_unlock_irqrestore(&msd->lock, flags);

	block->id = id;
	block->time = get_kernel_time();
	memcpy(&(block->map), &map, sizeof(struct mif_irq_map));
	if (data)
		memcpy(block->buff, data,
			(len > MAX_IRQ_LOG_SIZE) ? MAX_IRQ_LOG_SIZE : len);
}

void _mif_com_log(enum mif_log_id id,
	struct modem_shared *msd, const char *format, ...)
{
	struct mif_common_block *block;
	unsigned long int flags;
	va_list args;

	spin_lock_irqsave(&msd->lock, flags);

	block = (struct mif_common_block *)
		(msd->storage.addr + (MAX_LOG_SIZE * msd->storage.cnt));
	msd->storage.cnt = ((msd->storage.cnt + 1) < MAX_LOG_CNT) ?
		msd->storage.cnt + 1 : 0;

	spin_unlock_irqrestore(&msd->lock, flags);

	block->id = id;
	block->time = get_kernel_time();

	va_start(args, format);
	vsnprintf(block->buff, MAX_COM_LOG_SIZE, format, args);
	va_end(args);
}

void _mif_time_log(enum mif_log_id id, struct modem_shared *msd,
	struct timespec epoch, const char *data, size_t len)
{
	struct mif_time_block *block;
	unsigned long int flags;

	spin_lock_irqsave(&msd->lock, flags);

	block = (struct mif_time_block *)
		(msd->storage.addr + (MAX_LOG_SIZE * msd->storage.cnt));
	msd->storage.cnt = ((msd->storage.cnt + 1) < MAX_LOG_CNT) ?
		msd->storage.cnt + 1 : 0;

	spin_unlock_irqrestore(&msd->lock, flags);

	block->id = id;
	block->time = get_kernel_time();
	memcpy(&block->epoch, &epoch, sizeof(struct timespec));

	if (data)
		memcpy(block->buff, data,
			(len > MAX_IRQ_LOG_SIZE) ? MAX_IRQ_LOG_SIZE : len);
}

/* dump2hex
 * dump data to hex as fast as possible.
 * the length of @buff must be greater than "@len * 3"
 * it need 3 bytes per one data byte to print.
 */
static inline void dump2hex(char *buff, size_t buff_size,
			    const char *data, size_t data_len)
{
	char *dest = buff;
	size_t len;
	int i;

	if (buff_size < (data_len * 3))
		len = buff_size / 3;
	else
		len = data_len;

	for (i = 0; i < len; i++) {
		*dest++ = hex[(data[i] >> 4) & 0xf];
		*dest++ = hex[data[i] & 0xf];
		*dest++ = ' ';
	}

	/* The last character must be overwritten with NULL */
	if (likely(len > 0))
		dest--;

	*dest = 0;
}

static inline u32 dump_skb(char *str, u32 size, struct sk_buff *skb)
{
	u32 len, buflen;

	len = min_t(unsigned int, skb->len, size);
	buflen = len ? len * 3 : 1;
	dump2hex(str, buflen, (char *)(skb->data), len);

	return buflen;
}

/**
@brief		print an IPC data @b WITH a @b LINK header

@param layer	the layer in the Samsung IPC
@param ch	the SIPC channel ID
@param skb	the pointer to a sk_buff instance
*/
inline void log_ipc_pkt(enum ipc_layer layer, u8 ch, struct sk_buff *skb)
{
	u32 buflen;
	char str[SZ_256];

	if (unlikely(!skb))
		return;

#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
	if (unlikely(layer == IOD_RX || layer == IOD_TX)) {
		if (unlikely(!test_bit(DEBUG_FLAG_IOD, &dflags))) {
			return;
		} else {
			buflen = dump_skb(str, SZ_16, skb);
			goto trace_log;
		}
	}
	if (sipc_ps_ch(ch) && test_bit(DEBUG_FLAG_PS, &dflags)) {
		buflen = dump_skb(str, SZ_64, skb);
		trace_mif_log(layer_str(layer), buflen, str);
		return;
	}
#endif
	if (sipc5_fmt_ch(ch) && test_bit(DEBUG_FLAG_FMT, &dflags))
		goto print_log;
	if (sipc5_rfs_ch(ch) && test_bit(DEBUG_FLAG_RFS, &dflags))
		goto print_log;
	if (sipc_csd_ch(ch) && test_bit(DEBUG_FLAG_CSVT, &dflags))
		goto print_log;
	if (sipc_log_ch(ch) && test_bit(DEBUG_FLAG_LOG, &dflags))
		goto print_log;
	if (sipc5_boot_ch(ch) && test_bit(DEBUG_FLAG_BOOT, &dflags))
		goto print_log;
	if (sipc5_dump_ch(ch) && test_bit(DEBUG_FLAG_DUMP, &dflags))
		goto print_log;
	if (!test_bit(DEBUG_FLAG_UNKNOWN, &dflags))
		return;

print_log:
	buflen = dump_skb(str, SZ_16, skb);

	mif_debug("%s: %s(%d): %s\n", MIF_TAG, layer_str(layer), buflen, str);

#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
trace_log:
	trace_mif_log(layer_str(layer), buflen, str);
#endif
}

/* print buffer as hex string */
int pr_buffer(const char *tag, const char *data, size_t data_len,
							size_t max_len)
{
	size_t len = min(data_len, max_len);
	unsigned char str[len ? len * 3 : 1]; /* 1 <= sizeof <= max_len*3 */
	dump2hex(str, (len ? len * 3 : 1), data, len);

	/* don't change this printk to mif_debug for print this as level7 */
	return pr_info("%s: %s(%ld): %s%s\n", MIF_TAG, tag, (long)data_len,
			str, (len == data_len) ? "" : " ...");
}

/* flow control CM from CP, it use in serial devices */
int link_rx_flowctl_cmd(struct link_device *ld, const char *data, size_t len)
{
	struct modem_shared *msd = ld->msd;
	unsigned short *cmd, *end = (unsigned short *)(data + len);

	mif_debug("flow control cmd: size=%ld\n", (long)len);

	for (cmd = (unsigned short *)data; cmd < end; cmd++) {
		switch (*cmd) {
		case CMD_SUSPEND:
			iodevs_for_each(msd, iodev_netif_stop, 0);
			ld->raw_tx_suspended = true;
			mif_info("flowctl CMD_SUSPEND(%04X)\n", *cmd);
			break;

		case CMD_RESUME:
			iodevs_for_each(msd, iodev_netif_wake, 0);
			ld->raw_tx_suspended = false;
			complete_all(&ld->raw_tx_resumed_by_cp);
			mif_info("flowctl CMD_RESUME(%04X)\n", *cmd);
			break;

		default:
			mif_err("flowctl BAD CMD: %04X\n", *cmd);
			break;
		}
	}

	return 0;
}

struct io_device *get_iod_with_format(struct modem_shared *msd,
			enum dev_format format)
{
	struct rb_node *n = msd->iodevs_tree_fmt.rb_node;

	while (n) {
		struct io_device *iodev;

		iodev = rb_entry(n, struct io_device, node_fmt);
		if (format < iodev->format)
			n = n->rb_left;
		else if (format > iodev->format)
			n = n->rb_right;
		else
			return iodev;
	}

	return NULL;
}

void insert_iod_with_channel(struct modem_shared *msd, unsigned int channel,
			     struct io_device *iod)
{
	unsigned idx = msd->num_channels;

	msd->ch2iod[channel] = iod;
	msd->ch[idx] = channel;
	msd->num_channels++;
}

struct io_device *insert_iod_with_format(struct modem_shared *msd,
		enum dev_format format, struct io_device *iod)
{
	struct rb_node **p = &msd->iodevs_tree_fmt.rb_node;
	struct rb_node *parent = NULL;

	while (*p) {
		struct io_device *iodev;

		parent = *p;
		iodev = rb_entry(parent, struct io_device, node_fmt);
		if (format < iodev->format)
			p = &(*p)->rb_left;
		else if (format > iodev->format)
			p = &(*p)->rb_right;
		else
			return iodev;
	}

	rb_link_node(&iod->node_fmt, parent, p);
	rb_insert_color(&iod->node_fmt, &msd->iodevs_tree_fmt);
	return NULL;
}

void iodevs_for_each(struct modem_shared *msd, action_fn action, void *args)
{
	int i;

	for (i = 0; i < msd->num_channels; i++) {
		u8 ch = msd->ch[i];
		struct io_device *iod = msd->ch2iod[ch];
		action(iod, args);
	}
}

void iodev_netif_wake(struct io_device *iod, void *args)
{
	if (iod->io_typ == IODEV_NET && iod->ndev) {
		netif_wake_queue(iod->ndev);
		mif_info("%s\n", iod->name);
	}
}

void iodev_netif_stop(struct io_device *iod, void *args)
{
	if (iod->io_typ == IODEV_NET && iod->ndev) {
		netif_stop_queue(iod->ndev);
		mif_info("%s\n", iod->name);
	}
}

static void iodev_set_tx_link(struct io_device *iod, void *args)
{
	struct link_device *ld = (struct link_device *)args;
	if (iod->format == IPC_RAW && IS_CONNECTED(iod, ld)) {
		set_current_link(iod, ld);
		mif_err("%s -> %s\n", iod->name, ld->name);
	}
}

void rawdevs_set_tx_link(struct modem_shared *msd, enum modem_link link_type)
{
	struct link_device *ld = find_linkdev(msd, link_type);
	if (ld)
		iodevs_for_each(msd, iodev_set_tx_link, ld);
}

void stop_net_iface(struct link_device *ld, unsigned int channel)
{
	struct io_device *iod;
	unsigned long flags;

	spin_lock_irqsave(&ld->netif_lock, flags);

	if (test_bit(channel, &ld->netif_stop_mask)) {
		mif_err("channel %d was already stopped!\n", channel);
		goto exit;
	}

	iod = link_get_iod_with_channel(ld, channel);
	iodev_netif_stop(iod, 0);
	set_bit(channel, &ld->netif_stop_mask);

exit:
	spin_unlock_irqrestore(&ld->netif_lock, flags);
}

void stop_net_ifaces(struct link_device *ld)
{
	struct io_device *iod;
	unsigned long flags;

	spin_lock_irqsave(&ld->netif_lock, flags);

	if (atomic_read(&ld->netif_stopped) > 0)
		goto exit;

	iod = link_get_iod_with_channel(ld, SIPC_CH_ID_PDP_0);
	if (iod)
		iodevs_for_each(iod->msd, iodev_netif_stop, 0);

	atomic_set(&ld->netif_stopped, 1);

exit:
	spin_unlock_irqrestore(&ld->netif_lock, flags);
}

void resume_net_iface(struct link_device *ld, unsigned int channel)
{
	struct io_device *iod;
	unsigned long flags;

	spin_lock_irqsave(&ld->netif_lock, flags);

	if (!test_bit(channel, &ld->netif_stop_mask)) {
		mif_err("channel %d was already resumed!\n", channel);
		goto exit;
	}

	iod = link_get_iod_with_channel(ld, channel);
	iodev_netif_wake(iod, 0);
	clear_bit(channel, &ld->netif_stop_mask);

exit:
	spin_unlock_irqrestore(&ld->netif_lock, flags);
}

void resume_net_ifaces(struct link_device *ld)
{
	struct io_device *iod;
	unsigned long flags;

	spin_lock_irqsave(&ld->netif_lock, flags);

	if (atomic_read(&ld->netif_stopped) == 0)
		goto exit;

	iod = link_get_iod_with_channel(ld, SIPC_CH_ID_PDP_0);
	if (iod)
		iodevs_for_each(iod->msd, iodev_netif_wake, 0);

	atomic_set(&ld->netif_stopped, 0);

exit:
	spin_unlock_irqrestore(&ld->netif_lock, flags);
}

/**
@brief		ipv4 string to be32 (big endian 32bits integer)
@return		zero when errors occurred
*/
__be32 ipv4str_to_be32(const char *ipv4str, size_t count)
{
	unsigned char ip[4];
	char *ipstr; /* == strlen("xxx.xxx.xxx.xxx") + 1 */
	char *next;
	int i;
	int size;

	if (!ipv4str)
		return 0;

	if ((size = strlen(ipv4str)) > 16)
		return 0;

	ipstr = kzalloc(size, GFP_ATOMIC);
	next = ipstr;

	strncpy(ipstr, ipv4str, size);

	for (i = 0; i < 4; i++) {
		char *p;

		p = strsep(&next, ".");
		if (kstrtou8(p, 10, &ip[i]) < 0) {
			kfree(ipstr);
			return 0; /* == 0.0.0.0 */
		}
	}

	kfree(ipstr);
	return *((__be32 *)ip);
}

void mif_add_timer(struct timer_list *timer, unsigned long expire,
		   void (*function)(unsigned long), unsigned long data)
{
	if (timer_pending(timer))
		return;

	init_timer(timer);
	timer->expires = jiffies + expire;
	timer->function = function;
	timer->data = data;
	add_timer(timer);
}

void mif_print_data(const u8 *data, int len)
{
	int words = len >> 4;
	int residue = len - (words << 4);
	int i;
	char *b;
	char last[80];

	/* Make the last line, if ((len % 16) > 0) */
	if (residue > 0) {
		char tb[8];

		sprintf(last, "%04X: ", (words << 4));
		b = (char *)data + (words << 4);

		for (i = 0; i < residue; i++) {
			sprintf(tb, "%02x ", b[i]);
			strcat(last, tb);
			if ((i & 0x3) == 0x3) {
				sprintf(tb, " ");
				strcat(last, tb);
			}
		}
	}

	for (i = 0; i < words; i++) {
		b = (char *)data + (i << 4);
		mif_err("%04X: "
			"%02x %02x %02x %02x  %02x %02x %02x %02x  "
			"%02x %02x %02x %02x  %02x %02x %02x %02x\n",
			(i << 4),
			b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
			b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
	}

	/* Print the last line */
	if (residue > 0)
		mif_err("%s\n", last);
}

void mif_dump2format16(const u8 *data, int len, char *buff, char *tag)
{
	char *d;
	int i;
	int words = len >> 4;
	int residue = len - (words << 4);
	char line[LINE_BUFF_SIZE];

	for (i = 0; i < words; i++) {
		memset(line, 0, LINE_BUFF_SIZE);
		d = (char *)data + (i << 4);

		if (tag)
			sprintf(line, "%s%04X| "
				"%02x %02x %02x %02x  "
				"%02x %02x %02x %02x  "
				"%02x %02x %02x %02x  "
				"%02x %02x %02x %02x\n",
				tag, (i << 4),
				d[0], d[1], d[2], d[3],
				d[4], d[5], d[6], d[7],
				d[8], d[9], d[10], d[11],
				d[12], d[13], d[14], d[15]);
		else
			sprintf(line, "%04X| "
				"%02x %02x %02x %02x  "
				"%02x %02x %02x %02x  "
				"%02x %02x %02x %02x  "
				"%02x %02x %02x %02x\n",
				(i << 4),
				d[0], d[1], d[2], d[3],
				d[4], d[5], d[6], d[7],
				d[8], d[9], d[10], d[11],
				d[12], d[13], d[14], d[15]);

		strcat(buff, line);
	}

	/* Make the last line, if (len % 16) > 0 */
	if (residue > 0) {
		char tb[8];

		memset(line, 0, LINE_BUFF_SIZE);
		memset(tb, 0, sizeof(tb));
		d = (char *)data + (words << 4);

		if (tag)
			sprintf(line, "%s%04X|", tag, (words << 4));
		else
			sprintf(line, "%04X|", (words << 4));

		for (i = 0; i < residue; i++) {
			sprintf(tb, " %02x", d[i]);
			strcat(line, tb);
			if ((i & 0x3) == 0x3) {
				sprintf(tb, " ");
				strcat(line, tb);
			}
		}
		strcat(line, "\n");

		strcat(buff, line);
	}
}

void mif_dump2format4(const u8 *data, int len, char *buff, char *tag)
{
	char *d;
	int i;
	int words = len >> 2;
	int residue = len - (words << 2);
	char line[LINE_BUFF_SIZE];

	for (i = 0; i < words; i++) {
		memset(line, 0, LINE_BUFF_SIZE);
		d = (char *)data + (i << 2);

		if (tag)
			sprintf(line, "%s%04X| %02x %02x %02x %02x\n",
				tag, (i << 2), d[0], d[1], d[2], d[3]);
		else
			sprintf(line, "%04X| %02x %02x %02x %02x\n",
				(i << 2), d[0], d[1], d[2], d[3]);

		strcat(buff, line);
	}

	/* Make the last line, if (len % 4) > 0 */
	if (residue > 0) {
		char tb[8];

		memset(line, 0, LINE_BUFF_SIZE);
		memset(tb, 0, sizeof(tb));
		d = (char *)data + (words << 2);

		if (tag)
			sprintf(line, "%s%04X|", tag, (words << 2));
		else
			sprintf(line, "%04X|", (words << 2));

		for (i = 0; i < residue; i++) {
			sprintf(tb, " %02x", d[i]);
			strcat(line, tb);
		}
		strcat(line, "\n");

		strcat(buff, line);
	}
}

void mif_print_dump(const u8 *data, int len, int width)
{
	char *buff;

	buff = kzalloc(len << 3, GFP_ATOMIC);
	if (!buff) {
		mif_err("ERR! kzalloc fail\n");
		return;
	}

	if (width == 16)
		mif_dump2format16(data, len, buff, LOG_TAG);
	else
		mif_dump2format4(data, len, buff, LOG_TAG);

	pr_info("%s", buff);

	kfree(buff);
}

static void strcat_tcp_header(char *buff, u8 *pkt)
{
	struct tcphdr *tcph = (struct tcphdr *)pkt;
	int eol;
	char line[LINE_BUFF_SIZE] = {0, };
	char flag_str[32] = {0, };

/*-------------------------------------------------------------------------

				TCP Header Format

	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	|          Source Port          |       Destination Port        |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	|                        Sequence Number                        |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	|                    Acknowledgment Number                      |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	|  Data |       |C|E|U|A|P|R|S|F|                               |
	| Offset| Rsvd  |W|C|R|C|S|S|Y|I|            Window             |
	|       |       |R|E|G|K|H|T|N|N|                               |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	|           Checksum            |         Urgent Pointer        |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	|                    Options                    |    Padding    |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	|                             data                              |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

-------------------------------------------------------------------------*/

	snprintf(line, LINE_BUFF_SIZE,
		"%s: TCP:: Src.Port %u, Dst.Port %u\n",
		MIF_TAG, ntohs(tcph->source), ntohs(tcph->dest));
	strcat(buff, line);

	snprintf(line, LINE_BUFF_SIZE,
		"%s: TCP:: SEQ 0x%08X(%u), ACK 0x%08X(%u)\n",
		MIF_TAG, ntohs(tcph->seq), ntohs(tcph->seq),
		ntohs(tcph->ack_seq), ntohs(tcph->ack_seq));
	strcat(buff, line);

	if (tcph->cwr)
		strcat(flag_str, "CWR ");
	if (tcph->ece)
		strcat(flag_str, "ECE");
	if (tcph->urg)
		strcat(flag_str, "URG ");
	if (tcph->ack)
		strcat(flag_str, "ACK ");
	if (tcph->psh)
		strcat(flag_str, "PSH ");
	if (tcph->rst)
		strcat(flag_str, "RST ");
	if (tcph->syn)
		strcat(flag_str, "SYN ");
	if (tcph->fin)
		strcat(flag_str, "FIN ");
	eol = strlen(flag_str) - 1;
	if (eol > 0)
		flag_str[eol] = 0;
	snprintf(line, LINE_BUFF_SIZE, "%s: TCP:: Flags {%s}\n",
		MIF_TAG, flag_str);
	strcat(buff, line);

	snprintf(line, LINE_BUFF_SIZE,
		"%s: TCP:: Window %u, Checksum 0x%04X, Urgent %u\n", MIF_TAG,
		ntohs(tcph->window), ntohs(tcph->check), ntohs(tcph->urg_ptr));
	strcat(buff, line);
}

static void strcat_udp_header(char *buff, u8 *pkt)
{
	struct udphdr *udph = (struct udphdr *)pkt;
	char line[LINE_BUFF_SIZE] = {0, };

/*-------------------------------------------------------------------------

				UDP Header Format

	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	|          Source Port          |       Destination Port        |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	|            Length             |           Checksum            |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	|                             data                              |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

-------------------------------------------------------------------------*/

	snprintf(line, LINE_BUFF_SIZE,
		"%s: UDP:: Src.Port %u, Dst.Port %u\n",
		MIF_TAG, ntohs(udph->source), ntohs(udph->dest));
	strcat(buff, line);

	snprintf(line, LINE_BUFF_SIZE,
		"%s: UDP:: Length %u, Checksum 0x%04X\n",
		MIF_TAG, ntohs(udph->len), ntohs(udph->check));
	strcat(buff, line);

	if (ntohs(udph->dest) == 53) {
		snprintf(line, LINE_BUFF_SIZE, "%s: UDP:: DNS query!!!\n",
			MIF_TAG);
		strcat(buff, line);
	}

	if (ntohs(udph->source) == 53) {
		snprintf(line, LINE_BUFF_SIZE, "%s: UDP:: DNS response!!!\n",
			MIF_TAG);
		strcat(buff, line);
	}
}

void print_ipv4_packet(const u8 *ip_pkt, enum ipc_layer layer)
{
	char *buff;
	struct iphdr *iph = (struct iphdr *)ip_pkt;
	char *pkt = (char *)ip_pkt + (iph->ihl << 2);
	u16 flags = (ntohs(iph->frag_off) & 0xE000);
	u16 frag_off = (ntohs(iph->frag_off) & 0x1FFF);
	int eol;
	char line[LINE_BUFF_SIZE] = {0, };
	char flag_str[16] = {0, };

/*---------------------------------------------------------------------------
				IPv4 Header Format

	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	|Version|  IHL  |Type of Service|          Total Length         |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	|         Identification        |C|D|M|     Fragment Offset     |
	|                               |E|F|F|                         |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	|  Time to Live |    Protocol   |         Header Checksum       |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	|                       Source Address                          |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	|                    Destination Address                        |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	|                    Options                    |    Padding    |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

	IHL - Header Length
	Flags - Consist of 3 bits
		The 1st bit is "Congestion" bit.
		The 2nd bit is "Dont Fragment" bit.
		The 3rd bit is "More Fragments" bit.

---------------------------------------------------------------------------*/

	if (iph->version != 4)
		return;

	buff = kzalloc(4096, GFP_ATOMIC);
	if (!buff)
		return;

	if (layer == PS_TX)
		snprintf(line, LINE_BUFF_SIZE, "%s\n", TX_SEPARATOR);
	else
		snprintf(line, LINE_BUFF_SIZE, "%s\n", RX_SEPARATOR);
	strcat(buff, line);

	snprintf(line, LINE_BUFF_SIZE, "%s\n", LINE_SEPARATOR);
	strcat(buff, line);

	snprintf(line, LINE_BUFF_SIZE,
		"%s: IP4:: Version %u, Header Length %u, TOS %u, Length %u\n",
		MIF_TAG, iph->version, (iph->ihl << 2), iph->tos,
		ntohs(iph->tot_len));
	strcat(buff, line);

	snprintf(line, LINE_BUFF_SIZE, "%s: IP4:: ID %u, Fragment Offset %u\n",
		MIF_TAG, ntohs(iph->id), frag_off);
	strcat(buff, line);

	if (flags & IP_CE)
		strcat(flag_str, "CE ");
	if (flags & IP_DF)
		strcat(flag_str, "DF ");
	if (flags & IP_MF)
		strcat(flag_str, "MF ");
	eol = strlen(flag_str) - 1;
	if (eol > 0)
		flag_str[eol] = 0;
	snprintf(line, LINE_BUFF_SIZE, "%s: IP4:: Flags {%s}\n",
		MIF_TAG, flag_str);
	strcat(buff, line);

	snprintf(line, LINE_BUFF_SIZE,
		"%s: IP4:: TTL %u, Protocol %u, Header Checksum 0x%04X\n",
		MIF_TAG, iph->ttl, iph->protocol, ntohs(iph->check));
	strcat(buff, line);

	snprintf(line, LINE_BUFF_SIZE,
		"%s: IP4:: Src.IP %u.%u.%u.%u, Dst.IP %u.%u.%u.%u\n",
		MIF_TAG, ip_pkt[12], ip_pkt[13], ip_pkt[14], ip_pkt[15],
		ip_pkt[16], ip_pkt[17], ip_pkt[18], ip_pkt[19]);
	strcat(buff, line);

	switch (iph->protocol) {
	case 6: /* TCP */
		strcat_tcp_header(buff, pkt);
		break;

	case 17: /* UDP */
		strcat_udp_header(buff, pkt);
		break;

	default:
		break;
	}

	snprintf(line, LINE_BUFF_SIZE, "%s\n", LINE_SEPARATOR);
	strcat(buff, line);

	pr_err("%s\n", buff);

	kfree(buff);
}

bool is_dns_packet(const u8 *ip_pkt)
{
	struct iphdr *iph = (struct iphdr *)ip_pkt;
	struct udphdr *udph = (struct udphdr *)(ip_pkt + (iph->ihl << 2));

	/* If this packet is not a UDP packet, return here. */
	if (iph->protocol != 17)
		return false;

	if (ntohs(udph->dest) == 53 || ntohs(udph->source) == 53)
		return true;
	else
		return false;
}

bool is_syn_packet(const u8 *ip_pkt)
{
	struct iphdr *iph = (struct iphdr *)ip_pkt;
	struct tcphdr *tcph = (struct tcphdr *)(ip_pkt + (iph->ihl << 2));

	/* If this packet is not a TCP packet, return here. */
	if (iph->protocol != 6)
		return false;

	if (tcph->syn || tcph->fin)
		return true;
	else
		return false;
}

/**
@brief		initialize a modem_irq instance

@param irq	the pointer to a modem_irq instance
@param num	IRQ number
@param name	the name of the interrupt
@param flags	the bitmask of interrupt flags
*/
void mif_init_irq(struct modem_irq *irq, unsigned int num, const char *name,
		  unsigned long flags)
{
	spin_lock_init(&irq->lock);
	irq->num = num;
	strncpy(irq->name, name, (MAX_NAME_LEN - 1));
	irq->flags = flags;
	mif_info("%s: name:%s num:%d flags:0x%08lX\n",
		FUNC, name, num, flags);
}

int mif_request_irq(struct modem_irq *irq, irq_handler_t isr, void *data)
{
	int ret;

	ret = request_irq(irq->num, isr, irq->flags, irq->name, data);
	if (ret) {
		mif_err("%s: %s: ERR! request_irq fail (%d)\n",
			FUNC, irq->name, ret);
		return ret;
	}

	irq->active = true;

	ret = enable_irq_wake(irq->num);
	if (ret) {
		mif_err("%s: %s: ERR! enable_irq_wake fail (%d)\n",
			FUNC, irq->name, ret);
	}

	mif_info("%s: %s(#%d) handler registered (flags:0x%08lX)\n",
		FUNC, irq->name, irq->num, irq->flags);

	return 0;
}

void mif_enable_irq(struct modem_irq *irq)
{
	unsigned long flags;

	spin_lock_irqsave(&irq->lock, flags);

	if (irq->active) {
		mif_err("%s: %s(#%d) is already active <%pf>\n",
			FUNC, irq->name, irq->num, CALLER);
		goto exit;
	}

	enable_irq(irq->num);

	irq->active = true;

	mif_info("%s: %s(#%d) is enabled <%pf>\n",
		FUNC, irq->name, irq->num, CALLER);

exit:
	spin_unlock_irqrestore(&irq->lock, flags);
}

void mif_disable_irq(struct modem_irq *irq)
{
	unsigned long flags;

	spin_lock_irqsave(&irq->lock, flags);

	if (!irq->active) {
		mif_err("%s: %s(#%d) is not active <%pf>\n",
			FUNC, irq->name, irq->num, CALLER);
		goto exit;
	}

	disable_irq_nosync(irq->num);

	irq->active = false;

	mif_info("%s: %s(#%d) is disabled <%pf>\n",
		FUNC, irq->name, irq->num, CALLER);

exit:
	spin_unlock_irqrestore(&irq->lock, flags);
}

struct file *mif_open_file(const char *path)
{
	struct file *fp;
	mm_segment_t old_fs;

	old_fs = get_fs();
	set_fs(get_ds());

	fp = filp_open(path, O_RDWR|O_CREAT|O_APPEND, 0666);

	set_fs(old_fs);

	if (IS_ERR(fp))
		return NULL;

	return fp;
}

void mif_save_file(struct file *fp, const char *buff, size_t size)
{
	int ret;
	mm_segment_t old_fs;

	old_fs = get_fs();
	set_fs(get_ds());

	ret = fp->f_op->write(fp, buff, size, &fp->f_pos);
	if (ret < 0)
		mif_err("ERR! write fail\n");

	set_fs(old_fs);
}

void mif_close_file(struct file *fp)
{
	mm_segment_t old_fs;

	old_fs = get_fs();
	set_fs(get_ds());

	filp_close(fp, NULL);

	set_fs(old_fs);
}

int board_gpio_export(struct device *dev,
		unsigned gpio, bool dir, const char *name)
{
	int ret = 0;

	if (!gpio_is_valid(gpio)) {
		mif_err("invalid gpio pins - %s\n", name);
		return -EINVAL;
	}

	ret = gpio_export(gpio, dir);
	if (ret) {
		mif_err("%s: failed to export gpio (%d)\n", name, ret);
		return ret;
	}

	ret = gpio_export_link(dev, name, gpio);
	if (ret) {
		mif_err("%s: failed to export link_gpio (%d)\n", name, ret);
		return ret;
	}

	mif_info("%s exported\n", name);

	return 0;
}

void make_gpio_floating(unsigned int gpio, bool floating)
{
	if (floating)
		gpio_direction_input(gpio);
	else
		gpio_direction_output(gpio, 0);
}

int __ref register_cp_crash_notifier(struct notifier_block *nb)
{
	return raw_notifier_chain_register(&cp_crash_notifier, nb);
}

void __ref modemctl_notify_event(enum modemctl_event evt)
{
	raw_notifier_call_chain(&cp_crash_notifier, evt, NULL);
}

/* Create Baseband info proc */
static struct mif_baseband_info {
	atomic_t num;
	struct list_head modems;
	spinlock_t lock;
} bb_info = {
	.num = {.counter = 0},
	.modems = LIST_HEAD_INIT(bb_info.modems),
	.lock = __SPIN_LOCK_UNLOCKED(bb_info.lock),
};

static int mif_bbinfo_show(struct seq_file *f, void *offset)
{
	int num = atomic_read(&bb_info.num);
	struct modem_ctl *mc;

	seq_printf(f, "Baseband:%d\n", num);
	spin_lock_bh(&bb_info.lock);
	list_for_each_entry(mc, &bb_info.modems, bbinfo) {
		seq_printf(f, "Modem:%s\n"
		"Link:%s\n"
		"LinkBoot:%s\n"
		"LinkMain:%s\n"
		"spi_lnk:%s\n"
		"Boot:%s\n"
		"Binary:%s\n\n",
		mc->name,
		dev_name(mc->dev),
		get_current_link(mc->bootd)->name,
		get_current_link(mc->iod)->name,
#ifdef CONFIG_LINK_DEVICE_SPI
		"LNK",
#else
		"BOOT",
#endif
		mc->bootd->name,
		"TBD");
	}
	spin_unlock_bh(&bb_info.lock);
	return 0;
}

static int bbinfo_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, mif_bbinfo_show, inode->i_private);
}

static const struct file_operations baseband_file_ops = {
	.owner = THIS_MODULE,
	.open		= bbinfo_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};
int create_baseband_info(struct modem_ctl *mc)
{
	int num = atomic_inc_return(&bb_info.num);

	mif_err("&&** new base band info created\n" );
	spin_lock_bh(&bb_info.lock);
	list_add(&mc->bbinfo, &bb_info.modems);
	spin_unlock_bh(&bb_info.lock);

	if (num == 1) {
		struct proc_dir_entry *entry;
		entry = proc_create_data("baseband", S_IFREG | S_IRUGO, NULL, &baseband_file_ops, mc);
		if (!entry) {
			mif_err("failed to create proc/baseband entry\n" );
			return 0;
		}
	}
	mif_info("Baseband:%d\n"
		 "Modem:%s\n"
		 "Link:%s\n"
		 "LinkBoot:%s\n"
		 "LinkMain:%s\n"
		 "spi_lnk:%s\n"
		 "Boot:%s\n"
		 "Binary:%s\n\n",
		 num,
		 mc->name,
		 dev_name(mc->dev),
		 get_current_link(mc->bootd)->name,
		 get_current_link(mc->iod)->name,
#ifdef CONFIG_LINK_DEVICE_SPI
		"LNK",
#else
		"BOOT",
#endif
		 mc->bootd->name,
		 "TBD");

	return 0;
}

