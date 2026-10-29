# ronin 1.0.1: the release checklist

For the owner, in this order, from the release pull request's branch `chore/45-release-1.0.1` to the published
release. Step numbers in "tests" are the HUMAN steps of docs/MANUAL_TESTS.md, whose full text is there.

**The zip that is published is the one release-check.bat made and tested.** Its dist step makes
dist\ronin-1.0.1-windows-x64.zip and dist\SHA256SUMS.txt on the branch's last commit, and its zip and e2e steps (all
eleven scenarios) run on that very file. So from the start of step 1 to the end of step 8 nothing may rebuild them: **no
`build.bat release`, `verify.bat`, `dist.bat` or `build.bat clean`** (each replaces build\ronin.exe or dist\). Step 2's
`release-check.bat quick` builds only the debug exe and only reads dist\. The zip is not byte-reproducible (the link
time, the file times), so a rebuilt one would not be the file that was tested.

Filled in on 2026-10-29 (#48) from the maintainer's report, release-check's log and GitHub; steps 2 and 3 were done
earlier, on code that has not changed since.

| step | tests | what to do | status |
|---|---|---|---|
| 1 | AF1 | `release-check.bat` (the full run, about 13 minutes, the machine left alone) on the branch's last commit, pushed: the last line exactly `release-check: PASS` (not "(dry run)", not "(quick)"). Write here its log's name and its dist line, `dist: PASS: dist\ronin-1.0.1-windows-x64.zip (<bytes> bytes, SHA-256 <hash>)`. | done: build\release-check\20261028-211200.log, on abc4172 (origin's chore/45-release-1.0.1), last line `release-check: PASS`; `dist: PASS: dist\ronin-1.0.1-windows-x64.zip (264468 bytes, SHA-256 9b5dd1bcacfdb6f36f9aaf4d8a91eab5398310b337db30ea3492867766729b98)` |
| 2 | 69 (AF13) | As MANUAL_TESTS gives it, in a cmd window in the repository's root: `release-check.bat quick`, then the start-close line. It leaves the zip unpacked in `build\tmp\release-check\zip\İndirilenler ronin\ronin-1.0.1\`. | done: the maintainer, 2026-10-25; the code it covers unchanged since |
| 3 | 67 (AB11) | #1's double click with that folder's ronin.exe (the exe being shipped) on ronin's repository with uncommitted changes in two or more files, one changed further down than its first screen: C-x g, at once a double click on a file further down: it opens at its first change, not at line 1. Then undo those changes (`git checkout -- <files>`): step 5 needs a clean tree. | done: the maintainer, 2026-10-22; the code it covers unchanged since |
| 4 | 72 (AH15) | Its four parts with that folder's ronin.exe: two lines pasted into Notepad; text copied in a browser yanked; ronin.conf saved from Notepad with `theme = vague` applied at once, no flash (put the line back); C-S-p `find` RET and C-S-p ESC on the physical keyboard. | done: the maintainer, before the merge of #46 |
| 5 | | The pull request merged (Rebase and merge, after the `build and test` check). Then `git switch main`, `git pull --ff-only`, and `git diff --quiet <step 1's commit> HEAD`: exit 0, main's new HEAD has exactly the tree release-check passed (the rebase gives the commits new hashes, not new contents). | done: #46 merged 2026-10-28 (Rebase and merge); TREE OK: abc4172 against main's HEAD a9e3d31 |
| 6 | AA8 | The tag on main's HEAD: `git tag -a v1.0.1 -m "ronin 1.0.1"`, `git push origin v1.0.1`. | done: the annotated tag v1.0.1 ("ronin 1.0.1") on a9e3d31, on origin |
| 7 | | Before the upload, nothing rebuilt: `Get-FileHash dist\ronin-1.0.1-windows-x64.zip` and the line of dist\SHA256SUMS.txt both give the SHA-256 of step 1's dist line. | done: the maintainer, the zip's hash checked before the upload |
| 8 | AA9 | The GitHub release from tag v1.0.1: title "ronin 1.0.1", the text of docs/releases/1.0.1.md with the zip's size and its SHA-256 (both from step 1's dist line) added under Download, dist\ronin-1.0.1-windows-x64.zip and dist\SHA256SUMS.txt attached, set as the latest release. | done: "ronin 1.0.1" published 2026-10-28 19:37 UTC from tag v1.0.1, the text of docs/releases/1.0.1.md with "ronin-1.0.1-windows-x64.zip: 264,468 bytes, SHA-256 9b5dd1bcacfdb6f36f9aaf4d8a91eab5398310b337db30ea3492867766729b98" under Download (checked by the maintainer in the preview), the zip and SHA256SUMS.txt attached, the latest release (GitHub's releases/latest: v1.0.1) |
| 9 | 68 (AC4) | Once GitHub Pages has published the merge: https://bskasan.github.io/ronin-text-editor/changelog.html#changed-1, and CHANGELOG.md on GitHub with `#changed-1`: both land on 1.0.0's "Changed", the second one on the page. | done: the maintainer, after publishing |
| 10 | AA7, AA11 | The zip downloaded from the release page with a browser, into a new folder: `Get-FileHash` equals SHA256SUMS.txt and the release text; Explorer's Extract All, double-click ronin.exe: SmartScreen, More info, Run anyway, "*scratch* - ronin" opens; `ronin.exe --version` in cmd prints "ronin 1.0.1". | done: the maintainer: the downloaded zip's Get-FileHash equal; SmartScreen, then "*scratch* - ronin"; `--version` printed "ronin 1.0.1" |
| 11 | AA10 | The Releases page lists ronin 1.0.1 with both files; README's release badge says v1.0.1. | done: the maintainer: the release with both files, README's badge v1.0.1 |

After step 11, `build.bat clean` may run again (it removes dist\).
