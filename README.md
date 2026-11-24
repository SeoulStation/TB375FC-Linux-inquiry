# TB375FC Linux display-resume investigation

> 사용자 승인에 따라 Codex가 자동으로 게시했습니다.
> Automatically posted by Codex with the user's authorization.

**UNRESOLVED:** vertical brightness bands on a later display activation in the same Linux boot. Latest observation: 2026-11-24 KST. Logs were recovered successfully, but visual stability failed.

This is a developer inquiry and selected evidence/code snapshot, not an installable Linux distribution or a proven fix.

## Read first

1. [English inquiry](QUESTION.en.md) — environment, failure, observations, attempted changes and questions.
2. [한국어 질문](QUESTION.ko.md) and [전체 개발일지](TB375FC-development-diary-20261124.ko.md).
3. [Evidence index](EVIDENCE-INDEX.md) — where each fact comes from.
4. [Code scope](code/README.md) — included files, dependencies and limits.
5. [Publication contents](PUBLICATION-CONTENTS.md) and [source provenance](PROVENANCE.json).

## Verify offline

From this directory, with Python 3.9 or later:

```text
python tools/verify_bundle.py
```

The verifier reads these local files only. It verifies SHA256SUMS and checks selected log counts and phase values. It does not open USB, access the tablet, flash, compile or execute the historical device tools.

`evidence/first` contains 4 original logs; `evidence/second` contains 8. Their bytes are unchanged from the supplied inquiry package. The original FIRST host result failed; separate completion recovery passed. SECOND recovery/cleanup passed while the user's optical observation failed. See `evidence/summary.json`.

The exact matching vendor driver C source is not included. No fresh hardware experiment was performed to prepare this publication. Working C/assembly/Python files are provided for review with their original dependencies and boot-specific bindings; they are not a standalone build.

For posting instructions, see [GitHub 사용 안내](GITHUB-UPLOAD.ko.md). A public repository is a place to host evidence; it does not automatically notify relevant developers.

## Historical optical photos

[Gallery, contact sheets and all 30 original photographs](evidence/photos-20261101/README.md) from the 2026-11-01 A candidate **third activation** are available. This is a separate historical trial, not the 2026-11-24 FIRST/SECOND comparison. The separately supplied frames 028 and 029 are included once each, with original bytes preserved.
