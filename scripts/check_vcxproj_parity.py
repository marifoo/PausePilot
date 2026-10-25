#!/usr/bin/env python3
"""Guard against PausePilot.pro/qtest.pro/apptest.pro drifting apart from their MSVC
.vcxproj counterparts (see plan4.md A1). Compares the SOURCES/HEADERS file lists
on each side and fails on any difference.

Run: python3 scripts/check_vcxproj_parity.py
"""
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NS = "{http://schemas.microsoft.com/developer/msbuild/2003}"


def parse_pro_list(pro_text, var_name):
    """Extract the file list assigned to `var_name +=` (or `=`) in a .pro file."""
    m = re.search(rf"^{var_name}\b\s*[+]?=\s*(.*?)(?=^\S|\Z)", pro_text, re.M | re.S)
    if not m:
        return []
    entries = [e.strip().rstrip("\\").strip() for e in m.group(1).splitlines()]
    return [e for e in entries if e]


def normalize_pro_entry(entry):
    """Return (is_parent_dir, basename) for a .pro SOURCES/HEADERS entry."""
    entry = entry.replace("$$PWD/../", "../").replace("$$PWD/", "")
    is_parent = entry.startswith("../")
    return (is_parent, entry.split("/")[-1])


def normalize_vcxproj_entry(include):
    """Return (is_parent_dir, basename) for a vcxproj Include path."""
    is_parent = include.startswith("..\\") or include.startswith("../")
    return (is_parent, include.replace("\\", "/").split("/")[-1])


def vcxproj_includes(vcxproj_path, tags):
    root = ET.parse(vcxproj_path).getroot()
    entries = []
    for tag in tags:
        for el in root.iter(f"{NS}{tag}"):
            include = el.get("Include")
            if include:
                entries.append(normalize_vcxproj_entry(include))
    return entries


def check(pro_path, vcxproj_path):
    pro_text = pro_path.read_text()
    pro_sources = {normalize_pro_entry(e) for e in parse_pro_list(pro_text, "SOURCES")}
    pro_headers = {normalize_pro_entry(e) for e in parse_pro_list(pro_text, "HEADERS")}

    vcx_sources = set(vcxproj_includes(vcxproj_path, ["ClCompile"]))
    vcx_headers = set(vcxproj_includes(vcxproj_path, ["QtMoc", "ClInclude"]))

    problems = []
    if pro_sources != vcx_sources:
        problems.append(
            f"SOURCES mismatch between {pro_path} and {vcxproj_path}:\n"
            f"  missing from vcxproj: {sorted(pro_sources - vcx_sources)}\n"
            f"  extra in vcxproj:     {sorted(vcx_sources - pro_sources)}"
        )
    if pro_headers != vcx_headers:
        problems.append(
            f"HEADERS mismatch between {pro_path} and {vcxproj_path}:\n"
            f"  missing from vcxproj: {sorted(pro_headers - vcx_headers)}\n"
            f"  extra in vcxproj:     {sorted(vcx_headers - pro_headers)}"
        )
    return problems


def main():
    problems = []
    problems += check(ROOT / "PausePilot.pro", ROOT / "PausePilot.vcxproj")
    problems += check(ROOT / "qtest" / "qtest.pro", ROOT / "qtest" / "qtest.vcxproj")
    problems += check(ROOT / "apptest" / "apptest.pro", ROOT / "apptest" / "apptest.vcxproj")

    if problems:
        print("\n\n".join(problems))
        print("\nFAIL: .pro and .vcxproj file lists have drifted apart.")
        return 1

    print("OK: .pro and .vcxproj file lists match.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
