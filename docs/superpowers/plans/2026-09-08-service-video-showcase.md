# 서비스 영상 소개 구현 계획

> 단계: operate
> 설계: [서비스 영상 소개](../specs/2026-09-08-service-video-showcase-design.md)

1. 기존 root·ko/en landing, Header, check-site·Pages 발행 경계를 확인한다.
2. 성공 촬영본의 공개용 6개 장면·포스터·재현 가능한 manifest를 준비한다.
3. 정적 컴포넌트·영상 manifest·단일 player 상태 전환을 구현한다.
4. hero CTA·서비스 메뉴·문서 앞 소개 영역과 두 언어를 연결한다.
5. 실제 브라우저로 초기 영상 요청 0, 재생·seek·전환 정지, 오류 복구·키보드·모바일·dark,
   문서·검색 보존을 검증한다. 사이트 build·공개 소스 주석 정책·diff gate를 통과한다.
6. 같은 에이전트의 코드·시각 리뷰 후 공개 main·Pages를 배포하고 실제 HTTPS 재생을 확인한다.
7. 복구용 이전 commit, 공개 asset hash, 검증 한계와 운영 인계를 기록한다.

## Engineering Review

정적 MP4는 같은 origin에서 제공하며 외부 embed나 추적 SDK가 없다. poster와 고정 종횡비가
첫 화면을 안정화한다. 사용자 입력을 HTML로 만들지 않고 DOM textContent·속성으로
갱신한다. video src 제거와 load로 재생·다운로드 상태를 종료한다. 실패한 이전 재생
promise가 새 선택의 상태를 덮지 않도록 selection generation을 검사한다.
기존 문서 route·navigation은 추가 항목 외에는 바꾸지 않는다. rollback은 이 소개 변경의
원복 commit과 Pages 재발행이다. 제품 서비스 재시작은 필요하지 않다.


## 코드·시각 리뷰와 완료

한국어 landing 생성기의 상대 import를 새 컴포넌트까지 확장했다. 기존 retired selector와
충돌한 이름을 `pcv-video-scene`으로 분리했고, Starlight sibling margin으로 생긴 탭·frame
간격을 수정했다. pending play의 선택 generation과 페이지 이탈 정리를 확인했다.

로컬·공개 도메인에서 각각 9개 layout·6개 실제 재생, 키보드·오류 복구·문서 보존이
PASS다. 마지막 상단 밀도와 light/dark 시각 4개 상태를 확인했다. 공개 commit
`7633ba434a957813b1d374984c787173ccb18887`의 Pages 게시를 완료했으며 상세 복구와
검증 한계는 [운영 인계](../../operations/2026-09-08-service-video-showcase-handoff.md)를 따른다.
