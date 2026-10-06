# Project instructions

Read the user's current request and README before making changes.

- Follow the approved UI mockup at `ui/v32_ui_mockup.png`. Keep its left sidebar and all eight labelled navigation items. Ask before a fundamental layout change or any departure from the requested design.
- Before publishing a version, compare the actual rendered result with the user's request and the mockup. Review screenshots and check the requested behavior. If there is doubt about meeting the request, ask the user before publishing.
- When a request is UI-only, preserve firmware behavior, Bluetooth protocols, RF generation/timing, scheduling, timezone calculations, configuration and API handlers. Only the UI asset and required version metadata may change in firmware; verify that scope by comparing sources.
- Bump the version for each released change. Keep the active sketch, UI labels, build/test paths and current documentation consistent.
- Publish source only unless compiled packages are explicitly requested.
- After publishing, provide the direct GitHub tag ZIP link and verify that it downloads the tested source.
- Preserve original code credits and existing user edits.

Use `python3 scripts/embed-ui.py` after UI changes. Run `bash scripts/test.sh`, appropriate browser checks and a local compile check. Use existing cloud checkouts and the retained toolchain; do not create a worktree unless requested.
