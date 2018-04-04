/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2018 Intel Corporation
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>

#include "ishtp-dev.h"
#include "client.h"

/* Rx ring buffer pool size */
#define TTY_CL_RX_RING_SIZE	32
#define TTY_CL_TX_RING_SIZE	16

enum ish_uart_command{
	UART_GET_CONFIG = 1,
	UART_SET_CONFIG,
	UART_SEND_DATA,
	UART_RECV_DATA,
	UART_ABORT_WRITE,
	UART_ABORT_READ,
};

#define CMD_MASK	GENMASK(6,0)
#define IS_RESPONSE	BIT(7)

struct ishtp_tty_msg {
	u8 command; /* Bit 7 : is_response */
	u8 status;
	u16 size;
};

struct uart_config {
	u32 baud;
	u8 bits_length:4;
	u8 stop_bits:2;
	u8 parity:1;
	u8 even_parity:1;
	u8 flow_control:1;
	u8 reserved:7;
};

struct ishtp_cl_tty {
	struct tty_port port;
	struct ishtp_cl_device *cl_device;
	struct ishtp_cl *ishtp_cl;
	u32 baud;
	u8 bits_length;
	int max_msg_size;
	bool get_report_done;
	int last_cmd_status;
	wait_queue_head_t ishtp_tty_wait;
};

#define cl_tty_dev(tp) &tp->ishtp_cl->device->dev
static struct ishtp_cl_tty *ishtp_cl_tty_device;

static const guid_t tty_ishtp_guid = GUID_INIT(0x6f2647c7, 0x3e16, 0x4d79,
					       0xb4, 0xff, 0x02, 0x89, 0x28,
					       0xee, 0xeb, 0xca);

static int ishtp_wait_for_response(struct ishtp_cl_tty *tp)
{
	if (tp->get_report_done)
		return 0;

	/*
	 * ISH firmware max delay for one time sending failure is 1Hz,
	 * and firmware will retry 2 times, so 3Hz is used for timeout.
	 */
	wait_event_interruptible_timeout(tp->ishtp_tty_wait,
					 tp->get_report_done, 3 * HZ);

	if (!tp->get_report_done) {
		dev_err(cl_tty_dev(tp), "Timeout waiting for response from ISHTP device\n");
		return -ETIMEDOUT;
	}

	return 0;
}

static int ish_tty_install(struct tty_driver *driver, struct tty_struct *tty)
{
	tty->driver_data = ishtp_cl_tty_device;

	return tty_port_install(&ishtp_cl_tty_device->port, driver, tty);
}

static int ish_tty_open(struct tty_struct *tty, struct file *filp)
{
	struct ishtp_cl_tty *tp = tty->driver_data;

	return tty_port_open(&tp->port, tty, filp);
}

static void ish_tty_close(struct tty_struct *tty, struct file *filp)
{
	struct ishtp_cl_tty *tp = tty->driver_data;

	tty_port_close(&tp->port, tty, filp);
}

static int ish_tty_write(struct tty_struct *tty, const unsigned char *buf,
			 int count)
{
	struct ishtp_cl_tty *tp = tty->driver_data;
	unsigned char *ishtp_buf;
	struct ishtp_tty_msg *ishtp_msg;
	unsigned char *msg_buf;
	u32 max_msg_payload_size;
	int c, ret = 0;

	dev_dbg(tty->dev, "write_req: len=%d\n", count);

	ishtp_buf = kzalloc(sizeof(struct ishtp_tty_msg) + tp->max_msg_size,
			    GFP_KERNEL);
	ishtp_msg = (struct ishtp_tty_msg *)ishtp_buf;
	if (!ishtp_msg)
		return -ENOMEM;

	ishtp_msg->command = UART_SEND_DATA;
	msg_buf = (unsigned char *)&ishtp_msg[1];

	max_msg_payload_size = tp->max_msg_size -
					sizeof(struct ishtp_tty_msg);

	while (count > 0) {
		c = count;
		if (c > max_msg_payload_size)
			c = max_msg_payload_size;

		ishtp_msg->size = c;
		memcpy(msg_buf, buf, c);

		tp->get_report_done = false;
		tp->last_cmd_status = -EIO;
		/* ishtp message send out */
		ishtp_cl_send(tp->ishtp_cl, ishtp_buf,
			      sizeof(struct ishtp_tty_msg) + c);

		buf += c;
		count -= c;
		ret += c;

		/* wait for message send completely */
		if (ishtp_wait_for_response(tp) < 0) {
			ret = -ETIMEDOUT;
			goto free_buffer;
		}

		if (tp->last_cmd_status) {
			ret = tp->last_cmd_status;
			goto free_buffer;
		}
	}
free_buffer:
	kfree(ishtp_buf);
	return ret;
}

static void ish_tty_set_termios(struct tty_struct *tty,
					struct ktermios *old_termios)
{
	struct ishtp_cl_tty *tp = tty->driver_data;
	static unsigned char ishtp_buf[10];
	struct ishtp_tty_msg *ishtp_msg;
	struct ktermios *termios;
	struct uart_config *cfg;
	unsigned int baud;

	if (old_termios && !tty_termios_hw_change(&tty->termios, old_termios))
		return;

	termios = &tty->termios;
	ishtp_msg = (struct ishtp_tty_msg *)ishtp_buf;

	ishtp_msg->command = UART_SET_CONFIG;
	cfg = (struct uart_config *)&ishtp_msg[1];

	switch (C_CSIZE(tty)) {
	case CS5:
		cfg->bits_length = 5;
	case CS6:
		cfg->bits_length = 6;
	case CS7:
		cfg->bits_length = 7;
	default:
	case CS8:
		cfg->bits_length = 8;
	}

	if (C_CRTSCTS(tty))
		cfg->flow_control = true;
	else
		cfg->flow_control = false;

	baud = tty_termios_baud_rate(termios);

	if (baud != 9600 && baud != 57600 && baud != 19200 &&
	    baud != 38400 && baud != 115200 && baud != 921600 &&
	    baud != 2000000 && baud != 3000000 && baud != 3250000 &&
	    baud != 3500000 && baud != 4000000) {
		dev_err(tty->dev, "%s: baud[%d] is not supported\n",
			__func__, baud);
		return;
	}

	cfg->baud = baud;

	ishtp_msg->size = sizeof(struct uart_config);

	tp->get_report_done = false;
	/* ishtp message send out */
	ishtp_cl_send(tp->ishtp_cl, ishtp_buf,
		      sizeof(struct ishtp_tty_msg) + ishtp_msg->size);

	/* wait for message send completely */
	ishtp_wait_for_response(tp);
}

static int ish_tty_write_room(struct tty_struct *tty)
{
	struct ishtp_cl_tty *tp = tty->driver_data;

	return tp->max_msg_size;
}

static struct tty_operations ish_tty_ops = {
	.install = ish_tty_install,
	.open = ish_tty_open,
	.close = ish_tty_close,
	.write = ish_tty_write,
	.set_termios	= ish_tty_set_termios,
	.write_room = ish_tty_write_room,
};

static struct tty_driver *ish_tty_driver;

static void process_recv(struct ishtp_cl_device *cl_device, void *recv_buf,
			 size_t data_len)
{
	struct ishtp_tty_msg *ishtp_msg;
	struct ishtp_cl_tty *tp = ishtp_cl_tty_device;
	struct uart_config *cfg;
	unsigned char	*payload;
	size_t payload_len, total_len, cur_pos;

	dev_dbg(&cl_device->dev, "ishtp receive ():+++ len=%zu\n", data_len);

	if (data_len < sizeof(struct ishtp_tty_msg)) {
		dev_err(&cl_device->dev,
			"Error, received %zu which is ", data_len);
		dev_err(&cl_device->dev, " less than data header %lu\n",
			sizeof(struct ishtp_tty_msg));
		return;
	}

	payload = recv_buf + sizeof(struct ishtp_tty_msg);
	total_len = data_len;
	cur_pos = 0;

	do {
		ishtp_msg = (struct ishtp_tty_msg *)(recv_buf + cur_pos);
		payload_len = ishtp_msg->size;

		switch (ishtp_msg->command & CMD_MASK) {
		case UART_GET_CONFIG:
			tp->get_report_done = true;
			if (!(ishtp_msg->command & IS_RESPONSE) ||
			    (ishtp_msg->status)) {
				dev_err(&cl_device->dev,
					"Recv command with status error\n");
				wake_up_interruptible(&tp->ishtp_tty_wait);
				break;
			}

			cfg = (struct uart_config *)payload;
			tp->baud = cfg->baud;
			tp->bits_length = cfg->bits_length;
			dev_dbg(&cl_device->dev,
				"Command: get config: %d:%d\n",
				tp->baud, tp->bits_length);
			wake_up_interruptible(&tp->ishtp_tty_wait);
			break;
		case UART_SET_CONFIG:
			tp->get_report_done = true;
			if (!(ishtp_msg->command & IS_RESPONSE) ||
			    (ishtp_msg->status)) {
				dev_err(&cl_device->dev,
					"Recv command with status error\n");
				wake_up_interruptible(&tp->ishtp_tty_wait);
				break;
			}

			tp->last_cmd_status = 0;
			dev_dbg(&cl_device->dev,
				"Command: set config success\n");
			wake_up_interruptible(&tp->ishtp_tty_wait);
			break;
		case UART_SEND_DATA:
			tp->get_report_done = true;
			if (!(ishtp_msg->command & IS_RESPONSE) ||
			    (ishtp_msg->status)) {
				dev_err(&cl_device->dev,
					"Recv command with status error\n");
				wake_up_interruptible(&tp->ishtp_tty_wait);
				break;
			}

			tp->last_cmd_status = 0;
			dev_dbg (&cl_device->dev,
				 "Command: send data done\n");
			wake_up_interruptible(&tp->ishtp_tty_wait);
			break;
		case UART_RECV_DATA:
			dev_dbg(&cl_device->dev,
				"Command: recv data: len=%ld\n",
				payload_len);
			print_hex_dump_bytes("", DUMP_PREFIX_NONE, payload,
					     payload_len);
			tty_insert_flip_string(&tp->port, payload,
					       payload_len);
			tty_flip_buffer_push(&tp->port);
			break;
		case UART_ABORT_WRITE:
			tp->get_report_done = true;
			wake_up_interruptible(&tp->ishtp_tty_wait);
			break;
		case UART_ABORT_READ:
			tp->get_report_done = true;
			wake_up_interruptible(&tp->ishtp_tty_wait);
			break;
		default:
			break;
		}

		cur_pos += payload_len + sizeof(struct ishtp_tty_msg);
		payload += payload_len + sizeof(struct ishtp_tty_msg);

	} while (cur_pos < total_len);
}

static void tty_ishtp_cl_event_cb(struct ishtp_cl_device *cl_device)
{
	struct ishtp_cl	*tty_ishtp_cl = ishtp_get_drvdata(cl_device);
	struct ishtp_cl_rb *rb_in_proc;

	if (!tty_ishtp_cl)
		return;

	while ((rb_in_proc = ishtp_cl_rx_get_rb(tty_ishtp_cl)) &&
	       rb_in_proc->buffer.data) {
		process_recv(cl_device, rb_in_proc->buffer.data,
			     rb_in_proc->buf_idx);
		ishtp_cl_io_rb_recycle(rb_in_proc);
	}
}

static const struct tty_port_operations ish_port_ops;

static int ishtp_cl_tty_connect(struct ishtp_cl_tty *tty_dev,
				struct ishtp_cl_device *cl_device)
{
	struct ishtp_cl *cl;
	struct ishtp_fw_client *fw_client;
	int ret;

	cl = ishtp_cl_allocate(cl_device->ishtp_dev);
	if (!cl) {
		ret = -ENOMEM;
		return ret;
	}

	ret = ishtp_cl_link(cl, ISHTP_HOST_CLIENT_ID_ANY);
	if (ret)
		goto out_client_free;


	fw_client = ishtp_fw_cl_get_client(cl->dev, &tty_ishtp_guid);
	if (!fw_client) {
		ret = -ENOENT;
		goto out_client_free;
	}

	cl->fw_client_id = fw_client->client_id;
	cl->state = ISHTP_CL_CONNECTING;
	cl->rx_ring_size = TTY_CL_RX_RING_SIZE;
	cl->tx_ring_size = TTY_CL_TX_RING_SIZE;
	ret = ishtp_cl_connect(cl);
	if (ret) {
		dev_err(&cl_device->dev, "client connect failed\n");
		goto out_client_free;
	}

	init_waitqueue_head(&tty_dev->ishtp_tty_wait);
	tty_dev->max_msg_size = cl_device->fw_client->props.max_msg_length;
	ishtp_set_drvdata(cl_device, cl);
	tty_dev->ishtp_cl= cl;

	/* Register read callback */
	ishtp_register_event_cb(cl_device, tty_ishtp_cl_event_cb);

        ishtp_get_device(cl_device);

	return 0;

out_client_free:
	ishtp_cl_free(cl);

	return ret;
}

static void ishtp_cl_tty_disconnect(struct ishtp_cl_device *cl_device)
{
	struct ishtp_cl	*cl = ishtp_get_drvdata(cl_device);

	ishtp_register_event_cb(cl->device, NULL);
	cl->state = ISHTP_CL_DISCONNECTING;
	ishtp_cl_disconnect(cl);
	ishtp_put_device(cl_device);
	ishtp_cl_unlink(cl);
	ishtp_cl_flush_queues(cl);
	ishtp_cl_free(cl);
}

static int ishtp_cl_tty_init(struct ishtp_cl_device *cl_device)
{
	struct device *dev;
	int ret;

	ishtp_cl_tty_device = kzalloc(sizeof(*ishtp_cl_tty_device),
				      GFP_KERNEL);
	if (!ishtp_cl_tty_device)
		return -ENOMEM;

	ish_tty_driver = alloc_tty_driver(1);
	if (!ish_tty_driver) {
		ret = -ENOMEM;
		goto free_device;
	}

	ish_tty_driver->owner = THIS_MODULE;
	ish_tty_driver->driver_name = "ish-serial";
	ish_tty_driver->name = "ttyISH";
	ish_tty_driver->minor_start = 0;
	ish_tty_driver->major = 0;
	ish_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	ish_tty_driver->subtype = SERIAL_TYPE_NORMAL;
	ish_tty_driver->flags = TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV;

	ish_tty_driver->init_termios = tty_std_termios;
	ish_tty_driver->init_termios.c_cflag = B115200 | CS8 | HUPCL | CLOCAL;
	ish_tty_driver->init_termios.c_lflag = ISIG | ICANON | IEXTEN;
	tty_set_operations(ish_tty_driver, &ish_tty_ops);

	ishtp_cl_tty_device->cl_device = cl_device;
	tty_port_init(&ishtp_cl_tty_device->port);

	ret = tty_register_driver(ish_tty_driver);
	if (ret) {
		put_tty_driver(ish_tty_driver);
		return ret;
	}

	ret = ishtp_cl_tty_connect(ishtp_cl_tty_device, cl_device);
	if (ret)
		goto free_tty_driver;

	ishtp_cl_tty_device->cl_device = cl_device;
	tty_port_init(&ishtp_cl_tty_device->port);
	ishtp_cl_tty_device->port.ops = &ish_port_ops;
	dev = tty_port_register_device(&ishtp_cl_tty_device->port,
				       ish_tty_driver, 0, &cl_device->dev);

	if (IS_ERR(dev)) {
		tty_port_put(&ishtp_cl_tty_device->port);
		ret = PTR_ERR(dev);
		goto disconnect_tty_driver;
	}

	return 0;
disconnect_tty_driver:
	ishtp_cl_tty_disconnect(cl_device);
free_tty_driver:
	tty_unregister_driver(ish_tty_driver);
	put_tty_driver(ish_tty_driver);
free_device:
	kfree(ishtp_cl_tty_device);
	ishtp_cl_tty_device = NULL;

	return ret;
}

static void ishtp_cl_tty_deinit(void)
{
	tty_unregister_device(ish_tty_driver, 0);
	tty_port_destroy(&ishtp_cl_tty_device->port);
	tty_unregister_driver(ish_tty_driver);
	put_tty_driver(ish_tty_driver);
	ishtp_cl_tty_disconnect(ishtp_cl_tty_device->cl_device);
	ishtp_set_drvdata(ishtp_cl_tty_device->cl_device, NULL);
	kfree(ishtp_cl_tty_device);
	ishtp_cl_tty_device = NULL;
}

static int ishtp_cl_tty_probe(struct ishtp_cl_device *cl_device)
{
	int ret;

	if (ishtp_cl_tty_device)
		return -EEXIST;

	if (!cl_device)
		return -ENODEV;

	if (uuid_le_cmp(tty_ishtp_guid,
			cl_device->fw_client->props.protocol_name) != 0)
		return	-ENODEV;

	ret = ishtp_cl_tty_init(cl_device);
	if (ret)
		return ret;

	return 0;
}

static int ishtp_cl_tty_remove(struct ishtp_cl_device *cl_device)
{
	ishtp_cl_tty_deinit();

	return 0;
}

static struct ishtp_cl_driver ishtp_cl_tty_driver = {
	.name = "ishtp-client",
	.probe = ishtp_cl_tty_probe,
	.remove = ishtp_cl_tty_remove,
};

static int __init ishtp_tty_client_init(void)
{
	return ishtp_cl_driver_register(&ishtp_cl_tty_driver);
}
module_init(ishtp_tty_client_init);

static void __exit ishtp_tty_client_exit(void)
{
	ishtp_cl_driver_unregister(&ishtp_cl_tty_driver);
}
module_exit(ishtp_tty_client_exit);

MODULE_DESCRIPTION("ISH ISHTP TTY client driver");
MODULE_AUTHOR("Even Xu <even.xu@intel.com>");
MODULE_AUTHOR("Srinivas Pandruvada <srinivas.pandruvada@linux.intel.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("ishtp:*");
