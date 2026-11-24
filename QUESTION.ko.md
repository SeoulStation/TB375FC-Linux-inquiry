# Lenovo TB375FC: 같은 Linux 부팅의 두 번째 화면 표시에서 세로 명암선이 생깁니다

> 사용자 승인에 따라 Codex가 자동으로 게시했습니다.
> Automatically posted by Codex with the user's authorization.

Lenovo TB375FC에서 네이티브 Linux를 실행하는 작업을 진행하고 있습니다. MediaTek DRM/패널 resume 또는 Novatek 터치 firmware에 익숙한 개발자분께 기록 검토와 다음 진단 방향에 대한 의견을 부탁드립니다. 현재 원인은 미확정이며 최신 후보도 문제를 해결하지 못했습니다.

## 환경과 증상

- TB375FC PRC / P98300DA2, MT6897/MT8792 계열, RAM 8GB.
- 원래 Android 14 / ZUI 16.0.20.041, 실행 커널 `6.1.25-android14-11-g4528d59d4335-ab11203497`와 수정된 vendor `nt36532.ko`를 사용합니다.
- BusyBox PID1과 RAM Ubuntu ARM64 환경입니다. mainline 이식이나 내부 저장소의 독립 Ubuntu 설치를 완료한 상태는 아닙니다.
- MediaTek DRM의 DSI-1, 2944×1840, 패널이 반환한 정확한 30Hz 모드로 사진을 표시합니다.
- 새 Linux 부팅의 첫 화면은 정상적으로 표시된 이력이 있습니다. 같은 부팅에서 다시 표시하면 처음에는 정상 사진이 보이다가 세로 방향의 밝고 어두운 띠가 생깁니다. 터치하지 않은 시험에서도 재현됐습니다.

## 최신 시험 — 2026-11-24 KST

첫 사진은 안정적으로 보였고 자동으로 꺼졌습니다. 같은 부팅의 두 번째 사진은 정상적으로 나타난 뒤 1초가 채 되기 전에 세로 명암선이 발생했고 자동으로 꺼졌습니다. USB 제어는 유지됐습니다. 로그 회수·DRM 원복 성공과 실제 화면 안정 실패를 구분하고 있습니다.

두 시험은 같은 boot·후보 모듈·사진을 사용했지만 약 22시간 간격이 있고 중간에 충전·USB 재연결이 있었습니다. 두 번째 viewer에는 phase logging이 추가돼 binary도 다릅니다. 완전히 통제된 A/B 실험으로 해석하지 않습니다.

| 수집 항목 | 첫 표시 | 두 번째 표시 |
|---|---:|---:|
| 광학 관찰 | 안정 | 정상 사진 후 1초 미만에 세로 명암선 |
| 관측 SPI read/write 쌍 | 0 / 1 | 311 / 564 |
| trace 행 | 2 | 1,750 |
| trace drop/overrun/missed | 0 | 0 |

두 번째 표시의 875개 SPI post는 ret/status 0이고 요청·실제 길이가 일치했습니다. 하지만 연속 kernel stream에는 `checksum not match` **80건**, FW reset 회복 8회, FW update success 9회가 있습니다. checksum 함수명이 포함된 960행은 덤프를 포함한 행수이며 오류 960건이 아닙니다.

이번 초기 worker의 순서는 FW success → pen 7B00 성공 → restore 진입 → game 7900 성공 → restore pen 7B00 실패 → restore 종료 → 첫 IRQ checksum 불일치입니다. 첫 FD 접두부 point read는 첫 IRQ grid write보다 약 271.627ms 앞섭니다. 따라서 나중 grid write 하나로 앞선 FD를 설명할 수는 없습니다. 실제 광학 원인은 아직 모릅니다.

## 적용한 변경과 한계

과거 B/A/C는 초기 pen 조건분기, grid 잠금, 두 변경의 조합을 시험했으나 안정적인 재표시를 확보하지 못했습니다. 최신 후보는 C의 restore-grid 호출 `.text+0x56d0`만 NOP로 바꾸고 relocation 한 개를 제거했습니다. IRQ `0x428c`, userspace `0xa4f0` 경로는 남아 있으며 전체 grid 제거 시험이 아닙니다. pen 조건분기 `0x7110`의 기존 NOP도 남아 있습니다. 원래 분기 복원은 pen 호출뿐 아니라 `ts+604(0x25c)` clear 조건도 바꿉니다.

최신 모듈 SHA256: `e6df2e95385bbb7a03ab9f7f90f02faf2ff20614860420b36a6b4e190c1e320c`.

## 도움을 구하는 내용

1. 첫 already-resumed/상속 상태와 후속 firmware·restore 경로의 차이를 어떤 이벤트로 비교하면 좋을까요?
2. point IRQ, FW 회복, page/명령/ACK를 개별 SPI mutex보다 상위에서 직렬화할 때 어떤 lock/IRQ/workqueue 제약을 검토해야 할까요?
3. touch FW 회복과 패널이 reset·전원을 공유하는지, 두 오류가 공통 잘못된 상태의 결과인지 어떻게 분리하면 좋을까요?
4. IRQ enable과 pen/game/grid 복원에 필요한 ready·ACK 기준, 정상 Android에서 먼저 확보할 기준선은 무엇일까요?
5. DSI/DSC/PPS·reset·전원 등에서 다음 단일 변수 비교 전에 추가할 최소 계측 지점은 어디일까요?

첨부 자료에는 첫/두 번째 원본 로그, 상세 개발일지, 정적 분석, 선별 작업코드와 오프라인 검증기가 있습니다. 정확한 vendor driver C 전체나 완전 재현 빌드는 포함하지 않습니다. 광학 동기 영상·물리 SPI/MIPI 파형도 없습니다. ROM·FW binary·파티션/보정값 백업·인증 자료·플래시 도구는 제외했습니다. 기록을 기준으로 다음 진단 방향을 검토해 주시면 감사하겠습니다.

## 과거 실기기 사진

[연속사진 30장·전체 미리보기·원본 보기](evidence/photos-20261101/README.md)를 추가했습니다. **2026-11-01 A 후보의 세 번째 활성화** 기록이며, 11월 24일 최신 FIRST/SECOND 시험 사진과 구분합니다. 별도 지정한 028·029도 원본으로 포함돼 있습니다.
