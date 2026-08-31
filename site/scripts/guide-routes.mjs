const definitions = [
  [1, "시작하기", "시작하기", "getting-started", "overview", "1-시작하기"],
  [2, "설치 및 환경 구성", "시작하기", "getting-started", "installation", "2-설치-및-환경-구성"],
  [3, "VM 관리", "워크로드", "workloads", "virtual-machines", "3-vm-관리"],
  [4, "컨테이너 관리", "워크로드", "workloads", "containers", "4-컨테이너-관리"],
  [5, "스토리지", "인프라", "infrastructure", "storage", "5-스토리지"],
  [6, "네트워크", "인프라", "infrastructure", "networking", "6-네트워크"],
  [8, "모니터링 & 알림", "운영·복구", "operations", "monitoring-alerts", "8-모니터링-알림"],
  [9, "백업 & 복원", "운영·복구", "operations", "backup-restore", "9-백업-복원"],
  [10, "보안", "보안·자동화", "security", "security", "10-보안"],
  [11, "클라우드 마이그레이션", "보안·자동화", "security", "cloud-migration", "11-클라우드-마이그레이션"],
  [12, "AI & 자가치유", "보안·자동화", "security", "ai-self-healing", "12-ai-자가치유"],
  [13, "Web UI", "인터페이스", "interfaces", "web-ui", "13-web-ui"],
  [14, "REST API", "인터페이스", "interfaces", "rest-api", "14-rest-api"],
  [15, "CLI 레퍼런스", "인터페이스", "interfaces", "cli", "15-cli-레퍼런스"],
  [16, "설정 레퍼런스", "설정·문제 해결", "reference", "configuration", "16-설정-레퍼런스"],
  [17, "트러블슈팅", "설정·문제 해결", "reference", "troubleshooting", "17-트러블슈팅"],
  [18, "부록", "설정·문제 해결", "reference", "appendix", "18-부록"],
  [19, "개발자 & 엔지니어 가이드", "개발·출시", "development", "engineering", "19-개발자-엔지니어-가이드"],
  [20, "영업 & 마케팅 가이드", "개발·출시", "development", "sales-marketing", "20-영업-마케팅-가이드"],
  [21, "아키텍처 리팩토링 가이드", "개발·출시", "development", "architecture-refactoring", "21-아키텍처-리팩토링-가이드"],
  [22, "품질 게이트 가이드", "개발·출시", "development", "quality-gates", "22-품질-게이트-가이드"]
];

export const guideChapters = definitions.map(([
  number,
  title,
  group,
  directory,
  slug,
  legacyAnchor
]) => ({
  number,
  title,
  group,
  directory,
  slug,
  legacyAnchor,
  contentSlug: `ko/${directory}/${slug}`,
  path: `/ko/${directory}/${slug}/`
}));

export const supplementalDocuments = [
  {
    title: "데이터베이스 아키텍처",
    sourceTitle: "PureCvisor Single Edge 데이터베이스 아키텍처 설명서",
    description: "PureCvisor Single Edge 로컬 SQLite 9개와 영구 테이블 26개의 흐름, 책임과 복구 경계",
    group: "개발·출시",
    directory: "development",
    slug: "database-architecture",
    source: "DATABASE_STRUCTURE.md",
    contentSlug: "ko/development/database-architecture",
    path: "/ko/development/database-architecture/"
  }
];

export const readerDocuments = [...guideChapters, ...supplementalDocuments];

export const landingDocuments = readerDocuments;

export const guideGroups = [...new Set(readerDocuments.map((document) => document.group))].map(
  (label) => ({
    label,
    items: readerDocuments
      .filter((document) => document.group === label)
      .map((document) => ({ label: document.title, link: `/${document.directory}/${document.slug}/` }))
  })
);

export const guideEntryPath = "/ko/getting-started/installation/";

export function guidePath(number, hash = "") {
  const chapter = guideChapters.find((item) => item.number === number);
  if (!chapter) throw new Error(`unknown guide chapter: ${number}`);
  return `${chapter.path}${hash}`;
}
