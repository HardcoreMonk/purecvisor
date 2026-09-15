#!/usr/bin/env python3
import os
from pathlib import Path
import re
import resource
import shlex
import subprocess
import tempfile
import unittest

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    m = re.search(r'(?m)^(?:static\s+)?[\w *]+\s+' + re.escape(name) + r'\([^;]*?\)\s*\{', source)
    if not m:
        raise ValueError(name)
    depth = 1
    for token in re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', source[m.end():]):
        depth += (token.group() == '{') - (token.group() == '}')
        if not depth:
            return source[m.start():m.end() + token.end()]
    raise ValueError(name)


PREAMBLE = r'''
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <lxc/lxccontainer.h>
typedef enum { PCV_LXC_STORAGE_ZFS, PCV_LXC_STORAGE_BTRFS } PcvLxcStorageKind;
typedef struct { PcvLxcStorageKind kind; gchar *dataset; } PcvLxcStorage;
typedef struct { gchar *name; gchar *snap; } LxcSnapData;
typedef struct { gchar *source; gchar *target; gchar *owner_sub; } CloneCtx;
typedef struct { gchar *name; gchar *image; guint memory_mb; guint vcpu_count; gchar *bridge; gchar *owner_sub; gint rootless; } LxcCreateData;
#define PCV_LXC_ZFS_BASE "changed/pool"
#define PCV_LXC_PATH fixture_path
#define PCV_LXC_DEFAULT_BRIDGE "pcv-test"
#define LXC_LOG_DOM "test"
#define PCV_LOG_WARN(domain, ...) ((void)0)
#define PCV_LOG_INFO(domain, ...) ((void)0)
static char *fixture_path;
static gboolean btrfs, running, locked, invalid, recover_fail, nested, record_fail, duplicate, native_fail;
static gboolean wrong_root, external_mount, external_fstab, owner_fail, cpu_fail;
static void _ensure_zfs_mounts(void) {}
static guint lock_count, command_count, btrfs_count, recovered;
static gchar *command;
static gboolean _lock_container_op(const gchar *name) { (void)name; if(locked)return FALSE; lock_count++; return TRUE; }
static void _unlock_container_op(const gchar *name) { (void)name; lock_count--; }
static gboolean fault(GError **error,const gchar *msg) { g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_FAILED,msg);return FALSE; }
static const gchar *pcv_config_get_string(const gchar *a,const gchar *b,const gchar *d) {(void)a;(void)b;return d;}
static gboolean pcv_lxc_storage_default(PcvLxcStorageKind *k,GError **e) {*k=btrfs?PCV_LXC_STORAGE_BTRFS:PCV_LXC_STORAGE_ZFS;return invalid?fault(e,"invalid backend"):TRUE;}
static gboolean _rootless_apply_config(struct lxc_container *c,gint a,gint b) {(void)c;(void)a;(void)b;return TRUE;}
static gint pcv_config_get_int(const gchar*a,const gchar*b,gint d) {(void)a;(void)b;return d;}
static gboolean pcv_lxc_stamp_owner(const gchar *n,const gchar *s) {(void)n;g_assert_cmpstr(s,==,"operator-a");g_assert_cmpuint(lock_count,>,0);return !owner_fail;}
static gboolean pcv_lxc_storage_resolve(const gchar *name,PcvLxcStorage *s,GError **e) {
    (void)name; if(invalid)return fault(e,"identity mismatch");
    s->kind=btrfs?PCV_LXC_STORAGE_BTRFS:PCV_LXC_STORAGE_ZFS;s->dataset=btrfs?NULL:g_strdup("actual/pool/source");return TRUE;
}
static void pcv_lxc_storage_clear(PcvLxcStorage *s) {g_clear_pointer(&s->dataset,g_free);}
static gboolean pcv_lxc_storage_recover(const gchar *n,GError **e) {(void)n;recovered++;return recover_fail?fault(e,"ambiguous recovery"):TRUE;}
static gboolean pcv_lxc_storage_prepare_create(const gchar *n,PcvLxcStorageKind k,GError **e) {(void)n;(void)k;return duplicate?fault(e,"target exists"):TRUE;}
static gboolean pcv_lxc_storage_record(const gchar *n,PcvLxcStorageKind k,const gchar *d,GError **e) {(void)n;(void)k;(void)d;return record_fail?fault(e,"record failed"):TRUE;}
static gboolean pcv_lxc_storage_btrfs_validate_copy(const gchar *n,GError **e) {(void)n;return nested?fault(e,"nested subvolume"):TRUE;}
static gboolean pcv_lxc_storage_btrfs_snapshot_create(const gchar *n,const gchar *s,GError **e) {(void)n;(void)s;btrfs_count++;return nested?fault(e,"nested subvolume"):TRUE;}
static gboolean pcv_lxc_storage_btrfs_snapshot_rollback(const gchar *n,const gchar *s,GError **e) {(void)n;(void)s;btrfs_count++;return nested?fault(e,"nested subvolume"):TRUE;}
static gboolean pcv_lxc_storage_btrfs_snapshot_delete(const gchar *n,const gchar *s,GError **e) {(void)n;(void)s;(void)e;btrfs_count++;return TRUE;}
static GPtrArray *pcv_lxc_storage_btrfs_snapshot_list(const gchar *n,GError **e) {(void)n;(void)e;btrfs_count++;GPtrArray*a=g_ptr_array_new_with_free_func(g_free);g_ptr_array_add(a,g_strdup("baseline"));return a;}
static bool fixture_defined(struct lxc_container *c) {(void)c;return TRUE;}
static bool fixture_running(struct lxc_container *c) {(void)c;return running;}
static bool fixture_set(struct lxc_container*c,const char*k,const char*v) {(void)c;(void)v;return strstr(k,"cpu.")?!cpu_fail:TRUE;}
static bool fixture_clear(struct lxc_container*c,const char*k) {(void)c;(void)k;return TRUE;}
static bool fixture_save(struct lxc_container*c,const char*p) {(void)c;(void)p;return TRUE;}
static int fixture_get(struct lxc_container*c,const char*k,char*v,int n) {
    (void)c;gchar *value=NULL;
    if(g_str_equal(k,"lxc.rootfs.path"))value=wrong_root?g_strdup("/foreign/rootfs"):g_build_filename(fixture_path,"source","rootfs",NULL);
    else if(g_str_equal(k,"lxc.mount.fstab"))value=g_strdup(external_fstab?"/tmp/guest-fstab":"");
    else if(g_str_equal(k,"lxc.mount.entry"))value=g_strdup(external_mount?"/data host none bind 0 0":"/sys/fs/fuse/connections sys/fs/fuse/connections none bind,optional 0 0");
    else value=g_strdup("");
    int len=(int)strlen(value);if(v&&n>0)g_strlcpy(v,value,n);g_free(value);return len;
}
static struct lxc_container fake;
struct lxc_container *lxc_container_new(const char*n,const char*p) {(void)n;(void)p;fake.is_running=fixture_running;fake.is_defined=fixture_defined;fake.set_config_item=fixture_set;fake.clear_config_item=fixture_clear;fake.save_config=fixture_save;fake.get_config_item=fixture_get;return &fake;}
int lxc_container_put(struct lxc_container*c) {(void)c;return 0;}
static struct lxc_container *_lxc_get(const gchar*n,GError**e) {(void)e;return lxc_container_new(n,PCV_LXC_PATH);}
static gboolean _run_argv(const gchar *const*argv,GError **e) {(void)e;command_count++;g_free(command);command=g_strjoinv(" ",(gchar**)argv);if(g_str_equal(argv[0],"lxc-copy")){gchar*p=g_build_filename(fixture_path,"target",NULL);g_mkdir(p,0700);g_free(p);}if(native_fail)return fault(e,"native create failed");return TRUE;}
static gchar *_run_argv_capture(const gchar *const*argv,GError **e) {_run_argv(argv,e);return g_strdup("actual/pool/source@baseline\n");}
static gboolean pcv_spawn_sync(const gchar *const*argv,gchar**out,gchar**err,GError**e) {(void)out;(void)err;return _run_argv(argv,e);}
'''

MAIN = r'''
int main(int argc,char**argv) {
    g_assert_cmpint(argc,==,3);fixture_path=argv[2];const char *scenario=argv[1];
    btrfs=strstr(scenario,"zfs-")==NULL;
    running=strstr(scenario,"running")!=NULL;locked=strstr(scenario,"busy")!=NULL;
    invalid=strstr(scenario,"invalid")!=NULL;recover_fail=strstr(scenario,"recovery")!=NULL;
    nested=strstr(scenario,"nested")!=NULL;record_fail=strstr(scenario,"record-fail")!=NULL;
    duplicate=strstr(scenario,"duplicate")!=NULL;native_fail=strstr(scenario,"native-fail")!=NULL;
    cpu_fail=strstr(scenario,"cpu-fail")!=NULL;owner_fail=strstr(scenario,"owner-fail")!=NULL;external_fstab=strstr(scenario,"external-fstab")!=NULL;wrong_root=strstr(scenario,"wrong-root")!=NULL;external_mount=strstr(scenario,"external-mount")!=NULL;
    gboolean container=g_str_has_prefix(scenario,"container-");gboolean rootless=strstr(scenario,"rootless")!=NULL;
    GTask *task=g_task_new(NULL,NULL,NULL,NULL);LxcSnapData data={"source","baseline"};
    if(container) {LxcCreateData d={"source","ubuntu:24.04",512,1,"pcv-test","operator-a",rootless?1:0};_lxc_create_thread(task,NULL,&d,NULL);}
    else if(strstr(scenario,"clone")) {CloneCtx ctx={"source","target","operator-a"};_clone_worker(task,NULL,&ctx,NULL);}
    else if(strstr(scenario,"rollback")) _snap_rollback_thread(task,NULL,&data,NULL);
    else if(strstr(scenario,"delete")) _snap_delete_thread(task,NULL,&data,NULL);
    else if(strstr(scenario,"list")) _snap_list_thread(task,NULL,"source",NULL);
    else _snap_create_thread(task,NULL,&data,NULL);
    GError *error=NULL;
    gboolean expect_fail=running||locked||invalid||recover_fail||nested||record_fail||duplicate||native_fail||wrong_root||external_mount||external_fstab||owner_fail||cpu_fail||(btrfs&&rootless);
    if(strstr(scenario,"list")) {GPtrArray *a=g_task_propagate_pointer(task,&error);g_assert_nonnull(a);g_assert_cmpstr(g_ptr_array_index(a,0),==,"baseline");g_ptr_array_unref(a);}
    else g_assert_cmpint(g_task_propagate_boolean(task,&error),==,!expect_fail);
    if(expect_fail) {g_assert_nonnull(error);if(!nested&&!record_fail&&!native_fail&&!owner_fail&&!cpu_fail)g_assert_cmpuint(command_count,==,0);if(native_fail&&btrfs)g_assert_cmpuint(command_count,==,1);}
    else if(container) {g_assert_no_error(error);g_assert_nonnull(command);if(btrfs){g_assert_nonnull(strstr(command,"-B btrfs"));g_assert_cmpuint(command_count,==,1);}}
    else {g_assert_no_error(error);if(btrfs) {if(strstr(scenario,"clone")) {g_assert_nonnull(strstr(command,"-B btrfs"));g_assert_nonnull(strstr(command,"-s"));}else {g_assert_cmpuint(btrfs_count,==,1);g_assert_cmpuint(command_count,==,0);}}else {g_assert_nonnull(strstr(command,"actual/pool/source"));g_assert_null(strstr(command,"changed/pool"));}}
    g_assert_cmpuint(lock_count,==,0);g_clear_error(&error);g_object_unref(task);g_free(command);return 0;
}
'''


class DriverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='pcv-lxc-driver-')
        cls.exe = Path(cls.tmp.name) / 'test'
        source = (ROOT / 'src/modules/lxc/lxc_driver.c').read_text()
        names = ['_container_config_value', '_btrfs_config_ready', '_container_stopped', '_storage_prepare_operation', '_snap_storage_begin', '_snap_storage_end', '_snap_create_thread', '_snap_rollback_thread', '_snap_delete_thread', '_snap_list_thread', '_clone_worker', '_configure_create_cpu', '_lxc_create_locked', '_lxc_create_thread']
        bodies = []
        for name in names:
            if re.search(r'(?m)^'+re.escape(name)+r'\(',source):
                bodies.append(function(source,name))
        unit = Path(cls.tmp.name) / 'test.c'
        unit.write_text(PREAMBLE+'\n'+'\n'.join(bodies)+'\n'+MAIN)
        flags = shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','gio-2.0','glib-2.0','lxc'],text=True))
        subprocess.run([os.environ.get('CC','gcc'),'-std=gnu23','-Werror=implicit-function-declaration','-Wno-unused-function',str(unit),'-o',str(cls.exe),*flags],check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_backend_operations(self):
        for case in ['create','rollback','delete','list','clone','create-running','clone-running','create-busy','clone-busy','create-invalid','clone-invalid','create-recovery','clone-nested','clone-record-fail','zfs-create','zfs-rollback','zfs-delete','zfs-list','container-create','container-rootless','container-busy','container-invalid','container-duplicate','container-native-fail','container-record-fail','container-zfs-create','create-wrong-root','clone-external-mount','create-external-fstab','container-owner-fail','clone-owner-fail','container-cpu-fail']:
            with self.subTest(case=case), tempfile.TemporaryDirectory(prefix='pcv-lxc-case-') as path:
                Path(path,'source').mkdir()
                Path(path,'source','purecvisor.meta').write_text('ubuntu:noble')
                completed=subprocess.run([str(self.exe),case,path],text=True,capture_output=True)
                self.assertEqual(completed.returncode,0,completed.stdout+completed.stderr)


class SurfaceTests(unittest.TestCase):
    def test_cpu_config_supports_cgroup_v2_and_rejects_total_failure(self):
        source=(ROOT/'src/modules/lxc/lxc_driver.c').read_text()
        c=r"""
#include <gio/gio.h>
#include <lxc/lxccontainer.h>
static gboolean v1,v2;
static gchar *shares,*weight;
static bool set(struct lxc_container *c,const char *key,const char *value){(void)c;if(g_str_equal(key,"lxc.cgroup.cpu.shares")){g_free(shares);shares=g_strdup(value);return v1;}g_assert_cmpstr(key,==,"lxc.cgroup2.cpu.weight");g_free(weight);weight=g_strdup(value);return v2;}
"""+function(source,'_configure_create_cpu')+r"""
int main(void){struct lxc_container c={0};c.set_config_item=set;v2=TRUE;g_assert_true(_configure_create_cpu(&c,1));g_assert_cmpstr(shares,==,"1024");g_assert_cmpstr(weight,==,"100");g_assert_true(_configure_create_cpu(&c,2));g_assert_cmpstr(shares,==,"2048");g_assert_cmpstr(weight,==,"200");g_assert_true(_configure_create_cpu(&c,G_MAXUINT));g_assert_cmpstr(shares,==,"262144");g_assert_cmpstr(weight,==,"10000");v2=FALSE;v1=TRUE;g_assert_true(_configure_create_cpu(&c,0));g_assert_cmpstr(shares,==,"1024");g_assert_cmpstr(weight,==,"100");v1=FALSE;g_assert_false(_configure_create_cpu(&c,1));g_free(shares);g_free(weight);return 0;}
"""
        with tempfile.TemporaryDirectory(prefix='pcv-cpu-version-') as tmp:
            unit=Path(tmp,'test.c');unit.write_text(c);exe=Path(tmp,'test')
            flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','gio-2.0','lxc'],text=True))
            subprocess.run(['gcc','-std=gnu23','-Wall','-Wextra','-Werror',str(unit),'-o',str(exe),*flags],check=True)
            subprocess.run([str(exe)],check=True)

    def test_health_restart_defers_to_storage_worker_lock(self):
        source=(ROOT/'src/modules/lxc/lxc_driver.c').read_text()
        handlers=(ROOT/'src/modules/dispatcher/handler_container.c').read_text()
        start=handlers.rfind('typedef struct {',0,handlers.index('} ContainerHealthProbe;'))
        end=handlers.index('} ContainerHealthProbe;')+len('} ContainerHealthProbe;')
        c=PREAMBLE+handlers[start:end]+r"""
static ContainerHealthProbe g_health_probes[32];
static gint g_n_health_probes=1;
static GMutex g_health_mu;
static guint start_calls;
static gboolean start_success;
typedef struct { gchar *name; gboolean force; } LxcStopData;
"""+function(source,'_lxc_stop_thread')+function(source,'pcv_lxc_stop_finish')+r"""
static gboolean pcv_lxc_start_finish(GAsyncResult *r,GError **e){return g_task_propagate_boolean(G_TASK(r),e);}
static void pcv_lxc_stop_async(const gchar *name,gboolean force,GCancellable *cancel,GAsyncReadyCallback callback,gpointer data){GTask *task=g_task_new(NULL,cancel,callback,data);LxcStopData d={(gchar*)name,force};_lxc_stop_thread(task,NULL,&d,cancel);g_object_unref(task);}
static void pcv_lxc_start_async(const gchar *name,GCancellable *cancel,GAsyncReadyCallback callback,gpointer data){(void)name;start_calls++;GTask *task=g_task_new(NULL,cancel,callback,data);if(start_success)g_task_return_boolean(task,TRUE);else g_task_return_new_error(task,G_IO_ERROR,G_IO_ERROR_FAILED,"identity/recovery rejected");g_object_unref(task);}
"""
        for name in ['_health_find','_health_restart_finished','_health_restart_started','_health_restart_stopped','_health_check_tick']:
            c+=function(handlers,name)+'\n'
        allocation=re.search(r'idx = g_n_health_probes\+\+;[^}]+',function(handlers,'handle_container_health_set')).group()
        c+='\nstatic void allocate_reused_slot(void){gint idx;'+allocation+'(void)idx;}\n'
        c+=r"""
static void drain(void){while(g_main_context_iteration(NULL,FALSE));}
int main(void){ContainerHealthProbe *p=&g_health_probes[0];g_strlcpy(p->name,"source",sizeof(p->name));p->auto_restart=TRUE;p->failure_threshold=1;locked=TRUE;_health_check_tick(NULL);g_assert_true(p->restart_pending);drain();g_assert_false(p->restart_pending);g_assert_cmpuint(start_calls,==,0);g_assert_cmpint(p->restart_count,==,0);g_assert_cmpuint(command_count,==,1);locked=FALSE;_health_check_tick(NULL);drain();g_assert_cmpuint(start_calls,==,1);g_assert_cmpint(p->restart_count,==,0);g_assert_false(p->restart_pending);start_success=TRUE;_health_check_tick(NULL);drain();g_assert_cmpint(p->restart_count,==,1);g_assert_cmpint(p->consecutive_failures,==,0);g_assert_cmpuint(lock_count,==,0);p->restart_pending=TRUE;g_n_health_probes=0;allocate_reused_slot();g_strlcpy(p->name,"replacement",sizeof(p->name));_health_restart_finished("source",FALSE);g_assert_false(p->restart_pending);g_assert_cmpint(p->restart_count,==,0);g_free(command);return 0;}
"""
        with tempfile.TemporaryDirectory(prefix='pcv-health-lock-') as tmp:
            unit=Path(tmp,'test.c');unit.write_text(c);exe=Path(tmp,'test')
            flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','gio-2.0','lxc'],text=True))
            subprocess.run(['gcc','-std=gnu23','-Wall','-Wextra','-Werror','-Wno-unused-function',str(unit),'-o',str(exe),*flags],check=True)
            subprocess.run([str(exe)],check=True)

    def test_config_mutations_share_worker_lock(self):
        source=(ROOT/'src/modules/lxc/lxc_driver.c').read_text()
        handlers=(ROOT/'src/modules/dispatcher/handler_container.c').read_text()
        for name in ['handle_container_volume_attach','handle_container_volume_detach','handle_container_env_set','handle_container_env_delete']:
            body=function(handlers,name)
            self.assertIn('g_autoptr(PcvLxcOperationGuard) guard = _container_config_guard(',body)
            self.assertLess(body.index('_container_config_guard('),body.index('_ensure_container_config_ready('))
            if 'pcv_lxc_get_state(' in body:
                self.assertLess(body.index('_container_config_guard('),body.index('pcv_lxc_get_state('))
        for name in ['pcv_lxc_nic_attach','pcv_lxc_nic_detach','pcv_lxc_set_resource_limits','pcv_lxc_set_seccomp_profile']:
            self.assertIn('g_autoptr(PcvLxcOperationGuard) guard = pcv_lxc_operation_guard_acquire(',function(source,name))
        c=r"""
#include <gio/gio.h>
static GHashTable *g_ctr_locks;
static GMutex g_ctr_lock_mu;
typedef struct _PcvLxcOperationGuard { gchar *name; } PcvLxcOperationGuard;
"""
        for name in ['_lock_container_op','_unlock_container_op','pcv_lxc_operation_guard_acquire','pcv_lxc_operation_guard_free']:
            c+=function(source,name)+'\n'
        c+=r"""
G_DEFINE_AUTOPTR_CLEANUP_FUNC(PcvLxcOperationGuard,pcv_lxc_operation_guard_free)
static gpointer mutate_while_busy(gpointer data){(void)data;GError *error=NULL;g_autoptr(PcvLxcOperationGuard) same=pcv_lxc_operation_guard_acquire("source",&error);g_assert_null(same);g_assert_error(error,G_IO_ERROR,G_IO_ERROR_BUSY);g_clear_error(&error);g_autoptr(PcvLxcOperationGuard) other=pcv_lxc_operation_guard_acquire("unrelated",&error);g_assert_nonnull(other);g_assert_no_error(error);return NULL;}
static gboolean early_return(void){g_autoptr(PcvLxcOperationGuard) guard=pcv_lxc_operation_guard_acquire("source",NULL);g_assert_nonnull(guard);g_assert_false(_lock_container_op("source"));return FALSE;}
int main(void){g_assert_true(_lock_container_op("source"));GThread *thread=g_thread_new("config-mutator",mutate_while_busy,NULL);g_thread_join(thread);_unlock_container_op("source");g_assert_false(early_return());g_assert_true(_lock_container_op("source"));_unlock_container_op("source");g_assert_true(_lock_container_op("unrelated"));_unlock_container_op("unrelated");g_hash_table_destroy(g_ctr_locks);return 0;}
"""
        with tempfile.TemporaryDirectory(prefix='pcv-config-lock-') as tmp:
            unit=Path(tmp,'test.c');unit.write_text(c);exe=Path(tmp,'test')
            flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','gio-2.0'],text=True))
            subprocess.run(['gcc','-std=gnu23','-Wall','-Wextra','-Werror',str(unit),'-o',str(exe),*flags],check=True)
            subprocess.run([str(exe)],check=True)

    def test_exact_zfs_mount_layouts(self):
        source = (ROOT / 'src/modules/lxc/lxc_driver.c').read_text()
        c = r"""
#include <gio/gio.h>
#include <string.h>
#define PCV_LXC_PATH "/managed/lxc"
static guint mounts;
static gchar *fixture_find(const gchar *name) {(void)name;return g_strdup("/usr/sbin/zfs");}
#define g_find_program_in_path fixture_find
static gboolean pcv_lxc_storage_zfs_mount_allowed(const gchar *name,const gchar *dataset,gboolean rootfs) {(void)dataset;(void)rootfs;return !g_str_equal(name,"blocked");}
static gboolean pcv_validate_vm_name(const gchar *name) {if(!*name)return FALSE;for(;*name;name++)if(!g_ascii_isalnum(*name)&&*name!='_'&&*name!='-')return FALSE;return TRUE;}
static gchar *_run_argv_capture(const gchar *const *args,GError **e) {(void)args;(void)e;return g_strdup("pool/a\t/managed/lxc/a\tno\npool/b\t/managed/lxc/b/rootfs\tno\npool/c\t/managed/lxc/c/rootfs/foreign\tno\npool/d\t/managed/lxcx/d\tno\npool/e\t/managed/lxc/e\tyes\npool/blocked\t/managed/lxc/blocked/rootfs\tno\n");}
static gboolean _run_argv(const gchar *const *args,GError **e) {(void)e;g_assert_cmpstr(args[1],==,"mount");g_assert_true(g_str_equal(args[2],"pool/a")||g_str_equal(args[2],"pool/b"));mounts++;return TRUE;}
"""
        c += function(source, '_ensure_zfs_mounts')
        c += '\nint main(void){_ensure_zfs_mounts();g_assert_cmpuint(mounts,==,2);return 0;}\n'
        with tempfile.TemporaryDirectory(prefix='pcv-zfs-mount-') as tmp:
            unit=Path(tmp,'test.c');unit.write_text(c);exe=Path(tmp,'test')
            flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','gio-2.0'],text=True))
            subprocess.run(['gcc','-std=gnu23','-Wall','-Wextra','-Werror',str(unit),'-o',str(exe),*flags],check=True)
            subprocess.run([str(exe)],check=True)

    def test_config_file_symlink_and_permissions(self):
        source=(ROOT/'src/modules/lxc/lxc_driver.c').read_text()
        c=r"""
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>
#define PCV_LXC_PATH base
static gchar *base;
static gboolean owner_bad;
static int fixture_lstat(const gchar *p,struct stat *s){int rc=lstat(p,s);if(!rc)s->st_uid=owner_bad?123:0;return rc;}
#define lstat fixture_lstat
"""+function(source,'_container_config_file_valid')+r"""
int main(int argc,char **argv){g_assert_cmpint(argc,==,2);base=argv[1];gchar*p=g_build_filename(base,"source",NULL);g_mkdir(p,0700);g_free(p);p=g_build_filename(base,"source","config",NULL);g_file_set_contents(p,"safe",-1,NULL);g_chmod(p,0600);GError *e=NULL;g_assert_true(_container_config_file_valid("source",&e));g_assert_no_error(e);g_chmod(p,0666);g_assert_false(_container_config_file_valid("source",&e));g_clear_error(&e);g_chmod(p,0600);owner_bad=TRUE;g_assert_false(_container_config_file_valid("source",&e));g_clear_error(&e);owner_bad=FALSE;g_unlink(p);g_assert_cmpint(symlink("/etc/passwd",p),==,0);g_assert_false(_container_config_file_valid("source",&e));g_clear_error(&e);g_unlink(p);g_free(p);return 0;}
"""
        with tempfile.TemporaryDirectory(prefix='pcv-config-path-') as tmp:
            unit=Path(tmp,'test.c');unit.write_text(c);exe=Path(tmp,'test')
            flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','gio-2.0'],text=True))
            subprocess.run(['gcc','-std=gnu23','-Wall','-Wextra','-Werror',str(unit),'-o',str(exe),*flags],check=True)
            subprocess.run([str(exe),tmp],check=True)

    def test_ui_waits_for_job_result(self):
        source=(ROOT/'ui/modules/container.js').read_text()
        start=source.index('async function ctrWaitJobResult(')
        end=source.index('\nasync function ctrSnapCreate(',start)
        body=source[start:end]
        js=r"""
const vm=require('vm'),assert=require('assert');
const source=BODY;
async function check(){let status='completed',calls=0;const context={unwrapData:r=>r.data,PCV:{api:{waitForJob:async()=>{calls++;if(status==='failed')throw new Error('worker failed');return {status};}}}};const wait=vm.runInNewContext('('+source+')',context);await wait({data:{job_id:'job-1'}});assert.equal(calls,1);status='failed';await assert.rejects(wait({data:{job_id:'job-2'}}),/worker failed/);status='running';await assert.rejects(wait({data:{job_id:'job-3'}}),/still running/);await assert.rejects(wait({error:{message:'request rejected'}}),/request rejected/);assert.equal(calls,3);}
check().catch(e=>{console.error(e);process.exitCode=1;});
"""
        import json
        subprocess.run(['node','-e',js.replace('BODY',json.dumps(body),1)],check=True)


if __name__ == '__main__':
    unittest.main()
