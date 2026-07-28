#!/usr/bin/env python3
"""Rasterize SVG path assets into square, 1bpp C arrays."""

from __future__ import annotations

import argparse
import math
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path


ICON_SIZE = 64
CURVE_STEPS = 8
ARC_MIN_STEPS = 8
ARC_MAX_STEPS = 64
TOKEN_RE = re.compile(
    r"[AaCcHhLlMmQqSsTtVvZz]|[-+]?(?:\d+\.?(?:\d*)?|\.\d+)(?:[eE][-+]?\d+)?"
)

@dataclass
class State:
    x: float = 0.0
    y: float = 0.0
    start_x: float = 0.0
    start_y: float = 0.0
    last_cpx: float = 0.0
    last_cpy: float = 0.0
    initialized: bool = False


def c_round(value: float) -> int:
    """Match C lround() for the coordinates used by the firmware renderer."""
    return math.floor(value + 0.5) if value >= 0 else math.ceil(value - 0.5)


def tokenize(path_data: str) -> list[str]:
    tokens = TOKEN_RE.findall(path_data)
    remainder = TOKEN_RE.sub("", path_data).replace(",", "")
    if not remainder.isspace() and remainder:
        raise ValueError(f"unsupported SVG path syntax near {remainder!r}")
    return tokens


class PathRasterizer:
    def __init__(self, min_x: float, min_y: float, width: float, height: float) -> None:
        if width <= 0 or height <= 0:
            raise ValueError("viewBox width and height must be positive")
        if not math.isclose(width, height):
            raise ValueError("only square SVG viewBox values are supported")
        self.min_x = min_x
        self.min_y = min_y
        self.scale = ICON_SIZE / width
        self.edges: list[tuple[int, int, int, int]] = []

    def point(self, x: float, y: float) -> tuple[float, float]:
        return (x - self.min_x) * self.scale, (y - self.min_y) * self.scale

    def add_edge(self, x0: float, y0: float, x1: float, y1: float) -> None:
        x0, y0 = self.point(x0, y0)
        x1, y1 = self.point(x1, y1)
        self.edges.append((c_round(x0), c_round(y0), c_round(x1), c_round(y1)))

    def close_for_fill(self, state: State) -> None:
        if state.initialized and (state.x != state.start_x or state.y != state.start_y):
            self.add_edge(state.x, state.y, state.start_x, state.start_y)

    def line_to(self, state: State, x: float, y: float) -> None:
        if not state.initialized:
            raise ValueError("path drawing command before move command")
        self.add_edge(state.x, state.y, x, y)
        state.x, state.y = x, y

    def cubic(
        self, state: State, c1x: float, c1y: float, c2x: float, c2y: float, x: float, y: float
    ) -> None:
        start_x, start_y = state.x, state.y
        px, py = start_x, start_y
        for step in range(1, CURVE_STEPS + 1):
            t = step / CURVE_STEPS
            mt = 1.0 - t
            bx = mt**3 * start_x + 3 * mt**2 * t * c1x + 3 * mt * t**2 * c2x + t**3 * x
            by = mt**3 * start_y + 3 * mt**2 * t * c1y + 3 * mt * t**2 * c2y + t**3 * y
            self.add_edge(px, py, bx, by)
            px, py = bx, by
        state.x, state.y = x, y
        state.last_cpx, state.last_cpy = c2x, c2y

    def quadratic(self, state: State, cpx: float, cpy: float, x: float, y: float) -> None:
        start_x, start_y = state.x, state.y
        px, py = start_x, start_y
        for step in range(1, CURVE_STEPS + 1):
            t = step / CURVE_STEPS
            mt = 1.0 - t
            bx = mt**2 * start_x + 2 * mt * t * cpx + t**2 * x
            by = mt**2 * start_y + 2 * mt * t * cpy + t**2 * y
            self.add_edge(px, py, bx, by)
            px, py = bx, by
        state.x, state.y = x, y
        state.last_cpx, state.last_cpy = cpx, cpy

    def arc(
        self,
        state: State,
        rx: float,
        ry: float,
        rotation: float,
        large_arc: bool,
        sweep: bool,
        x: float,
        y: float,
    ) -> None:
        if (state.x == x and state.y == y) or rx <= 0 or ry <= 0:
            return
        x1, y1 = state.x, state.y
        rx, ry = abs(rx), abs(ry)
        phi = math.radians(rotation)
        cos_phi, sin_phi = math.cos(phi), math.sin(phi)
        dx2, dy2 = (x1 - x) / 2, (y1 - y) / 2
        x1p = cos_phi * dx2 + sin_phi * dy2
        y1p = -sin_phi * dx2 + cos_phi * dy2
        lam = x1p**2 / rx**2 + y1p**2 / ry**2
        if lam > 1:
            factor = math.sqrt(lam)
            rx, ry = rx * factor, ry * factor
        numerator = max(0.0, rx**2 * ry**2 - rx**2 * y1p**2 - ry**2 * x1p**2)
        denominator = rx**2 * y1p**2 + ry**2 * x1p**2
        sign = -1.0 if large_arc == sweep else 1.0
        coefficient = 0.0 if denominator <= 0 else sign * math.sqrt(numerator / denominator)
        cxp = coefficient * rx * y1p / ry
        cyp = coefficient * -ry * x1p / rx
        cx = cos_phi * cxp - sin_phi * cyp + (x1 + x) / 2
        cy = sin_phi * cxp + cos_phi * cyp + (y1 + y) / 2
        theta1 = math.atan2(y1p - cyp, x1p - cxp)
        theta2 = math.atan2(-y1p - cyp, -x1p - cxp)
        delta = theta2 - theta1
        if sweep and delta < 0:
            delta += 2 * math.pi
        elif not sweep and delta > 0:
            delta -= 2 * math.pi
        steps = max(ARC_MIN_STEPS, min(ARC_MAX_STEPS, int(max(abs(delta * rx), abs(delta * ry)) / 2)))
        px, py = x1, y1
        for step in range(1, steps + 1):
            theta = theta1 + delta * step / steps
            bx, by = rx * math.cos(theta), ry * math.sin(theta)
            ax = bx * cos_phi - by * sin_phi + cx
            ay = bx * sin_phi + by * cos_phi + cy
            self.add_edge(px, py, ax, ay)
            px, py = ax, ay
        self.add_edge(px, py, x, y)
        state.x, state.y = x, y

    def add_path(self, path_data: str) -> None:
        tokens = tokenize(path_data)
        state = State()
        index = 0
        command = ""

        def number() -> float:
            nonlocal index
            if index >= len(tokens) or tokens[index].isalpha():
                raise ValueError(f"missing argument for SVG command {command}")
            value = float(tokens[index])
            index += 1
            return value

        while index < len(tokens):
            if tokens[index].isalpha():
                command = tokens[index]
                index += 1
            elif not command:
                raise ValueError("SVG path must begin with a command")

            relative = command.islower()
            base = command.upper()
            old_x, old_y = state.x, state.y

            if base == "M":
                self.close_for_fill(state)
                x, y = number(), number()
                if relative:
                    x, y = x + old_x, y + old_y
                state.x = state.start_x = x
                state.y = state.start_y = y
                state.initialized = True
                command = "l" if relative else "L"
            elif base == "L":
                x, y = number(), number()
                self.line_to(state, x + old_x if relative else x, y + old_y if relative else y)
            elif base == "H":
                x = number()
                self.line_to(state, x + old_x if relative else x, old_y)
            elif base == "V":
                y = number()
                self.line_to(state, old_x, y + old_y if relative else y)
            elif base == "C":
                values = [number() for _ in range(6)]
                if relative:
                    values = [v + (old_x if i % 2 == 0 else old_y) for i, v in enumerate(values)]
                self.cubic(state, *values)
            elif base == "S":
                values = [number() for _ in range(4)]
                if relative:
                    values = [v + (old_x if i % 2 == 0 else old_y) for i, v in enumerate(values)]
                self.cubic(state, 2 * old_x - state.last_cpx, 2 * old_y - state.last_cpy, *values)
            elif base == "Q":
                values = [number() for _ in range(4)]
                if relative:
                    values = [v + (old_x if i % 2 == 0 else old_y) for i, v in enumerate(values)]
                self.quadratic(state, *values)
            elif base == "T":
                x, y = number(), number()
                if relative:
                    x, y = x + old_x, y + old_y
                self.quadratic(state, 2 * old_x - state.last_cpx, 2 * old_y - state.last_cpy, x, y)
            elif base == "A":
                rx, ry, rotation = number(), number(), number()
                large_arc, sweep = number() != 0, number() != 0
                x, y = number(), number()
                if relative:
                    x, y = x + old_x, y + old_y
                self.arc(state, rx, ry, rotation, large_arc, sweep, x, y)
            elif base == "Z":
                self.line_to(state, state.start_x, state.start_y)
                command = ""
            else:
                raise ValueError(f"unsupported SVG path command {command!r}")

        self.close_for_fill(state)

    def bitmap(self) -> list[int]:
        bytes_per_row = (ICON_SIZE + 7) // 8
        row_bits = bytes_per_row * 8
        rows = [0] * ICON_SIZE
        for y in range(ICON_SIZE):
            intersections: list[tuple[float, int]] = []
            yf = y + 0.5
            for x0, y0, x1, y1 in self.edges:
                if y0 == y1 or yf < min(y0, y1) or yf >= max(y0, y1):
                    continue
                x = x0 + (x1 - x0) * (yf - y0) / (y1 - y0)
                intersections.append((x, 1 if y0 < y1 else -1))
            intersections.sort(key=lambda item: item[0])
            winding = 0
            start = 0.0
            for x, direction in intersections:
                was_inside = winding != 0
                winding += direction
                if not was_inside and winding != 0:
                    start = x
                elif was_inside and winding == 0:
                    for pixel_x in range(ICON_SIZE):
                        center = pixel_x + 0.5
                        if start <= center < x:
                            rows[y] |= 1 << (row_bits - 1 - pixel_x)
        data: list[int] = []
        for row in rows:
            for byte_index in range(bytes_per_row):
                shift = (bytes_per_row - 1 - byte_index) * 8
                data.append(row >> shift & 0xFF)
        return data


def rasterize(svg_path: Path) -> list[int]:
    root = ET.parse(svg_path).getroot()
    view_box = root.get("viewBox")
    if view_box is None:
        raise ValueError("SVG has no viewBox")
    values = [float(value) for value in re.split(r"[\s,]+", view_box.strip())]
    if len(values) != 4:
        raise ValueError("SVG viewBox must contain four numbers")
    rasterizer = PathRasterizer(*values)
    paths = [element for element in root.iter() if element.tag.rsplit("}", 1)[-1] == "path"]
    if not paths:
        raise ValueError("SVG contains no path elements")
    for element in paths:
        path_data = element.get("d")
        if not path_data:
            raise ValueError("SVG path has no data")
        rasterizer.add_path(path_data)
    return rasterizer.bitmap()


def identifier(path: Path) -> str:
    name = re.sub(r"[^a-zA-Z0-9]+", "_", path.stem).strip("_").lower()
    if not name or name[0].isdigit():
        name = "icon_" + name
    return "s_icon_" + name


def format_header(assets: list[Path]) -> str:
    if ICON_SIZE <= 0 or ICON_SIZE > 255:
        raise ValueError("ICON_SIZE must fit in bruce_icon_t dimensions (1..255)")
    bytes_per_row = (ICON_SIZE + 7) // 8
    lines = [
        "/* Generated by tools/svg_to_icon_assets.py. Do not edit manually. */",
        "#pragma once",
        "",
        '#include "core_sdk/icon.h"',
        "",
        "#include <stdint.h>",
        "",
        "typedef struct {",
        "    const char *name;",
        "    bruce_icon_t icon;",
        "} icon__entry_t;",
        "",
        "#define ICON__ENTRY(icon_name, icon_bits) \\",
        f"    {{ icon_name, {{ {ICON_SIZE}, {ICON_SIZE}, icon_bits }} }}",
        "",
    ]
    names: set[str] = set()
    for asset in assets:
        name = identifier(asset)
        if name in names:
            raise ValueError(f"multiple SVG filenames produce the identifier {name}")
        names.add(name)
        data = rasterize(asset)
        lines.append(f"static const uint8_t {name}[{len(data)}] = {{")
        for offset in range(0, len(data), bytes_per_row):
            row = ", ".join(f"0x{value:02x}" for value in data[offset : offset + bytes_per_row])
            lines.append(f"    {row},")
        lines.extend(("};", ""))

    lines.append("static const icon__entry_t s_icons[] = {")
    for asset in assets:
        lines.append(f'    ICON__ENTRY("{asset.stem}", {identifier(asset)}),')
    lines.extend(("};", ""))
    return "\n".join(lines)


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--assets-dir", type=Path, default=repo_root / "src/core/icon/assets", help="directory containing SVG files"
    )
    parser.add_argument(
        "--output", type=Path, default=repo_root / "src/core/icon/icon_assets.h", help="generated C header"
    )
    args = parser.parse_args()
    assets = sorted(args.assets_dir.glob("*.svg"), key=lambda path: path.name)
    if not assets:
        parser.error(f"no SVG files found in {args.assets_dir}")
    try:
        output = format_header(assets)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output, encoding="ascii")
    except (ET.ParseError, OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"generated {args.output} from {len(assets)} SVG files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
