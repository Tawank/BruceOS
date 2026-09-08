import re

from pytest_embedded import Dut


def test_firmware_selftest(dut: Dut) -> None:
    # The reconfigure (if any) that made this build's boot command
    # "selftest" (or "selftest <filter>...") already happened in the
    # _reconfigure_boot_command autouse fixture, before this `dut` fixture
    # flashed and booted build-qemu/'s image -- main.c launches that command
    # itself right after "QEMU READY", so there's nothing left to type here.
    dut.expect_exact("QEMU READY", timeout=60)

    status_pattern = re.compile(
        rb"(\[selftest\] \S+ (?:PASS|FAIL)|SELFTEST PASS|SELFTEST FAIL|Guru Meditation Error|assert failed)"
    )
    while True:
        result = dut.expect(status_pattern, timeout=60)
        output = result.group(1).decode()
        if not output.startswith("[selftest]"):
            break

    assert output == "SELFTEST PASS", f"Firmware returned: {output}"
