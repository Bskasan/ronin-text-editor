**What and why**

<!-- The issue's summary in a few sentences. -->

Issue: #
Closes #

**Checklist** (CONTRIBUTING.md in the repository explains each line)

- [ ] Branch `<type>/<issue>-<slug>` from an up-to-date `main`; commit messages `<type>(<scope>): <summary> (#<issue>)`, one logical change each
- [ ] `verify.bat` prints `verify: PASS`; its last line:
- [ ] `verify.bat phase` passes (the full tests, the seed sweep, the soak, the leak check), unless a small fix
- [ ] `/W4 /WX` clean; no C runtime functions; no third-party code
- [ ] `res/imports.txt` unchanged (no new DLL)
- [ ] New source files start with the standard header, their description in docs/ARCHITECTURE.md's module map (`verify.bat`'s source check)
- [ ] A new feature is a named command with a one-line description
- [ ] Tests for the change, and how I saw them fail without it:
- [ ] README.md, the site's pages (`docs/src`, then `build.bat docs`), docs/MANUAL_TESTS.md and docs/ARCHITECTURE.md updated where they describe what changed
- [ ] CHANGELOG.md's "Unreleased" section updated, for a change a user sees, installs or configures
- [ ] A screenshot, for a visual change
