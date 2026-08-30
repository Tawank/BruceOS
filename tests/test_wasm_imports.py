import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parents[1] / "native_apps" / "tools"))

from wasm_imports import WasmImportError, validate_bruce_sdk_imports


def leb(value):
    result = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        result.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(result)


def section(section_id, payload):
    return bytes((section_id,)) + leb(len(payload)) + payload


def text(value):
    encoded = value.encode()
    return leb(len(encoded)) + encoded


def module_with_import(name="runtime__delay", module="bruce_sdk", parameters=(0x7F,), results=(0x7F,), kind=0):
    function_type = b"\x60" + leb(len(parameters)) + bytes(parameters) + leb(len(results)) + bytes(results)
    type_section = section(1, leb(1) + function_type)
    imported = text(module) + text(name) + bytes((kind,)) + leb(0)
    import_section = section(2, leb(1) + imported)
    return b"\0asm\x01\0\0\0" + type_section + import_section


def test_accepts_exact_bruce_import():
    validate_bruce_sdk_imports(module_with_import())


@pytest.mark.parametrize("module", ("env", "wasi_snapshot_preview1"))
def test_rejects_other_import_modules(module):
    with pytest.raises(WasmImportError, match="unsupported import module"):
        validate_bruce_sdk_imports(module_with_import(module=module))


def test_rejects_unknown_symbol():
    with pytest.raises(WasmImportError, match="unsupported Bruce SDK import"):
        validate_bruce_sdk_imports(module_with_import(name="not__a_real_symbol"))


def test_rejects_signature_mismatch():
    with pytest.raises(WasmImportError, match="signature mismatch"):
        validate_bruce_sdk_imports(module_with_import(parameters=()))


def test_rejects_non_function_import():
    with pytest.raises(WasmImportError, match="non-function import"):
        validate_bruce_sdk_imports(module_with_import(kind=1))


@pytest.mark.parametrize("wasm", (b"", b"\0asm\x01\0\0\0\x01\x80", b"\0asm\x01\0\0\0\x01\x80\x80\x80\x80\x10"))
def test_rejects_malformed_binary(wasm):
    with pytest.raises(WasmImportError):
        validate_bruce_sdk_imports(wasm)


def test_accepts_data_count_before_code():
    wasm = module_with_import() + section(12, leb(0)) + section(10, leb(0)) + section(11, leb(0))
    validate_bruce_sdk_imports(wasm)


@pytest.mark.parametrize("feature", ("reference-types", "call-indirect-overlong"))
def test_rejects_unsupported_wamr_features(feature):
    target_features = text("target_features") + leb(1) + b"+" + text(feature)
    wasm = module_with_import() + section(0, target_features)
    with pytest.raises(WasmImportError, match="unsupported WAMR feature"):
        validate_bruce_sdk_imports(wasm)
