#!/usr/bin/env python3
"""
fpj2me — tools/make_usbcast_jar.py : regenerates UsbCast.jar (Round 10).

UsbCast turns the it2163 into a live 128x160 USB monitor. The MIDlet
lives ENTIRELY on the C fast path: per frame it does ~25 bytecode ops
of header checking, one bulk native read (C memcpy), one native
drawPixels (C row-memcpy RGB565 blit), repaint + serviceRepaints and
a 1-byte native ACK write. ZERO per-pixel math in bytecode, ZERO
per-frame heap allocation: the 40,978-byte frame buffer, the mutable
Image, its Graphics and the DirectGraphics handle are all allocated
once in <init> and reused until the MIDlet exits.

Generated classes (hand-assembled classfiles, javac-free, same style
as tools/make_test_jar.py):

  UsbCast (extends javax.microedition.midlet.MIDlet)
      static UsbCast inst;
      startApp()      inst=this; Display.getDisplay(this);
                      CastScr s = new CastScr(); d.setCurrent(s);
                      s.run();                // BLOCKING cast loop
                      inst.notifyDestroyed(); // clean return to launcher
      pauseApp()      {}
      destroyApp(Z)   {}

  CastScr (extends javax.microedition.lcdui.Canvas)
      byte[] buf = new byte[40978];          // 9 hdr + 40960 payload
      Image img; Graphics g; DirectGraphics dg;    // cached once
      StreamConnection conn; InputStream in; OutputStream out;
      boolean closed;  int state;             // 0=waiting, 1=casting
      static int frames, painted, acks;       // hostcheck/boot vitals
      paint(g)       painted++; black clear; state==1
                      ? g.drawImage(img,0,0,TOP|LEFT=20)
                      : "USB CAST" / "WAITING FOR USB" text
      keyPressed(k)  k==-7 (RSOFT = the exit key) -> closed=true;
                      in.close() — close() ABORTS any blocked native
                      read, so the exit key always breaks a USB wait
      run()          status screen; Connector.open("comm:usb");
                      streams; out.write('R') READY hello;
                      loop { sync FPJ2 magic; read w/h/flags;
                             128x160 only (else resync);
                             readN(9, 40960) payload;
                             dg.drawPixels(buf, null, 9, 128, 0, 0,
                                128, 160, 0, 565  = 565 RGB);
                             state=1; repaint(); serviceRepaints();
                             out.write('K') ACK; acks++; }
                      out.write('Q') QUIT; close everything;
                      catch(Exception) -> just return frames
      sync()Z        scan for "FPJ2" magic byte-by-byte (self-sync:
                      a mid-stream connect resyncs at the next header)
      readN(II)I     fill buf[off..off+len) via in.read([BII); the
                      native fills the WHOLE request or reports -1
                      (close()/link-EOF) — never a partial lie

Frame protocol (see tools/usbcast_host.py, the sender):
  "FPJ2" | u16le width=128 | u16le height=160 | u8 flags=0 (RGB565)
  | 128*160*2 bytes little-endian RGB565 payload
  Phone->host: 'R'(ready) on boot, 'K' after each displayed frame
  (backpressure: the host waits for it), 'Q' when the MIDlet exits.

Byte-order note: fpj2me's DirectGraphics.drawPixels byte[] overload
treats `offset` as a BYTE offset (payload starts at buf[9]) and the
pixel pairs as little-endian RGB565 — exactly the wire format, so
the blit is one memcpy per row on the little-endian ARM926.
"""
import struct, sys, os, zipfile

def u1(n): return struct.pack('>B', n)
def u2(n): return struct.pack('>H', n)
def u4(n): return struct.pack('>I', n)

def utf8(s):
    b = s.encode()
    return u1(1) + u2(len(b)) + b

# ------------------------------------------------------------------ #
# constant pool with computed slots (1-based; Long/Double take 2)     #
# ------------------------------------------------------------------ #
class CP:
    def __init__(self):
        self.e = [None]
        self.n = 1
        self.cache = {}

    def add(self, tag, payload):
        key = (tag, payload)
        if key in self.cache:
            return self.cache[key]
        idx = self.n
        self.e.append(u1(tag) + payload)
        self.n += 2 if tag in (5, 6) else 1
        self.cache[key] = idx
        return idx

    def utf(self, s):     return self.add(1, utf8(s)[1:])
    def cls(self, name):  return self.add(7, u2(self.utf(name)))
    def cstr(self, i):    return self.add(8, u2(i))
    def cint(self, v):    return self.add(3, u4(v & 0xFFFFFFFF))
    def nat(self, n, d):  return self.add(12, u2(n) + u2(d))
    def mref(self, c, nt): return self.add(10, u2(c) + u2(nt))
    def fref(self, c, nt): return self.add(9, u2(c) + u2(nt))

    def fld(self, cls_name, fname, sig):
        return self.fref(self.cls(cls_name),
                         self.nat(self.utf(fname), self.utf(sig)))
    def meth(self, cls_name, mname, sig):
        return self.mref(self.cls(cls_name),
                         self.nat(self.utf(mname), self.utf(sig)))

    def bytes(self):
        return b''.join(x for x in self.e if x is not None)

# ------------------------------------------------------------------ #
# bytecode emitter with symbolic labels (branch fixups)               #
# ------------------------------------------------------------------ #
class Code:
    def __init__(self):
        self.buf = bytearray()
        self.labels = {}
        self.fix = []            # (pos, label)

    def op(self, *bs):
        for b in bs:
            self.buf.append(b & 0xFF)

    def ref(self, op, idx):      # opcode + u2 cp index
        self.buf.append(op)
        self.buf += u2(idx)

    def ldc_w(self, idx):         # ldc_w 0x13: u2 cp index (safe > 255)
        self.buf.append(0x13)
        self.buf += u2(idx)

    def branch(self, op, label):
        self.buf.append(op)
        self.fix.append((len(self.buf), label))
        self.buf += u2(0)

    def goto_(self, label):      self.branch(0xA7, label)
    def if_(self, op, label):    self.branch(op, label)

    def label(self, name):
        self.labels[name] = len(self.buf)

    def finish(self):
        # JVMS: branch offset is relative to the OPCODE's address
        # (fpj2me's BRANCH macro does pc += o - 3, i.e. opcode + o).
        # fix positions were recorded at the operand (opcode + 1).
        for pos, lab in self.fix:
            off = self.labels[lab] - (pos - 1)
            self.buf[pos:pos + 2] = u2(off & 0xFFFF)
        return bytes(self.buf)

# opcodes
ALOAD_0, ALOAD_1 = 0x2A, 0x2B
ASTORE_1, ASTORE_2 = 0x4C, 0x4D
ILOAD_1, ILOAD_2, ILOAD_3 = 0x1B, 0x1C, 0x1D
ISTORE_1, ISTORE_2, ISTORE_3 = 0x3C, 0x3D, 0x3E
ILOAD_N, ISTORE_N = 0x15, 0x36
ICONST_0, ICONST_1, ICONST_2, ICONST_3, ICONST_4, ICONST_5 = 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
ICONST_M1 = 0x02
IADD, ISUB, IAND, ISHL, IOR = 0x60, 0x64, 0x7E, 0x78, 0x80
BALOAD = 0x33
ACONST_NULL = 0x01
POP, RETURN, IRETURN = 0x57, 0xB1, 0xAC
GETFIELD, PUTFIELD = 0xB4, 0xB5
GETSTATIC, PUTSTATIC = 0xB2, 0xB3
INVOKEVIRTUAL, INVOKESPECIAL, INVOKESTATIC = 0xB6, 0xB7, 0xB8
LDC = 0x12
NEWARRAY, T_BYTE = 0xBC, 4
SIPUSH, BIPUSH = 0x11, 0x10
IFEQ, IFNE, IFLT, IF_ICMPEQ, IF_ICMPNE, IF_ICMPGE = \
    0x99, 0x9A, 0x9B, 0x9F, 0xA0, 0xA2
IFNULL = 0xC6
DUP, NEW = 0x59, 0xBB

def mkmethod(cp, access, name, sig, code, stack, locals_n, exc_entries=()):
    """exc_entries: list of (start_lab, end_lab, handler_lab, class_name
    or None for catch-all), where the *_lab are Lab objects whose
    .offset was set after Code.finish() (labels don't move: branch
    fixups never change code length)."""
    body = u2(stack) + u2(locals_n) + u4(len(code)) + code
    tbl = b''
    for s_lab, e_lab, h_lab, cname in exc_entries:
        tbl += u2(s_lab.offset) + u2(e_lab.offset) + u2(h_lab.offset)
        tbl += u2(cp.cls(cname) if cname else 0)
    body += u2(len(exc_entries)) + tbl
    body += u2(0)
    return u2(access) + u2(cp.utf(name)) + u2(cp.utf(sig)) + \
        u2(1) + u2(cp.utf("Code")) + u4(len(body)) + body

class Lab:
    """named label handle; .offset is valid after the code is emitted"""
    def __init__(self, c):
        self.c = c
        self.offset = -1
    def here(self, name):
        self.c.label(name)
        self.offset = len(self.c.buf)
        return self

def classfile(cp, access_flags, this_name, super_idx, field_list, methods):
    out = u4(0xCAFEBABE) + u2(0) + u2(48)      # v1.4, CLDC-safe
    out += u2(cp.n) + cp.bytes()               # JVMS: count = last idx + 1
    out += u2(access_flags)
    out += u2(cp.cls(this_name)) + u2(super_idx) + u2(0)  # no ifaces
    out += u2(len(field_list)) + b''.join(field_list)
    out += u2(len(methods))
    for m in methods:
        out += m
    out += u2(0)                                # class attrs
    return out

# ------------------------------------------------------------------ #
# UsbCast (the MIDlet)                                                #
# ------------------------------------------------------------------ #
def build_usbcast():
    cp = CP()
    n_midlet = cp.cls("javax/microedition/midlet/MIDlet")
    m_midlet_init = cp.meth("javax/microedition/midlet/MIDlet", "<init>", "()V")
    m_notifyDest = cp.meth("javax/microedition/midlet/MIDlet",
                           "notifyDestroyed", "()V")
    m_getDisplay = cp.meth("javax/microedition/lcdui/Display", "getDisplay",
                           "(Ljavax/microedition/midlet/MIDlet;)"
                           "Ljavax/microedition/lcdui/Display;")
    m_setCurrent = cp.meth("javax/microedition/lcdui/Display", "setCurrent",
                           "(Ljavax/microedition/lcdui/Displayable;)V")
    n_castscr = cp.cls("CastScr")
    m_scr_init = cp.meth("CastScr", "<init>", "()V")
    m_scr_run = cp.meth("CastScr", "run", "()I")
    f_inst = cp.fld("UsbCast", "inst", "LUsbCast;")

    # <init>()V: super(); return
    c = Code()
    c.op(ALOAD_0)
    c.ref(INVOKESPECIAL, m_midlet_init)
    c.op(RETURN)
    init_m = mkmethod(cp, 0x0001, "<init>", "()V", c.finish(), 1, 1)

    # startApp()V
    c = Code()
    c.op(ALOAD_0); c.ref(PUTSTATIC, f_inst)              # inst = this
    c.op(ALOAD_0); c.ref(INVOKESTATIC, m_getDisplay); c.op(ASTORE_1)
    c.op(NEW); c.buf += u2(n_castscr)
    c.op(DUP); c.ref(INVOKESPECIAL, m_scr_init); c.op(ASTORE_2)
    c.op(ALOAD_1); c.buf.append(0x2C)                   # aload_2 (s)
    c.ref(INVOKEVIRTUAL, m_setCurrent)
    c.buf.append(0x2C)                                   # aload_2 (s)
    c.ref(INVOKEVIRTUAL, m_scr_run)
    c.op(POP)
    c.ref(GETSTATIC, f_inst)
    c.ref(INVOKEVIRTUAL, m_notifyDest)
    c.op(RETURN)
    start_m = mkmethod(cp, 0x0001, "startApp", "()V", c.finish(), 2, 3)

    pause_m = mkmethod(cp, 0x0001, "pauseApp", "()V", b'\xB1', 0, 1)
    destr_m = mkmethod(cp, 0x0001, "destroyApp", "(Z)V", b'\xB1', 0, 2)

    fields = [u2(0x000A) + u2(cp.utf("inst")) + u2(cp.utf("LUsbCast;")) + u2(0)]
    return classfile(cp, 0x0021, "UsbCast", n_midlet, fields,
                     [init_m, start_m, pause_m, destr_m])

# ------------------------------------------------------------------ #
# CastScr (the cast screen — Canvas)                                  #
# ------------------------------------------------------------------ #
def build_castscr():
    cp = CP()
    n_canvas = cp.cls("javax/microedition/lcdui/Canvas")
    m_canvas_init = cp.meth("javax/microedition/lcdui/Canvas", "<init>", "()V")
    m_repaint = cp.meth("javax/microedition/lcdui/Canvas", "repaint", "()V")
    m_service = cp.meth("javax/microedition/lcdui/Canvas",
                        "serviceRepaints", "()V")

    m_img_create = cp.meth("javax/microedition/lcdui/Image", "createImage",
                           "(II)Ljavax/microedition/lcdui/Image;")
    m_img_gfx = cp.meth("javax/microedition/lcdui/Image", "getGraphics",
                        "()Ljavax/microedition/lcdui/Graphics;")
    m_getdg = cp.meth("com/nokia/mid/ui/DirectUtils", "getDirectGraphics",
                      "(Ljavax/microedition/lcdui/Graphics;)"
                      "Lcom/nokia/mid/ui/DirectGraphics;")
    m_drawpixels = cp.meth("com/nokia/mid/ui/DirectGraphics", "drawPixels",
                           "([B[BIIIIIIII)V")

    m_setcolor = cp.meth("javax/microedition/lcdui/Graphics", "setColor", "(I)V")
    m_fillrect = cp.meth("javax/microedition/lcdui/Graphics", "fillRect", "(IIII)V")
    m_drawimg = cp.meth("javax/microedition/lcdui/Graphics", "drawImage",
                        "(Ljavax/microedition/lcdui/Image;III)V")
    m_drawstr = cp.meth("javax/microedition/lcdui/Graphics", "drawString",
                        "(Ljava/lang/String;III)V")

    m_open = cp.meth("javax/microedition/io/Connector", "open",
                     "(Ljava/lang/String;)"
                     "Ljavax/microedition/io/StreamConnection;")
    m_sc_in = cp.meth("javax/microedition/io/StreamConnection",
                      "openInputStream", "()Ljava/io/InputStream;")
    m_sc_out = cp.meth("javax/microedition/io/StreamConnection",
                       "openOutputStream", "()Ljava/io/OutputStream;")
    m_sc_close = cp.meth("javax/microedition/io/StreamConnection",
                         "close", "()V")
    m_in_read = cp.meth("java/io/InputStream", "read", "()I")
    m_in_read3 = cp.meth("java/io/InputStream", "read", "([BII)I")
    m_in_close = cp.meth("java/io/InputStream", "close", "()V")
    m_out_write = cp.meth("java/io/OutputStream", "write", "(I)V")
    m_out_close = cp.meth("java/io/OutputStream", "close", "()V")

    c_str_url = cp.cstr(cp.utf("comm:usb"))
    c_str_t1 = cp.cstr(cp.utf("USB CAST"))
    c_str_t2 = cp.cstr(cp.utf("WAITING FOR USB"))
    c_int_white = cp.cint(0x00FFFFFF)

    f_buf = cp.fld("CastScr", "buf", "[B")
    f_img = cp.fld("CastScr", "img", "Ljavax/microedition/lcdui/Image;")
    f_g = cp.fld("CastScr", "g", "Ljavax/microedition/lcdui/Graphics;")
    f_dg = cp.fld("CastScr", "dg", "Lcom/nokia/mid/ui/DirectGraphics;")
    f_conn = cp.fld("CastScr", "conn",
                    "Ljavax/microedition/io/StreamConnection;")
    f_in = cp.fld("CastScr", "in", "Ljava/io/InputStream;")
    f_out = cp.fld("CastScr", "out", "Ljava/io/OutputStream;")
    f_closed = cp.fld("CastScr", "closed", "Z")
    f_state = cp.fld("CastScr", "state", "I")
    sf_frames = cp.fld("CastScr", "frames", "I")
    sf_painted = cp.fld("CastScr", "painted", "I")
    sf_acks = cp.fld("CastScr", "acks", "I")
    m_sync = cp.meth("CastScr", "sync", "()Z")
    m_readn = cp.meth("CastScr", "readN", "(II)I")

    # ---- <init>()V -----------------------------------------------
    c = Code()
    c.op(ALOAD_0); c.ref(INVOKESPECIAL, m_canvas_init)
    c.op(ALOAD_0); c.ldc_w(cp.cint(40978))     # ldc 40978 (>32767:
    c.op(NEWARRAY); c.buf.append(T_BYTE); c.ref(PUTFIELD, f_buf)  # no sipush)
    c.op(ALOAD_0); c.op(SIPUSH); c.buf += u2(128)
    c.op(SIPUSH); c.buf += u2(160)
    c.ref(INVOKESTATIC, m_img_create); c.ref(PUTFIELD, f_img)
    c.op(ALOAD_0); c.op(ALOAD_0); c.ref(GETFIELD, f_img)
    c.ref(INVOKEVIRTUAL, m_img_gfx); c.ref(PUTFIELD, f_g)
    c.op(ALOAD_0); c.op(ALOAD_0); c.ref(GETFIELD, f_g)
    c.ref(INVOKESTATIC, m_getdg); c.ref(PUTFIELD, f_dg)
    c.op(RETURN)
    init_m = mkmethod(cp, 0x0001, "<init>", "()V", c.finish(), 3, 2)

    # ---- paint(Graphics)V ----------------------------------------
    c = Code()
    c.ref(GETSTATIC, sf_painted); c.op(ICONST_1); c.op(IADD)
    c.ref(PUTSTATIC, sf_painted)
    c.op(ALOAD_1); c.op(ICONST_0); c.ref(INVOKEVIRTUAL, m_setcolor)
    c.op(ALOAD_1); c.op(ICONST_0); c.op(ICONST_0)
    c.op(SIPUSH); c.buf += u2(128); c.op(SIPUSH); c.buf += u2(160)
    c.ref(INVOKEVIRTUAL, m_fillrect)
    c.op(ALOAD_0); c.ref(GETFIELD, f_state)
    c.if_(IFEQ, "text")
    c.op(ALOAD_1); c.op(ALOAD_0); c.ref(GETFIELD, f_img)
    c.op(ICONST_0); c.op(ICONST_0); c.op(BIPUSH); c.buf.append(20)
    c.ref(INVOKEVIRTUAL, m_drawimg)
    c.goto_("end")
    c.label("text")
    c.op(ALOAD_1); c.ldc_w(c_int_white)
    c.ref(INVOKEVIRTUAL, m_setcolor)
    c.op(ALOAD_1); c.ldc_w(c_str_t1)
    c.op(ICONST_0); c.op(SIPUSH); c.buf += u2(20)
    c.op(BIPUSH); c.buf.append(20)
    c.ref(INVOKEVIRTUAL, m_drawstr)
    c.op(ALOAD_1); c.ldc_w(c_str_t2)
    c.op(ICONST_0); c.op(SIPUSH); c.buf += u2(40)
    c.op(BIPUSH); c.buf.append(20)
    c.ref(INVOKEVIRTUAL, m_drawstr)
    c.label("end")
    c.op(RETURN)
    paint_m = mkmethod(cp, 0x0001, "paint",
                       "(Ljavax/microedition/lcdui/Graphics;)V",
                       c.finish(), 4, 2)

    # ---- keyPressed(I)V: -7 (RSOFT) = the exit key ---------------
    c = Code()
    c.op(ILOAD_1); c.op(BIPUSH); c.buf.append(0xF9)      # -7
    c.if_(IF_ICMPNE, "end")
    c.op(ALOAD_0); c.op(ICONST_1); c.ref(PUTFIELD, f_closed)
    c.op(ALOAD_0); c.ref(GETFIELD, f_in)
    c.if_(IFNULL, "end")
    c.op(ALOAD_0); c.ref(GETFIELD, f_in)
    c.ref(INVOKEVIRTUAL, m_in_close)
    c.label("end")
    c.op(RETURN)
    key_m = mkmethod(cp, 0x0001, "keyPressed", "(I)V", c.finish(), 2, 2)

    # ---- sync()Z : scan for the "FPJ2" magic ----------------------
    c = Code()
    c.op(ICONST_0); c.op(ISTORE_1)                      # match = 0
    c.label("loop")
    c.op(ILOAD_1); c.op(ICONST_4)
    c.if_(IF_ICMPGE, "done")
    c.op(ALOAD_0); c.ref(GETFIELD, f_in)
    c.ref(INVOKEVIRTUAL, m_in_read); c.op(ISTORE_2)     # b = in.read()
    c.op(ILOAD_2); c.if_(IFLT, "false")                 # EOF/abort
    c.op(ILOAD_1)
    c.if_(IFNE, "m1")
    c.op(ILOAD_2); c.op(BIPUSH); c.buf.append(70)      # 'F'
    c.if_(IF_ICMPEQ, "hit"); c.goto_("nomatch")
    c.label("m1")
    c.op(ILOAD_1); c.op(ICONST_1); c.if_(IF_ICMPNE, "m2")
    c.op(ILOAD_2); c.op(BIPUSH); c.buf.append(80)      # 'P'
    c.if_(IF_ICMPEQ, "hit"); c.goto_("nomatch")
    c.label("m2")
    c.op(ILOAD_1); c.op(ICONST_2); c.if_(IF_ICMPNE, "m3")
    c.op(ILOAD_2); c.op(BIPUSH); c.buf.append(74)      # 'J'
    c.if_(IF_ICMPEQ, "hit"); c.goto_("nomatch")
    c.label("m3")
    c.op(ILOAD_2); c.op(BIPUSH); c.buf.append(50)      # '2'
    c.if_(IF_ICMPEQ, "hit")
    c.label("nomatch")                                  # match = (b=='F')
    c.op(ILOAD_2); c.op(BIPUSH); c.buf.append(70)
    c.if_(IF_ICMPNE, "loop")
    c.op(ICONST_1); c.op(ISTORE_1)
    c.goto_("loop")
    c.label("hit")
    c.op(ILOAD_1); c.op(ICONST_1); c.op(IADD); c.op(ISTORE_1)
    c.goto_("loop")
    c.label("done")
    c.op(ICONST_1); c.op(IRETURN)
    c.label("false")
    c.op(ICONST_0); c.op(IRETURN)
    sync_m = mkmethod(cp, 0x0001, "sync", "()Z", c.finish(), 2, 3)

    # ---- readN(II)I : fill buf[off..off+len) ----------------------
    c = Code()
    c.op(ICONST_0); c.op(ISTORE_3)                      # total = 0
    c.label("loop")
    c.op(ILOAD_3); c.op(ILOAD_2); c.if_(IF_ICMPGE, "done")
    c.op(ALOAD_0); c.ref(GETFIELD, f_in)                # in.read(buf,
    c.op(ALOAD_0); c.ref(GETFIELD, f_buf)               #   off+total,
    c.op(ILOAD_1); c.op(ILOAD_3); c.op(IADD)            #   len-total)
    c.op(ILOAD_2); c.op(ILOAD_3); c.op(ISUB)
    c.ref(INVOKEVIRTUAL, m_in_read3); c.op(ISTORE_N); c.buf.append(4)
    c.op(ILOAD_N); c.buf.append(4)
    c.if_(IFLT, "neg")
    c.op(ILOAD_3); c.op(ILOAD_N); c.buf.append(4)
    c.op(IADD); c.op(ISTORE_3)
    c.goto_("loop")
    c.label("neg")
    c.op(ICONST_M1); c.op(IRETURN)
    c.label("done")
    c.op(ILOAD_3); c.op(IRETURN)
    readn_m = mkmethod(cp, 0x0001, "readN", "(II)I", c.finish(), 4, 5)

    # ---- run()I : the cast loop -----------------------------------
    c = Code()
    L = {k: Lab(c) for k in
         ("try", "loop", "cont", "cleanup", "try_end", "catch", "end")}
    c.op(ALOAD_0); c.ref(INVOKEVIRTUAL, m_repaint)
    c.op(ALOAD_0); c.ref(INVOKEVIRTUAL, m_service)
    L["try"].here("try")                                # == handler start
    c.op(ALOAD_0)
    c.ldc_w(c_str_url)
    c.ref(INVOKESTATIC, m_open)
    c.ref(PUTFIELD, f_conn)                             # conn = open(..)
    c.op(ALOAD_0); c.op(ALOAD_0); c.ref(GETFIELD, f_conn)
    c.ref(INVOKEVIRTUAL, m_sc_in); c.ref(PUTFIELD, f_in)
    c.op(ALOAD_0); c.op(ALOAD_0); c.ref(GETFIELD, f_conn)
    c.ref(INVOKEVIRTUAL, m_sc_out); c.ref(PUTFIELD, f_out)
    c.op(ALOAD_0); c.ref(GETFIELD, f_out)               # out.write('R')
    c.op(BIPUSH); c.buf.append(82)
    c.ref(INVOKEVIRTUAL, m_out_write)
    L["loop"].here("loop")
    c.op(ALOAD_0); c.ref(GETFIELD, f_closed)
    c.if_(IFNE, "cleanup")                              # while (!closed)
    c.op(ALOAD_0); c.ref(INVOKEVIRTUAL, m_sync)         # if (!sync())
    c.if_(IFEQ, "cleanup")
    c.op(ALOAD_0); c.op(SIPUSH); c.buf += u2(4)         # readN(4, 5)
    c.op(SIPUSH); c.buf += u2(5)
    c.ref(INVOKEVIRTUAL, m_readn)
    c.if_(IFLT, "cleanup")                              # hdr < 0 -> break
    # w = (buf[4]&255) | ((buf[5]&255)<<8)
    c.op(ALOAD_0); c.ref(GETFIELD, f_buf); c.op(ICONST_4); c.op(BALOAD)
    c.op(SIPUSH); c.buf += u2(255); c.op(IAND); c.op(ISTORE_1)
    c.op(ALOAD_0); c.ref(GETFIELD, f_buf); c.op(ICONST_5); c.op(BALOAD)
    c.op(SIPUSH); c.buf += u2(255); c.op(IAND)
    c.op(BIPUSH); c.buf.append(8); c.op(ISHL)
    c.op(ILOAD_1); c.op(IOR); c.op(ISTORE_1)
    # h = (buf[6]&255) | ((buf[7]&255)<<8)
    c.op(ALOAD_0); c.ref(GETFIELD, f_buf)
    c.op(BIPUSH); c.buf.append(6); c.op(BALOAD)
    c.op(SIPUSH); c.buf += u2(255); c.op(IAND); c.op(ISTORE_2)
    c.op(ALOAD_0); c.ref(GETFIELD, f_buf); c.op(BIPUSH); c.buf.append(7)
    c.op(BALOAD)
    c.op(SIPUSH); c.buf += u2(255); c.op(IAND)
    c.op(BIPUSH); c.buf.append(8); c.op(ISHL)
    c.op(ILOAD_2); c.op(IOR); c.op(ISTORE_2)
    # if (w != 128 || h != 160) continue
    c.op(ILOAD_1); c.op(SIPUSH); c.buf += u2(128)
    c.if_(IF_ICMPNE, "cont")
    c.op(ILOAD_2); c.op(SIPUSH); c.buf += u2(160)
    c.if_(IF_ICMPNE, "cont")
    # if (readN(9, 40960) < 0) break   (40960 > 32767 -> ldc int)
    c.op(ALOAD_0); c.op(BIPUSH); c.buf.append(9)
    c.ldc_w(cp.cint(40960))
    c.ref(INVOKEVIRTUAL, m_readn)
    c.if_(IFLT, "cleanup")
    # dg.drawPixels(buf, null, 9, 128, 0, 0, 128, 160, 0, 565)
    c.op(ALOAD_0); c.ref(GETFIELD, f_dg)
    c.op(ALOAD_0); c.ref(GETFIELD, f_buf)
    c.op(ACONST_NULL)
    c.op(BIPUSH); c.buf.append(9)
    c.op(SIPUSH); c.buf += u2(128)
    c.op(ICONST_0); c.op(ICONST_0)
    c.op(SIPUSH); c.buf += u2(128); c.op(SIPUSH); c.buf += u2(160)
    c.op(ICONST_0)
    c.op(SIPUSH); c.buf += u2(565)
    c.ref(INVOKEVIRTUAL, m_drawpixels)
    # state = 1; frames++
    c.op(ALOAD_0); c.op(ICONST_1); c.ref(PUTFIELD, f_state)
    c.ref(GETSTATIC, sf_frames); c.op(ICONST_1); c.op(IADD)
    c.ref(PUTSTATIC, sf_frames)
    # repaint(); serviceRepaints()
    c.op(ALOAD_0); c.ref(INVOKEVIRTUAL, m_repaint)
    c.op(ALOAD_0); c.ref(INVOKEVIRTUAL, m_service)
    # out.write('K' 75); acks++
    c.op(ALOAD_0); c.ref(GETFIELD, f_out)
    c.op(BIPUSH); c.buf.append(75)
    c.ref(INVOKEVIRTUAL, m_out_write)
    c.ref(GETSTATIC, sf_acks); c.op(ICONST_1); c.op(IADD)
    c.ref(PUTSTATIC, sf_acks)
    L["cont"].here("cont")
    c.goto_("loop")
    L["cleanup"].here("cleanup")
    c.op(ALOAD_0); c.ref(GETFIELD, f_out)               # out.write('Q')
    c.op(BIPUSH); c.buf.append(81)
    c.ref(INVOKEVIRTUAL, m_out_write)
    c.op(ALOAD_0); c.ref(GETFIELD, f_in)
    c.if_(IFNULL, "skipin")
    c.op(ALOAD_0); c.ref(GETFIELD, f_in)
    c.ref(INVOKEVIRTUAL, m_in_close)
    c.label("skipin")
    c.op(ALOAD_0); c.ref(GETFIELD, f_out)
    c.if_(IFNULL, "skipout")
    c.op(ALOAD_0); c.ref(GETFIELD, f_out)
    c.ref(INVOKEVIRTUAL, m_out_close)
    c.label("skipout")
    c.op(ALOAD_0); c.ref(GETFIELD, f_conn)
    c.if_(IFNULL, "skipconn")
    c.op(ALOAD_0); c.ref(GETFIELD, f_conn)
    c.ref(INVOKEVIRTUAL, m_sc_close)
    c.label("skipconn")
    L["try_end"].here("try_end")
    c.goto_("end")
    L["catch"].here("catch")
    c.op(POP)                                           # swallow it
    L["end"].here("end")
    c.ref(GETSTATIC, sf_frames)
    c.op(IRETURN)
    code = c.finish()
    run_m = mkmethod(cp, 0x0001, "run", "()I", code, 12, 3,
                     exc_entries=[(L["try"], L["try_end"], L["catch"],
                                   "java/lang/Exception")])

    # ---- fields ---------------------------------------------------
    fields = []
    for name, sig in (("buf", "[B"),
                      ("img", "Ljavax/microedition/lcdui/Image;"),
                      ("g", "Ljavax/microedition/lcdui/Graphics;"),
                      ("dg", "Lcom/nokia/mid/ui/DirectGraphics;"),
                      ("conn",
                       "Ljavax/microedition/io/StreamConnection;"),
                      ("in", "Ljava/io/InputStream;"),
                      ("out", "Ljava/io/OutputStream;"),
                      ("closed", "Z"),
                      ("state", "I")):
        fields.append(u2(0x0002) + u2(cp.utf(name)) + u2(cp.utf(sig)) + u2(0))
    for name in ("frames", "painted", "acks"):
        fields.append(u2(0x000A) + u2(cp.utf(name)) + u2(cp.utf("I")) + u2(0))

    return classfile(cp, 0x0021, "CastScr", n_canvas, fields,
                     [init_m, paint_m, key_m, sync_m, readn_m, run_m])

def main(path):
    with zipfile.ZipFile(path, 'w') as z:
        z.writestr('UsbCast.class', build_usbcast(),
                   compress_type=zipfile.ZIP_DEFLATED)
        z.writestr('CastScr.class', build_castscr(),
                   compress_type=zipfile.ZIP_DEFLATED)
        z.writestr('META-INF/MANIFEST.MF',
                   'MIDlet-1: UsbCast, , UsbCast\r\nMIDlet-Name: UsbCast\r\n'
                   'MIDlet-Version: 1.0\r\nMIDlet-Description: '
                   'fpj2me USB monitor\r\n',
                   compress_type=zipfile.ZIP_STORED)
    print("wrote", path)

if __name__ == '__main__':
    here = os.path.dirname(os.path.abspath(__file__))
    main(sys.argv[1] if len(sys.argv) > 1
         else os.path.join(here, '..', 'tests', 'UsbCast.jar'))
