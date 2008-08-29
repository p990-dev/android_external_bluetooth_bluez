/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2007  Nokia Corporation
 *  Copyright (C) 2004-2008  Marcel Holtmann <marcel@holtmann.org>
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

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include <glib.h>

#include <dbus/dbus.h>

#include <gdbus.h>

#include "logging.h"
#include "adapter.h"
#include "error.h"
#include "manager.h"

static DBusConnection *connection = NULL;
static int default_adapter_id = -1;
static GSList *adapters = NULL;

int manager_update_adapter(uint16_t dev_id)
{
	struct adapter *adapter;

	adapter = manager_find_adapter_by_id(dev_id);
	if (!adapter)
		return -EINVAL;

	return adapter_update(adapter);
}

int manager_get_adapter_class(uint16_t dev_id, uint8_t *cls)
{
	struct adapter *adapter;

	adapter = manager_find_adapter_by_id(dev_id);
	if (!adapter)
		return -EINVAL;

	return adapter_get_class(adapter, cls);
}

int manager_set_adapter_class(uint16_t dev_id, uint8_t *cls)
{
	struct adapter *adapter;

	adapter = manager_find_adapter_by_id(dev_id);
	if (!adapter)
		return -EINVAL;

	return adapter_set_class(adapter, cls);
}

static inline DBusMessage *invalid_args(DBusMessage *msg)
{
	return g_dbus_create_error(msg,
			ERROR_INTERFACE ".InvalidArguments",
			"Invalid arguments in method call");
}

static inline DBusMessage *no_such_adapter(DBusMessage *msg)
{
	return g_dbus_create_error(msg,
			ERROR_INTERFACE ".NoSuchAdapter",
			"No such adapter");
}

static int find_by_address(const char *str)
{
	struct hci_dev_list_req *dl;
	struct hci_dev_req *dr;
	bdaddr_t ba;
	int i, sk;
	int devid = -1;

	sk = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	if (sk < 0)
		return -1;

	dl = g_malloc0(HCI_MAX_DEV * sizeof(*dr) + sizeof(*dl));

	dl->dev_num = HCI_MAX_DEV;
	dr = dl->dev_req;

	if (ioctl(sk, HCIGETDEVLIST, dl) < 0)
		goto out;

	dr = dl->dev_req;
	str2ba(str, &ba);

	for (i = 0; i < dl->dev_num; i++, dr++) {
		struct hci_dev_info di;

		if (hci_devinfo(dr->dev_id, &di) < 0)
			continue;

		if (hci_test_bit(HCI_RAW, &di.flags))
			continue;

		if (!bacmp(&ba, &di.bdaddr)) {
			devid = dr->dev_id;
			break;
		}
	}

out:
	g_free(dl);
	close(sk);
	return devid;
}

static DBusMessage *default_adapter(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	struct adapter *adapter;
	const gchar *path;

	adapter = manager_find_adapter_by_id(default_adapter_id);
	if (!adapter)
		return no_such_adapter(msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	path = adapter_get_path(adapter);

	dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *find_adapter(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	struct adapter *adapter;
	struct hci_dev_info di;
	const char *pattern;
	int dev_id;
	const gchar *path;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &pattern,
							DBUS_TYPE_INVALID))
		return NULL;

	/* hci_devid() would make sense to use here, except it
	   is restricted to devices which are up */
	if (!strncmp(pattern, "hci", 3) && strlen(pattern) >= 4)
		dev_id = atoi(pattern + 3);
	else
		dev_id = find_by_address(pattern);

	if (dev_id < 0)
		return no_such_adapter(msg);

	if (hci_devinfo(dev_id, &di) < 0)
		return no_such_adapter(msg);

	if (hci_test_bit(HCI_RAW, &di.flags))
		return no_such_adapter(msg);

	adapter = manager_find_adapter_by_id(dev_id);
	if (!adapter)
		return no_such_adapter(msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	path = adapter_get_path(adapter);

	dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *list_adapters(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBusMessageIter iter;
	DBusMessageIter array_iter;
	DBusMessage *reply;
	GSList *l;
	uint16_t dev_id;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
				DBUS_TYPE_OBJECT_PATH_AS_STRING, &array_iter);

	for (l = adapters; l; l = l->next) {
		struct adapter *adapter = l->data;
		struct hci_dev_info di;
		dev_id = adapter_get_dev_id(adapter);
		const gchar *path = adapter_get_path(adapter);

		if (hci_devinfo(dev_id, &di) < 0)
			continue;

		if (hci_test_bit(HCI_RAW, &di.flags))
			continue;

		dbus_message_iter_append_basic(&array_iter,
					DBUS_TYPE_OBJECT_PATH, &path);
	}

	dbus_message_iter_close_container(&iter, &array_iter);

	return reply;
}

static GDBusMethodTable manager_methods[] = {
	{ "DefaultAdapter",	"",	"o",	default_adapter	},
	{ "FindAdapter",	"s",	"o",	find_adapter	},
	{ "ListAdapters",	"",	"ao",	list_adapters	},
	{ }
};

static GDBusSignalTable manager_signals[] = {
	{ "AdapterAdded",		"o"	},
	{ "AdapterRemoved",		"o"	},
	{ "DefaultAdapterChanged",	"o"	},
	{ }
};

dbus_bool_t manager_init(DBusConnection *conn, const char *path)
{
	connection = conn;

	return g_dbus_register_interface(conn, "/", MANAGER_INTERFACE,
			manager_methods, manager_signals,
			NULL, NULL, NULL);
}

void manager_cleanup(DBusConnection *conn, const char *path)
{
	g_dbus_unregister_interface(conn, "/", MANAGER_INTERFACE);
}

static gint adapter_id_cmp(gconstpointer a, gconstpointer b)
{
	struct adapter *adapter = (struct adapter *) a;
	uint16_t id = GPOINTER_TO_UINT(b);
	uint16_t dev_id = adapter_get_dev_id(adapter);

	return dev_id == id ? 0 : -1;
}

static gint adapter_path_cmp(gconstpointer a, gconstpointer b)
{
	struct adapter *adapter = (struct adapter *) a;
	const char *path = b;
	const gchar *adapter_path = adapter_get_path(adapter);

	return strcmp(adapter_path, path);
}

static gint adapter_address_cmp(gconstpointer a, gconstpointer b)
{
	struct adapter *adapter = (struct adapter *) a;
	const char *address = b;
	const gchar *source = adapter_get_address(adapter);

	return strcmp(source, address);
}

struct adapter *manager_find_adapter(const bdaddr_t *sba)
{
	GSList *match;
	char address[18];

	ba2str(sba, address);

	match = g_slist_find_custom(adapters, address, adapter_address_cmp);
	if (!match)
		return NULL;

	return match->data;
}

struct adapter *manager_find_adapter_by_path(const char *path)
{
	GSList *match;

	match = g_slist_find_custom(adapters, path, adapter_path_cmp);
	if (!match)
		return NULL;

	return match->data;
}

struct adapter *manager_find_adapter_by_id(int id)
{
	GSList *match;

	match = g_slist_find_custom(adapters, GINT_TO_POINTER(id), adapter_id_cmp);
	if (!match)
		return NULL;

	return match->data;
}

static void manager_add_adapter(struct adapter *adapter)
{
	const gchar *path = adapter_get_path(adapter);

	g_dbus_emit_signal(connection, "/",
			MANAGER_INTERFACE, "AdapterAdded",
			DBUS_TYPE_OBJECT_PATH, &path,
			DBUS_TYPE_INVALID);

	adapters = g_slist_append(adapters, adapter);
}

static void manager_remove_adapter(struct adapter *adapter)
{
	uint16_t dev_id = adapter_get_dev_id(adapter);
	const gchar *path = adapter_get_path(adapter);

	g_dbus_emit_signal(connection, "/",
			MANAGER_INTERFACE, "AdapterRemoved",
			DBUS_TYPE_OBJECT_PATH, &path,
			DBUS_TYPE_INVALID);

	if ((default_adapter_id == dev_id || default_adapter_id < 0)) {
		int new_default = hci_get_route(NULL);

		if (new_default >= 0)
			manager_set_default_adapter(new_default);
	}

	adapters = g_slist_remove(adapters, adapter);
}

int manager_register_adapter(int id)
{
	struct adapter *adapter = adapter_create(connection, id);

	if (!adapter)
		return -1;

	manager_add_adapter(adapter);

	return 0;
}

int manager_unregister_adapter(int id)
{
	struct adapter *adapter;
	const gchar *path;

	adapter = manager_find_adapter_by_id(id);
	if (!adapter)
		return -1;

	path = adapter_get_path(adapter);

	info("Unregister path: %s", path);

	adapter_stop(adapter);

	manager_remove_adapter(adapter);

	adapter_remove(adapter);

	return 0;
}

int manager_start_adapter(int id)
{
	struct adapter* adapter;
	int ret;

	adapter = manager_find_adapter_by_id(id);
	if (!adapter) {
		error("Getting device data failed: hci%d", id);
		return -EINVAL;
	}

	ret = adapter_start(adapter);
	if (ret < 0)
		return ret;

	if (manager_get_default_adapter() < 0)
		manager_set_default_adapter(id);

	return 0;
}

int manager_stop_adapter(int id)
{
	struct adapter *adapter;

	adapter = manager_find_adapter_by_id(id);
	if (!adapter) {
		error("Getting device data failed: hci%d", id);
		return -EINVAL;
	}

	return adapter_stop(adapter);
}

int manager_get_default_adapter()
{
	return default_adapter_id;
}

void manager_set_default_adapter(int id)
{
	struct adapter *adapter = manager_find_adapter_by_id(id);
	const gchar *path = adapter_get_path(adapter);

	default_adapter_id = id;

	g_dbus_emit_signal(connection, "/",
			MANAGER_INTERFACE,
			"DefaultAdapterChanged",
			DBUS_TYPE_OBJECT_PATH, &path,
			DBUS_TYPE_INVALID);
}
