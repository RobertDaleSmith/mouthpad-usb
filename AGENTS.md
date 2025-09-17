# Repository Guidelines

## Project Structure & Module Organization
- `app/src/` carries the firmware modules (BLE transport, USB HID, inputs); pair each new driver with a matching header.
- `app/prj.conf` and `CMakeLists.txt` hold Zephyr configuration, while optional board overlays sit beside the sources they affect.
- `docs/` stores build notes and assets; `scripts/`, `tools/`, the `Dockerfile`, and `docker-compose.yml` provide repeatable workflows.
- Dependency trees (`modules/`, `zephyr/`, `nrf/`, `nrfxlib/`) are managed by `west` and should remain untouched except through updates.

## Build, Test, and Development Commands
- `docker-compose run --rm mouthpad-build` rebuilds both supported boards inside a containerized Zephyr environment.
- `docker-compose run --rm mouthpad-dev` drops you into an interactive shell with the toolchain and `west` installed.
- `make build BOARD=xiao_ble` (or `adafruit_feather_nrf52840`) wraps `west build -b <board> app --pristine=always`; results land in `build/app`.
- `make flash`, `make flash-uf2`, and `make monitor` program hardware via J-Link, UF2 mass storage, or RTT logging respectively.
- For lighter iterations, invoke `west build -b xiao_ble app` directly and clean with `rm -rf build` when switching boards.

## Coding Style & Naming Conventions
- Follow Zephyr C style: tabs for indentation, 100-character lines, `UPPER_SNAKE_CASE` macros, and module-prefixed statics such as `ble_transport_*`.
- Keep headers limited to public interfaces, declare file-local helpers as `static`, and minimize cross-module coupling.
- Run `clang-format -i <files>` from the repo root so the Zephyr `.clang-format` is honored before sending patches.
- Log through `LOG_INF`, `LOG_DBG`, and friends with concise, hardware-focused phrasing.

## Testing Guidelines
- After flashing, confirm BLE pairing and USB enumeration on a host; capture RTT or USB CDC logs for regressions.
- Place automated checks under `app/tests` with Zephyr `ztest`; reuse the `test/cmock` utilities for mocking hardware seams.
- Execute `west twister -p native_posix_64 --testsuite-root app/tests` to run suites locally and gate pull requests.

## Commit & Pull Request Guidelines
- Adopt Conventional Commit prefixes (`feat`, `fix`, `chore`, `docs`, etc.) consistent with the current history.
- Keep commits focused, exclude generated artefacts (`build/`, `*.uf2`), and ensure `west.yml` changes are intentional.
- PRs should outline tested hardware, configuration implications, and include logs or screenshots when touching DFU or user-visible flows.

## DFU & Hardware Tips
- Copy `build/app/zephyr/app.uf2` to the board’s storage volume (double-tap reset first) for UF2 updates.
- When using J-Link, verify probe firmware and power stability before `make flash`; erratic RTT output usually signals cabling issues.
