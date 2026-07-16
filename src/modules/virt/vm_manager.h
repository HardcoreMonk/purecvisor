/**
 * @file vm_manager.h
 * @brief VM 생명주기 관리자 — GObject 기반 비동기 VM CRUD 인터페이스
 *
 * == 아키텍처에서의 위치 ==
 *   디스패처(dispatcher.c) → handler_vm_*.c → vm_manager → libvirt API
 *
 *   PureCVisorVmManager는 GObject 타입으로, libvirt-gobject의 GVirConnection을
 *   래핑하여 VM의 생성/시작/중지/삭제/리소스 튜닝을 비동기(GTask)로 처리합니다.
 *
 * == 비동기 패턴 (GTask) ==
 *   모든 오래 걸리는 작업은 _async() / _finish() 쌍으로 제공됩니다.
 *   - _async(): GTask를 생성하고 워커 스레드에서 libvirt API를 호출
 *   - _finish(): 콜백에서 호출하여 결과를 꺼냄 (메인 스레드에서 실행)
 *
 * == GObject Signal (GIO P6) ==
 *   VM 상태 변화를 외부에 알리기 위해 3개의 시그널을 정의합니다:
 *   - "vm-started"          : VM 시작 성공 시
 *   - "vm-stopped"          : VM 정지 성공 시
 *   - "vm-metrics-updated"  : 텔레메트리 데몬이 메트릭 갱신 시
 *
 * == 주의사항 ==
 *   - 이 헤더의 PureCVisorVmConfig 와 vm_types.h의 동명 구조체는 별개입니다.
 *     vm_types.h는 간단한 DTO이고, vm_config_builder.h의 것은 불투명(opaque) 빌더입니다.
 *   - GVirConnection은 자체 도메인 캐시를 가집니다. virt_conn_pool의 virConnectPtr로
 *     define한 도메인은 GVirConnection에서 보이지 않을 수 있으므로, create/start/stop/delete는
 *     모두 raw libvirt API(virDomainLookupByName 등)를 사용합니다.
 *   - list_vms만 GVirConnection 경유로 도메인을 조회합니다 (VNC 포트 파싱 등).
 */
/* src/modules/virt/vm_manager.h */

#ifndef PURECVISOR_VM_MANAGER_H
#define PURECVISOR_VM_MANAGER_H

#include <glib-object.h>
#include <json-glib/json-glib.h>
#include <libvirt-gobject/libvirt-gobject.h>

G_BEGIN_DECLS

/* GObject 타입 매크로 — G_DEFINE_TYPE에 대응하는 타입 등록 */
#define PURECVISOR_TYPE_VM_MANAGER (purecvisor_vm_manager_get_type())

/* GObject 표준 타입 선언 매크로: 캐스팅, IS 체크, GET_CLASS 등 자동 생성 */
G_DECLARE_FINAL_TYPE(PureCVisorVmManager, purecvisor_vm_manager, PURECVISOR, VM_MANAGER, GObject)

/**
 * purecvisor_vm_manager_new:
 * @conn: libvirt-gobject 연결 객체 (list_vms에서 사용, ref 증가됨)
 *
 * VM 매니저 인스턴스를 생성합니다.
 * main.c에서 GVirConnection 초기화 후 1회 호출합니다.
 *
 * @return 새로 생성된 VM 매니저 (호출자가 g_object_unref로 해제)
 */
/* 생성자: Connection 인자 추가됨 */
PureCVisorVmManager *purecvisor_vm_manager_new(GVirConnection *conn);

/* =========================================================================
 * 비동기 VM 라이프사이클 API (Phase 5 Signature)
 *
 * 모든 _async 함수는 내부적으로:
 *   1. GTask + TaskData 구조체 생성
 *   2. g_task_run_in_thread()로 워커 스레드에 위임
 *   3. 워커 스레드에서 virt_conn_pool_acquire() → libvirt API → release()
 *   4. 완료 시 callback이 메인 스레드에서 호출됨
 *
 * _finish 함수는 callback 안에서 호출하여 결과/에러를 추출합니다.
 * ========================================================================= */

/* Async Method Definitions (Phase 5 Signature) */

/**
 * purecvisor_vm_manager_create_vm_async:
 * @self:           VM 매니저 인스턴스
 * @name:           VM 이름 (ZFS zvol 이름으로도 사용됨)
 * @vcpu:           가상 CPU 개수
 * @ram_mb:         메모리 크기 (MB)
 * @disk_size_gb:   디스크 크기 (GB, 0이면 기본 50GB)
 * @iso_path:       (nullable) 설치 ISO 경로
 * @network_bridge: (nullable) 브릿지 이름 (예: "pcvbr0", NULL이면 NIC 미추가)
 * @vlan_id:        VLAN ID (0이면 태깅 없음, 1~4094 범위)
 * @storage_pool:   (nullable) zvol 부모 데이터셋. NULL이면 daemon.conf 기본값
 * @image_dir:      (nullable) qcow2/raw 파일 디스크 저장 디렉터리. NULL이면 daemon.conf 기본값
 * @owner:          (nullable) 인증된 VM 생성자 username
 * @callback:       완료 콜백 (메인 스레드에서 호출됨)
 * @user_data:      콜백에 전달할 사용자 데이터
 *
 * 워커 스레드에서 수행하는 작업:
 *   1. ZFS zvol 생성 (zfs create -V <size>G pcvpool/vms/<name>)
 *   2. VM XML 빌드 (vm_config_builder + NIC XML 직접 패치)
 *   3. virDomainDefineXML로 libvirt에 등록
 *   4. etcd에 VM XML 동기화 (페일오버 대비)
 */
// Create: Config 객체 대신 개별 인자 사용
void purecvisor_vm_manager_create_vm_async(PureCVisorVmManager *self,
                                           const gchar *name,
                                           gint vcpu,
                                           gint ram_mb,
                                           gint disk_size_gb, // [Added]
                                           const gchar *iso_path,
                                           const gchar *network_bridge, // [Added]
                                           gint         vlan_id,        // [Sprint G]
                                           gint         boot_mode,      // 0=BIOS, 1=UEFI, 2=UEFI+SecureBoot
                                           gboolean     tpm,            // TPM 2.0
                                           gint         cpu_mode,       // 0=Single Edge default(host-passthrough), 1=host-passthrough, 2=host-model
                                           gboolean     hugepages,      // 2MB huge pages
                                           const gchar *storage_type,   // "zvol"(기본), "qcow2", "raw"
                                           const gchar *storage_pool,   // zvol 부모 데이터셋 (예: tank/vms)
                                           const gchar *image_dir,      // qcow2/raw 저장 디렉터리
                                           const gchar *nic_type,       // "bridge"(기본), "dpdk", "sriov"
                                           const gchar *pci_addr,       // SR-IOV VF PCI 주소
                                           const gchar *base_image,     // BUG-16: cloud image 경로
                                           const gchar *owner,          // RBAC: VM 생성자 metadata
                                           GCancellable *cancellable,   // [A1] 취소 토큰 (NULL 허용)
                                           GAsyncReadyCallback callback,
                                           gpointer user_data);
/**
 * purecvisor_vm_manager_create_vm_finish:
 * @manager: VM 매니저
 * @res:     GAsyncResult (콜백 파라미터)
 * @error:   (out)(nullable) 에러 정보
 *
 * @return TRUE면 VM 생성 성공, FALSE면 error에 원인이 설정됨
 */
gboolean purecvisor_vm_manager_create_vm_finish(PureCVisorVmManager *manager,
                                                GAsyncResult *res,
                                                GError **error);

/**
 * purecvisor_vm_resolve_network_bridge:
 * @requested: (nullable) 요청된 브릿지 이름 (vm.create 파라미터 / 템플릿 프리셋)
 *
 * VP-1 — vm.create/템플릿의 기본 브릿지 결정을 단일화합니다.
 *   NULL/"" → config network.default_bridge (기본 "pcvnat0" 관리형 NAT 네트워크)
 *   "none"  → NULL (NIC 미부착 opt-out)
 *   그 외    → 그대로 통과
 *
 * @return 새로 할당된 브릿지 이름(호출자가 g_free) 또는 NULL(NIC 미부착)
 */
gchar *purecvisor_vm_resolve_network_bridge(const gchar *requested);

/**
 * purecvisor_vm_manager_start_vm_async:
 * @self:     VM 매니저
 * @name:     시작할 VM 이름
 * @callback: 완료 콜백
 * @user_data: 콜백 데이터
 *
 * virDomainCreate를 호출하여 VM을 부팅합니다.
 * 성공 시 "vm-started" 시그널이 finish()에서 emit됩니다.
 */
// Start
void purecvisor_vm_manager_start_vm_async(PureCVisorVmManager *self,
                                          const gchar *name,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);
gboolean purecvisor_vm_manager_start_vm_finish(PureCVisorVmManager *manager,
                                               GAsyncResult *res,
                                               GError **error);

/**
 * purecvisor_vm_manager_stop_vm_async:
 * @self:     VM 매니저
 * @name:     중지할 VM 이름
 * @callback: 완료 콜백
 * @user_data: 콜백 데이터
 *
 * virDomainShutdown(ACPI) 시도 → 실패 시 virDomainDestroy(강제)로 폴백.
 * 성공 시 "vm-stopped" 시그널이 finish()에서 emit됩니다.
 */
// Stop
void purecvisor_vm_manager_stop_vm_async(PureCVisorVmManager *self,
                                         const gchar *name,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data);
gboolean purecvisor_vm_manager_stop_vm_finish(PureCVisorVmManager *manager,
                                              GAsyncResult *res,
                                              GError **error);

/**
 * purecvisor_vm_manager_delete_vm_async:
 * @self:     VM 매니저
 * @name:     삭제할 VM 이름
 * @callback: 완료 콜백
 * @user_data: 콜백 데이터
 *
 * 삭제 순서:
 *   1. 실행 중이면 virDomainDestroy로 강제 종료
 *   2. virDomainUndefineFlags로 libvirt 등록 해제
 *   3. ZFS zvol 삭제를 별도 GTask로 fire-and-forget 실행
 *      (5GB+ 볼륨은 수십 초 소요 → 소켓 타임아웃 방지)
 *   4. etcd에서 VM XML 제거
 */
// Delete
void purecvisor_vm_manager_delete_vm_async(PureCVisorVmManager *self,
                                           const gchar *name,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data);
gboolean purecvisor_vm_manager_delete_vm_finish(PureCVisorVmManager *manager,
                                                GAsyncResult *res,
                                                GError **error);

/**
 * pcv_vm_delete_status_get:
 * @vm: VM 이름
 *
 * 비동기 스토리지 삭제 상태 조회.
 * 반환값: "pending" / "deleting" / "done" / "failed" / "not_found"
 */
const gchar *pcv_vm_delete_status_get(const gchar *vm);

/** 데몬 종료 시 호출 — 삭제 상태 해시테이블 정리 */
void pcv_vm_manager_cleanup(void);

/**
 * purecvisor_vm_manager_list_vms_async:
 * @self:     VM 매니저
 * @callback: 완료 콜백
 * @user_data: 콜백 데이터
 *
 * GVirConnection 경유로 모든 도메인 목록을 JSON 배열로 반환합니다.
 * 각 VM 항목에 name, uuid, state("running"/"shutoff"), vnc_port가 포함됩니다.
 *
 * _finish()의 반환값: JsonNode* (호출자가 json_node_free로 해제)
 */
// List
void purecvisor_vm_manager_list_vms_async(PureCVisorVmManager *self,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);
JsonNode *purecvisor_vm_manager_list_vms_finish(PureCVisorVmManager *manager,
                                                GAsyncResult *res,
                                                GError **error);

/* ========================================================================= */
/* Phase 6-2: Runtime Resource Tuning (동적 리소스 튜닝)                     */
/*                                                                           */
/* VM이 실행 중인 상태에서 메모리와 vCPU를 라이브로 조절합니다.               */
/* virDomainSetMemoryFlags / virDomainSetVcpusFlags에 LIVE+CONFIG 플래그를  */
/* 사용하여 즉시 적용 + 영구 저장을 동시에 수행합니다.                       */
/*                                                                           */
/* 주의: 게스트 OS에 virtio-balloon / CPU hotplug 지원이 필요합니다.         */
/* ========================================================================= */

/**
 * purecvisor_vm_manager_set_memory_async:
 * @self:        VM 매니저
 * @name:        대상 VM 이름
 * @memory_mb:   새 메모리 크기 (MB)
 * @cancellable: (nullable) 취소 토큰
 * @callback:    완료 콜백
 * @user_data:   콜백 데이터
 *
 * 내부적으로 virDomainSetMemoryFlags(VIR_DOMAIN_AFFECT_LIVE | AFFECT_CONFIG)를 호출합니다.
 * memory_mb를 KB로 변환(×1024)하여 libvirt에 전달합니다.
 */
// 1. Memory Ballooning
void purecvisor_vm_manager_set_memory_async(PureCVisorVmManager *self,
                                            const gchar *name,
                                            guint memory_mb,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data);

gboolean purecvisor_vm_manager_set_memory_finish(PureCVisorVmManager *self,
                                                 GAsyncResult *res,
                                                 GError **error);

/**
 * purecvisor_vm_manager_set_vcpu_async:
 * @self:       VM 매니저
 * @name:       대상 VM 이름
 * @vcpu_count: 새 vCPU 개수
 * @cancellable: (nullable) 취소 토큰
 * @callback:   완료 콜백
 * @user_data:  콜백 데이터
 *
 * 내부적으로 virDomainSetVcpusFlags(VIR_DOMAIN_AFFECT_LIVE | AFFECT_CONFIG)를 호출합니다.
 * 게스트의 최대 vCPU 이하로만 설정 가능합니다.
 */
// 2. vCPU Tuning
void purecvisor_vm_manager_set_vcpu_async(PureCVisorVmManager *self,
                                          const gchar *name,
                                          guint vcpu_count,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);

gboolean purecvisor_vm_manager_set_vcpu_finish(PureCVisorVmManager *self,
                                               GAsyncResult *res,
                                               GError **error);


/* =========================================================================
 * GIO P6: GObject Signals
 *
 * PureCVisorVmManager 는 아래 세 신호를 발생시킵니다.
 *
 *  "vm-started"
 *      void (*handler)(PureCVisorVmManager *mgr,
 *                      const gchar         *vm_name,
 *                      gpointer             user_data);
 *      VM start_async 가 성공적으로 완료될 때 메인 스레드에서 emit.
 *
 *  "vm-stopped"
 *      void (*handler)(PureCVisorVmManager *mgr,
 *                      const gchar         *vm_name,
 *                      gpointer             user_data);
 *      VM stop_async 가 성공적으로 완료될 때 메인 스레드에서 emit.
 *
 *  "vm-metrics-updated"
 *      void (*handler)(PureCVisorVmManager *mgr,
 *                      GHashTable          *metrics_cache,
 *                      gpointer             user_data);
 *      텔레메트리 데몬이 메트릭 캐시를 갱신할 때마다 메인 스레드에서 emit.
 *      metrics_cache 의 Key = VM UUID (gchar*),
 *                     Value = VmMetrics* (modules/daemons/telemetry.h 참조).
 *      핸들러 내에서 캐시를 직접 수정하거나 g_free() 하면 안 됩니다.
 *
 * 사용 예:
 *   g_signal_connect(mgr, PCV_VM_SIGNAL_STARTED,
 *                    G_CALLBACK(on_vm_started), NULL);
 * ========================================================================= */

/**
 * purecvisor_vm_resize_disk:
 * @name:        대상 VM 이름
 * @new_size_gb: 새 디스크 크기 (GB)
 * @target:      (nullable) 블록 디바이스 타겟 (기본 "vda")
 * @holds_lock:  호출자(핸들러)가 이 VM의 VM_OP_TUNING 락을 이미 획득했는지.
 *               TRUE면 워커 완료(GDestroyNotify) 시점에 unlock_vm_operation(name)으로
 *               해제한다 — acquire는 핸들러, release는 워커 종료지점(단일 해제).
 *
 * fire-and-forget으로 디스크 리사이즈를 실행합니다.
 * ZFS zvol이면 zfs set volsize, qcow2이면 qemu-img resize 후
 * VM 실행 중이면 virDomainBlockResize로 게스트에 통지합니다.
 */
void purecvisor_vm_resize_disk(const gchar *name, gint new_size_gb, const gchar *target,
                                gboolean holds_lock);

/* =========================================================================
 * VM Clone — ZFS CoW 또는 Full Copy 기반 VM 복제
 *
 * fire-and-forget 비동기 패턴으로 동작합니다.
 * 1. ZFS 스냅샷 생성
 * 2. CoW 클론 또는 full send/recv 복사
 * 3. 소스 VM XML 추출 → 이름/UUID/MAC/디스크 경로 패치
 * 4. virDomainDefineXML로 클론 VM 등록
 * 5. etcd 동기화
 * ========================================================================= */

/**
 * purecvisor_vm_clone_async:
 * VM 클론 비동기 요청을 시작합니다.
 *
 * @param source_name  원본 VM 이름
 * @param clone_name   클론 VM 이름
 * @param full_copy    TRUE=full send/recv, FALSE=CoW 클론 (기본)
 * @param cancellable  (nullable) 취소 토큰
 * @param callback     완료 콜백 (메인 스레드에서 호출)
 * @param user_data    콜백 사용자 데이터
 */
void purecvisor_vm_clone_async(const gchar *source_name, const gchar *clone_name,
                                gboolean full_copy, GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer user_data);

/** GObject signal name 상수 — g_signal_connect() 의 문자열 인자로 사용 */
#define PCV_VM_SIGNAL_STARTED          "vm-started"
#define PCV_VM_SIGNAL_STOPPED          "vm-stopped"
#define PCV_VM_SIGNAL_METRICS_UPDATED  "vm-metrics-updated"

/**
 * purecvisor_vm_manager_emit_metrics_updated:
 * @self:  vm_manager 인스턴스
 * @cache: (transfer none): 새로 교체된 메트릭 해시테이블
 *
 * 텔레메트리 데몬이 메트릭 캐시를 교체한 뒤 반드시 메인 스레드에서 호출합니다.
 * vm-metrics-updated 신호를 발생시켜 연결된 모든 핸들러에 새 캐시를 전달합니다.
 *
 * 호출 컨텍스트: telemetry.c의 g_main_context_invoke() 콜백 내부
 * 주의: 메인 스레드 전용 — 워커 스레드에서 직접 호출하면 안 됩니다.
 */
void purecvisor_vm_manager_emit_metrics_updated(PureCVisorVmManager *self,
                                                GHashTable          *cache);

G_END_DECLS

#endif /* PURECVISOR_VM_MANAGER_H */
