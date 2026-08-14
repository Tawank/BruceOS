#!/usr/bin/env python3
"""Discover and build manifest-enabled Bruce modules as ELF or WASM apps."""

import argparse
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
MODULES_DIR = REPO_ROOT / "src" / "modules"
OUTPUT_DIR = SCRIPT_DIR.parent / "build_modules"
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}
WASM_GUEST_SOURCE = MODULES_DIR / "loaders" / "wasm" / "wasm_bruce_guest_adapter.c"


@dataclass(frozen=True)
class Module:
    name: str
    directory: Path
    manifest_path: Path
    entry_point: str
    sources: tuple[Path, ...]
    include_dirs: tuple[Path, ...]
    definitions: tuple[str, ...]
    compile_options: tuple[str, ...]
    targets: tuple[str, ...]


def run(command, cwd, env=None):
    print("+", " ".join(str(part) for part in command))
    subprocess.check_call([str(part) for part in command], cwd=cwd, env=env)


def resolve_module_path(module_dir, value, manifest_path):
    if not isinstance(value, str) or not value:
        raise RuntimeError(f"Build paths must be non-empty strings in {manifest_path}")
    path = (module_dir / value).resolve()
    try:
        path.relative_to(REPO_ROOT)
    except ValueError as error:
        raise RuntimeError(f"Build path leaves the repository in {manifest_path}: {value}") from error
    return path


def read_string_list(build, key, manifest_path):
    values = build.get(key, [])
    if not isinstance(values, list) or any(not isinstance(value, str) or not value for value in values):
        raise RuntimeError(f"build.{key} must be an array of non-empty strings in {manifest_path}")
    return tuple(values)


def load_module(manifest_path):
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"Invalid manifest: {manifest_path}") from error
    build = manifest.get("build")
    if build is None:
        return None
    if not isinstance(build, dict):
        raise RuntimeError(f"build must be an object in {manifest_path}")

    module_dir = manifest_path.parent
    name = build.get("name", module_dir.name)
    if not isinstance(name, str) or not name or not all(character.isalnum() or character in "_-" for character in name):
        raise RuntimeError(f"Invalid build.name in {manifest_path}")
    entry_point = manifest.get("entryPoint", "app_main")
    if not isinstance(entry_point, str) or not entry_point.isidentifier():
        raise RuntimeError(f"Invalid entryPoint in {manifest_path}")

    source_values = read_string_list(build, "sources", manifest_path)
    if not source_values:
        raise RuntimeError(f"build.sources cannot be empty in {manifest_path}")
    sources = tuple(resolve_module_path(module_dir, value, manifest_path) for value in source_values)
    for source in sources:
        if not source.is_file() or source.suffix.lower() not in SOURCE_SUFFIXES:
            raise RuntimeError(f"Module source not found or unsupported: {source}")

    include_dirs = tuple(
        resolve_module_path(module_dir, value, manifest_path)
        for value in read_string_list(build, "includeDirs", manifest_path)
    )
    for include_dir in include_dirs:
        if not include_dir.is_dir():
            raise RuntimeError(f"Module include directory not found: {include_dir}")

    targets = read_string_list(build, "targets", manifest_path) or ("elf", "wasm")
    if any(target not in ("elf", "wasm") for target in targets):
        raise RuntimeError(f"build.targets may only contain elf and wasm in {manifest_path}")
    return Module(
        name=name,
        directory=module_dir,
        manifest_path=manifest_path,
        entry_point=entry_point,
        sources=sources,
        include_dirs=include_dirs,
        definitions=read_string_list(build, "compileDefinitions", manifest_path),
        compile_options=read_string_list(build, "compileOptions", manifest_path),
        targets=targets,
    )


def discover_modules():
    modules = {}
    for manifest_path in sorted(MODULES_DIR.rglob("manifest.json")):
        module = load_module(manifest_path)
        if module is None:
            continue
        if module.name in modules:
            raise RuntimeError(
                f"Duplicate module build name {module.name}: {modules[module.name].manifest_path} and {manifest_path}"
            )
        modules[module.name] = module
    return modules


def cmake_quote(value):
    return '"' + str(value).replace("\\", "/").replace('"', '\\"') + '"'


def write_elf_project(module, project_dir):
    main_dir = project_dir / "main"
    main_dir.mkdir(parents=True, exist_ok=True)
    component_dir = REPO_ROOT / "components" / "elf_loader"
    top_level = "\n".join([
        "cmake_minimum_required(VERSION 3.16)",
        f"set(EXTRA_COMPONENT_DIRS {cmake_quote(component_dir)})",
        "include($ENV{IDF_PATH}/tools/cmake/project.cmake)",
        f"project({module.name})",
        "include(elf_loader)",
        f"project_elf({module.name})",
        "",
    ])
    include_dirs = (REPO_ROOT / "native_apps" / "include", REPO_ROOT / "src", *module.include_dirs)
    component = [
        "idf_component_register(",
        "    SRCS " + " ".join(cmake_quote(source) for source in module.sources),
        "    INCLUDE_DIRS " + " ".join(cmake_quote(path) for path in include_dirs),
        ")",
    ]
    definitions = (*module.definitions, f"{module.entry_point}=app_main")
    if definitions:
        component.append("target_compile_definitions(${COMPONENT_LIB} PRIVATE " + " ".join(cmake_quote(v) for v in definitions) + ")")
    if module.compile_options:
        component.append(
            "target_compile_options(${COMPONENT_LIB} PRIVATE "
            + " ".join(cmake_quote(value) for value in module.compile_options)
            + ")"
        )
    component.append("")
    (project_dir / "CMakeLists.txt").write_text(top_level, encoding="utf-8")
    (main_dir / "CMakeLists.txt").write_text("\n".join(component), encoding="utf-8")
    (project_dir / "sdkconfig.defaults").write_text(
        "CONFIG_ESP_SYSTEM_MEMPROT=n\n"
        "CONFIG_ELF_LOADER_LIBC_SYMBOLS=n\n"
        "CONFIG_ELF_LOADER_ESPIDF_SYMBOLS=n\n",
        encoding="utf-8",
    )


def add_elf_manifest(source_elf, output, manifest_path, target, env):
    prefix = "riscv32-esp-elf" if target.startswith("esp32c") or target == "esp32p4" else f"xtensa-{target}-elf"
    temporary = source_elf.with_suffix(".tmp.elf")
    shutil.copy2(source_elf, temporary)
    run([
        f"{prefix}-objcopy", "--add-section", f".bruce.manifest={manifest_path}",
        "--set-section-flags", ".bruce.manifest=contents,readonly", temporary, output,
    ], source_elf.parent, env)
    temporary.unlink()


def build_elf(module, target, idf_path):
    project_dir = OUTPUT_DIR / ".work" / module.name / "elf"
    build_dir = project_dir / f"build-{target}"
    write_elf_project(module, project_dir)
    idf_py = Path(idf_path) / "tools" / "idf.py"
    if not idf_py.exists():
        raise RuntimeError(f"idf.py not found at {idf_py}")
    env = os.environ.copy()
    env["IDF_PATH"] = str(idf_path)
    env["IDF_TARGET"] = target
    run([sys.executable, idf_py, "-B", build_dir, "elf"], project_dir, env)
    source_elf = build_dir / f"{module.name}.app.elf"
    if not source_elf.exists():
        raise RuntimeError(f"Missing ELF output for {module.name}: {source_elf}")
    output = OUTPUT_DIR / f"{module.name}.elf"
    add_elf_manifest(source_elf, output, module.manifest_path, target, env)
    print(f"Built {output}")


def leb(value):
    result = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        result.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(result)


def add_wasm_manifest(wasm_path, manifest_path):
    manifest = manifest_path.read_bytes()
    section_name = b"bruce.manifest"
    payload = leb(len(section_name)) + section_name + manifest
    with wasm_path.open("ab") as output:
        output.write(b"\x00" + leb(len(payload)) + payload)


def build_wasm(module, compiler):
    cxx_sources = [source for source in module.sources if source.suffix.lower() != ".c"]
    if cxx_sources:
        raise RuntimeError(
            "C++ WASM inputs are not supported yet: "
            + ", ".join(str(source.relative_to(REPO_ROOT)) for source in cxx_sources)
        )
    output = OUTPUT_DIR / f"{module.name}.wasm"
    include_dirs = (REPO_ROOT / "native_apps" / "include", REPO_ROOT / "src", *module.include_dirs)
    command = [
        compiler, "--target=wasm32", "-O2", "-nostdlib", "-ffreestanding",
        f"-D{module.entry_point}=main",
        *[f"-D{value}" for value in module.definitions],
        *module.compile_options,
        "-Wl,--no-entry", "-Wl,--allow-undefined", "-Wl,--export=main",
        "-Wl,--export-memory", "-Wl,--initial-memory=65536", "-Wl,--max-memory=262144",
        *[part for path in include_dirs for part in ("-I", path)],
        "-o", output, *module.sources, WASM_GUEST_SOURCE,
    ]
    run(command, module.directory)
    add_wasm_manifest(output, module.manifest_path)
    print(f"Built {output}")


def main():
    try:
        modules = discover_modules()
    except RuntimeError as error:
        raise SystemExit(str(error)) from error
    parser = argparse.ArgumentParser(description="Build manifest-enabled src/modules apps")
    parser.add_argument("--target", choices=("elf", "wasm"), default="elf")
    parser.add_argument("--idf-target", default="esp32s3", help="ESP-IDF chip target for ELF builds")
    parser.add_argument("--idf-path", default=os.environ.get("IDF_PATH", ""))
    parser.add_argument("--compiler", default=os.environ.get("WASM_CLANG", "clang"))
    parser.add_argument("--module", action="append", choices=sorted(modules))
    parser.add_argument("--list", action="store_true", help="list discovered modules and exit")
    args = parser.parse_args()
    if args.list:
        for module in modules.values():
            print(f"{module.name}: {module.directory.relative_to(REPO_ROOT)} ({', '.join(module.targets)})")
        return

    selected = list(dict.fromkeys(
        args.module or (name for name, module in modules.items() if args.target in module.targets)
    ))
    if not selected:
        raise SystemExit(f"No modules support target {args.target} under {MODULES_DIR}")
    unsupported = [name for name in selected if args.target not in modules[name].targets]
    if unsupported:
        raise SystemExit(f"Target {args.target} is not supported by: {', '.join(unsupported)}")
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    if args.target == "wasm":
        if shutil.which(args.compiler) is None:
            raise SystemExit(f"WASM compiler not found: {args.compiler}")
        for name in selected:
            build_wasm(modules[name], args.compiler)
    else:
        if not args.idf_path or not Path(args.idf_path).is_dir():
            raise SystemExit("IDF_PATH not set or invalid")
        for name in selected:
            build_elf(modules[name], args.idf_target, args.idf_path)


if __name__ == "__main__":
    main()
