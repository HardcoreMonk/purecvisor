---
title: PureCVisor
description: Linux/KVM Single Edge 하이퍼바이저 운영 문서
template: splash
hero:
  title: 하나의 노드, 하나의 제어면
  tagline: PureCVisor 2.0.0은 VM, 컨테이너, 스토리지, 네트워크와 운영 관측성을 단일 Linux/KVM 노드에서 관리합니다.
  actions:
    - text: 전체 가이드 읽기
      link: /guide.html
      icon: right-arrow
      variant: primary
    - text: GitHub 저장소
      link: https://github.com/HardcoreMonk/purecvisor
      icon: external
      variant: minimal
---

## Single Edge 운영을 위한 공개 문서

PureCVisor 문서는 설치, VM과 컨테이너 운영, ZFS 스토리지, OVS·OVN 네트워크, REST API,
CLI, 보안, 백업과 품질 게이트를 한 가이드에서 제공합니다.

## 빠른 시작

```shell
make single
make test
make release
```

Ubuntu 22.04 LTS와 24.04 LTS에서 KVM·libvirt 기반 독립 노드 배포를 지원합니다. 실제 설치 전
[전체 운영 가이드](/guide.html)의 환경 전제와 공개판 경계를 확인하세요.

## 문서 운영 원칙

- 제품과 문서는 `2.0.0` Single Edge 공개 범위만 설명합니다.
- 공개 가이드의 단일 진실은 저장소의 `docs/GUIDE.md`입니다.
- GitHub Pages에는 Astro가 생성한 정적 artifact만 배포합니다.
- 다중 노드 클러스터, 라이브 마이그레이션과 페더레이션은 공개 범위가 아닙니다.
