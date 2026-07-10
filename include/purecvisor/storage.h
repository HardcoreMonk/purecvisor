/**
 * @file storage.h
 * @brief 스토리지 드라이버 공개 API -- ZFS zvol 볼륨 관리
 *
 * ====================================================================
 *  파일 역할
 * ====================================================================
 *  스토리지 백엔드 드라이버의 공개 인터페이스를 정의한다.
 *  Opaque Pointer + VTable(함수 포인터 테이블) 패턴으로 백엔드를
 *  추상화하며, 현재는 ZFS 드라이버(zfs_driver.c)가 유일한 구현체이다.
 *
 * ====================================================================
 *  아키텍처 위치
 * ====================================================================
 *  스토리지 모듈(src/modules/storage/)의 공개 헤더.
 *  디스패처 핸들러(handler_storage.c)가 이 API를 호출하여
 *  zvol 생성/삭제를 수행한다.
 *
 *    handler_storage.c (RPC 핸들러)
 *        | storage_create_vol() / storage_destroy()
 *    StorageDriver (이 헤더의 Opaque Pointer)
 *        | VTable dispatch
 *    zfs_driver.c (ZFS 구현: zfs create, zfs destroy 등)
 *
 * ====================================================================
 *  핵심 패턴
 * ====================================================================
 *  - Opaque Pointer: StorageDriver 내부 구조는 .c에서만 정의.
 *  - Factory Method: storage_driver_new_zfs(pool_name)으로 ZFS 드라이버
 *    인스턴스를 생성한다. 향후 Ceph/LVM 등 추가 시 new_ceph() 등 추가.
 *  - VTable Wrappers: storage_create_vol() 등은 내부적으로 드라이버의
 *    함수 포인터를 호출하여 백엔드 독립적으로 동작한다.
 *
 * ====================================================================
 *  주의사항
 * ====================================================================
 *  - pool_name 기본값: "pcvpool/vms" (daemon.conf [storage] zvol_pool로 변경 가능).
 *  - storage_destroy()는 드라이버 객체 자체를 해제한다. 호출 후 재사용 불가.
 *  - 볼륨 크기 단위는 MB이다.
 */

#ifndef PURECVISOR_STORAGE_H
#define PURECVISOR_STORAGE_H

#include <glib.h>
#include <stdbool.h>

/* Opaque Pointer: 구현 은닉 */
typedef struct StorageDriver StorageDriver;

/* Factory Method -- ZFS 백엔드 드라이버 생성 */
StorageDriver* storage_driver_new_zfs(const char *pool_name);

/* Public API (VTable Wrappers) -- 백엔드 독립적 볼륨 관리 */
bool storage_create_vol(StorageDriver *self, const char *name, size_t size_mb);
void storage_destroy(StorageDriver *self);

#endif /* PURECVISOR_STORAGE_H */