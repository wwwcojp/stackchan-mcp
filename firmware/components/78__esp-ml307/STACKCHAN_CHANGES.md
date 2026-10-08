# StackChan changes to the vendored esp-ml307

This directory is an unchanged copy of the managed component `78/esp-ml307` 3.6.7
(`repository_info.commit_sha: 40d99e50367034832a6790c17832ca2cb5b91f8b`), taken from the
stage-5 prototype commit ee74637 of the fork without `.component_hash` and `CHECKSUMS.json`.
It is vendored so that FW-A2 can change it (design `docs/superpowers/specs/2026-10-07-fw-a2-design.md`
§4.2 in stackchan-works). `main/idf_component.yml` still asks for `~3.6.5`; the component
manager resolves it to this local copy (`dependencies.lock`: `type: local`).

## Changes

None yet. FW-A2 plan 2B adds design §4.2 changes 1-7 here, one entry each.
