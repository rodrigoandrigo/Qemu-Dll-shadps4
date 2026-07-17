#!/usr/bin/env python3
"""Compare QEMU shadPS4 HLE NIDs with shadPS4 LIB_FUNCTION providers."""

import argparse
import collections
import importlib.util
import json
import pathlib
import re


HLE_MAP_RE = re.compile(
    r'HLE_MAP\("([^"]+)",\s*(SHADPS4_HLE_[A-Z0-9_]+|[0-9]+)\)')
STATIC_EXPORT_RE = re.compile(
    r'SHADPS4_HLE_EXPORT\(\s*"([^"]+)"\s*,\s*"[^"]+"\s*,\s*'
    r'(SHADPS4_HLE_[A-Z0-9_]+|[0-9]+)\s*\)')
STATIC_ENTRY_RE = re.compile(
    r'\{\s*"([^"]+)"\s*,\s*"[^"]+"\s*,\s*"[^"]+"\s*,\s*'
    r'(SHADPS4_HLE_[A-Z0-9_]+|[0-9]+)\s*[,}]')
LIB_FUNCTION_RE = re.compile(r"LIB_FUNCTION\((.*?)\);", re.S)
GENERIC_DISPATCHES = {"SHADPS4_HLE_SUCCESS", "SHADPS4_HLE_STUB_SUCCESS"}


def qemu_nids(source):
    entries = collections.defaultdict(set)
    text = source.read_text("utf-8")
    for nid, dispatch in HLE_MAP_RE.findall(text):
        entries[nid].add(dispatch)
    for nid, dispatch in STATIC_EXPORT_RE.findall(text):
        entries[nid].add(dispatch)
    for nid, dispatch in STATIC_ENTRY_RE.findall(text):
        entries[nid].add(dispatch)
    gnm_start = text.find("static uint32_t shadps4_gnm_compat_dispatch")
    gnm_end = text.find("typedef struct ShadPS4HLECompatMap", gnm_start)
    if gnm_start >= 0 and gnm_end > gnm_start:
        for nid in re.findall(
                r'(?:\\||")([A-Za-z0-9+_-]{11})(?=\\||")',
                text[gnm_start:gnm_end]):
            entries[nid].add("SHADPS4_HLE_GNM_COMPAT_ROUTED")
    return entries


def load_classifier(qemu_root):
    path = qemu_root / "scripts/shadps4-generate-trivial-providers.py"
    spec = importlib.util.spec_from_file_location("shadps4_provider_classifier", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def generated_trivial_nids(path):
    entries = collections.defaultdict(set)
    if not path.exists():
        return entries
    for module, packed in re.findall(r'\{ "([^"]+)", "([^"]+)" \}',
                                     path.read_text("utf-8")):
        for nid in packed.strip("|").split("|"):
            if nid:
                entries[nid].add(module)
    return entries


def shadps4_nids(libraries, classifier):
    entries = collections.defaultdict(set)
    providers = collections.defaultdict(set)
    semantics = collections.defaultdict(list)
    for source in libraries.rglob("*.cpp"):
        text = source.read_text("utf-8", errors="ignore")
        for match in LIB_FUNCTION_RE.finditer(text):
            body = match.group(1)
            nid_match = re.match(r'\s*"([^"]+)"', body)
            function_match = re.search(
                r",\s*([A-Za-z_][A-Za-z0-9_:~]*)\s*$", body)
            if not nid_match or not function_match:
                continue
            nid = nid_match.group(1)
            function = function_match.group(1)
            entries[nid].add(function)
            providers[nid].add(str(source.relative_to(libraries)))
            semantics[nid].append(classifier.is_exact_success_stub(
                classifier.function_body(text, function)))
    return entries, providers, semantics


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu-root", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[1])
    parser.add_argument("--shadps4-root", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()

    classifier = load_classifier(args.qemu_root)
    qemu = qemu_nids(args.qemu_root / "hw/i386/shadps4.c")
    trivial = generated_trivial_nids(
        args.qemu_root / "hw/i386/shadps4-trivial-providers.inc")
    reviewed = generated_trivial_nids(
        args.qemu_root / "hw/i386/shadps4-reviewed-providers.inc")
    shad, providers, semantics = shadps4_nids(
        args.shadps4_root / "src/core/libraries", classifier)
    qemu_set = set(qemu)
    effective_set = qemu_set | set(trivial) | set(reviewed)
    shad_set = set(shad)
    shared = qemu_set & shad_set
    generic_shared = {
        nid for nid in shared if qemu[nid] & GENERIC_DISPATCHES
    }
    generic_aligned = {
        nid for nid in generic_shared
        if semantics[nid] and all(semantics[nid])
    }
    report = {
        "summary": {
            "qemu_unique": len(qemu_set),
            "generated_exact_success_stubs": len(set(trivial) - qemu_set),
            "reviewed_nontrivial_provider_routes": len(
                set(reviewed) - qemu_set - set(trivial)),
            "qemu_effectively_covered": len(effective_set),
            "shadps4_provider_unique": len(shad_set),
            "shared": len(shared),
            "missing_in_qemu": len(shad_set - qemu_set),
            "missing_effective_behavior": len(shad_set - effective_set),
            "without_shadps4_provider": len(qemu_set - shad_set),
            "generic_in_qemu": len(generic_shared),
            "generic_matching_exact_success_stub": len(generic_aligned),
            "generic_requiring_semantic_review": len(
                generic_shared - generic_aligned),
            "conflicting_qemu_dispatches": sum(
                len(dispatches) > 1 for dispatches in qemu.values()),
        },
        "missing_in_qemu": [
            {"nid": nid, "functions": sorted(shad[nid]),
             "providers": sorted(providers[nid])}
            for nid in sorted(shad_set - qemu_set)
        ],
        "missing_effective_behavior": [
            {"nid": nid, "functions": sorted(shad[nid]),
             "providers": sorted(providers[nid])}
            for nid in sorted(shad_set - effective_set)
        ],
        "generic_in_qemu": [
            {"nid": nid, "dispatches": sorted(qemu[nid]),
             "functions": sorted(shad[nid]),
             "providers": sorted(providers[nid])}
            for nid in sorted(shared)
            if qemu[nid] & GENERIC_DISPATCHES
        ],
        "generic_requiring_semantic_review": [
            {"nid": nid, "dispatches": sorted(qemu[nid]),
             "functions": sorted(shad[nid]),
             "providers": sorted(providers[nid])}
            for nid in sorted(generic_shared - generic_aligned)
        ],
        "without_shadps4_provider": [
            {"nid": nid, "dispatches": sorted(qemu[nid])}
            for nid in sorted(qemu_set - shad_set)
        ],
        "conflicting_qemu_dispatches": [
            {"nid": nid, "dispatches": sorted(dispatches)}
            for nid, dispatches in sorted(qemu.items())
            if len(dispatches) > 1
        ],
    }
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered, "utf-8")
    else:
        print(rendered, end="")


if __name__ == "__main__":
    main()
