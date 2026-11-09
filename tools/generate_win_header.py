#!/usr/bin/env python3
"""Generate a C++ header for the Blackmagic RAW API from the Windows SDK IDL.

The Windows Blackmagic RAW SDK only ships ``BlackmagicRawAPI.idl``; upstream
projects run it through ``midl.exe`` to obtain ``BlackmagicRawAPI.h``. That tool
is only available with the Windows SDK, so this script reproduces the same COM
interface declarations (identical vtable layout) using plain C++.

Usage:
    generate_win_header.py <path/to/BlackmagicRawAPI.idl> <output header>
"""

import re
import sys

INTERFACE_RE = re.compile(r"\s+(\w+)\s*:\s*IUnknown\s*\{")
FORWARD_RE = re.compile(r"\s+(\w+)\s*;")
ENUM_RE = re.compile(r"\s*\[v1_enum\]\s*enum\s+(\w+)\s*\{")
METHOD_RE = re.compile(r"(HRESULT|void)\s+(\w+)\s*\(([^;]*?)\)\s*;")
UUID_RE = re.compile(r"uuid\(\s*([0-9A-Fa-f]{8})-([0-9A-Fa-f]{4})-([0-9A-Fa-f]{4})-"
                     r"([0-9A-Fa-f]{4})-([0-9A-Fa-f]{12})\s*\)")
KEYWORD_RE = re.compile(r"\b(typedef|interface|coclass|importlib)\b")


class ParseError(Exception):
    pass


def strip_comments(text):
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def matching_brace(text, open_index):
    depth = 0
    for index in range(open_index, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    raise ParseError("unbalanced braces")


def library_body(text):
    match = re.search(r"\blibrary\s+\w+\s*\{", text)
    if not match:
        raise ParseError("no library block found")
    start = text.index("{", match.start())
    return text[start + 1:matching_brace(text, start)]


def preceding_attributes(text, position):
    """Return the ``[ ... ]`` attribute block that precedes ``position``."""
    end = text.rfind("]", 0, position)
    if end == -1:
        return ""
    depth = 0
    for index in range(end, -1, -1):
        if text[index] == "]":
            depth += 1
        elif text[index] == "[":
            depth -= 1
            if depth == 0:
                return text[index:end + 1]
    return ""


def format_guid(uuid):
    data1, data2, data3, data4, data5 = uuid
    data4_bytes = data4 + data5
    tail = ", ".join("0x" + data4_bytes[index:index + 2] for index in range(0, 16, 2))
    return "0x%s, 0x%s, 0x%s, { %s }" % (data1.upper(), data2.upper(), data3.upper(), tail)


def clean_parameters(parameters):
    parameters = re.sub(r"\[[^\]]*\]", "", parameters)
    result = []
    for parameter in parameters.split(","):
        parameter = re.sub(r"\s+", " ", parameter).strip()
        if parameter:
            result.append(parameter)
    return result


def parse(text):
    enums = []
    typedefs = []
    forwards = []
    interfaces = []
    guids = []

    position = 0
    while True:
        match = KEYWORD_RE.search(text, position)
        if not match:
            break
        keyword = match.group(1)
        rest = text[match.end():]

        if keyword == "importlib":
            position = text.index(";", match.end()) + 1
            continue

        if keyword == "typedef":
            enum_match = ENUM_RE.match(rest)
            if enum_match:
                brace = match.end() + enum_match.end() - 1
                end = matching_brace(text, brace)
                entries = [re.sub(r"\s+", " ", line).strip()
                           for line in text[brace + 1:end].strip().splitlines()]
                trailing = re.compile(r"\s*(\w+)\s*;").match(text, end + 1)
                if not trailing:
                    raise ParseError("enum without a name")
                enums.append((trailing.group(1), [entry for entry in entries if entry]))
                position = trailing.end()
            else:
                semicolon = text.index(";", match.end())
                typedefs.append(re.sub(r"\s+", " ", text[match.end():semicolon]).strip())
                position = semicolon + 1
            continue

        if keyword == "coclass":
            brace = text.index("{", match.end())
            position = text.index(";", matching_brace(text, brace)) + 1
            continue

        forward_match = FORWARD_RE.match(rest)
        if forward_match:
            forwards.append(forward_match.group(1))
            position = match.end() + forward_match.end()
            continue

        header_match = INTERFACE_RE.match(rest)
        if not header_match:
            raise ParseError("unsupported interface declaration near: %r" % rest[:60])
        name = header_match.group(1)
        brace = text.index("{", match.end())
        end = matching_brace(text, brace)
        inner = text[brace + 1:end]
        position = text.index(";", end) + 1

        uuid_match = UUID_RE.search(preceding_attributes(text, match.start()))
        if uuid_match:
            guids.append((name, uuid_match.groups()))

        methods = []
        for method in METHOD_RE.finditer(inner):
            methods.append((method.group(1), method.group(2),
                            clean_parameters(method.group(3))))
        interfaces.append((name, methods))

    return typedefs, enums, forwards, interfaces, guids


def render(typedefs, enums, forwards, interfaces, guids):
    lines = [
        "// Generated by tools/generate_win_header.py from the Blackmagic RAW SDK's",
        "// BlackmagicRawAPI.idl. Do not edit by hand; re-run the script to regenerate.",
        "//",
        "// These declarations are equivalent to the header midl.exe produces for the IDL,",
        "// which the Windows Blackmagic RAW SDK ships instead of a ready-made header.",
        "",
        "#pragma once",
        "",
        "#ifndef NOMINMAX",
        "#define NOMINMAX",
        "#endif",
        "#ifndef WIN32_LEAN_AND_MEAN",
        "#define WIN32_LEAN_AND_MEAN",
        "#endif",
        "#include <windows.h>",
        "#include <oaidl.h>",
        "",
        "// Interface ID Declarations",
        "",
    ]
    for name, uuid in guids:
        lines.append("inline constexpr GUID IID_%s = { %s };" % (name, format_guid(uuid)))
    lines += ["", "// Type Declarations", ""]
    for typedef in typedefs:
        lines.append("typedef %s;" % typedef)
    for name, entries in enums:
        lines.append("enum %s {" % name)
        lines += ["    %s" % entry for entry in entries]
        lines.append("};")
    lines.append("")
    lines += ["struct %s;" % name for name in forwards]
    lines.append("")
    for name, methods in interfaces:
        lines.append("struct %s : public IUnknown {" % name)
        for return_type, method_name, parameters in methods:
            lines.append("    virtual %s STDMETHODCALLTYPE %s(%s) = 0;"
                         % (return_type, method_name, ", ".join(parameters)))
        lines += ["};", ""]
    return "\n".join(lines).rstrip() + "\n"


def main():
    if len(sys.argv) != 3:
        sys.stderr.write(__doc__)
        return 1
    with open(sys.argv[1], "r", encoding="utf-8-sig") as idl_file:
        text = library_body(strip_comments(idl_file.read()))
    header = render(*parse(text))
    with open(sys.argv[2], "w", encoding="utf-8", newline="\n") as header_file:
        header_file.write(header)
    return 0


if __name__ == "__main__":
    sys.exit(main())
