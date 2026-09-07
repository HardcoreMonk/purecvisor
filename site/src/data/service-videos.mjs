const media = (id) => ({ id, src: `/assets/service-videos/${id}.mp4`, poster: `/assets/service-videos/${id}.webp` });
export const serviceGroups = [
  {
    id: 'vpc', label: 'Local VPC',
    ko: { tag: '가상 사설망', title: '내 서비스만의 네트워크를 만드세요.', description: 'VPC와 서브넷을 구성하고 주소 공간을 나눕니다. 필요한 네트워크를 Web UI에서 단계별로 만들 수 있습니다.', note: 'Linux bridge 기반 Local VPC의 실제 구성 화면입니다.' },
    en: { tag: 'Private networks', title: 'Give your services a network of their own.', description: 'Create a VPC, add subnets, and organize address space. Follow the configuration step by step in the Web UI.', note: 'Recorded with the Linux bridge backend for Local VPC.' },
    clips: [
      { ...media('vpc-create'), ko: { title: 'VPC와 첫 서브넷 만들기', summary: '이름과 네트워크 방식, 주소 대역을 지정해 VPC와 첫 서브넷을 함께 생성합니다.' }, en: { title: 'Create a VPC and its first subnet', summary: 'Set the name, network mode, and address range to create a VPC with its first subnet.' } },
      { ...media('vpc-subnet'), ko: { title: '서브넷으로 주소 공간 나누기', summary: '기존 VPC에 두 번째 서브넷을 추가하고, 분리된 주소 대역과 생성 상태를 확인합니다.' }, en: { title: 'Organize your address space', summary: 'Add a second subnet to the VPC and review its address range and creation state.' } }
    ]
  },
  {
    id: 'ovn', label: 'OVN SDN',
    ko: { tag: '논리 네트워크', title: '스위치부터 라우팅까지, 하나의 흐름으로.', description: '논리 스위치와 DHCP, 라우터와 NAT의 구성 결과를 확인합니다. CLI로 구성하고 Web UI에서 네트워크 상태를 살펴봅니다.', note: 'Single Edge의 로컬 OVN 구성 시험 영상입니다.' },
    en: { tag: 'Logical networking', title: 'From switching to routing, one connected flow.', description: 'See logical switches, DHCP, routers, and NAT in action. Configure with the CLI and review network state in the Web UI.', note: 'A recorded test of local OVN on a Single Edge node.' },
    clips: [
      { ...media('ovn-switch'), ko: { title: '논리 스위치와 DHCP 확인', summary: '논리 스위치와 포트 구성, DHCP 주소 할당과 L2 통신을 확인하는 과정을 보여 줍니다.' }, en: { title: 'Logical switches and DHCP', summary: 'See switch and port configuration, DHCP address allocation, and L2 connectivity checks.' } },
      { ...media('ovn-router'), ko: { title: '라우터와 NAT 연결 확인', summary: '논리 라우터를 연결하고 서브넷 간 통신, SNAT와 일대일 NAT의 동작을 확인합니다.' }, en: { title: 'Routing and NAT in action', summary: 'Connect a logical router and check inter-subnet traffic, SNAT, and one-to-one NAT.' } }
    ]
  },
  {
    id: 'vxlan', label: 'VXLAN',
    ko: { tag: '오버레이 연결', title: '호스트 사이로 네트워크를 넓히세요.', description: 'VXLAN 네트워크와 피어를 구성하고 연결 상태를 확인합니다. 두 독립 호스트를 수동 피어로 연결하는 흐름을 담았습니다.', note: 'UI는 상태 조회용이며 구성은 CLI로 수행합니다. 운영 로그 영역은 기능 설명으로 편집했습니다.' },
    en: { tag: 'Overlay connections', title: 'Extend a network between hosts.', description: 'Create a VXLAN network, add a peer, and review the connection state. See two independent hosts linked through manually configured peers.', note: 'Configuration uses the CLI; the UI displays state. Operational logs are replaced with explanatory panels.' },
    clips: [
      { ...media('vxlan-create'), ko: { title: 'VXLAN 네트워크 구성', summary: '네트워크를 생성한 뒤 이름, VNI, 주소 대역과 활성 상태가 표시되는 과정을 확인합니다.' }, en: { title: 'Create a VXLAN network', summary: 'Watch a newly created network appear with its name, VNI, address range, and active state.' } },
      { ...media('vxlan-peer'), ko: { title: '피어 연결과 상태 확인', summary: '피어를 추가한 뒤 화면의 피어 수와 연결 상태를 확인합니다. 원본 촬영은 실제 VM 간 통신 시험 과정에서 생성했습니다.' }, en: { title: 'Connect a peer and review its state', summary: 'Review the peer count and network state after adding a peer. The source recording was made during VM connectivity testing.' } }
    ]
  }
];

export const showcaseCopy = {
  ko: { heading: '서비스 기능 소개', lead: '네트워크를 만들고, 연결하고, 확인하는 과정.\n실제 동작을 영상으로 만나보세요.', category: '네트워크 서비스 선택', chapters: '다음 장면 살펴보기', play: '영상 재생', ready: '원하는 장면을 선택하고 재생하세요.', loading: '영상을 불러오고 있습니다…', playing: '재생 중', paused: '일시 정지', ended: '재생 완료 · 다른 장면도 살펴보세요.', failed: '영상을 불러오지 못했습니다. 다시 시도하거나 영상 파일을 직접 열어주세요.', retry: '다시 시도', direct: '영상 파일 열기', guide: '네트워크 가이드', context: '기능 테스트에서 촬영한 무음 화면 영상입니다. 아래 장면 설명과 함께 보세요.', browse: '서비스 영상 보기' },
  en: { heading: 'See the services in action', lead: 'Create, connect, and verify a network.\nExplore real workflows on screen.', category: 'Choose a network service', chapters: 'Explore the scenes', play: 'Play video', ready: 'Choose a scene and press play.', loading: 'Loading video…', playing: 'Playing', paused: 'Paused', ended: 'Video finished. Explore another scene.', failed: 'The video could not be loaded. Try again or open the video file directly.', retry: 'Try again', direct: 'Open video file', guide: 'Networking guide', context: 'Silent screen recordings from functional tests. Each scene includes a written description below.', browse: 'Watch service demos' }
};
