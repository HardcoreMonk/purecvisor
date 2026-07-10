/**
 * @file arena.c
 * @brief 아레나(Arena) 메모리 할당기 — 고성능 일시적 벌크 할당
 *
 * PureCVisor의 경량 아레나 할당기입니다.
 * 고정 크기 메모리 블록을 사전 할당한 후, bump pointer 방식으로
 * 빠르게 할당하고 한 번에 전체를 해제합니다.
 *
 * [아키텍처 위치]
 *   RPC 핸들러나 배치 처리에서 수명이 짧은 다수의 소규모 할당이
 *   필요할 때 GLib g_malloc/g_free 대신 사용할 수 있습니다.
 *   arena_create() → arena_alloc() x N → arena_reset() 또는 arena_destroy()
 *
 * [핵심 자료구조]
 *   Arena {
 *     buffer : malloc으로 할당된 고정 크기 메모리 블록
 *     size   : 전체 블록 크기 (바이트)
 *     offset : 다음 할당 위치 (bump pointer)
 *   }
 *
 * [할당 알고리즘 (Bump Allocator)]
 *   전통적인 malloc/free는 메모리 블록 관리를 위해 프리리스트(free list)를
 *   유지하고, 할당/해제 시마다 리스트를 순회하는 오버헤드가 있습니다.
 *   아레나 할당기는 이 오버헤드를 완전히 제거합니다:
 *
 *   1. 8바이트 정렬 패딩 계산: padding = (8 - offset % 8) % 8
 *   2. offset + padding + size > total이면 NULL 반환 (용량 초과)
 *   3. offset 전진 → 포인터 반환 → memset(0) 초기화
 *
 *   할당은 단순히 offset을 전진시키는 것이므로 O(1) 시간에 완료됩니다.
 *
 * [사용 패턴]
 *   Arena *a = arena_create(4096);         // 4KB 아레나 생성
 *   char *name = arena_alloc(a, 64);       // 64바이트 할당 (8바이트 정렬)
 *   int  *ids  = arena_alloc(a, 40);       // 40바이트 할당
 *   // ... name과 ids 사용 ...
 *   arena_reset(a);                         // 전체 초기화 (재사용)
 *   // 다시 arena_alloc() 가능
 *   arena_destroy(a);                       // 메모리 해제
 *
 * [g_malloc/g_free 대비 장점]
 *   - 개별 free 불필요: reset/destroy로 한 번에 해제 (메모리 누수 방지)
 *   - malloc 호출 최소화: 초기 1회 할당 후 bump pointer만 이동
 *   - 캐시 친화적: 연속 메모리 영역 사용 (spatial locality 향상)
 *   - 프래그멘테이션 없음: 연속 할당이므로 메모리 조각화가 발생하지 않음
 *
 * [다른 모듈과의 관계]
 *   현재 다른 모듈에서 직접 사용하는 곳은 제한적이나,
 *   배치 처리(예: 대량 VM 목록 생성), XML 빌더 등에서
 *   임시 문자열 할당에 활용할 수 있습니다.
 *
 * [주의사항]
 *   - 64비트 시스템 전용 (_Static_assert로 컴파일 시점 검증)
 *   - 용량 초과 시 NULL 반환 (동적 확장 없음 — 크기를 넉넉히 잡을 것)
 *   - 개별 블록 해제(free) 불가 — reset으로만 전체 초기화
 *   - 스레드 안전하지 않음: 단일 스레드 내에서만 사용
 *   - arena_alloc()은 항상 0으로 초기화된 메모리를 반환 (calloc 유사)
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "purecvisor/core.h"

/*
 * 64비트 시스템 확인 (Static Assert)
 *
 * PureCVisor는 64비트 시스템에서만 동작합니다.
 * 32비트 시스템에서는 주소 공간(4GB)이 부족하여
 * 대규모 VM 관리에 적합하지 않습니다.
 * _Static_assert는 C11 표준으로, 컴파일 시점에 조건을 검증합니다.
 * 조건이 FALSE면 컴파일 에러가 발생합니다.
 */
_Static_assert(sizeof(void*) == 8, "PureCVisor requires 64-bit system");

/**
 * Arena - 아레나 할당기 구조체
 *
 * [메모리 레이아웃]
 *   Arena 헤더  : malloc으로 할당 (24바이트: buffer 포인터 + size + offset)
 *   buffer      : malloc으로 할당 (사용자 지정 크기)
 *
 *   buffer 내부:
 *   [  할당된 영역  |  패딩  |  할당된 영역  |  미사용 영역  ]
 *   ^                                         ^
 *   buffer[0]                                 buffer[offset]
 *
 * [8바이트 정렬]
 *   모든 할당은 8바이트 경계에 정렬됩니다.
 *   64비트 시스템에서 double, int64_t, 포인터 등이
 *   정렬 제한 없이 안전하게 접근 가능합니다.
 *   정렬되지 않은 접근은 x86에서는 성능 저하,
 *   ARM/RISC-V에서는 Bus Error(SIGBUS)를 유발할 수 있습니다.
 */
typedef struct {
    uint8_t *buffer;    /* 사전 할당된 메모리 블록 */
    size_t size;        /* buffer의 전체 크기 (바이트) */
    size_t offset;      /* 다음 할당 위치 (0부터 시작하여 증가) */
} Arena;

/**
 * arena_create - 아레나 생성
 * @size: 아레나 전체 크기 (바이트)
 *
 * @return: 새 Arena 포인터 또는 NULL (malloc 실패 시)
 *
 * buffer를 한 번 할당한 후 arena_alloc()으로 내부 영역을 분배합니다.
 * 사용 후 반드시 arena_destroy()로 해제해야 합니다.
 *
 * [크기 결정 가이드]
 *   - RPC 핸들러 임시 할당: 1024~4096 바이트
 *   - 배치 VM 목록 생성: 64KB~256KB
 *   - 용량 초과 시 NULL 반환이므로, 예상 최대치의 2배를 권장
 *
 * [malloc 실패 처리]
 *   Arena 헤더 또는 buffer 할당 실패 시 NULL을 반환합니다.
 *   호출자는 반드시 NULL 체크를 해야 합니다.
 */
Arena* arena_create(size_t size) {
    Arena *a = malloc(sizeof(Arena));
    if (!a) return NULL;
    a->buffer = malloc(size);
    if (!a->buffer) {
        free(a);  /* 부분 할당 정리 */
        return NULL;
    }
    a->size = size;
    a->offset = 0;  /* 다음 할당 위치: buffer 시작점 */
    return a;
}

/**
 * arena_alloc - 아레나에서 메모리 블록 할당 (bump pointer 방식)
 * @a:    아레나 포인터
 * @size: 요청 크기 (바이트)
 *
 * @return: 0으로 초기화된 메모리 포인터 또는 NULL (용량 초과 시)
 *
 * [동작 흐름]
 *   1. 현재 offset을 8바이트 경계로 정렬하기 위한 패딩 계산
 *      padding = (8 - (offset % 8)) % 8
 *      예: offset=5 → padding=3 → 정렬된 위치=8
 *      예: offset=8 → padding=0 → 이미 정렬됨
 *
 *   2. 용량 확인: offset + padding + size > total → NULL 반환
 *
 *   3. offset에 padding 추가 (정렬)
 *   4. buffer[offset]의 포인터 반환
 *   5. offset += size (다음 할당 위치 전진)
 *   6. memset(0) 초기화
 *
 * [8바이트 정렬의 의미]
 *   64비트 시스템에서 최대 기본 타입 크기가 8바이트(double, int64_t, pointer)
 *   이므로, 8바이트 정렬은 모든 기본 타입에 대해 안전합니다.
 *   예: arena_alloc(a, sizeof(int64_t)) → 반환 포인터가 8의 배수 주소
 *
 * [0 초기화]
 *   memset(ptr, 0, size)로 할당된 영역을 0으로 초기화합니다.
 *   calloc()과 유사한 동작으로, 미초기화 메모리 사용 버그를 방지합니다.
 *
 * [성능]
 *   O(1) 시간 복잡도. 패딩 계산(산술 연산) + 포인터 이동만 수행합니다.
 *   malloc의 O(log n) 또는 O(n) 프리리스트 탐색 대비 훨씬 빠릅니다.
 *
 * [개별 free 불가]
 *   할당된 블록을 개별적으로 해제할 수 없습니다.
 *   arena_reset()으로 전체를 한 번에 초기화하거나
 *   arena_destroy()로 아레나 자체를 해제해야 합니다.
 */
void* arena_alloc(Arena *a, size_t size) {
    /* 8바이트 정렬을 위한 패딩 계산 */
    size_t padding = (8 - (a->offset % 8)) % 8;

    /* 용량 초과 확인 — 동적 확장 없이 즉시 NULL 반환 */
    if (a->offset + padding + size > a->size) return NULL;

    /* 정렬 패딩 적용 */
    a->offset += padding;

    /* 현재 위치의 포인터를 반환값으로 저장 */
    void *ptr = &a->buffer[a->offset];

    /* offset 전진 — 다음 할당은 여기서부터 시작 */
    a->offset += size;

    /* 0으로 초기화 — calloc과 유사한 안전성 보장 */
    memset(ptr, 0, size);
    return ptr;
}

/**
 * arena_reset - 아레나 오프셋을 0으로 되돌림 (메모리 재사용)
 * @a: 아레나 포인터 (NULL 안전)
 *
 * buffer 메모리는 해제하지 않고, offset만 0으로 되돌립니다.
 * 이전에 할당된 포인터는 모두 무효화됩니다.
 *
 * [재사용 패턴]
 *   while (has_work) {
 *       char *tmp = arena_alloc(a, 256);
 *       // tmp 사용
 *       arena_reset(a);  // 다음 반복에서 같은 메모리 재사용
 *   }
 *
 * [주의: Dangling Pointer]
 *   reset 후 이전에 반환된 포인터는 유효하지 않습니다.
 *   다음 arena_alloc()이 같은 메모리를 덮어쓸 수 있습니다.
 */
void arena_reset(Arena *a) {
    if (a) a->offset = 0;
}

/**
 * arena_destroy - 아레나 및 내부 buffer 완전 해제
 * @a: 아레나 포인터 (NULL 안전)
 *
 * buffer와 Arena 헤더를 모두 free()합니다.
 * 이후 아레나 포인터와 arena_alloc()으로 반환된 모든 포인터가 무효화됩니다.
 *
 * [NULL 안전]
 *   a가 NULL이어도 크래시하지 않습니다.
 *   이는 GLib의 g_free(NULL) 안전성 관례를 따릅니다.
 */
void arena_destroy(Arena *a) {
    if (a) {
        free(a->buffer);   /* 사전 할당된 메모리 블록 해제 */
        free(a);           /* Arena 헤더 해제 */
    }
}
