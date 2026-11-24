# 실제 FIRST/SECOND 비교의 소스 범위와 다음 PC 검토 후보

현재 원인은 미해결이다. 설치 파일이 `e6df2e…`인 grid-omission 후보에서도 두 번째 사진은 처음 잠깐 정상으로 보인 뒤 약 1초 미만에 세로 명암선이 재발했다. 기계적 캡처·회수·정리 검증 통과를 화면 안정성 성공으로 표현하면 안 된다. 이 문서는 PC 분석이며 새 모듈 생성·설치·추가 표시를 수행하지 않았다.

## 확인된 사실

- FIRST는 trace 2행, write 1회다. viewer PID24449의 유효 패킷은 `d0 11`, 길이2이며 read 관측은 0회다. 6초 수집창 밖의 부팅 전체 IO가 없었다는 뜻은 아니다. 종료 인접 시각이지만 FIRST에는 phase 시각이 없어 이 write가 정확히 어떤 표시 단계였는지는 확정하지 않는다.
- SECOND는 trace 1,750행, read 311회·write 564회다. 모든 875개 post의 ret/status는 0이고 frame/actual/request 길이가 일치한다. 이는 SPI 전송 결과이며 명령 수락·정상 touch 데이터·정상 화면을 보증하지 않는다.
- 이번 kernel stream의 실제 `checksum not match` 메시지는 **80건**이다. 함수명 `nvt_ts_point_data_checksum`이 있는 **960행**에는 각 오류 뒤의 hex dump가 포함되므로 960건의 오류라고 쓰면 안 된다. `Recover for fw reset`은8건, `Update firmware success`는9건이다.
- 이번 초기 worker T26676에서는 firmware success → `7B 00` 성공 → restore 시작 → `79 00` 성공 → restore의 `7B 00` 실패 → restore 종료 → IRQ 첫 checksum 불일치 순서다. 이후 IRQ에서는 `79` 실패8건·`7B` 실패8건이 추가된다. 총 `7B` 실패는 초기 worker1건을 포함한9건이다. 과거 다른 boot의 초기79 실패 순서를 이번 실행에 대입하면 안 된다.
- trace에서 첫 길이110 point read post는99047.616543이며 유효 RX 접두부가 `00 00 fd fd fd fd fd fd fd fd`다. 첫 IRQ `d0 7f 01 …` 길이9 write는99047.888170이다. 같은 trace 안에서 FD 접두부가 약271.627ms 먼저 관측된다. 이 순서는 그 첫 FD를 나중의 IRQ grid write가 소급해 만들었다는 해석을 지지하지 않는다. 화면 깨짐의 물리 시각이나 모든 선행 사건을 확정하는 자료는 아니다.
- 사용자의 <1초 재발 보고는, 사진 가시 시작이 `ACTIVE` 근방이라는 가정 아래, 정상 복원 시작 `ACTIVE+4729.871ms`보다 이른 구간이다. 실제 사진 가시 시작과 명암선 발생을 공통 시계로 기록하지 않았으므로 정확한 인과관계로 바꿔 쓰지 않는다.

## grid-omission은 전체 grid 전송 제거가 아니다

정확한 원 C 파일 `d742bdb8…`와 현재 후보 `e6df2e95…`를 다시 byte/ELF 비교했다. 현재 후보는 `.text+0x56d0`의 CALL26을 NOP로 바꾸고 해당 relocation 한 개를 제거한 파일과 정확히 일치한다. 다른 실행 text 바이트는 원 C와 같다. 파일 전체 차이17,876바이트의 대부분은 relocation 테이블 이동이므로 “파일에서4바이트만 달라졌다”라고 표현하지 않는다.

| 위치 | 소유 함수 | 현재 파일의 상태 |
|---|---|---|
| `.text+0x56d0` | `nvt_restore_cmd_func` | NOP, grid wrapper relocation 없음 |
| `.text+0x428c` | `nvt_ts_work_func` | grid wrapper CALL26 보존 |
| `.text+0xa4f0` | `nvt_edge_grid_zone_store` | grid wrapper CALL26 보존 |

`nvt_edge_grid_zone_set`의 현재 함수 바이트는 원 C 및 검증된 prototype disassembly와 같다. 이 함수는 buffer에 `50 7f 01`과6개 인자 바이트를 넣고 길이9 write를 호출한다. SPI write의 첫 바이트 OR 0x80 처리로 trace에는 `d0 7f 01 …`로 나타난다. `BA` 길이2는 별도의 edge-reject 명령 경로와 일치하며 `7F` 길이9 grid 패킷과 구분해야 한다.

이번 초기 worker에는 `7F` 길이9가 없고, IRQ 문맥에는8회 있다. 따라서 restore 한 지점의 생략과 IRQ 뒤 grid 복원 잔존이 함께 관측된 것이다. 실제 caller PC/LR을 수집하지 않았으므로 IRQ의 정확한 명령 주소는 byte·문맥·정적 경로가 일치한다는 수준의 해석이다. 현재 RAM의 실행 코드 전체 hash를 읽어 대조한 것은 아니다.

## 기존 pen 변경도 그대로 남아 있다

현재 후보는 원 C가 이미 변경했던 `Boot_Update_Firmware`의 `.text+0x7110` NOP를 물려받는다. 검증된 부모 A의 원 바이트는 `a1 00 00 54`이며, `ts+604`의 byte를1과 비교한 뒤 `0x7124`로 건너뛰는 조건분기다. 원 분기가 건너뛰는 구간에는 `nvt_support_pen_set(0)` 호출과 해당 byte를0으로 지우는 코드가 있다.

C의 NOP는 이 구간을 조건 없이 실행하게 한다. 이번 초기 worker에서 먼저 성공한 pen 명령과 restore 중 다시 실패한 pen 명령을 구분해야 하는 이유다. 단, `ts+604`의 정확한 vendor 멤버명·당시 값은 이번 자료에서 측정하지 않았고 두 pen 호출이 화면 깨짐의 원인이라는 결론은 아니다.

## 미해결 가설과 반대 근거

| 가설 | 이번 근거 | 아직 부족하거나 반대되는 점 |
|---|---|---|
| warm firmware/restore/IRQ의 준비 상태 전이가 맞지 않는다 | SECOND에만9묶음의 큰 firmware 전송 서명과 reset 회복8회, 반복 FD·checksum 불일치가 관측됨 | 캡처창 안의 상관관계다. 패널·touch 공유 경로의 직접 원인, 최초 controller 상태, 실제 화면 발생 시각은 미측정 |
| pen 초기화와 restore의 재호출/상태 전이가 관련된다 | 초기 worker의 pen 성공 뒤 restore pen 실패, 기존7110 조건 NOP가 남아 있음 | 두 호출의 정확한 상태 변수 값과 vendor 의도 미확정. 명령 실패가 원인인지 이미 생긴 결함의 결과인지 모름 |
| 남아 있는 IRQ grid 복원이 반복 증상을 악화시킨다 | IRQ grid7F가8회 남아 있고 각 복구 루프에서 반복됨 | 첫 FD 접두부는 첫 IRQ grid보다 먼저다. 초기 결함을 이 한 write로만 설명하기 어렵다 |
| panel/KMS 상태나 공유 전원·리셋 순서가 관련된다 | 정상 사진 후 조기 재발이며 정상 restore 시작보다 이른 사용자 보고 | 현재 trace는 touch SPI 중심이며 실제 DSI 영상 신호·패널 register·광학 시각은 관측하지 않음 |

## 다음 PC 작업에서 한 변수만 분리한다면

우선 **현재 e6의 다른 모든 바이트와 `.text+0x56d0` 생략을 고정한 채, `.text+0x7110`의 기존 NOP만 부모 A의 원 조건분기로 복원하는 후보를 PC에서 설계·감사하는 방안**이 구체적이다. 원본 expected bytes, 함수 경계, relocation 없음, 전후 byte diff와 부수 상태 변경을 확인하는 계획이며 이 문서에서 후보 바이너리를 만들거나 시험하지 않았다.

이 선택은 “pen 명령 한 개만 삭제”와 동일하지 않다. 조건분기 복원은 pen 호출과 `ts+604` 초기화 여부를 함께 원래 조건에 돌려놓는다. 당시 해당 byte가 이미1이었다면 실행 경로 차이가 없을 수 있으므로 같은 버전 BTF/원 vendor 소스와 상태 의미를 먼저 확인해야 한다. 과거 A/C 비교가 같은 조건으로 이미 검증됐다면 그 자료 검토를 우선하고 중복 실험을 제안하지 않는다.

다른 독립 후보는 IRQ grid callsite `0x428c` 한 지점만 생략하는 PC 계획이다. 그러나 첫 FD보다 뒤에 나타난 패킷이므로 초기 결함보다 반복 복원 영향의 분리 질문에 가깝다. grid 생략은 명령뿐 아니라 page write·ACK read·20ms 대기·mutex 점유도 바꾸며 필수 설정을 잃을 수 있다. 두 위치를 한꺼번에 변경하면 어떤 차이가 영향을 줬는지 구분할 수 없다.

IRQ enable을 무조건 뒤로 옮기기, 회복 기능을 끄기, 임의 sleep/retry 추가, 서로 main mutex를 다시 잡는 helper 전체를 한 lock으로 감싸기는 현재 자료에서 안전한 수정으로 확정할 수 없다. exact vendor C의 빌드 일치성·비동기 완료·cancel/suspend/error 경로와 lock 순서 검토가 먼저다.

## 자료 한계와 보존

첫/둘 표시는 같은 boot·같은 후보 모듈 파일과 같은 이미지 자산을 사용했지만, viewer 실행파일과 phase 계측은 서로 다르고 장시간 간격 및 USB epoch 변화가 있다. 완전 통제된 A/B 원인 실험으로 다루지 않는다. FIRST에는 같은 kernel stream/phase 자료가 없다.

`stack`은 수집한 SP 값이며 호출 스택이 아니다. TX는최대9바이트, RX는최대10바이트다. 논리 len 밖의 잔여 배열 바이트를 패킷 내용으로 해석하지 않았다. kernel timestamp와 사용자 CLOCK_MONOTONIC의 동일성, 전체 ring/정확한 전체 수집창 이력, 광학 발생 시각은 보장되지 않는다. reference C에는 실제 vendor ELF와 다른 수정도 있어 그대로 동일 소스로 간주하지 않는다.

FIRST 원 host `result.Success=false`는 그대로이고 별도 completion `afcb…`만 성공한 사실을 유지한다. SECOND의 수집·정리 성공과 사용자 광학 실패도 분리한다. 원본4/8개 자료, source/candidate/runtime, 실패 영수증과 intent를 변경하지 않았다. 추가 표시·flash·reboot·장치 쓰기를 허가하는 문서는 아니다. 정확한 byte/relocation 근거는 `source-scope.json`, IO/phase 수치는 `spi-analysis*`와 `phase-analysis*`에 있다.
