/*
 * OpenConnect (SSL + DTLS) VPN client
 *
 * Copyright © 2008-2014 Intel Corporation.
 *
 * Author: David Woodhouse <dwmw2@infradead.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include <errno.h>
#include <stdio.h>

#include <openconnect-internal.h>

/*
 * TAP-Windows support inspired by http://i3.cs.berkeley.edu/ with
 * permission.
 */
#define _TAP_IOCTL(nr) CTL_CODE(FILE_DEVICE_UNKNOWN, nr, METHOD_BUFFERED, \
				FILE_ANY_ACCESS)

#define TAP_IOCTL_GET_MAC               _TAP_IOCTL(1)
#define TAP_IOCTL_GET_VERSION           _TAP_IOCTL(2)
#define TAP_IOCTL_GET_MTU               _TAP_IOCTL(3)
#define TAP_IOCTL_GET_INFO              _TAP_IOCTL(4)
#define TAP_IOCTL_CONFIG_POINT_TO_POINT _TAP_IOCTL(5)
#define TAP_IOCTL_SET_MEDIA_STATUS      _TAP_IOCTL(6)
#define TAP_IOCTL_CONFIG_DHCP_MASQ      _TAP_IOCTL(7)
#define TAP_IOCTL_GET_LOG_LINE          _TAP_IOCTL(8)
#define TAP_IOCTL_CONFIG_DHCP_SET_OPT   _TAP_IOCTL(9)
#define TAP_IOCTL_CONFIG_TUN            _TAP_IOCTL(10)

#define TAP_COMPONENT_ID "tap0901"

#define DEVTEMPLATE "\\\\.\\Global\\%s.tap"

#define NETDEV_GUID "{4D36E972-E325-11CE-BFC1-08002BE10318}"
#define CONTROL_KEY "SYSTEM\\CurrentControlSet\\Control\\"

#define ADAPTERS_KEY CONTROL_KEY "Class\\" NETDEV_GUID
#define CONNECTIONS_KEY CONTROL_KEY "Network\\" NETDEV_GUID

typedef int (tap_callback)(struct openconnect_info *vpninfo, char *idx, char *name);

static int search_taps(struct openconnect_info *vpninfo, tap_callback *cb)
{
	LONG status;
	HKEY adapters_key;
	DWORD len;
	char buf[40], name[40];
	char keyname[strlen(CONNECTIONS_KEY) + sizeof(buf) + 1 + strlen("\\Connection")];
	int i = 0, ret = 0;

	status = RegOpenKeyEx(HKEY_LOCAL_MACHINE, ADAPTERS_KEY, 0,
			      KEY_READ, &adapters_key);
	if (status) {
		fprintf(stderr, "Error accessing registry\n");
		return -EIO;
	}
	while (!ret) {
		len = sizeof(buf);
		status = RegEnumKeyEx(adapters_key, i++, buf, &len,
				       NULL, NULL, NULL, NULL);
		if (status) {
			if (status != ERROR_NO_MORE_ITEMS)
				ret = -EIO;
			break;
		}

		snprintf(keyname, sizeof(keyname), "%s\\%s",
			 ADAPTERS_KEY, buf);

		len = sizeof(buf);
		status = RegGetValue(HKEY_LOCAL_MACHINE, keyname,
				     "ComponentId", RRF_RT_REG_SZ,
				     NULL, buf, &len);
		if (status || strcmp(buf, TAP_COMPONENT_ID))
			continue;

		len = sizeof(buf);
		status = RegGetValue(HKEY_LOCAL_MACHINE, keyname,
				     "NetCfgInstanceId", RRF_RT_REG_SZ,
				     NULL, buf, &len);
		if (status)
			continue;

		snprintf(keyname, sizeof(keyname), "%s\\%s\\Connection",
			 CONNECTIONS_KEY, buf);

		len = sizeof(name);
		status = RegGetValue(HKEY_LOCAL_MACHINE, keyname, "Name",
				     RRF_RT_REG_SZ, NULL, name, &len);
		if (status)
			continue;

		ret = cb(vpninfo, buf, name);
	}

	RegCloseKey(adapters_key);
	return ret;
}

static int open_tun(struct openconnect_info *vpninfo, char *guid, char *name)
{
	char devname[80];
	HANDLE tun_fh;
	ULONG data[3];
	DWORD len;

	if (vpninfo->ifname && strcmp(name, vpninfo->ifname))
		return 0;

	snprintf(devname, sizeof(devname), DEVTEMPLATE, guid);
	tun_fh = CreateFile(devname, GENERIC_WRITE|GENERIC_READ, 0, 0,
			    OPEN_EXISTING, FILE_ATTRIBUTE_SYSTEM, 0);
	if (tun_fh == INVALID_HANDLE_VALUE) {
		vpn_progress(vpninfo, PRG_ERR, _("Failed to open %s\n"),
			     devname);
		return -1;

	}
	vpn_progress(vpninfo, PRG_TRACE, _("Opened tun device %s\n"), devname);

	if (!DeviceIoControl(tun_fh, TAP_IOCTL_GET_VERSION,
			     NULL, 0, data, sizeof(data), &len, NULL)) {
		DWORD err = GetLastError();

		vpn_progress(vpninfo, PRG_ERR,
			     _("Failed to obtain TAP driver version: %lx\n"), err);
		return -1;
	}
	if (data[0] < 9 || (data[0] == 9 && data[1] < 9)) {
		vpn_progress(vpninfo, PRG_ERR,
			     _("Error: TAP-Windows driver v9.9 or greater is required (found %ld.%ld)\n"),
			     data[0], data[1]);
		return -1;
	}
	vpn_progress(vpninfo, PRG_TRACE, "TAP-Windows driver v%ld.%ld (%ld)\n",
		     data[0], data[1], data[2]);

	data[0] = inet_addr(vpninfo->ip_info.addr);
	data[2] = inet_addr(vpninfo->ip_info.netmask);
	data[1] = data[0] & data[2];

	if (!DeviceIoControl(tun_fh, TAP_IOCTL_CONFIG_TUN,
			     data, sizeof(data), NULL, 0, &len, NULL)) {
		DWORD err = GetLastError();

		vpn_progress(vpninfo, PRG_ERR,
			     _("Failed to set TAP IP addresses: %lx\n"), err);
		return -1;
	}

	data[0] = 1;
	if (!DeviceIoControl(tun_fh, TAP_IOCTL_SET_MEDIA_STATUS,
			     data, sizeof(data[0]), NULL, 0, &len, NULL)) {
		DWORD err = GetLastError();

		vpn_progress(vpninfo, PRG_ERR,
			     _("Failed to set TAP media status: %lx\n"), err);
		return -1;
	}
	if (!vpninfo->ifname)
		vpninfo->ifname = strdup(name);

	vpninfo->tun_fh = tun_fh;
	return 1;
}

int win32_setup_tun(struct openconnect_info *vpninfo)
{
	if (search_taps(vpninfo, open_tun) != 1)
		return -1;

	return 0;
}