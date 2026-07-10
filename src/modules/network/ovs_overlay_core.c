/*
 * OVS overlay 공통 구현을 빌드에 포함시키는 얇은 translation unit.
 *
 * [비전공자 설명]
 * overlay 네트워크 구현 자체는 ovs_overlay.c에 있고, 이 파일은 빌드 구성을
 * 맞추기 위한 연결용 파일입니다.
 *
 * [주니어 참고]
 * 여기에 새 로직을 넣으면 같은 기능이 wrapper와 실제 구현으로 갈라져 추적이
 * 어려워집니다. 동작 변경은 ovs_overlay.c에서 하고, 이 파일은 include만 유지합니다.
 */
#include "ovs_overlay.c"
