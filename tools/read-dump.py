#!/usr/bin/env python3
"""Summarize a Windows minidump: exception record, faulting thread registers,
and a module+RVA-resolved stack scan (the same technique src/core/util/crash.cpp
uses at dump time - no dbghelp, no symbols).

Usage:
  python tools/read-dump.py <dump.dmp> [--threads all|<tid>] [--max-frames N]

The OUTPUT of this tool against a game dump is game-derived content: summarize
findings in the per-game ENGINE_NOTES, never commit the raw output.
Requires: pip install minidump
"""

import argparse
import struct
import sys

from minidump.minidumpfile import MinidumpFile

# x86 CONTEXT offsets (WOW64/native 32-bit CONTEXT layout)
CTX32 = {
    "edi": 0x9C, "esi": 0xA0, "ebx": 0xA4, "edx": 0xA8, "ecx": 0xAC,
    "eax": 0xB0, "ebp": 0xB4, "eip": 0xB8, "eflags": 0xC0, "esp": 0xC4,
}

EXC_NAMES = {
    0xC0000005: "ACCESS_VIOLATION",
    0xC0000409: "STACK_BUFFER_OVERRUN",
    0xC00000FD: "STACK_OVERFLOW",
    0x80000003: "BREAKPOINT",
    0xC000001D: "ILLEGAL_INSTRUCTION",
    0xC0000374: "HEAP_CORRUPTION",
    0xE06D7363: "CPP_EXCEPTION",
}

AV_KIND = {0: "READ", 1: "WRITE", 8: "DEP_EXECUTE"}


def load_modules(mf):
    mods = []
    for m in mf.modules.modules:
        name = m.name.split("\\")[-1] if m.name else "?"
        mods.append((m.baseaddress, m.baseaddress + m.size, name))
    mods.sort()
    return mods


def resolve(mods, addr):
    for base, end, name in mods:
        if base <= addr < end:
            return "%s+0x%X" % (name, addr - base)
    return None


def read_file_bytes(path, rva, size):
    with open(path, "rb") as f:
        f.seek(rva)
        return f.read(size)


def parse_ctx32(raw):
    if len(raw) < 0xC8:
        return None
    return {k: struct.unpack_from("<I", raw, off)[0] for k, off in CTX32.items()}


def loc_rva(loc):
    # minidump exposes MINIDUMP_LOCATION_DESCRIPTOR fields with either name
    return getattr(loc, "Rva", None) if hasattr(loc, "Rva") else None


def thread_context(path, thread, exc_ctx_loc=None):
    """Prefer the exception stream's CONTEXT (state AT the fault) over the
    thread-list one (state when the crash handler wrote the dump)."""
    if exc_ctx_loc is not None:
        rva = getattr(exc_ctx_loc, "Rva", None)
        size = getattr(exc_ctx_loc, "DataSize", None)
        if rva is None:  # this package version names them differently
            rva = getattr(exc_ctx_loc, "location", None)
        if rva is not None and size:
            ctx = parse_ctx32(read_file_bytes(path, rva, size))
            if ctx:
                return ctx
    return parse_ctx32(read_file_bytes(path, thread.ThreadContext.Rva,
                                       thread.ThreadContext.DataSize))


def stack_scan(path, mf, mods, thread, ctx, max_frames):
    """Resolve every stack dword that lands inside a module to module+RVA.
    Not a real unwind - a superset of the true stack, exactly like crash.cpp's
    walk. Frames are printed stack-top first."""
    stack_va = thread.Stack.StartOfMemoryRange
    stack_rva = thread.Stack.MemoryLocation.Rva
    stack_size = thread.Stack.MemoryLocation.DataSize
    raw = read_file_bytes(path, stack_rva, stack_size)
    start = 0
    if ctx and stack_va <= ctx["esp"] < stack_va + stack_size:
        start = ctx["esp"] - stack_va
    frames = []
    for off in range(start & ~3, len(raw) - 3, 4):
        val = struct.unpack_from("<I", raw, off)[0]
        hit = resolve(mods, val)
        if hit:
            frames.append((stack_va + off, val, hit))
            if len(frames) >= max_frames:
                break
    return frames


def ebp_walk(path, mods, thread, ctx, max_frames=32):
    """Standard x86 frame-pointer walk: ebp -> [saved ebp, return address].
    Only trustworthy while frames keep FPO off, but when it works it gives the
    TRUE call chain, unlike the scan's superset."""
    stack_va = thread.Stack.StartOfMemoryRange
    stack_rva = thread.Stack.MemoryLocation.Rva
    stack_size = thread.Stack.MemoryLocation.DataSize
    raw = read_file_bytes(path, stack_rva, stack_size)
    frames = []
    ebp = ctx["ebp"]
    for _ in range(max_frames):
        off = ebp - stack_va
        if off < 0 or off + 8 > len(raw):
            break
        saved_ebp, ret = struct.unpack_from("<II", raw, off)
        if ret == 0:
            break
        frames.append((ebp, ret, resolve(mods, ret) or "UNMAPPED %08X" % ret))
        if saved_ebp <= ebp:  # must move toward the stack base
            break
        ebp = saved_ebp
    return frames


def dump_thread(path, mf, mods, thread, max_frames, faulting=False,
                exc_ctx_loc=None):
    ctx = thread_context(path, thread, exc_ctx_loc if faulting else None)
    tag = " (FAULTING, context at fault)" if faulting and exc_ctx_loc else (
        " (FAULTING)" if faulting else "")
    print("\n-- thread tid=%u%s" % (thread.ThreadId, tag))
    if ctx:
        print("   eip=%08X esp=%08X ebp=%08X" % (ctx["eip"], ctx["esp"], ctx["ebp"]))
        print("   eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X" %
              (ctx["eax"], ctx["ebx"], ctx["ecx"], ctx["edx"], ctx["esi"], ctx["edi"]))
        at = resolve(mods, ctx["eip"])
        print("   eip resolves: %s" % (at or "UNMAPPED (freed/dynamic memory)"))
    try:
        if ctx:
            chain = ebp_walk(path, mods, thread, ctx)
            if chain:
                print("   ebp chain (true frames while FPO is off):")
                for ebp, ret, hit in chain:
                    print("     ebp=%08X ret %08X  %s" % (ebp, ret, hit))
        frames = stack_scan(path, mf, mods, thread, ctx, max_frames)
        print("   raw scan (superset - includes stale values):")
        for va, val, hit in frames:
            print("   [%08X] %08X  %s" % (va, val, hit))
    except Exception as e:  # stack memory absent from the dump
        print("   stack scan failed: %s" % e)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--threads", default=None,
                    help="'all' or a tid; default: faulting thread only")
    ap.add_argument("--max-frames", type=int, default=48)
    args = ap.parse_args()

    mf = MinidumpFile.parse(args.dump)
    mods = load_modules(mf)

    print("== %s" % args.dump)
    if mf.sysinfo:
        print("os build %s, arch %s" % (mf.sysinfo.BuildNumber,
                                        mf.sysinfo.ProcessorArchitecture))
    print("\n-- modules of interest")
    for base, end, name in mods:
        n = name.lower()
        if any(k in n for k in ("bioshock", "bvr", "xinput", "openxr", "d3d11",
                                "dxgi", "user32", "kernel32", "ntdll")):
            print("   %08X-%08X  %s" % (base, end, name))

    exc_tid = None
    exc_ctx_loc = None
    if mf.exception and mf.exception.exception_records:
        for rec in mf.exception.exception_records:
            er = rec.ExceptionRecord
            exc_tid = rec.ThreadId
            exc_ctx_loc = rec.ThreadContext
            code = er.ExceptionCode
            code = int(getattr(code, "value", code)) & 0xFFFFFFFF
            name = EXC_NAMES.get(code, "0x%08X" % code)
            print("\n-- exception: %s at %08X (%s) tid=%u" %
                  (name, er.ExceptionAddress,
                   resolve(mods, er.ExceptionAddress) or "UNMAPPED",
                   rec.ThreadId))
            info = [int(x) for x in (er.ExceptionInformation or [])]
            if code == 0xC0000005 and len(info) >= 2:
                kind = AV_KIND.get(info[0], str(info[0]))
                print("   %s of address %08X" % (kind, info[1]))
    else:
        print("\n-- no exception stream")

    threads = mf.threads.threads
    if args.threads == "all":
        for t in threads:
            dump_thread(args.dump, mf, mods, t, args.max_frames,
                        faulting=(t.ThreadId == exc_tid), exc_ctx_loc=exc_ctx_loc)
    elif args.threads:
        want = int(args.threads)
        for t in threads:
            if t.ThreadId == want:
                dump_thread(args.dump, mf, mods, t, args.max_frames,
                            faulting=(t.ThreadId == exc_tid), exc_ctx_loc=exc_ctx_loc)
    else:
        for t in threads:
            if t.ThreadId == exc_tid:
                dump_thread(args.dump, mf, mods, t, args.max_frames, faulting=True,
                            exc_ctx_loc=exc_ctx_loc)


if __name__ == "__main__":
    sys.exit(main())
