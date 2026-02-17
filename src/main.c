// src/main.c
#include <stdlib.h>
#include "purecvisor/core.h"

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    // 1. 초기화
    if (pv_init() < 0) {
        return EXIT_FAILURE;
    }

    // 2. 엔진 실행
    pv_run();

    // 3. 정리 및 종료
    pv_cleanup();
    
    return EXIT_SUCCESS;
}