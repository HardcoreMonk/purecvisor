/**
 * @file version.h
 * @brief 제품 버전 단일 소스 — PCV_PRODUCT_VERSION.
 *
 * 데몬/CLI/UI/health 응답이 노출하는 릴리스 버전 문자열의 유일한 정의처다.
 * 릴리스 때 이 매크로 하나만 올린다(버전을 다른 곳에 하드코딩하지 않는다).
 * 값을 바꾸면 about 표시·sw.js 캐시 bump·health 버전 필드가 함께 움직이므로,
 * 정식 릴리스 절차 밖에서 임의로 수정하지 않는다.
 */
#ifndef PURECVISOR_VERSION_H
#define PURECVISOR_VERSION_H

#define PCV_PRODUCT_VERSION "1.5.0"

#endif /* PURECVISOR_VERSION_H */
