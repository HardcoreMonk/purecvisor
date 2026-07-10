/**
 * @file fuzz_pcv_validate.c
 * @brief libFuzzer harness for pcv_validate.* — 입력 검증 레이어 퍼징
 *
 * 빌드: make fuzz   (clang + -fsanitize=fuzzer,address,undefined)
 * 실행: ./fuzz_pcv_validate -max_total_time=300 corpus/
 *
 * 본 harness는 단일 입력을 모든 검증 함수에 전달해 충돌/UB/오버런을 유도한다.
 * pcv_validate_*는 NULL/임의 길이/비-UTF8에 안전해야 한다 (PCV 보안 컨벤션).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "purecvisor/pcv_validate.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* NUL 종료 사본 — 검증 함수는 C 문자열 가정 */
    char *s = (char *)malloc(size + 1);
    if (!s) return 0;
    if (size) memcpy(s, data, size);
    s[size] = '\0';

    /* 문자열 기반 검증 — NULL/임의 입력 안전성 */
    pcv_validate_vm_name(s);
    pcv_validate_snap_name(s);
    pcv_validate_bridge_name(s);
    pcv_validate_iso_path(s);
    pcv_validate_container_image(s);
    pcv_validate_exec_cmd(s);
    pcv_validate_pci_addr(s);
    pcv_validate_cidr(s);

    /* 정수 검증 — 입력 앞 8바이트를 int64로 사용 */
    if (size >= 8) {
        gint64 n;
        memcpy(&n, data, sizeof(n));
        pcv_validate_memory_mb(n);
        pcv_validate_vcpu(n);
        pcv_validate_disk_gb(n);
    }

    /* 통합 검증 — GError 경로 */
    GError *err = NULL;
    pcv_validate_vm_create_params(s, 2, 1024, 10, NULL, NULL, &err);
    if (err) g_error_free(err);

    err = NULL;
    pcv_validate_network_create_params(s, "nat", s, NULL, &err);
    if (err) g_error_free(err);

    free(s);
    return 0;
}
