#!/usr/bin/env python3
                                                                 
                                                  
                                                
                                     
import os
import resource
from pathlib import Path
import shlex
import re
import sys
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class FlowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='pcv-vnc-flow-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary=Path(cls.temp.name)/'flow'
        flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','libsoup-3.0','gio-2.0','json-glib-1.0','libvirt','libxml-2.0'],text=True))
        source=os.environ.get('PCV_VNC_SOURCE','src/api/ws_server.c')
        cmd=['gcc-14','-std=gnu23','-D_GNU_SOURCE','-DPCV_CLUSTER_ENABLED=0','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-g','-O1','-ffunction-sections','-fdata-sections','-I.','-Isrc','-Isrc/api','-Iinclude','-Iinclude/purecvisor',f'-DPCV_VNC_SOURCE="{source}"','tests/fixtures/vnc_flow_control.c','-Wl,--gc-sections','-o',str(cls.binary),*flags,*shlex.split(os.environ.get('PCV_VNC_FLAGS',''))]
        cmd[cmd.index('-Wl,--gc-sections'):cmd.index('-Wl,--gc-sections')] = ['src/modules/dispatcher/rpc_utils.c', 'src/api/drain.c', 'src/modules/dispatcher/rpc_completion.c', 'src/api/rest_shutdown.c', 'src/utils/pcv_worker_pool.c']
        r=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True,timeout=90)
        if r.returncode:raise RuntimeError(r.stdout+r.stderr)
    def run_mode(self,mode):
        r=subprocess.run([*shlex.split(os.environ.get('PCV_VNC_PREFIX','')),str(self.binary),mode],cwd=ROOT,capture_output=True,text=True,timeout=30)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertIn('PASS '+mode,r.stdout)
        print(r.stdout.strip(),flush=True)
        if os.environ.get('PCV_VNC_PREFIX'):print(r.stderr,flush=True)
    def test_no_tcp_before_authentication(self):self.run_mode('unauthenticated')
    def test_invalid_authentication_no_tcp(self):self.run_mode('auth-rejected')
    def test_viewer_no_tcp(self):self.run_mode('viewer-rejected')
    def test_admin_binary_bytes(self):self.run_mode('binary')
    def test_operator_owned_domain(self):self.run_mode('operator')
    def test_read_paused_until_current_ack(self):self.run_mode('checkpoint-pause')
    def test_wrong_ack_does_not_restore_credit(self):self.run_mode('wrong-ack')
    def test_ack_deadline_closes_stalled_peer(self):self.run_mode('ack-timeout')
    def test_frame_limit_closes_without_tcp_delivery(self):self.run_mode('oversized-frame')
    def test_xml_owner_namespace_and_listen(self):self.run_mode('owner-projection')
    def test_session_limit_covers_closing_objects(self):self.run_mode('session-cap')
    def test_async_auth_cannot_reopen_closed_proxy(self):self.run_mode('close-during-auth')
    def test_rest_shutdown_closes_active_transport(self):self.run_mode('shutdown-active')
    def test_shutdown_not_starved_by_default_ready_source(self):self.run_mode('shutdown-busy')
    def test_rest_shutdown_drains_pending_auth(self):self.run_mode('shutdown-auth')
                                                                                            
    def test_delete_closes_subject_vnc_on_owner(self):self.run_mode('subject-close')
    def test_auth_done_rechecks_subject(self):self.run_mode('subject-auth-race')
    def test_delete_closes_subject_events_ws(self):self.run_mode('events-subject')
                                                                             
    def test_worker_broadcast_stays_on_owner(self):self.run_mode('broadcast-affinity')
    def test_worker_broadcast_queue_is_bounded(self):self.run_mode('broadcast-flood')
    def test_queued_broadcast_released_after_shutdown(self):self.run_mode('broadcast-shutdown')
    def test_slow_tcp_queue_cap(self):self.run_mode('queue-cap')
    def test_nonblocking_partial_writes_preserve_bytes(self):self.run_mode('partial-write')

def _no_core_dump():
                                                                                     
                                                                                   
                                                                                             
    try: resource.setrlimit(resource.RLIMIT_CORE,(1,1))
    except (ValueError,OSError): resource.setrlimit(resource.RLIMIT_CORE,(0,0))

def mutations():
                                                         
    source=(ROOT/'src/api/ws_server.c').read_text().replace('#include "vnc_proxy.h"',(ROOT/'src/api/vnc_proxy.h').read_text())
    variants={
      'accept-wrong-ack':('gboolean match=!g_strcmp0(nonce,vp->nonce) && !g_strcmp0(seq,expected) && json_object_get_size(object)==3;', 'gboolean match=TRUE; (void)nonce; (void)seq;', 'wrong-ack'),
      'ignore-owner':('if (!name || !subject || strcmp((char *)name, subject)) port = 0;', '(void)subject;', 'owner-projection'),
      'ignore-queue-cap':('length > VNC_QUEUE_CAP - vp->pending->len', 'FALSE', 'queue-cap'),
      'blocking-socket':('SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);', 'SOCK_STREAM | SOCK_CLOEXEC, 0);', 'partial-write'),
                                                                  
      'broadcast-inline':('if (!pcv_ws_dispatch_on_owner(owner, _ws_broadcast_fanout, msg, queued ? _ws_fanout_queued_free : g_free)) {', 'GDestroyNotify keep = _ws_fanout_queued_free; (void)keep; _ws_broadcast_fanout(msg); if (TRUE) {', 'broadcast-affinity'),
      'no-broadcast-queue-cap':('if (queued && !_ws_fanout_slot_take()) {', 'if (queued && !_ws_fanout_slot_take() && FALSE) {', 'broadcast-flood'),
    }
    with tempfile.TemporaryDirectory(prefix='pcv-vnc-flow-mutants-') as directory:
      for name,(old,new,mode) in variants.items():
        assert source.count(old)==1,(name,source.count(old))
        path=Path(directory)/(name+'.c');path.write_text(source.replace(old,new))
        prior=os.environ.get('PCV_VNC_SOURCE');os.environ['PCV_VNC_SOURCE']=str(path)
        try:
          FlowTests.setUpClass()
          result=subprocess.run([str(FlowTests.binary),mode],cwd=ROOT,capture_output=True,text=True,timeout=15,preexec_fn=_no_core_dump)
          assert result.returncode!=0 and re.search(r'assertion failed|ERROR:tests/fixtures/vnc_flow_control.c:.*should be',result.stderr),(name,result.stderr)
          print('REJECT '+name+': '+next(line for line in result.stderr.splitlines() if 'ERROR:tests/fixtures/vnc_flow_control.c:' in line),flush=True)
        finally:
          FlowTests.temp.cleanup()
          if prior is None:os.environ.pop('PCV_VNC_SOURCE',None)
          else:os.environ['PCV_VNC_SOURCE']=prior

if __name__=='__main__':
    if sys.argv[1:]==['--mutations']:mutations()
    else:unittest.main(verbosity=2)
