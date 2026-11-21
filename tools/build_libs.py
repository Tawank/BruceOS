#!/usr/bin/env python3
"""Builds the audio codec libraries for /lib from Espressif's prebuilt
libesp_audio_codec.a, one shared object per codec (see core/lib and
core/audio/audio_codec_lib.h), named <codec>_<target>.so like ELF apps.
Fails when a library imports a name Core's library table
(core/lib/lib_symbols.c) lacks.

    ./tools/build_libs.py --target esp32s3      # -> build-lib/esp32s3/*_esp32s3.so

Without the target's compiler on PATH it reruns itself in the ESP-IDF Docker
image. The firmware build runs it too, with every path given."""

import argparse
import os
import re
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IDF_IMAGE = "espressif/idf:v6.1"
TARGETS = ("esp32", "esp32s2", "esp32s3")

# name: (header, [(type, ops macro)])
CODECS = {
    "aac": ("esp_aac_dec.h", [("AAC", "AAC")]),
    "adpcm": ("esp_adpcm_dec.h", [("ADPCM", "ADPCM")]),
    "alac": ("esp_alac_dec.h", [("ALAC", "ALAC")]),
    "amrnb": ("esp_amrnb_dec.h", [("AMRNB", "AMRNB")]),
    "amrwb": ("esp_amrwb_dec.h", [("AMRWB", "AMRWB")]),
    "flac": ("esp_flac_dec.h", [("FLAC", "FLAC")]),
    "g711": ("esp_g711_dec.h", [("G711A", "G711A"), ("G711U", "G711U")]),
    "g722": ("esp_g722_dec.h", [("G722", "G722")]),
    "lc3": ("esp_lc3_dec.h", [("LC3", "LC3")]),
    "opus": ("esp_opus_dec.h", [("OPUS", "OPUS")]),
    "pcm": ("esp_pcm_dec.h", [("PCM", "PCM")]),
    "sbc": ("esp_sbc_dec.h", [("SBC", "SBC")]),
    "vorbis": ("esp_vorbis_dec.h", [("VORBIS", "VORBIS")]),
}

SOURCE = """#include "{header}"
#include "esp_audio_codec_version.h"
#include "audio_codec_lib.h"

static const audio_codec_lib_t s_lib = {{
    .abi = AUDIO_CODEC_LIB_ABI,
    .codec_version = esp_audio_codec_get_version,
    .decoder_count = {count},
    .decoders = {{{decoders}}},
}};

const audio_codec_lib_t *lib_main(void) {{ return &s_lib; }}
"""

# Hand-written assembly keeps its L32R literal pools in .iram1.literal, which
# must sit before the code using them.
LINKER_SCRIPT = """SECTIONS
{
  .iram_text : { *(.iram1.literal .iram1.*.literal) *(.iram1 .iram1.*) }
}
INSERT BEFORE .text;
"""


def run(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout + result.stderr)
        sys.exit(f"build_libs: failed: {' '.join(command)}")
    return result.stdout


def tool(cc, name):
    return re.sub(r"gcc(\.exe)?$", name, cc)


def in_docker(argv):
    """Reruns this script in the ESP-IDF image, the repo mounted at the same path."""
    if shutil.which("docker") is None:
        sys.exit("build_libs: no xtensa compiler on PATH and no docker; run it from an ESP-IDF shell")
    command = ". /opt/esp/idf/export.sh >/dev/null 2>&1 && python3 " + " ".join(
        [os.path.abspath(__file__)] + [f"'{a}'" for a in argv]
    )
    docker = ["docker", "run", "--rm", "-e", "HOME=/tmp", "-v", f"{REPO}:{REPO}", "-w", os.getcwd()]
    if hasattr(os, "getuid"):
        docker += ["-u", f"{os.getuid()}:{os.getgid()}"]
    sys.exit(subprocess.call(docker + [IDF_IMAGE, "bash", "-c", command]))


def main():
    parser = argparse.ArgumentParser(description="Build the audio codec libraries for /lib.")
    parser.add_argument("--target", default="esp32s3", choices=TARGETS)
    parser.add_argument("--out", help="where the .so files go (default: build-lib/<target>)")
    parser.add_argument("--cc", help="the target's gcc (default: xtensa-<target>-elf-gcc from PATH)")
    parser.add_argument(
        "--codec-dir", default=os.path.join(REPO, "managed_components", "espressif__esp_audio_codec")
    )
    parser.add_argument(
        "--include", default=os.path.join(REPO, "src", "core", "audio"), help="directory with audio_codec_lib.h"
    )
    parser.add_argument("--symbols", default=os.path.join(REPO, "src", "core", "lib", "lib_symbols.c"))
    args = parser.parse_args()
    if args.cc is None:
        args.cc = shutil.which(f"xtensa-{args.target}-elf-gcc")
        if args.cc is None:
            in_docker(sys.argv[1:])
    if args.out is None:
        args.out = os.path.join(REPO, "build-lib", args.target)
    if not os.path.isdir(args.codec_dir):
        sys.exit(f"build_libs: {args.codec_dir} is missing; build the firmware once to download it")
    args.out = os.path.abspath(args.out)

    archive = os.path.join(args.codec_dir, "lib", args.target, "libesp_audio_codec.a")
    inputs = [archive, os.path.abspath(__file__), args.symbols, os.path.join(args.include, "audio_codec_lib.h")]
    table = open(args.symbols).read()
    allowed = set(re.findall(r"ESP_ELFSYM_EXPORT\((\w+)\)", table)) | set(re.findall(r'\{"(\w+)",', table))
    includes = [f"-I{args.include}"] + [
        f"-I{os.path.join(args.codec_dir, 'include', sub)}" for sub in ("", "decoder", "decoder/impl")
    ]
    # Only the libraries go to --out, ready to copy to /lib.
    work = args.out + "_build"
    os.makedirs(args.out, exist_ok=True)
    os.makedirs(work, exist_ok=True)
    script = os.path.join(work, "lib.ld")
    with open(script, "w") as f:
        f.write(LINKER_SCRIPT)
    newest_input = max(os.path.getmtime(p) for p in inputs)
    libm = run([args.cc, "-mlongcalls", "-print-file-name=libm.a"]).strip()
    libgcc = run([args.cc, "-mlongcalls", "-print-libgcc-file-name"]).strip()
    # Some codecs keep pointer tables in .rodata, which a shared object may
    # not relocate. The loader copies every section to RAM and relocates it
    # there, so a copy of the archive with writable .rodata links.
    writable = os.path.join(work, "libesp_audio_codec_rw.a")
    if not os.path.exists(writable) or os.path.getmtime(writable) < newest_input:
        run([tool(args.cc, "objcopy"), "--set-section-flags", ".rodata*=alloc,load,contents,data", archive, writable])

    for name, (header, decoders) in CODECS.items():
        output = os.path.join(args.out, f"{name}_{args.target}.so")
        if os.path.exists(output) and os.path.getmtime(output) >= newest_input:
            continue
        source = os.path.join(work, f"{name}_lib.c")
        entries = ", ".join(
            f"{{ESP_AUDIO_TYPE_{t}, ESP_{m}_DEC_DEFAULT_OPS()}}" for t, m in decoders
        )
        with open(source, "w") as f:
            f.write(SOURCE.format(header=header, count=len(decoders), decoders=entries))
        obj = source[:-2] + ".o"
        merged = source[:-2] + "_all.o"
        run([args.cc, "-mlongcalls", "-Os", "-fPIC", "-ffunction-sections", "-fdata-sections",
             *includes, "-c", source, "-o", obj])
        # One object with everything lib_main needs, every symbol but
        # lib_main made local: the archive's direct calls between its own
        # functions then resolve inside the library. (Hiding them with
        # --exclude-libs instead crashes ld 2.45 on some codecs.)
        run([tool(args.cc, "ld"), "-r", "-u", "lib_main", "-e", "lib_main", "--gc-sections", "-o", merged, obj,
             "--start-group", writable, libm, libgcc, "--end-group"])
        run([tool(args.cc, "objcopy"), "--keep-global-symbol=lib_main", merged])
        # --no-keep-memory: without it ld 2.45 corrupts its heap on LC3.
        run([args.cc, "-mlongcalls", "-fPIC", "-shared", "-nostdlib", "-nostartfiles", "-e", "lib_main",
             "-Wl,--gc-sections", "-Wl,--no-warn-rwx-segments", "-Wl,--no-keep-memory", f"-Wl,-T,{script}",
             "-o", output, merged])
        run([tool(args.cc, "strip"), "--strip-unneeded", "--remove-section=.comment",
             "--remove-section=.got.loc", "--remove-section=.xt.lit", "--remove-section=.xt.prop",
             "--remove-section=.xtensa.info", output])
        listing = run([tool(args.cc, "nm"), "-D", "--undefined-only", output])
        imports = set(line.split()[-1] for line in listing.splitlines() if line.strip())
        missing = sorted(imports - allowed)
        if missing:
            os.remove(output)
            sys.exit(f"build_libs: {name} imports {', '.join(missing)}; add them to {args.symbols}")
        print(f"build_libs: {output} ({os.path.getsize(output)} bytes)")


if __name__ == "__main__":
    main()
