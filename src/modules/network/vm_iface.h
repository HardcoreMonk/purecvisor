
#pragma once
#include <glib.h>

GPtrArray *pcv_vm_iface_parse_domiflist(const gchar *out);
GPtrArray *pcv_vm_iface_list(const gchar *vm_name);
