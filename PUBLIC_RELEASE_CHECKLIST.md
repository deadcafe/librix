# Public Release Checklist

Use this checklist before publishing a public release of `librix`.

## Scope

- [ ] Confirm the release scope (`librix`, `flowtable`, or both)
- [ ] CI passes for `gcc` and `clang`
- [ ] Confirm the release tag (for example `v0.1.1`)
- [ ] Confirm what is considered stable vs experimental

## Documentation

- [ ] README describes the project at a glance
- [ ] README includes build and test commands
- [ ] README includes the current validation status
- [ ] README includes the supported sample variants (`flow4`, `flow6`, `flowu`)
- [ ] `CHANGELOG.md` is updated for the release
- [ ] `RELEASE_NOTES_vX.Y.Z.md` is updated or copied to the actual release note

## Validation

- [ ] `make -C flowtable/test test`
- [ ] `make -C flowtable/test bench`
- [ ] GCC build confirmed
- [ ] Clang build confirmed
- [ ] Generic path tested
- [ ] AVX2 path tested
- [ ] AVX-512 status explicitly documented

## API / Compatibility

- [ ] New public APIs are documented
- [ ] Experimental APIs are labeled as such if needed
- [ ] Public headers build cleanly in a minimal external include test
- [ ] `git diff --check` is clean

## Repository Hygiene

- [ ] No generated binaries are staged unintentionally
- [ ] No local-only paths remain in public-facing docs
- [ ] License file and copyright headers are present
- [ ] The repository root is in a clean state before tagging

## Release Assets

- [ ] GitHub repository description updated
- [ ] GitHub topics updated
- [ ] Short release summary prepared
- [ ] Validation status copied into the release body
- [ ] Known limitations copied into the release body

## Suggested Known Limitations

- [ ] AVX-512 is implemented but not yet validated on AVX-512 hardware
- [ ] Most tuning so far focuses on `flow4`
- [ ] `flow6` and `flowu` are functionally covered, but have seen less performance tuning
