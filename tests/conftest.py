import subprocess
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]
BUILD_DIR = REPO_ROOT / "build-qemu"
BOOT_COMMAND_CONFIG = "CONFIG_BRUCE_QEMU_TEST_BOOT_COMMAND"


def pytest_addoption(parser):
    parser.addoption(
        "--selftest-filter",
        action="append",
        default=[],
        metavar="TEXT",
        help="Only run selftest cases whose name contains TEXT (case-insensitive). "
        "Repeatable -- e.g. --selftest-filter=args --selftest-filter=notification runs "
        "every case mentioning either (see `selftest --list` on the device, or the "
        "selftest__cases table in selftest.c, for the names to filter on). Default: run "
        "the full suite.",
    )


def pytest_configure(config):
    config.addinivalue_line(
        "markers", "docs: generates a doc file as a side effect instead of asserting firmware behavior"
    )


def _current_boot_command() -> str | None:
    sdkconfig = BUILD_DIR / "sdkconfig"
    if not sdkconfig.exists():
        return None
    prefix = f"{BOOT_COMMAND_CONFIG}="
    for line in sdkconfig.read_text().splitlines():
        if line.startswith(prefix):
            return line[len(prefix) :].strip().strip('"')
    return None


def _ensure_boot_command(command: str) -> None:
    """Edits build-qemu/sdkconfig and rebuilds so CONFIG_BRUCE_QEMU_TEST_BOOT_COMMAND
    matches `command`, the exact line main.c hands to app_runner__run_command()
    right after "QEMU READY" -- but only when it doesn't already match. The
    QEMU serial chardev only emulates the guest-to-host (TX) direction in
    this configuration (confirmed experimentally: bytes written to QEMU's
    stdin never reach the firmware's UART RX), so there's no way to type a
    command into the booted shell the way a person at a real console would;
    this build-time override is what stands in for that. Most runs want
    nothing but the Kconfig default ("selftest") and pay no rebuild at all.
    """
    if _current_boot_command() == command:
        return
    sdkconfig = BUILD_DIR / "sdkconfig"
    prefix = f"{BOOT_COMMAND_CONFIG}="
    escaped = command.replace("\\", "\\\\").replace('"', '\\"')
    new_line = f'{prefix}"{escaped}"'
    lines = sdkconfig.read_text().splitlines()
    replaced = False
    for i, line in enumerate(lines):
        if line.startswith(prefix):
            lines[i] = new_line
            replaced = True
            break
    if not replaced:
        # Fresh build-qemu/, or CONFIG_BRUCE_QEMU_TEST_MODE wasn't on yet in
        # this sdkconfig -- either way, `idf.py build` below regenerates
        # sdkconfig from Kconfig defaults plus this file's contents, so
        # appending here is enough for confgen to pick it up.
        lines.append(new_line)
    sdkconfig.write_text("\n".join(lines) + "\n")
    # A build.py `build` (not `reconfigure`) is enough: CMake tracks
    # sdkconfig as a configure-time dependency and reruns confgen (which
    # regenerates sdkconfig.h from this edited file) automatically when it
    # sees this file changed -- confirmed empirically; `idf.py -D ...`
    # does NOT work here since an existing sdkconfig value already on disk
    # takes precedence over a `-D`-supplied one, which only seeds a value
    # that isn't set yet.
    subprocess.run(["idf.py", "-B", str(BUILD_DIR), "build"], cwd=REPO_ROOT, check=True)


@pytest.fixture(autouse=True)
def _reconfigure_boot_command(request):
    # Autouse fixtures run before explicitly-requested fixtures of the same
    # scope (both are function-scoped here), so this always reconfigures and
    # rebuilds -- if needed -- before pytest-embedded's `dut` fixture flashes
    # and boots build-qemu/'s current image.
    if request.node.get_closest_marker("docs") is not None:
        command = "man --gen-md --line-marker"
    else:
        filters = request.config.getoption("--selftest-filter")
        command = " ".join(["selftest", *filters])
    _ensure_boot_command(command)
