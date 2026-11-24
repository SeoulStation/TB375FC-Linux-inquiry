SPI 분석 계수 및 초기 명령 결과 정정

앞선 보고서와 최종답변에서 “체크섬 오류 960건”이라고 쓴 것은 잘못입니다. 함수 이름이 포함된 기록 수를 오류 메시지 수로 셌습니다. 실제 `checksum not match` 메시지는 **80건**입니다. 각 메시지 뒤에 같은 함수 이름이 붙은 덤프 기록 11개가 이어져, 총 80 × (1 + 11) = **관련 기록 960개**가 됩니다. 80개 묶음의 연속 sequence와 전체 960개 포함 관계를 원 metadata로 다시 확인했습니다.

| 실제 메시지 기준 | 건수 | 기록된 caller |
|---|---:|---|
| checksum not match | 80 | T993 |
| Recover for fw reset | 8 | T993 |
| Update firmware success | 9 | T26676 1건 + T993 8건 |
| 0x79 0x00 failed | 8 | T993 |
| 0x7B 0x00 failed | 9 | T26676 1건 + T993 8건 |

이번 실행의 초기 worker T26676 순서는 다음과 같습니다. 다른 부팅의 과거 결과를 적용하지 않았습니다.

1. sequence 935327: `0x7B 0x00 success`.
2. 935328: `nvt_restore_cmd_func++`.
3. 935329: `0xBA success`.
4. 935330: **`0x79 0x00 success`**.
5. 935331: restore 중 **`0x7B 0x00 failed`**.
6. 935332: `nvt_restore_cmd_func--`.
7. 935333: 첫 `checksum not match`.

따라서 이번 초기 worker의 0x79가 실패했다고 해석하면 안 됩니다. 0x7B는 초기 한 번 성공한 뒤 restore에서 실패했고, 전체 실패 17건은 IRQ의 0x79 8건과 0x7B 9건(worker 1건 + IRQ 8건)으로 구분됩니다.

기존 `spi-analysis.*`, 패킷 순서 보충, 독립 8파일 검토 증거는 발행 당시 해시 그대로 보존했습니다. 이 정정 문서는 해당 문서·JSON의 “checksum mismatch 960” 명칭과 수치 해석, 그리고 같은 표현의 앞선 최종답변을 명시적으로 정정합니다. 960이라는 원래 계수는 함수명 관련 기록 수로만 유효합니다. 수정된 실제 mismatch 80건도 모두 viewer ACTIVE 구간 안에서 수신됐습니다. reader 수신 시각을 정확한 kernel 발생 시각으로 바꾸지는 않았습니다.

SPI pre/post 875쌍, firmware reset 8건, 실제 파일 바이트와 검증 통과 결과에는 변화가 없습니다. 화면 재발 방지 실패와 원인 미확정도 그대로입니다. read 길이110의 88회가 checksum80 + recovery8과 정확히 대응한다는 해석은 별도 소스 입증 전에는 하지 않습니다. 원본·소스 변경과 장치 조회는 없었습니다.
