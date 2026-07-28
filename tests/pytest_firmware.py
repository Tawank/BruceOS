import re

from pytest_embedded import Dut


def test_firmware_selftest(dut: Dut) -> None:
    dut.expect_exact("SELFTEST READY", timeout=15)

    dut.write("selftest\r\n")

    status_pattern = re.compile(
        rb"(\[selftest\] selftest__\w+ (?:PASS|FAIL)|SELFTEST PASS|SELFTEST FAIL|Guru Meditation Error|assert failed)"
    )
    while True:
        result = dut.expect(status_pattern, timeout=60)
        output = result.group(1).decode()
        if not output.startswith("[selftest]"):
            break

    assert output == "SELFTEST PASS", f"Firmware returned: {output}"
