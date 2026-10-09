# Project instructions

Read the user's current request and README before making changes.

- Follow the approved UI mockup at `ui/v32_ui_mockup.png`. Keep its left sidebar and all eight labelled navigation items. Ask before a fundamental layout change or any departure from the requested design.
- Before publishing a version, compare the actual rendered result with the user's request and the mockup. Review screenshots and check the requested behavior. If there is doubt about meeting the request, ask the user before publishing.
- After every code change or new feature, complete a fresh embedded reliability review before publishing. Check bounded memory/stack use, allocation failures, task and callback lifetimes, timing/deadline/rollover behavior, RF/BLE exclusion, Wi-Fi/NTP recovery, durable configuration and flash-write frequency. Address new regressions, run appropriate fault-injection tests, and record evidence and remaining physical-test limits in the release review. Do not substitute periodic reboots or flash logs for recovery.
- When a request is UI-only, preserve firmware behavior, Bluetooth protocols, RF generation/timing, scheduling, timezone calculations, configuration and API handlers. Only the UI asset and required version metadata may change in firmware; verify that scope by comparing sources.
- Bump the version for each released change. Keep the active sketch, UI labels, build/test paths and current documentation consistent.
- Publish source only unless compiled packages are explicitly requested.
- Follow the user's release order: complete the reliability review, push/publish the source version to GitHub, then run the full test suite. If that suite finds an issue, fix it, increment the build revision (R2, R3, etc.), review the correction and publish that revision before running the full suite again. Continue until checks pass, and report the actual validation status clearly. Preserve previous immutable version/revision tags.
- After publishing, provide the direct GitHub tag ZIP link and verify that it downloads the tested source.
- Publish a complete changelog for every version/build revision as GitHub release notes, including features, bug fixes, dependency/installation changes, validation status and known limitations. Update validation results after the post-publication suite; link source ZIPs and retain prior revision notes.
- For every future release, maintain a newest-first version/build changelog at the top of the active Arduino `.ino` sketch so it is visible immediately in the IDE. Include the release date, features, bug fixes and dependency/installation changes; match the GitHub release notes, retain prior version notes and preserve original author credits. Check this header before publishing.
- Preserve original code credits and existing user edits.

Use `python3 scripts/embed-ui.py` after UI changes. Run `bash scripts/test.sh`, appropriate browser checks and a local compile check. Use existing cloud checkouts and the retained toolchain; do not create a worktree unless requested.
