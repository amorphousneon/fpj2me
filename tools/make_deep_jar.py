#!/usr/bin/env python3
"""
fpj2me — tools/make_deep_jar.py : regenerates tests/deep.jar.

A 24-deep super chain of REAL (hand-assembled, deflated) classes:
    D0 extends D1 extends ... extends D23 extends java/lang/Object
Each class has an instance int field and an <init>()V that calls its
super's <init>; D0 also has startApp()V. MIDlet-1: D0.

Why: the app stack on the phone is fixed at 36 KB (fpdoom
sc6531e_fdl.ld EXTRA_SIZE 0x9000). The OLD class_load recursed once
per hierarchy level and every level's jar_read() kept an 8.4 KB
tinfl_decompressor on the C stack -> a real Gameloft hierarchy
(10+ classes) overflowed the stack and froze the phone mid-boot.
tests/run.sh boots THIS jar inside a 36 KB-stack thread to prove the
iterative loader + static inflate state keep the stack flat.
"""
import struct, zlib, os, sys

N_CHAIN = 24

# ZIP container fields are LITTLE-endian (PKWARE APPNOTE); the Java
# classfile payload inside is BIG-endian. Keep the two helpers apart:
#   z2/z4 = zip structure fields (local/central/EOCD headers)
#   u1/u2/u4 = classfile fields
def z2(n): return struct.pack('<H', n)
def z4(n): return struct.pack('<I', n)

def u1(n): return struct.pack('>B', n)
def u2(n): return struct.pack('>H', n)
def u4(n): return struct.pack('>I', n)
def utf8(s):
    b = s.encode()
    return u1(1) + u2(len(b)) + b
def cls(idx):  return u1(7) + u2(idx)
def nat(n, d): return u1(12) + u2(n) + u2(d)
def mref(c, nt): return u1(10) + u2(c) + u2(nt)

def code_attr(name_idx, code, max_stack=1, max_locals=1):
    body = u2(max_stack) + u2(max_locals) + u4(len(code)) + code \
         + u2(0) + u2(0)
    return u2(name_idx) + u4(len(body)) + body

def method(access, name, sig, code):
    return u2(access) + u2(name) + u2(sig) + u2(1) + code

def build_chain_class(i):
    """class D{i} extends D{i+1} (or java/lang/Object for the last)."""
    name = f"D{i}"
    sup  = f"D{i + 1}" if i + 1 < N_CHAIN else "java/lang/Object"

    cp = [None]                        # 1-based
    cp.append(utf8(name))              # 1
    cp.append(cls(1))                  # 2 this
    cp.append(utf8(sup))               # 3
    cp.append(cls(3))                  # 4 super
    cp.append(utf8("<init>"))          # 5
    cp.append(utf8("()V"))             # 6
    cp.append(nat(5, 6))               # 7 NameAndType <init> ()V
    cp.append(mref(4, 7))              # 8 Methodref super.<init>
    cp.append(utf8("Code"))            # 9
    cp.append(utf8(f"f{i}"))           # 10 instance field
    cp.append(utf8("I"))               # 11
    if i == 0:
        cp.append(utf8("startApp"))    # 12
        cp.append(utf8("()V"))         # 13 (same sig text, own entry)

    # <init>()V: aload_0; invokespecial #8; return
    init_code = b"\x2a\xb7" + u2(8) + b"\xb1"
    methods = method(0x0001, 5, 6, code_attr(9, init_code, 1, 1))
    if i == 0:
        # startApp()V: return
        methods += method(0x0001, 12, 13, code_attr(9, b"\xb1", 0, 0))

    out = u4(0xCAFEBABE) + u2(0) + u2(46)      # magic, minor, major
    out += u2(len(cp))                          # cp count
    for e in cp[1:]:
        out += e
    out += u2(0x0021)                           # public super
    out += u2(2)                                # this
    out += u2(4)                                # super
    out += u2(0)                                # interfaces
    out += u2(1) + u2(0x0002) + u2(10) + u2(11) + u2(0)   # f{i} I
    out += u2(2 if i == 0 else 1) + methods     # methods count + records
    return out

def local_entry(name, data, method):
    crc = zlib.crc32(data) & 0xFFFFFFFF
    usize = len(data)
    if method == 8:
        co = zlib.compressobj(9, zlib.DEFLATED, -15)
        data = co.compress(data) + co.flush()
    return name, data, method, crc, usize

def build_jar(path):
    files = [local_entry(f"{name}.class", build_chain_class(i), 8)
             for i, name in enumerate(f"D{i}" for i in range(N_CHAIN))]
    files.append(local_entry("META-INF/MANIFEST.MF",
        b"MIDlet-1: D0, , D0\nMIDlet-Name: deep\nMIDlet-Version: 1.0\n"
        b"MicroEdition-Profile: MIDP-2.0\nMicroEdition-Configuration: CLDC-1.1\n",
        8))
    files.sort(key=lambda f: f[0])

    out = b""
    central = b""
    for name, data, method, crc, usize in files:
        off = len(out)
        out += b"PK\x03\x04" + z2(20) + z2(0) + z2(method) + z2(0) + z2(0) \
             + z4(crc) + z4(len(data)) + z4(usize) + z2(len(name)) + z2(0) \
             + name.encode() + data
        central += b"PK\x01\x02" + z2(20) + z2(20) + z2(0) + z2(method) \
             + z2(0) + z2(0) + z4(crc) + z4(len(data)) + z4(usize) \
             + z2(len(name)) + z2(0) + z2(0) + z2(0) + z2(0) + z4(0) \
             + z4(off) + name.encode()
    out += central + b"PK\x05\x06" + z2(0) + z2(0) \
         + z2(len(files)) + z2(len(files)) + z4(len(central)) + z4(len(out)) \
         + z2(0)

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb") as f:
        f.write(out)
    print(f"{path}: {N_CHAIN} classes, {len(out)} bytes")

if __name__ == "__main__":
    build_jar(sys.argv[1] if len(sys.argv) > 1
              else os.path.join(os.path.dirname(__file__) or ".",
                                "..", "tests", "deep.jar"))
