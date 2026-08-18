"""Validate the restricted Bruce WebAssembly import ABI."""

from dataclasses import dataclass

I32 = 0x7F
I64 = 0x7E
UNSUPPORTED_WAMR_FEATURES = {"call-indirect-overlong", "reference-types"}


class WasmImportError(RuntimeError):
    pass


@dataclass(frozen=True)
class FunctionType:
    parameters: tuple[int, ...]
    results: tuple[int, ...]


SUPPORTED_BRUCE_SDK_SIGNATURES = {
    "runtime__now": FunctionType((), (I64,)),
    **{name: FunctionType((I32,), (I32,)) for name in ("runtime__sleep", "runtime__delay")},
    **{name: FunctionType((), (I32,)) for name in (
        "runtime__gui_requested", "process__current_id", "process__current_signal",
        "process__switch_next", "process__switch_previous", "process__to_background",
        "process__to_foreground", "config__get_time_clock24hr", "config__get_color_primary",
        "config__get_color_secondary", "config__get_color_background", "config__get_color_surface",
        "config__get_color_text", "config__get_color_text_muted", "config__get_color_border",
        "config__get_color_success", "config__get_color_warning", "config__get_color_error",
        "display__width", "display__height", "display__begin_frame", "display__present",
        "input__flush",
    )},
    **{name: FunctionType((I32,), (I32,)) for name in (
        "process__foreground", "process__terminate", "process__pause", "process__resume",
        "process__kill", "permission__check", "stdio__session_create", "stdio__session_close",
        "stdio__session_route_children", "memory__get_stats", "display__fill_screen",
        "display__set_text_bg_color", "display__set_text_color", "display__set_text_size",
        "clock__get_local", "memory__malloc",
    )},
    **{name: FunctionType((I32, I32), (I32,)) for name in (
        "process__signal", "process__wait", "stdio__write", "process__snapshot", "input__wait",
    )},
    **{name: FunctionType((I32, I32, I32), (I32,)) for name in (
        "stdio__read_line", "stdio__session_write_input", "display__draw_centre_string",
        "dialog__message",
    )},
    **{name: FunctionType((I32, I32, I32, I32), (I32,)) for name in (
        "stdio__read", "stdio__session_read_output",
    )},
    **{name: FunctionType((I32, I32, I32, I32, I32), (I32,)) for name in (
        "display__draw_rect", "dialog__choice", "dialog__number_input",
    )},
    "memory__free": FunctionType((I32,), ()),
    "storage__open": FunctionType((I32, I32, I32), (I32,)),
    "storage__read": FunctionType((I32, I32, I32, I32), (I32,)),
    "storage__write": FunctionType((I32, I32, I32, I32), (I32,)),
    "storage__seek": FunctionType((I32, I64, I32, I32), (I32,)),
    "storage__close": FunctionType((I32,), (I32,)),
    "dialog__pick_file": FunctionType((I32, I32, I32, I32), (I32,)),
    "display__game_mode": FunctionType((I32,), (I32,)),
    "display__color565": FunctionType((I32, I32, I32), (I32,)),
    "display__fill_rect": FunctionType((I32, I32, I32, I32, I32), (I32,)),
    "display__draw_rgb_bitmap": FunctionType((I32, I32, I32, I32, I32), (I32,)),
    "input__read": FunctionType((I32, I32), (I32,)),
    "audio__stream_sample_rate": FunctionType((), (I32,)),
    "audio__stream_open": FunctionType((I32,), (I32,)),
    "audio__stream_write": FunctionType((I32, I32), (I32,)),
    "audio__stream_close": FunctionType((), (I32,)),
    "runtime__timer_start": FunctionType((I32, I32, I32), (I32,)),
    "runtime__timer_wait": FunctionType((I32, I32), (I32,)),
    "runtime__timer_stop": FunctionType((I32,), (I32,)),
}


class _Reader:
    def __init__(self, data):
        self.data = data
        self.offset = 0

    def read(self, size):
        end = self.offset + size
        if size < 0 or end > len(self.data):
            raise WasmImportError("truncated WebAssembly data")
        value = self.data[self.offset:end]
        self.offset = end
        return value

    def u8(self):
        return self.read(1)[0]

    def u32(self):
        value = 0
        for index in range(5):
            byte = self.u8()
            if index == 4 and byte & 0xF0:
                raise WasmImportError("u32 LEB128 overflow")
            value |= (byte & 0x7F) << (index * 7)
            if not byte & 0x80:
                return value
        raise WasmImportError("overlong u32 LEB128")

    def text(self):
        try:
            return self.read(self.u32()).decode("utf-8")
        except UnicodeDecodeError as error:
            raise WasmImportError("invalid UTF-8 import name") from error

    def done(self):
        return self.offset == len(self.data)


def _vector(reader, item):
    return tuple(item(reader) for _ in range(reader.u32()))


def _value_type(reader):
    value = reader.u8()
    if value not in (I32, I64, 0x7D, 0x7C, 0x7B, 0x70, 0x6F):
        raise WasmImportError(f"invalid WebAssembly value type 0x{value:02x}")
    return value


def _function_type(reader):
    if reader.u8() != 0x60:
        raise WasmImportError("invalid WebAssembly function type")
    return FunctionType(_vector(reader, _value_type), _vector(reader, _value_type))


def _validate_target_features(reader):
    for _ in range(reader.u32()):
        prefix = reader.u8()
        feature = reader.text()
        if prefix not in (ord("+"), ord("-")):
            raise WasmImportError("invalid WebAssembly target feature prefix")
        if prefix == ord("+") and feature in UNSUPPORTED_WAMR_FEATURES:
            raise WasmImportError(f"unsupported WAMR feature {feature}")
    if not reader.done():
        raise WasmImportError("trailing WebAssembly target-features data")


def validate_bruce_sdk_imports(wasm):
    reader = _Reader(wasm)
    if reader.read(4) != b"\0asm" or reader.read(4) != b"\x01\0\0\0":
        raise WasmImportError("invalid WebAssembly header")
    sections = {}
    last_standard_rank = 0
    section_ranks = {section_id: section_id for section_id in range(1, 10)}
    section_ranks.update({12: 10, 10: 11, 11: 12})
    while not reader.done():
        section_id = reader.u8()
        payload = _Reader(reader.read(reader.u32()))
        if section_id == 0:
            if payload.text() == "target_features":
                _validate_target_features(payload)
        else:
            rank = section_ranks.get(section_id)
            if rank is None or rank < last_standard_rank:
                raise WasmImportError("invalid WebAssembly section order")
            if section_id in sections:
                raise WasmImportError(f"duplicate WebAssembly section {section_id}")
            sections[section_id] = payload
            last_standard_rank = rank
    types = ()
    if 1 in sections:
        types = _vector(sections[1], _function_type)
        if not sections[1].done():
            raise WasmImportError("trailing WebAssembly type-section data")
    imports = sections.get(2)
    if imports is None:
        return
    seen = set()
    for _ in range(imports.u32()):
        module = imports.text()
        name = imports.text()
        if imports.u8() != 0:
            raise WasmImportError(f"non-function import {module}.{name}")
        type_index = imports.u32()
        if type_index >= len(types):
            raise WasmImportError(f"invalid type index for import {module}.{name}")
        identity = (module, name)
        if identity in seen:
            raise WasmImportError(f"duplicate import {module}.{name}")
        seen.add(identity)
        if module != "bruce_sdk":
            raise WasmImportError(f"unsupported import module {module}.{name}")
        expected = SUPPORTED_BRUCE_SDK_SIGNATURES.get(name)
        if expected is None:
            raise WasmImportError(f"unsupported Bruce SDK import {name}")
        if types[type_index] != expected:
            raise WasmImportError(f"signature mismatch for Bruce SDK import {name}")
    if not imports.done():
        raise WasmImportError("trailing WebAssembly import-section data")


def validate_bruce_module(wasm):
    """Validate imports plus the restricted main-and-memory export contract."""
    validate_bruce_sdk_imports(wasm)
    reader = _Reader(wasm)
    reader.read(8)
    sections = {}
    while not reader.done():
        section_id = reader.u8()
        payload = _Reader(reader.read(reader.u32()))
        if section_id != 0:
            sections[section_id] = payload
    types = _vector(sections[1], _function_type) if 1 in sections else ()
    imported_types = []
    imports = sections.get(2)
    if imports is not None:
        for _ in range(imports.u32()):
            imports.text()
            imports.text()
            imports.u8()
            imported_types.append(imports.u32())
    function_types = _vector(sections[3], lambda value: value.u32()) if 3 in sections else ()
    exports = sections.get(7)
    if exports is None:
        raise WasmImportError("missing WebAssembly export section")
    found_main = False
    found_memory = False
    found_data_end = False
    found_heap_base = False
    for _ in range(exports.u32()):
        name = exports.text()
        kind = exports.u8()
        index = exports.u32()
        if name == "main" and kind == 0 and not found_main:
            local_index = index - len(imported_types)
            if index < len(imported_types):
                type_index = imported_types[index]
            elif local_index < len(function_types):
                type_index = function_types[local_index]
            else:
                raise WasmImportError("invalid main function index")
            if type_index >= len(types) or types[type_index] != FunctionType((I32, I32), (I32,)):
                raise WasmImportError("main must have signature (i32, i32) -> i32")
            found_main = True
        elif name == "memory" and kind == 2 and not found_memory:
            found_memory = True
        elif name == "__data_end" and kind == 3 and not found_data_end:
            found_data_end = True
        elif name == "__heap_base" and kind == 3 and not found_heap_base:
            found_heap_base = True
        else:
            raise WasmImportError(f"unsupported WebAssembly export {name}")
    if not exports.done():
        raise WasmImportError("trailing WebAssembly export-section data")
    if not found_main or not found_memory or not found_data_end or not found_heap_base:
        raise WasmImportError(
            "WebAssembly module must export main, memory, __data_end, and __heap_base"
        )
