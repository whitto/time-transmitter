#!/usr/bin/env python3
"""Validate the linked classic-ESP32 SDK panic path and cache-safe Off gate."""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WRAPPER = "__wrap_esp_core_dump_write"
WRITER = "esp_core_dump_write"
FLAG = "(anonymous namespace)::crashDumpEnabled"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def active_sketch():
    versions = []
    for directory in (ROOT / "firmware").iterdir():
        match = re.fullmatch(r"RadioClock_V(\d+)_(\d+)", directory.name)
        if match and directory.is_dir():
            versions.append((tuple(map(int, match.groups())), directory.name))
    require(versions, "No active versioned sketch folder found")
    return max(versions)[1]


def tool_pair(tools_dir):
    base = tools_dir / "data/packages/esp32/tools/esp-x32"
    for directory in sorted(base.glob("*/bin"), reverse=True):
        nm = directory / "xtensa-esp32-elf-nm"
        objdump = directory / "xtensa-esp32-elf-objdump"
        if nm.is_file() and objdump.is_file():
            return nm, objdump
    raise RuntimeError(f"Pinned Xtensa nm/objdump tools missing in {base}")


def run(tool, *arguments):
    return subprocess.check_output([str(tool), *map(str, arguments)], text=True)


def symbols(text):
    result = {}
    for line in text.splitlines():
        match = re.fullmatch(r"([\da-fA-F]+)\s+(?:([\da-fA-F]+)\s+)?([A-Za-z])\s+(.+)", line)
        if match:
            address, size, kind, name = match.groups()
            result[name] = {"address": int(address, 16),
                            "size": int(size, 16) if size else None, "kind": kind}
    return result


def sections(text):
    result = []
    for line in text.splitlines():
        match = re.match(r"\s*\d+\s+(\S+)\s+([\da-fA-F]+)\s+([\da-fA-F]+)\s+", line)
        if match:
            name, size, start = match.groups()
            result.append((name, int(start, 16), int(start, 16) + int(size, 16)))
    return result


def section_contains(all_sections, prefix, address, size=1):
    return any(name.startswith(prefix) and start <= address and address + size <= end
               for name, start, end in all_sections)


def instructions(text):
    result = []
    for line in text.splitlines():
        match = re.match(r"\s*([\da-fA-F]+):\s+[\da-fA-F]+\s+(\S+)\s*(.*)", line)
        if match:
            address, operation, arguments = match.groups()
            result.append({"address": int(address, 16), "operation": operation,
                           "arguments": arguments})
    return result


def literal(instruction):
    if instruction["operation"] != "l32r":
        return None
    match = re.match(r"(a\d+),\s*([\da-fA-F]+).*?\(([\da-fA-F]+)\s+<", instruction["arguments"])
    return (match.group(1), int(match.group(2), 16), int(match.group(3), 16)) if match else None


def resolved_calls(code):
    registers, calls = {}, []
    for instruction in code:
        operation, arguments = instruction["operation"], instruction["arguments"]
        loaded = literal(instruction)
        if loaded:
            registers[loaded[0]] = loaded[2]
        elif operation.startswith("callx"):
            calls.append((instruction["address"], registers.get(arguments.strip())))
        elif operation.startswith("call"):
            match = re.match(r"([\da-fA-F]+)\s", arguments)
            calls.append((instruction["address"], int(match.group(1), 16) if match else None))
        elif not operation.startswith(("b", "ret", "j", "memw", "entry")):
            destination = re.match(r"(a\d+)\s*,", arguments)
            if destination:
                registers.pop(destination.group(1), None)
    return calls


def validate(elf, tools_dir):
    require(elf.is_file(), f"ELF missing: {elf}")
    nm, objdump = tool_pair(tools_dir)
    table = symbols(run(nm, "-nSC", elf))
    all_sections = sections(run(objdump, "-h", elf))
    for name in (WRAPPER, WRITER, FLAG, "esp_panic_handler"):
        require(name in table, f"Required linked symbol missing: {name}")
    gate, writer, flag = table[WRAPPER], table[WRITER], table[FLAG]
    require(gate["size"] and section_contains(all_sections, ".iram", gate["address"], gate["size"]),
            "Complete wrapper must be in IRAM")
    require(flag["size"] == 4 and flag["address"] % 4 == 0 and
            section_contains(all_sections, ".dram", flag["address"], 4),
            "Gate flag must be one aligned word in internal DRAM")

    dump = {name: run(objdump, "-d", elf, "--disassemble=" + name)
            for name in ("esp_panic_handler", WRAPPER)}
    panic = instructions(dump["esp_panic_handler"])
    code = instructions(dump[WRAPPER])
    panic_calls = resolved_calls(panic)
    require(any(target == gate["address"] for _, target in panic_calls),
            "Actual SDK panic handler does not call the wrapper")
    require(not any(target == writer["address"] for _, target in panic_calls),
            "SDK panic handler can bypass the wrapper")
    gate_calls = resolved_calls(code)
    require(len(gate_calls) == 1 and gate_calls[0][1] == writer["address"],
            "Wrapper must have exactly one call, to the unchanged SDK writer")

    # This deliberately accepts only the small, verified load/branch wrapper.
    # A compiler/toolchain change that adds helpers or control flow fails here
    # and requires a fresh review instead of silently widening the contract.
    allowed = {"entry", "mov", "mov.n", "or", "memw", "l32r", "l32i", "l32i.n",
               "beqz", "beqz.n", "call8", "callx8", "retw", "retw.n", "nop", "nop.n"}
    require(code and all(item["operation"] in allowed for item in code),
            "Wrapper contains an unreviewed instruction/helper path")
    require(code[0]["operation"] == "entry" and
            sum(item["operation"] in ("retw", "retw.n") for item in code) == 1,
            "Wrapper must have one entry and one return")
    literals = [literal(item) for item in code if item["operation"] == "l32r"]
    require(literals and all(item is not None for item in literals), "Unresolved wrapper literal")
    require(all(section_contains(all_sections, ".iram", item[1], 4) for item in literals),
            "Wrapper literal pool must be in IRAM, not cached flash")
    require(all(item[2] in (flag["address"], writer["address"]) for item in literals),
            "Wrapper reads an unreviewed literal/data address")

    loads = [item for item in code if item["operation"] in ("l32i", "l32i.n")]
    branches = [item for item in code if item["operation"] in ("beqz", "beqz.n")]
    require(len(loads) == 1 and len(branches) == 1, "Expected one flag load and one Off branch")
    load = re.fullmatch(r"(a\d+),\s*(a\d+),\s*0", loads[0]["arguments"])
    branch = re.match(r"(a\d+),\s*([\da-fA-F]+)\s", branches[0]["arguments"])
    require(load and branch and load.group(1) == branch.group(1), "Off branch must test the loaded flag")
    # Follow the actual prefix register values, so a literal/register overwrite
    # cannot turn the flag load into an access to cached flash or other data.
    registers = {}
    for item in code:
        loaded = literal(item)
        if loaded:
            registers[loaded[0]] = loaded[2]
        elif item["operation"] in ("l32i", "l32i.n"):
            require(registers.get(load.group(2)) == flag["address"],
                    "Flag load must read the verified DRAM word")
            registers[load.group(1)] = "flag value"
        elif item["operation"] in ("mov", "mov.n", "or"):
            parts = [part.strip() for part in item["arguments"].split(",")]
            registers[parts[0]] = registers.get(parts[1]) if len(parts) >= 2 else None
        elif item["operation"] in ("beqz", "beqz.n"):
            require(registers.get(branch.group(1)) == "flag value",
                    "Off branch must test the actual DRAM flag value")
            break
        elif item["operation"] in ("retw", "retw.n"):
            raise RuntimeError("Wrapper returns before checking the gate")
    return_address = int(branch.group(2), 16)
    target = next((item for item in code if item["address"] == return_address), None)
    require(target and target["operation"] in ("retw", "retw.n"), "Off branch must return directly")
    require(loads[0]["address"] < branches[0]["address"] < gate_calls[0][0] < return_address,
            "Off branch must skip the sole SDK writer call")

    return {"elf": str(elf.resolve()), "elf_sha256": hashlib.sha256(elf.read_bytes()).hexdigest(),
            "wrapper_address": hex(gate["address"]), "writer_address": hex(writer["address"]),
            "flag_address": hex(flag["address"]), "wrapper_size": gate["size"],
            "sdk_panic_calls_wrapper": True, "wrapper_and_literal_pool_in_iram": True,
            "flag_is_aligned_dram_word": True, "off_returns_without_sdk_calls": True,
            "on_calls_only_real_sdk_writer": True, "physical_device_tested": False,
            "disassembly": dump}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", nargs="?", type=Path)
    parser.add_argument("--output", type=Path, help="Optional JSON evidence file")
    arguments = parser.parse_args()
    tools_dir = Path(os.environ.get("RADIOCLOCK_TOOLS_DIR", "/workspace/.radioclock-tools"))
    sketch = active_sketch()
    elf = arguments.elf or tools_dir / "output" / sketch / (sketch + ".ino.elf")
    try:
        report = validate(elf, tools_dir)
        if arguments.output:
            arguments.output.parent.mkdir(parents=True, exist_ok=True)
            arguments.output.write_text(json.dumps(report, indent=2) + "\n")
        print("PASS crash dump gate: SDK panic -> IRAM wrapper; Off skips flash writer; "
              "On delegates to SDK; aligned DRAM flag " + report["flag_address"])
        return 0
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print("FAIL crash dump gate: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
