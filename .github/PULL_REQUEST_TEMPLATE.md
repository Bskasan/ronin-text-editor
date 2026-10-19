**What and why**


Closes #

**Checklist** (CONTRIBUTING.md in the repository explains each line)

- [ ] `verify.bat` prints `verify: PASS`; its last line:
- [ ] `verify.bat phase` passes (the full tests, the seed sweep, the soak, the leak check)
- [ ] `/W4 /WX` clean; no C runtime functions; no third-party code
- [ ] `res/imports.txt` unchanged (no new DLL)
- [ ] A new feature is a named command with a one-line description
- [ ] Tests for the change, and how I saw them fail without it:
- [ ] README.md, docs/MANUAL_TESTS.md and docs/ARCHITECTURE.md updated where they describe what changed
- [ ] Commit messages are `<type>(<scope>): <summary>`, one logical change each
- [ ] A screenshot, for a visual change
