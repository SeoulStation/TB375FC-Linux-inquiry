# GitHub 업로드 안내

GitHub에서 파일을 보관하는 공간은 **Repository(저장소)**, 해결할 문제를 적고 댓글을 받는 글은 **Issue(이슈)**, 일반 질문·의견 교환 게시판은 **Discussions(토론)**라고 부릅니다.

이번에는 본인 계정의 공개 저장소 `TB375FC-Linux-inquiry`에 자료를 올린 뒤 그 저장소에 질문 Issue 하나를 작성하는 구성이 간단합니다. 관련 외부 프로젝트에 질문할 때는 그 프로젝트가 Discussions/Q&A를 제공하는지와 지원 범위를 먼저 확인하고, 같은 자료의 링크를 포함하면 됩니다. 본인 저장소를 만들기만 해서는 다른 개발자에게 알림이 가지 않습니다.

## 1. 저장소 만들기

[새 저장소 만들기](https://github.com/new)에서 Owner를 본인 계정, Repository name을 `TB375FC-Linux-inquiry`, Description을 `Lenovo TB375FC native Linux display resume investigation: logs and selected working code`로 지정합니다. 외부 개발자가 볼 수 있도록 Public을 선택합니다. README는 이 묶음에 있으므로 자동 생성하지 않아도 됩니다. 이 자료 전체에 적용할 새 라이선스는 임의로 지정하지 않습니다.

## 2. 파일 올리기

ZIP을 먼저 압축 해제합니다. 저장소의 `uploading an existing file` 또는 `Add file → Upload files`를 열고, 압축을 푼 `TB375FC-Linux-inquiry` 폴더 **안의 파일과 하위 폴더**를 업로드합니다. README가 저장소 최상단에 오도록 합니다. ZIP 한 개만 Code 탭에 올리면 파일별 열람이 불편합니다.

Commit message는 `Add TB375FC display-resume inquiry, evidence and selected code`로 작성하고 Commit changes를 누릅니다.

## 3. 질문 글 작성

저장소의 `Issues → New issue`를 엽니다. 제목은 `POST-TITLE.txt`의 한 줄을 사용합니다. 본문에는 아래 안내문을 먼저 넣고 `QUESTION.en.md`의 내용을 붙여 넣습니다. 한국어 개발자에게 물을 때는 `QUESTION.ko.md`를 사용해도 됩니다.

```text
Supporting logs, development diary and selected working code are available in this repository's README and evidence/ directory.
```

외부 저장소의 Issue/Discussion에 질문한다면 위 문장의 `this repository`를 본인 자료 저장소의 실제 URL로 바꿉니다. 본문 아래에 ZIP도 첨부할 수 있습니다. 업로드한 파일 목록과 최종 본문을 확인한 뒤 게시합니다.

## 이번 준비에서 확인한 상태

GitHub 연결 계정은 `SeoulStation`입니다. 연결 도구의 저장소 검색은 HTTP 422/권한 범위 오류로 실패했으며, 이것만으로 계정에 저장소가 없다고 판단하지 않았습니다. 브라우저 프로필에서는 로그인 상태와 공개 저장소가 없다는 안내를 확인했습니다. 이 안내와 로컬 ZIP의 생성은 실제 GitHub 게시 완료를 의미하지 않습니다.

공식 안내: [저장소 만들기](https://docs.github.com/en/repositories/creating-and-managing-repositories/creating-a-new-repository), [Issue 만들기](https://docs.github.com/en/issues/tracking-your-work-with-issues/using-issues/creating-an-issue), [Discussions 안내](https://docs.github.com/en/discussions/collaborating-with-your-community-using-discussions/about-discussions).
