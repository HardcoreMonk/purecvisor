/*
 * OVN 공통 구현을 빌드에 포함시키는 얇은 translation unit.
 *
 * [주니어 참고]
 * 실제 로직은 ovn_manager.c에 있습니다. 이 파일은 Makefile/에디션별 소스
 * 묶음에서 "OVN core"라는 이름으로 같은 구현을 참조하기 위한 래퍼입니다.
 * 버그 수정이나 기능 추가는 여기 말고 ovn_manager.c에 적용하세요.
 */
#include "ovn_manager.c"
