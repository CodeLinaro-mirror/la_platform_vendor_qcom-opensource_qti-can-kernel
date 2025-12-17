// SPDX-License-Identifier: GPL-2.0-only

/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */

#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/netdevice.h>
#include <linux/workqueue.h>
#include <linux/serial_core.h>
#include <linux/can.h>
#include <linux/can/dev.h>
#include <linux/can/error.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/uaccess.h>
#include <linux/pm.h>
#include <asm/arch_timer.h>
#include <asm/div64.h>
#include <linux/suspend.h>
#include <linux/pm_runtime.h>
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <linux/kthread.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/version.h>
#include <linux/kdev_t.h>

#define MAX_TX_BUFFERS			1
#define XFER_BUFFER_SIZE		64
#define RX_ASSEMBLY_BUFFER_SIZE		128
#define RX_FD_BUFFER_SIZE		82
#define UART_CAN_FW_QUERY_RETRY_COUNT	3
#define UART_CAN_TIME_SYNC_RETRY_COUNT   3
#define DRIVER_MODE_RAW_FRAMES		0
#define DRIVER_MODE_PROPERTIES		1
#define DRIVER_MODE_AMB			2
#define QUERY_FIRMWARE_TIMEOUT_MS	150
#define QUERY_TIME_REQUEST_TIMEOUT_MS    50
#define EUPGRADE			140
#define QTIMER_DIV			192
#define QTIMER_MUL			10000
#define TIMESTAMP_PRINT_CNTR		10
#define TIME_OFFSET_MAX_THD		30
#define TIME_OFFSET_MIN_THD		-30
#define CAN_FD_HEADER			14
#define CAN_FD_PACKET_DATA		32
#define CAN_FD_PACKET_SIZE		46
#define CAN_FD_MAX_DATA_SIZE		64
#define CAN_STANDARD_PACKET_SIZE	22
#define MAX_CAN_CLK_FREQ	40000000 /* 40MHz */
#define TIME_REQUEST_PERIOD         (30000) /* 30 Seconds */

/* Module parameters */
static char *uart_can_tty_name = "ttyHS1";
static struct tty_struct *tty_tst = NULL;
static struct uart_can *priv_data = NULL;

struct uart_can {

	/* TTY buffers */
	u8 txbuf[64];
	u8 rxbuf[64];

	/* TTY and netdev devices */
	struct net_device **netdev;
	struct tty_struct *tty;

	spinlock_t lock;

	/* TTY buffer accounting */
	struct work_struct tx_work;	/* Flushes TTY TX buffer */
	u8 *txhead;			/* Next TX byte */
	size_t txleft;			/* Bytes left to TX */
	int rxfill;			/* Bytes already RX'd in buffer */

	struct workqueue_struct *tx_wq;
	struct task_struct *timer_thread;
	struct timer_list timer;
	char *tx_buf, *rx_buf;
	int xfer_length;
	atomic_t msg_seq;
	char *assembly_buffer;
	u8 assembly_buffer_size;
	char *fd_buffer;
	atomic_t netif_queue_stop;
	struct completion response_completion;
	int wait_cmd;
	int cmd_result;
	int driver_mode;
	int clk_freq_mhz;
	int max_can_channels;
	int bits_per_word;
	int reset_delay_msec;
	int reset;
	bool support_can_fd;
	bool use_qtimer;
	bool can_fw_cmd_timeout_req;
	u32 rem_all_buffering_timeout_ms;
	u32 can_fw_cmd_timeout_ms;
	s64 time_diff;
	bool active_low;
	bool univ_acc_filter_flag;
	bool probe_query_resp;
	bool time_sync_from_soc_to_mcu;
};

struct uart_can_netdev_privdata {
	struct can_priv can;
	struct uart_can *uart_can;
	u8 netdev_index;
};

struct uart_can_tx_work {
	struct work_struct work;
	struct sk_buff *skb;
	struct net_device *netdev;
};

/* Message definitions */
struct tx_cmd { /* Struct for Tx CMD */
	u8 cmd;
#ifdef CONFIG_QTI_CAN_TARGET_2W_V2
	u16 len;
#else
	u8 len;
#endif
	u16 seq;
	u8 data[];
} __packed;

struct rx_resp { /* Struct for Rx Response */
	u8 cmd;
#ifdef CONFIG_QTI_CAN_TARGET_2W_V2
	u16 len;
#else
	u8 len;
#endif
	u16 seq;
	u8 data[];
} __packed;

#define CMD_GET_FW_VERSION		0x81
#define CMD_CAN_SEND_FRAME		0x82
#define CMD_CAN_ADD_FILTER		0x83
#define CMD_CAN_REMOVE_FILTER		0x84
#define CMD_CAN_RECEIVE_FRAME		0x85
#define CMD_CAN_CONFIG_BIT_TIMING	0x86
#define CMD_CAN_DATA_BUFF_ADD		0x87
#define CMD_CAN_DATA_BUFF_REMOVE	0X88
#define CMD_CAN_RELEASE_BUFFER		0x89
#define CMD_CAN_DATA_BUFF_REMOVE_ALL	0x8A
#define CMD_PROPERTY_WRITE		0x8B
#define CMD_PROPERTY_READ		0x8C
#define CMD_GET_FW_BR_VERSION		0x95
#define CMD_BEGIN_FIRMWARE_UPGRADE	0x96
#define CMD_FIRMWARE_UPGRADE_DATA	0x97
#define CMD_END_FIRMWARE_UPGRADE	0x98
#define CMD_BEGIN_BOOT_ROM_UPGRADE	0x99
#define CMD_BOOT_ROM_UPGRADE_DATA	0x9A
#define CMD_END_BOOT_ROM_UPGRADE	0x9B
#define CMD_END_FW_UPDATE_FILE		0x9C
#define CMD_UPDATE_TIME_INFO		0x9D
#define CMD_SUSPEND_EVENT		0x9E
#define CMD_RESUME_EVENT		0x9F

#define IOCTL_RELEASE_CAN_BUFFER	(SIOCDEVPRIVATE + 0)
#define IOCTL_ENABLE_BUFFERING		(SIOCDEVPRIVATE + 1)
#define IOCTL_ADD_FRAME_FILTER		(SIOCDEVPRIVATE + 2)
#define IOCTL_REMOVE_FRAME_FILTER	(SIOCDEVPRIVATE + 3)
#define IOCTL_DISABLE_BUFFERING		(SIOCDEVPRIVATE + 5)
#define IOCTL_DISABLE_ALL_BUFFERING	(SIOCDEVPRIVATE + 6)
#define IOCTL_GET_FW_BR_VERSION		(SIOCDEVPRIVATE + 7)
#define IOCTL_BEGIN_FIRMWARE_UPGRADE	(SIOCDEVPRIVATE + 8)
#define IOCTL_FIRMWARE_UPGRADE_DATA	(SIOCDEVPRIVATE + 9)
#define IOCTL_END_FIRMWARE_UPGRADE	(SIOCDEVPRIVATE + 10)
#define IOCTL_BEGIN_BOOT_ROM_UPGRADE	(SIOCDEVPRIVATE + 11)
#define IOCTL_BOOT_ROM_UPGRADE_DATA	(SIOCDEVPRIVATE + 12)
#define IOCTL_END_BOOT_ROM_UPGRADE	(SIOCDEVPRIVATE + 13)
#define IOCTL_END_FW_UPDATE_FILE	(SIOCDEVPRIVATE + 14)

#define IFR_DATA_OFFSET		0x100

struct can_write_req {
	u8 can_if;
	u32 mid;
	u8 dlc;
	u8 data[CAN_FD_MAX_DATA_SIZE];
} __packed;

struct can_receive_frame {
	u8 can_if;
	__le64 ts;
	__le32 mid;
	u8 dlc;
	u8 data[8];
} __packed;

struct can_config_bit_timing {
	u8 can_if;
	u32 prop_seg;
	u32 phase_seg1;
	u32 phase_seg2;
	u32 sjw;
	u32 brp;
} __packed;

static struct can_bittiming_const flexcan_bittiming_const = {
	.name = "can_uart",
	.tseg1_min = 4,
	.tseg1_max = 16,
	.tseg2_min = 2,
	.tseg2_max = 8,
	.sjw_max = 4,
	.brp_min = 1,
	.brp_max = 256,
	.brp_inc = 1,
};

static struct can_bittiming_const can_bittiming_const;

static struct can_bittiming_const can_data_bittiming_const = {
	.name = "can_uart",
	.tseg1_min = 1,
	.tseg1_max = 16,
	.tseg2_min = 1,
	.tseg2_max = 16,
	.sjw_max = 4,
	.brp_min = 1,
	.brp_max = 70,
	.brp_inc = 1,
};

static void uart_can_receive_frame(struct uart_can *priv_data,
				  struct can_receive_frame *frame)
{
	struct can_frame *cf;
	struct sk_buff *skb;
	struct skb_shared_hwtstamps *skt;
	ktime_t nsec;
	struct net_device *netdev;
	int i;
	s64 ts_offset_corrected;
	static u16 buff_frames_disc_cntr;
	static u8 disp_disc_cntr = 1;

	if (frame->can_if >= priv_data->max_can_channels) {
		pr_info("qti_can rcv error. Channel is %d\n", frame->can_if);
		return;
	}

	netdev = priv_data->netdev[frame->can_if];
	skb = alloc_can_skb(netdev, &cf);
	if (!skb) {
		pr_info("skb alloc failed. frame->can_if %d\n", frame->can_if);
		return;
	}

	pr_info("can_uart: rcv frame %d %llu %x %d %x %x %x %x %x %x %x %x\n",
	      frame->can_if, frame->ts, frame->mid, frame->dlc,
	      frame->data[0], frame->data[1], frame->data[2], frame->data[3],
	      frame->data[4], frame->data[5], frame->data[6], frame->data[7]);
	cf->can_id = le32_to_cpu(frame->mid);
	cf->can_dlc = get_can_dlc(frame->dlc);

	/* For 29-bit address, set CAN Extended Frame Flag */
	if ( cf->can_id > 0x7FF)
		cf->can_id |= CAN_EFF_FLAG ;

	for (i = 0; i < cf->can_dlc; i++)
		cf->data[i] = frame->data[i];

	ts_offset_corrected = le64_to_cpu(frame->ts)
		+ priv_data->time_diff;

	/* CAN frames which are received before SOC powers up are discarded */
	if (ts_offset_corrected > 0) {
		if (disp_disc_cntr == 1) {
			pr_info( "No of buff frames discarded is %d\n",buff_frames_disc_cntr);
			disp_disc_cntr = 0;
		}

		nsec = ms_to_ktime(ts_offset_corrected);
		skt = skb_hwtstamps(skb);
		skt->hwtstamp = nsec;
		skb->tstamp = nsec;

		netif_rx(skb);

		pr_info("hwtstamp: %lld\n", ktime_to_ms(skt->hwtstamp));
		netdev->stats.rx_packets++;
	} else {
		buff_frames_disc_cntr++;
		dev_kfree_skb(skb);
	}
}

static int uart_can_process_response(struct uart_can *priv_data,
				    struct rx_resp *resp, int length)
{
	int ret = 0;

	pr_info("can_uart: <%x %2d [%d]\n", resp->cmd, resp->len, resp->seq); 
	if (resp->cmd == CMD_CAN_RECEIVE_FRAME) {
		struct can_receive_frame *frame =
				(struct can_receive_frame *)&resp->data;

		if (resp->len > length) {
				/* Error. This should never happen */
				pr_info("can_uart: %s error: Saving %d bytes\n",
					__func__, length);
				memcpy(priv_data->assembly_buffer, (char *)resp,
				       length);
				priv_data->assembly_buffer_size = length;
			} else {
				uart_can_receive_frame(priv_data, frame);
			}
	}
	return ret;
}

static int uart_can_process_rx(struct uart_can *priv_data, char *rx_buf)
{
	struct rx_resp *resp;
	int length_processed = 0, actual_length = priv_data->xfer_length;
	int ret = 0;

	while (length_processed < actual_length) {
		int length_left = actual_length - length_processed;
		int length = 0; /* length of consumed chunk */
		void *data;

		if (priv_data->assembly_buffer_size > 0) {
			/* should copy just 1 byte instead, since cmd should */
			/* already been copied as being first byte */
			memcpy(priv_data->assembly_buffer +
			       priv_data->assembly_buffer_size,
			       rx_buf, 2);
			data = priv_data->assembly_buffer;
			resp = (struct rx_resp *)data;
			length = resp->len + sizeof(*resp)
					- priv_data->assembly_buffer_size;
			if (length > 0)
				memcpy(priv_data->assembly_buffer +
				       priv_data->assembly_buffer_size,
				       rx_buf, length);
			length_left += priv_data->assembly_buffer_size;
			priv_data->assembly_buffer_size = 0;
		} else {
			data = rx_buf + length_processed;
			resp = (struct rx_resp *)data;
			if (resp->cmd != 0x85) {
				length_processed += 1;
				continue;
			}
		}

		length = resp->len + sizeof(struct rx_resp);
/* 		pr_info("processing. p %d -> l %d (t %d)\n",
			length_processed, length_left, priv_data->xfer_length);  */
		length_processed += length;
		if (length_left >= sizeof(*resp) &&
		    resp->len + sizeof(*resp) <= length_left) {
			struct rx_resp *resp =
					(struct rx_resp *)data;
			ret = uart_can_process_response(priv_data, resp,
							       length_left);

		} else if (length_left > 0) {
			/* Not full message. Store however much we have for */
			/* later assembly */
			pr_info("can_uart: callback: Storing %d bytes of response\n", length_left);
			memcpy(priv_data->assembly_buffer, data, length_left);
			       priv_data->assembly_buffer_size = length_left;
			break;
		}
	}
	return ret;
}

static int uart_can_set_bitrate(struct net_device *netdev)
{
	int ret = 0;

	pr_info("can_uart: CAN bitrate CMD is not supported!\n");
	pr_info("can_uart: Default CAN bitrate is 500kbps\n");

	return ret;
}

static int uart_can_do_uart_transaction(struct uart_can *priv_data)
{
	int actual;

	pr_info("can_uart: uart_can_do_uart_transaction\n");
	set_bit(TTY_DO_WRITE_WAKEUP, &priv_data->tty->flags);
	actual = priv_data->tty->ops->write(priv_data->tty, priv_data->tx_buf, XFER_BUFFER_SIZE);

	return 0;
}

static int uart_can_write(struct uart_can *priv_data,
			 int can_channel, struct canfd_frame *cf)
{
	char *tx_buf, *rx_buf;
	int ret, i;
	struct tx_cmd *req;
	struct can_write_req *req_d;
	struct net_device *netdev;

	if (can_channel < 0 || can_channel >= priv_data->max_can_channels) {
		return -EINVAL;
	}

	tx_buf = priv_data->tx_buf;
	rx_buf = priv_data->rx_buf;
	memset(tx_buf, 0, XFER_BUFFER_SIZE);
	memset(rx_buf, 0, XFER_BUFFER_SIZE);
	priv_data->xfer_length = XFER_BUFFER_SIZE;

	req = (struct tx_cmd *)tx_buf;

		req->cmd = CMD_CAN_SEND_FRAME;
		req->len = CAN_FD_HEADER + cf->len;
		req->seq = atomic_inc_return(&priv_data->msg_seq);
		req_d = (struct can_write_req *)req->data;
		req_d->can_if = can_channel;
		req_d->mid = cf->can_id;
		req_d->dlc = cf->len;

		for (i = 0; i < cf->len; i++)
				req_d->data[i] = cf->data[i];

	ret = uart_can_do_uart_transaction(priv_data);

	netdev = priv_data->netdev[can_channel];
	netdev->stats.tx_packets++;

	return ret;
}

static int uart_can_netdev_open(struct net_device *netdev)
{
	int err;

	pr_info("can_uart: uart_can_netdev_open invoked!!\n");
	netdev_dbg(netdev, "Open");
	err = open_candev(netdev);
	if (err)
		return err;

	netif_start_queue(netdev);

	return 0;
}

static int uart_can_netdev_close(struct net_device *netdev)
{
	pr_info("can_uart: uart_can_netdev_close invoked!!\n");
	netdev_dbg(netdev, "Close");

	clear_bit(TTY_DO_WRITE_WAKEUP, &tty_tst->flags);

	netif_stop_queue(netdev);
	close_candev(netdev);

	/* close tty device node */
	tty_lock(tty_tst);
	if(tty_tst->ops->close)
		tty_tst->ops->close(tty_tst, NULL);
	tty_unlock(tty_tst);

	tty_kclose(tty_tst);

	return 0;
}

static void uart_can_send_can_frame(struct work_struct *ws)
{
	struct uart_can_tx_work *tx_work;
	struct canfd_frame *cf;
	struct uart_can *priv_data;
	struct net_device *netdev;
	struct uart_can_netdev_privdata *netdev_priv_data;
	int can_channel;

	tx_work = container_of(ws, struct uart_can_tx_work, work);
	netdev = tx_work->netdev;
	netdev_priv_data = netdev_priv(netdev);
	priv_data = netdev_priv_data->uart_can;
	can_channel = netdev_priv_data->netdev_index;

	cf = (struct canfd_frame *)tx_work->skb->data;
	uart_can_write(priv_data, can_channel, cf);

	kfree_skb(tx_work->skb);
	kfree(tx_work);
}

static netdev_tx_t uart_can_netdev_start_xmit(struct sk_buff *skb,
					     struct net_device *netdev)
{
	struct uart_can_netdev_privdata *netdev_priv_data = netdev_priv(netdev);
	struct uart_can *priv_data = netdev_priv_data->uart_can;
	struct uart_can_tx_work *tx_work;

	netdev_dbg(netdev, "netdev_start_xmit");
	if (can_dropped_invalid_skb(netdev, skb)) {
		netdev_err(netdev, "Dropping invalid can frame\n");
		return NETDEV_TX_OK;
	}
	tx_work = kzalloc(sizeof(*tx_work), GFP_ATOMIC);
	if (!tx_work)
		return NETDEV_TX_OK;
	INIT_WORK(&tx_work->work, uart_can_send_can_frame);
	tx_work->netdev = netdev;
	tx_work->skb = skb;
	queue_work(priv_data->tx_wq, &tx_work->work);

	return NETDEV_TX_OK;
}

static const struct net_device_ops uart_can_netdev_ops = {
	.ndo_open = uart_can_netdev_open,
	.ndo_stop = uart_can_netdev_close,
	.ndo_start_xmit = uart_can_netdev_start_xmit,
};

static int uart_can_create_netdev(struct tty_struct *tty,
				 struct uart_can *priv_data, int index)
{

	struct net_device *netdev;
	struct uart_can_netdev_privdata *netdev_priv_data;

	pr_info("can_uart: create_netdev invoked!!\n");

	if (index < 0 || index >= priv_data->max_can_channels) {
		return -EINVAL;
	}
	netdev = alloc_candev(sizeof(*netdev_priv_data), MAX_TX_BUFFERS);
	if (!netdev) {
		return -ENOMEM;
	}

	netdev->mtu = CANFD_MTU;

	netdev_priv_data = netdev_priv(netdev);
	netdev_priv_data->uart_can = priv_data;
	netdev_priv_data->netdev_index = index;

	priv_data->netdev[index] = netdev;

	netdev->netdev_ops = &uart_can_netdev_ops;
	//SET_NETDEV_DEV(netdev, &tty->dev);
	netdev_priv_data->can.ctrlmode_supported = CAN_CTRLMODE_3_SAMPLES |
						   CAN_CTRLMODE_LISTENONLY;
	netdev_priv_data->can.bittiming_const = &can_bittiming_const;
	netdev_priv_data->can.data_bittiming_const =
						&can_data_bittiming_const;
	netdev_priv_data->can.clock.freq = MAX_CAN_CLK_FREQ;
	netdev_priv_data->can.do_set_bittiming = uart_can_set_bitrate;

	return 0;
}

static struct uart_can* uart_can_create_priv_data(struct tty_struct *tty)
{
	struct uart_can *priv_data;
	int err;
	struct device **dev;

	dev = &tty->dev;
	priv_data = devm_kzalloc(*dev, sizeof(*priv_data), GFP_KERNEL);
	if (!priv_data) {
		err = -ENOMEM;
		return NULL;
	}
	atomic_set(&priv_data->netif_queue_stop, 0);
	priv_data->assembly_buffer = devm_kzalloc(*dev,
						  RX_ASSEMBLY_BUFFER_SIZE,
						  GFP_KERNEL);
	if (!priv_data->assembly_buffer) {
		err = -ENOMEM;
		goto cleanup_privdata;
	}
	priv_data->fd_buffer = devm_kzalloc(*dev, RX_FD_BUFFER_SIZE,
					    GFP_KERNEL);
	if (!priv_data->fd_buffer) {
		err = -ENOMEM;
		goto cleanup_privdata;
	}

	priv_data->tx_wq = alloc_workqueue("uart_can_tx_wq", 0, 0);
	if (!priv_data->tx_wq) {
		dev_err(priv_data->tty->dev, "Couldn't alloc workqueue\n");
		err = -ENOMEM;
		goto cleanup_privdata;
	}

	priv_data->tx_buf = devm_kzalloc(*dev,
					 XFER_BUFFER_SIZE,
					 GFP_KERNEL);
	priv_data->rx_buf = devm_kzalloc(*dev,
					 XFER_BUFFER_SIZE,
					 GFP_KERNEL);
	if (!priv_data->tx_buf || !priv_data->rx_buf) {
		pr_info("can_uart: Couldn't alloc tx or rx buffers\n");
		err = -ENOMEM;
		goto cleanup_privdata;
	}
	priv_data->xfer_length = 0;
	priv_data->driver_mode = DRIVER_MODE_RAW_FRAMES;

	/* Mark ldisc channel as alive */
	priv_data->tty = tty;
	tty->disc_data = priv_data;

	pr_info("can_uart: priv_data created!!\n");

	atomic_set(&priv_data->msg_seq, 0);
	return priv_data;

cleanup_privdata:
	if (priv_data) {
		if (priv_data->tx_wq)
			destroy_workqueue(priv_data->tx_wq);
	}
	return NULL;
}

static int uart_can_open(struct tty_struct *tty)
{
	int err;
	int i;

	struct device **dev;

	pr_info("can_uart: open invoked!!\n");

	dev = &tty->dev;

	priv_data = uart_can_create_priv_data(tty);
	if (!priv_data) {
		/* dev_err(dev, "Failed to create uart_can priv_data\n"); */
		err = -ENOMEM;
		return err;
	}

	priv_data->max_can_channels = 1;
	can_bittiming_const = flexcan_bittiming_const;
	priv_data->netdev = devm_kcalloc(*dev,
					 priv_data->max_can_channels,
					 sizeof(priv_data->netdev[0]),
					 GFP_KERNEL);
	if (!priv_data->netdev) {
		err = -ENOMEM;
		return err;
	}

	/* Configure TTY interface */
	tty->receive_room = 64;

	for (i = 0; i < priv_data->max_can_channels; i++) {
		err = uart_can_create_netdev(tty, priv_data, i);
		if (err) {
			goto cleanup_candev;
		}

		err = register_candev(priv_data->netdev[i]);
		if (err) {
			goto unregister_candev;
		}
	}

	return 0;

unregister_candev:
	for (i = 0; i < priv_data->max_can_channels; i++)
		unregister_candev(priv_data->netdev[i]);
cleanup_candev:
	if (priv_data) {
		for (i = 0; i < priv_data->max_can_channels; i++) {
			if (priv_data->netdev[i])
				free_candev(priv_data->netdev[i]);
		}
		if (priv_data->tx_wq)
			destroy_workqueue(priv_data->tx_wq);
	}
	return err;
}

static void uart_can_rx(struct tty_struct *tty, const unsigned char *cp,
			     char *fp, int count)
{
	struct uart_can *priv_data = (struct uart_can *)tty->disc_data;

 	pr_info("can_uart: rx invoked with count: %d!!\n", count); 

	if (unlikely(count > XFER_BUFFER_SIZE))
		count = XFER_BUFFER_SIZE;

	memcpy(priv_data->rx_buf, cp, count);
 	pr_info("can_uart: %x %x %x %x %x %x %x %x\n", priv_data->rx_buf[0], priv_data->rx_buf[1], priv_data->rx_buf[2], priv_data->rx_buf[3], 
											priv_data->rx_buf[4], priv_data->rx_buf[5], priv_data->rx_buf[6], priv_data->rx_buf[7]); 

	priv_data->xfer_length = count;
	uart_can_process_rx(priv_data, priv_data->rx_buf);
}

/* Close down uart_can channel.
 */
static void uart_can_close(struct tty_struct *tty)
{
	pr_info("can_uart: uart_can_close invoked!\n");
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0) || LINUX_VERSION_CODE == KERNEL_VERSION(5, 14, 0))
static void uart_can_hangup(struct tty_struct *tty)
{
        uart_can_close(tty);
        return;
}
#else
static int uart_can_hangup(struct tty_struct *tty)
{
        uart_can_close(tty);
        return 0;
}
#endif

static struct tty_ldisc_ops uart_can_ldisc = {
	.owner = THIS_MODULE,
	.name = "can_uart",
	.num =  N_CAN_UART,
	.receive_buf = uart_can_rx,
	.open = uart_can_open,
	.close = uart_can_close,
	.hangup = uart_can_hangup,
};

struct ktermios tty_tst_termios = {	/* for the benefit of tty drivers  */
	.c_iflag = ICRNL | IXON,
	.c_oflag = OPOST | ONLCR,
	.c_cflag = B115200 | CS8 | CREAD | HUPCL,
	.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK |
		   ECHOCTL | ECHOKE | IEXTEN,
	.c_cc = INIT_C_CC,
	.c_ispeed = 115200,
	.c_ospeed = 115200,
};

static int __init uart_can_init(void)
{
 	int status = 0;
	dev_t tty_device;
	int ret = 0;

	pr_info("can_uart: CAN over UART driver\n");

	status = tty_register_ldisc( N_CAN_UART, &uart_can_ldisc);
	if (status) {
		pr_err("can_uart: Can't register line discipline\n");
		return status;
	}
	pr_info("can_uart: dev name: %s",uart_can_tty_name);

	if(uart_can_tty_name != NULL) {
		status = tty_dev_name_to_number(uart_can_tty_name, &tty_device);
		pr_info("can_uart: 0x%x is dev_t & ret is %d\n", tty_device, ret);
		if(status) {
			pr_err("can_uart: Invalid tty name %s  ret = %d",uart_can_tty_name,status);
			tty_unregister_ldisc( N_CAN_UART);
			return -EINVAL;
		}
		tty_tst = tty_kopen(tty_device);
		if(IS_ERR(tty_tst)) {
			status = PTR_ERR(tty_tst);
			pr_err("can_uart: failed to open tty (ret=%d)",status);
			tty_unregister_ldisc( N_CAN_UART);
			return status;
		}

		pr_info("can_uart: device opened name: %s, major: %d, minor_start: %d & num: %d\n",
			tty_tst->name, tty_tst->driver->major, tty_tst->driver->minor_start, 
			tty_tst->driver->num);
		if(tty_tst->ops->open)
			ret = tty_tst->ops->open(tty_tst, NULL);
		pr_info("can_uart: tty->ops->open invoked! with ret %d\n", ret);

		tty_unlock(tty_tst);
		ret = tty_set_ldisc(tty_tst, N_CAN_UART);
		if (!ret) {
			pr_info("can_uart: tty_set_ldisc succeeded\n");
			ret = tty_set_termios(tty_tst, &tty_tst_termios);
			pr_info("can_uart: tty_set_termios returned %d\n", ret);
			return status;
		} else {
			pr_err("can_uart: tty_set_ldisc failed with %d\n", ret);
		}
		tty_lock(tty_tst);
		if(tty_tst->ops->close)
			tty_tst->ops->close(tty_tst, NULL);
		tty_unlock(tty_tst);
		tty_kclose(tty_tst);
	}
	return status;
}

static void __exit uart_can_exit(void)
{
	pr_info("can_uart: uart_can_exit invoked!\n");

	if(priv_data->netdev[0]) {
		unregister_candev(priv_data->netdev[0]);
		free_candev(priv_data->netdev[0]);
		pr_info("can_uart: unregistered!\n");
	}

	tty_unregister_ldisc( N_CAN_UART);
}

module_init(uart_can_init);
module_exit(uart_can_exit);

MODULE_ALIAS_LDISC(N_CAN_UART);
MODULE_DESCRIPTION("CAN UART controller module");
MODULE_LICENSE("GPL v2");
