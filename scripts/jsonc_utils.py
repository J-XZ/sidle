#!/usr/bin/env python3
"""Small string-aware JSONC loader shared by SIDLE host tooling."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any


def load_jsonc(path: str | Path) -> Any:
    text = Path(path).read_text()
    output: list[str] = []
    in_string = False
    escaped = False
    index = 0
    while index < len(text):
        char = text[index]
        next_char = text[index + 1] if index + 1 < len(text) else ""
        if in_string:
            output.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            index += 1
        elif char == '"':
            in_string = True
            output.append(char)
            index += 1
        elif char == "/" and next_char == "/":
            newline = text.find("\n", index)
            index = len(text) if newline < 0 else newline
        else:
            output.append(char)
            index += 1
    return json.loads("".join(output))
