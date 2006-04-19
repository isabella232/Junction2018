/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2000-2001  Qualcomm Incorporated
 *  Copyright (C) 2002-2003  Maxim Krasnyansky <maxk@qualcomm.com>
 *  Copyright (C) 2002-2006  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include "glib-ectomy.h"

#include "hcid.h"
#include "lib.h"
#include "sdp.h"

struct hcid_opts hcid;
struct device_opts default_device;
struct device_opts *parser_device;
static struct device_list *device_list = NULL;

static GMainLoop *event_loop;

static void usage(void)
{
	printf("hcid - HCI daemon ver %s\n", VERSION);
	printf("Usage: \n");
	printf("\thcid [-n not_daemon] [-f config file]\n");
}

static inline void init_device_defaults(struct device_opts *device_opts)
{
	memset(device_opts, 0, sizeof(*device_opts));
	device_opts->scan = SCAN_PAGE;
	device_opts->name = strdup("BlueZ");
	device_opts->discovto = HCID_DEFAULT_DISCOVERABLE_TIMEOUT;
}

struct device_opts *alloc_device_opts(char *ref)
{
	struct device_list *device;

	device = malloc(sizeof(struct device_list));
	if (!device) {
		info("Can't allocate devlist opts buffer: %s (%d)",
							strerror(errno), errno);
		exit(1);
	}

	device->ref = ref;
	device->next = device_list;
	device_list = device;

	memcpy(&device->opts, &default_device, sizeof(struct device_opts));
	device->opts.name = strdup(default_device.name);

	return &device->opts;
}

static void free_device_opts(void)
{
	struct device_list *device, *next;

	if (default_device.name) {
		free(default_device.name);
		default_device.name = NULL;
	}

	for (device = device_list; device; device = next) {
		free(device->ref);
		if (device->opts.name)
			free(device->opts.name);
		next = device->next;
		free(device);
	}

	device_list = NULL;
}

static inline struct device_opts *find_device_opts(char *ref)
{
	struct device_list *device;

	for (device = device_list; device; device = device->next)
		if (!strcmp(ref, device->ref))
			return &device->opts;

	return NULL;
}

static struct device_opts *get_device_opts(int sock, int hdev)
{
	struct device_opts *device_opts = NULL;
	struct hci_dev_info di;

	/* First try to get BD_ADDR based settings ... */
	di.dev_id = hdev;
	if (!ioctl(sock, HCIGETDEVINFO, (void *) &di)) {
		char addr[18];
		ba2str(&di.bdaddr, addr);
		device_opts = find_device_opts(addr);
	}

	/* ... then try HCI based settings ... */
	if (!device_opts) {
		char ref[8];
		snprintf(ref, sizeof(ref) - 1, "hci%d", hdev);
		device_opts = find_device_opts(ref);
	}

	/* ... and last use the default settings. */
	if (!device_opts)
		device_opts = &default_device;

	return device_opts;
}

int get_discoverable_timeout(int hdev)
{
	int sock, timeout;
	char address[18];
	struct device_opts *device_opts = NULL;
	struct hci_dev_info di;

	if (hdev < 0)
		return HCID_DEFAULT_DISCOVERABLE_TIMEOUT;

	sock = hci_open_dev(hdev);
	if (sock < 0)
		goto no_address;

	di.dev_id = hdev;
	if (!ioctl(sock, HCIGETDEVINFO, (void *) &di))
		ba2str(&di.bdaddr, address);
	else {
		close(sock);
		goto no_address;
	}

	close(sock);

	if (!read_discoverable_timeout(address, &timeout))
		return timeout;

	device_opts = find_device_opts(address);

no_address:
	if (!device_opts) {
		char ref[8];
		snprintf(ref, sizeof(ref) - 1, "hci%d", hdev);
		device_opts = find_device_opts(ref);
	}

	if (!device_opts)
		device_opts = &default_device;

	return device_opts->discovto;
}

static void configure_device(int hdev)
{
	struct device_opts *device_opts;
	struct hci_dev_req dr;
	struct hci_dev_info di;
	char mode[14];
	int s;

	/* Do configuration in the separate process */
	switch (fork()) {
		case 0:
			break;
		case -1:
			error("Fork failed. Can't init device hci%d: %s (%d)",
						hdev, strerror(errno), errno);
		default:
			return;
	}

	set_title("hci%d config", hdev);

	if ((s = hci_open_dev(hdev)) < 0) {
		error("Can't open device hci%d: %s (%d)",
						hdev, strerror(errno), errno);
		exit(1);
	}

	di.dev_id = hdev;
	if (ioctl(s, HCIGETDEVINFO, (void *) &di) < 0)
		exit(1);

	if (hci_test_bit(HCI_RAW, &di.flags))
		exit(0);

	dr.dev_id   = hdev;
	device_opts = get_device_opts(s, hdev);

	/* Set scan mode */
	if (!read_device_mode(&di.bdaddr, mode, sizeof(mode))) {
		if (!strcmp(mode, MODE_OFF))
			device_opts->scan = SCAN_DISABLED;
		else if (!strcmp(mode, MODE_CONNECTABLE))
			device_opts->scan = SCAN_PAGE;
		else if (!strcmp(mode, MODE_DISCOVERABLE))
			device_opts->scan = SCAN_PAGE | SCAN_INQUIRY;
	}

	dr.dev_opt = device_opts->scan;
	if (ioctl(s, HCISETSCAN, (unsigned long) &dr) < 0) {
		error("Can't set scan mode on hci%d: %s (%d)",
				hdev, strerror(errno), errno);
	}

	/* Set authentication */
	if (device_opts->auth)
		dr.dev_opt = AUTH_ENABLED;
	else
		dr.dev_opt = AUTH_DISABLED;

	if (ioctl(s, HCISETAUTH, (unsigned long) &dr) < 0) {
		error("Can't set auth on hci%d: %s (%d)",
						hdev, strerror(errno), errno);
	}

	/* Set encryption */
	if (device_opts->encrypt)
		dr.dev_opt = ENCRYPT_P2P;
	else
		dr.dev_opt = ENCRYPT_DISABLED;

	if (ioctl(s, HCISETENCRYPT, (unsigned long) &dr) < 0) {
		error("Can't set encrypt on hci%d: %s (%d)",
						hdev, strerror(errno), errno);
	}

	/* Set device name */
	if ((device_opts->flags & (1 << HCID_SET_NAME)) && device_opts->name) {
		change_local_name_cp cp;
		write_ext_inquiry_response_cp ip;
		char name[249];
		uint8_t len;

		memset(name, 0, sizeof(name));
		if (read_local_name(&di.bdaddr, name) < 0) {
			memset(cp.name, 0, sizeof(cp.name));
			expand_name((char *) cp.name, sizeof(cp.name),
						device_opts->name, hdev);
		} else
			memcpy(cp.name, name, sizeof(cp.name));

		ip.fec = 0x00;
		memset(ip.data, 0, sizeof(ip.data));
		len = strlen((char *) cp.name);
		if (len > 48) {
			len = 48;
			ip.data[1] = 0x08;
		} else
			ip.data[1] = 0x09;
		ip.data[0] = len + 1;
		memcpy(ip.data + 2, cp.name, len);

		hci_send_cmd(s, OGF_HOST_CTL, OCF_CHANGE_LOCAL_NAME,
					CHANGE_LOCAL_NAME_CP_SIZE, &cp);

		if (di.features[6] & LMP_EXT_INQ)
			hci_send_cmd(s, OGF_HOST_CTL, OCF_WRITE_EXT_INQUIRY_RESPONSE,
					WRITE_EXT_INQUIRY_RESPONSE_CP_SIZE, &ip);
	}

	/* Set device class */
	if ((device_opts->flags & (1 << HCID_SET_CLASS))) {
		write_class_of_dev_cp cp;
		uint32_t class;
		uint8_t cls[3];

		if (read_local_class(&di.bdaddr, cls) < 0) {
			class = htobl(device_opts->class);
			memcpy(cp.dev_class, &class, 3);
		} else
			memcpy(cp.dev_class, cls, 3);

		hci_send_cmd(s, OGF_HOST_CTL, OCF_WRITE_CLASS_OF_DEV,
					WRITE_CLASS_OF_DEV_CP_SIZE, &cp);
	}

	/* Set voice setting */
	if ((device_opts->flags & (1 << HCID_SET_VOICE))) {
		write_voice_setting_cp cp;

		cp.voice_setting = htobl(device_opts->voice);
		hci_send_cmd(s, OGF_HOST_CTL, OCF_WRITE_VOICE_SETTING,
					WRITE_VOICE_SETTING_CP_SIZE, &cp);
	}

	/* Set inquiry mode */
	if ((device_opts->flags & (1 << HCID_SET_INQMODE))) {
		write_inquiry_mode_cp cp;

		switch (device_opts->inqmode) {
		case 2:
			if (di.features[6] & LMP_EXT_INQ) {
				cp.mode = 2;
				break;
			}
		case 1:
			if (di.features[3] & LMP_RSSI_INQ) {
				cp.mode = 1;
				break;
			}
		default:
			cp.mode = 0;
			break;
		}

		hci_send_cmd(s, OGF_HOST_CTL, OCF_WRITE_INQUIRY_MODE,
					WRITE_INQUIRY_MODE_CP_SIZE, &cp);
	}

	/* Set page timeout */
	if ((device_opts->flags & (1 << HCID_SET_PAGETO))) {
		write_page_timeout_cp cp;

		cp.timeout = htobs(device_opts->pageto);
		hci_send_cmd(s, OGF_HOST_CTL, OCF_WRITE_PAGE_TIMEOUT,
					WRITE_PAGE_TIMEOUT_CP_SIZE, &cp);
	}

	/* Set default discoverable timeout if not set */
	if (!(device_opts->flags & (1 << HCID_SET_DISCOVTO)))
		device_opts->discovto = HCID_DEFAULT_DISCOVERABLE_TIMEOUT;

	exit(0);
}

static void init_device(int hdev)
{
	struct device_opts *device_opts;
	struct hci_dev_req dr;
	struct hci_dev_info di;
	int s;

	/* Do initialization in the separate process */
	switch (fork()) {
		case 0:
			break;
		case -1:
			error("Fork failed. Can't init device hci%d: %s (%d)",
						hdev, strerror(errno), errno);
		default:
			return;
	}

	set_title("hci%d init", hdev);

	if ((s = hci_open_dev(hdev)) < 0) {
		error("Can't open device hci%d: %s (%d)",
						hdev, strerror(errno), errno);
		exit(1);
	}

	/* Start HCI device */
	if (ioctl(s, HCIDEVUP, hdev) < 0 && errno != EALREADY) {
		error("Can't init device hci%d: %s (%d)",
						hdev, strerror(errno), errno);
		exit(1);
	}

	di.dev_id = hdev;
	if (ioctl(s, HCIGETDEVINFO, (void *) &di) < 0)
		exit(1);

	if (hci_test_bit(HCI_RAW, &di.flags))
		exit(0);

	dr.dev_id   = hdev;
	device_opts = get_device_opts(s, hdev);

	/* Set packet type */
	if ((device_opts->flags & (1 << HCID_SET_PTYPE))) {
		dr.dev_opt = device_opts->pkt_type;
		if (ioctl(s, HCISETPTYPE, (unsigned long) &dr) < 0) {
			error("Can't set packet type on hci%d: %s (%d)",
						hdev, strerror(errno), errno);
		}
	}

	/* Set link mode */
	if ((device_opts->flags & (1 << HCID_SET_LM))) {
		dr.dev_opt = device_opts->link_mode;
		if (ioctl(s, HCISETLINKMODE, (unsigned long) &dr) < 0) {
			error("Can't set link mode on hci%d: %s (%d)",
						hdev, strerror(errno), errno);
		}
	}

	/* Set link policy */
	if ((device_opts->flags & (1 << HCID_SET_LP))) {
		dr.dev_opt = device_opts->link_policy;
		if (ioctl(s, HCISETLINKPOL, (unsigned long) &dr) < 0) {
			error("Can't set link policy on hci%d: %s (%d)",
						hdev, strerror(errno), errno);
		}
	}

	exit(0);
}

static void init_all_devices(int ctl)
{
	struct hci_dev_list_req *dl;
	struct hci_dev_req *dr;
	int i;

	if (!(dl = malloc(HCI_MAX_DEV * sizeof(struct hci_dev_req) + sizeof(uint16_t)))) {
		info("Can't allocate devlist buffer: %s (%d)",
							strerror(errno), errno);
		exit(1);
	}
	dl->dev_num = HCI_MAX_DEV;
	dr = dl->dev_req;

	if (ioctl(ctl, HCIGETDEVLIST, (void *) dl) < 0) {
		info("Can't get device list: %s (%d)",
							strerror(errno), errno);
		exit(1);
	}

	for (i = 0; i < dl->dev_num; i++, dr++) {
		if (hcid.auto_init)
			init_device(dr->dev_id);

		add_device(dr->dev_id);

		if (hcid.auto_init && hci_test_bit(HCI_UP, &dr->dev_opt))
			configure_device(dr->dev_id);

		if (hcid.security && hci_test_bit(HCI_UP, &dr->dev_opt))
			start_security_manager(dr->dev_id);

		start_device(dr->dev_id);

#ifdef ENABLE_DBUS
		hcid_dbus_register_device(dr->dev_id);
#endif
	}

	free(dl);
}

static void init_defaults(void)
{
	hcid.auto_init = 1;
	hcid.security  = HCID_SEC_AUTO;

	init_device_defaults(&default_device);
}

static void sig_term(int sig)
{
	g_main_quit(event_loop);
}

static void sig_hup(int sig)
{
	info("Reloading config file");

	init_defaults();

	if (read_config(hcid.config_file) < 0)
		error("Config reload failed");

	init_security_data();

	init_all_devices(hcid.sock);
}

static inline void device_event(GIOChannel *chan, evt_stack_internal *si)
{
	evt_si_device *sd = (void *) &si->data;

	switch (sd->event) {
	case HCI_DEV_REG:
		info("HCI dev %d registered", sd->dev_id);
		if (hcid.auto_init)
			init_device(sd->dev_id);
		add_device(sd->dev_id);
#ifdef ENABLE_DBUS
		hcid_dbus_register_device(sd->dev_id);
#endif
		break;

	case HCI_DEV_UNREG:
		info("HCI dev %d unregistered", sd->dev_id);
#ifdef ENABLE_DBUS
		hcid_dbus_unregister_device(sd->dev_id);
#endif
		remove_device(sd->dev_id);
		break;

	case HCI_DEV_UP:
		info("HCI dev %d up", sd->dev_id);
		if (hcid.auto_init)
			configure_device(sd->dev_id);
		if (hcid.security)
			start_security_manager(sd->dev_id);
		start_device(sd->dev_id);
		break;

	case HCI_DEV_DOWN:
		info("HCI dev %d down", sd->dev_id);
		if (hcid.security)
			stop_security_manager(sd->dev_id);
		stop_device(sd->dev_id);
		break;
	}
}

static gboolean io_stack_event(GIOChannel *chan, GIOCondition cond, gpointer data)
{
	unsigned char buf[HCI_MAX_FRAME_SIZE], *ptr;
	evt_stack_internal *si;
	hci_event_hdr *eh;
	int type;
	size_t len;
	GIOError err;

	ptr = buf;

	if ((err = g_io_channel_read(chan, (gchar *) buf, sizeof(buf), &len))) {
		if (err == G_IO_ERROR_AGAIN)
			return TRUE;

		error("Read from control socket failed: %s (%d)",
							strerror(errno), errno);
		g_main_quit(event_loop);
		return FALSE;
	}

	type = *ptr++;

	if (type != HCI_EVENT_PKT)
		return TRUE;

	eh = (hci_event_hdr *) ptr;
	if (eh->evt != EVT_STACK_INTERNAL)
		return TRUE;

	ptr += HCI_EVENT_HDR_SIZE;

	si = (evt_stack_internal *) ptr;
	switch (si->type) {
	case EVT_SI_DEVICE:
		device_event(chan, si);
		break;
	}

	return TRUE;
}

extern int optind, opterr, optopt;
extern char *optarg;

int main(int argc, char *argv[], char *env[])
{
	struct sockaddr_hci addr;
	struct hci_filter flt;
	struct sigaction sa;
	GIOChannel *ctl_io;
	int opt, daemonize = 1, sdp = 0;

	/* Default HCId settings */
	hcid.auto_init   = 1;
	hcid.config_file = HCID_CONFIG_FILE;
	hcid.host_name   = get_host_name();
	hcid.security    = HCID_SEC_AUTO;
	hcid.pairing     = HCID_PAIRING_MULTI;

	strcpy((char *) hcid.pin_code, "BlueZ");
	hcid.pin_len = 5;

	init_defaults();

	while ((opt = getopt(argc, argv, "nsf:")) != EOF) {
		switch (opt) {
		case 'n':
			daemonize = 0;
			break;

		case 's':
			sdp = 1;
			break;

		case 'f':
			hcid.config_file = strdup(optarg);
			break;

		default:
			usage();
			exit(1);
		}
	}

	if (daemonize && daemon(0, 0)) {
		error("Can't daemonize: %s (%d)", strerror(errno), errno);
		exit(1);
	}

	umask(0077);

	init_title(argc, argv, env, "hcid: ");
	set_title("initializing");

	start_logging("hcid", "Bluetooth HCI daemon");

	memset(&sa, 0, sizeof(sa));
	sa.sa_flags = SA_NOCLDSTOP;
	sa.sa_handler = sig_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT,  &sa, NULL);
	sa.sa_handler = sig_hup;
	sigaction(SIGHUP, &sa, NULL);

	sa.sa_handler = SIG_IGN;
	sigaction(SIGCHLD, &sa, NULL);
	sigaction(SIGPIPE, &sa, NULL);

	enable_debug();

	/* Create and bind HCI socket */
	if ((hcid.sock = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI)) < 0) {
		error("Can't open HCI socket: %s (%d)",
							strerror(errno), errno);
		exit(1);
	}

	/* Set filter */
	hci_filter_clear(&flt);
	hci_filter_set_ptype(HCI_EVENT_PKT, &flt);
	hci_filter_set_event(EVT_STACK_INTERNAL, &flt);
	if (setsockopt(hcid.sock, SOL_HCI, HCI_FILTER, &flt, sizeof(flt)) < 0) {
		error("Can't set filter: %s (%d)",
							strerror(errno), errno);
		exit(1);
	}

	addr.hci_family = AF_BLUETOOTH;
	addr.hci_dev = HCI_DEV_NONE;
	if (bind(hcid.sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		error("Can't bind HCI socket: %s (%d)",
							strerror(errno), errno);
		exit(1);
	}

	if (read_config(hcid.config_file) < 0)
		error("Config load failed");

	init_devices();

#ifdef ENABLE_DBUS
	if (hcid_dbus_init() == FALSE) {
		error("Unable to get on D-Bus");
		exit(1);
	}
#endif

	init_security_data();

	/* Create event loop */
	event_loop = g_main_new(FALSE);

	/* Initialize already connected devices */
	init_all_devices(hcid.sock);

	set_title("processing events");

	ctl_io = g_io_channel_unix_new(hcid.sock);

	g_io_add_watch(ctl_io, G_IO_IN, io_stack_event, NULL);

	if (sdp)
		start_sdp_server();

	/* Start event processor */
	g_main_run(event_loop);

	g_main_unref(event_loop);

	if (sdp)
		stop_sdp_server();

	free_device_opts();

#ifdef ENABLE_DBUS
	hcid_dbus_exit();
#endif

	info("Exit");

	stop_logging();

	return 0;
}
