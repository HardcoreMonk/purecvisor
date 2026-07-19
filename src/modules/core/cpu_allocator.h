
#ifndef PURECVISOR_CPU_ALLOCATOR_H
#define PURECVISOR_CPU_ALLOCATOR_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include <libvirt/libvirt.h>

G_BEGIN_DECLS

typedef struct _CpuAllocator CpuAllocator;

extern CpuAllocator *global_allocator;

CpuAllocator *cpu_allocator_new(void);

void cpu_allocator_free(CpuAllocator *alloc);

void cpu_allocator_add_core(CpuAllocator *alloc,
                             guint logical_id,
                             guint physical_id,
                             guint numa_node,
                             gboolean is_isolated);

gboolean cpu_allocator_allocate_exclusive(CpuAllocator *alloc,
                                          const gchar  *vm_id,
                                          guint         numa_node,
                                          guint         vcpu_count,
                                          GArray      **out_cpus,
                                          gint         *out_numa_node);

void cpu_allocator_free_vm_cores(CpuAllocator *alloc, const gchar *vm_id);

void cpu_allocator_dump(CpuAllocator *alloc);

void cpu_allocator_set_overcommit(gboolean allow);

void cpu_allocator_mark_used(CpuAllocator *alloc, guint logical_id, const gchar *vm_name);

void cpu_allocator_reconcile(CpuAllocator *alloc, virConnectPtr conn);

JsonObject *cpu_allocator_get_numa_info(CpuAllocator *alloc);

G_END_DECLS

#endif
