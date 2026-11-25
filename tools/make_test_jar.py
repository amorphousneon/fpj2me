#!/usr/bin/env python3
"""
fpj2me — tools/make_test_jar.py : regenerates tests/test.jar.

Contents (what tests/hostcheck.c expects):
  a/b/Hello.class    STORED   8 bytes "CAFEBABE" (central-dir parse check)
  res/sheet.png      STORED   4097 bytes of PNG-ish noise
  d/deflate.bin      DEFLATE  "deflated-content-" x2 (REAL tinfl path)
  Tst.class          DEFLATE  hand-assembled classfile exercising the VM:
      static int x;
      static Tst inst;
      static int run()      { x = 2 + 3; return x; }          -> 5
      static int run2()     { return Math.abs(-5); }          -> 5 (native!)
      static int run3()     { return "a".equalsIgnoreCase("A") ? 1 : 0; }
                             -> 0: missing native -> no-op, NO panic
      static void startApp(){                                 (MIDlet boot)
          inst = this;
          Display d = Display.getDisplay(this);
          DemoScr scr = new DemoScr();
          d.setCurrent(scr);          (visible + showNotify)
          scr.run();                  GAMELOFT-STYLE BLOCKING LOOP
      }                             (must return: paint gets delivered)
  DemoScr.class      DEFLATE  hand-assembled Canvas subclass (the visible
                             M1 smoke test — also what runs on the phone):
      int color = 0x00CC00;
      static int painted;             (paint() count — the deadlock probe)
      static int spins, spins2;       (loop iterations before exit)
      void paint(Graphics g) { black clear; color box; text; border;
                                painted++; }
      void keyPressed(int k) {
          if (k == -7) { Tst.inst.notifyDestroyed(); return; }  EXIT
          color = 0xCC0000; repaint();
      }
      int run() {                   the blocking frame loop, two phases:
          while (spins < 80 && painted < 3)      // phase 1
              { repaint(); serviceRepaints(); Thread.sleep(1); }
          while (spins2 < 80 && painted < 6)     // phase 2: sleep only
              { repaint(); Thread.sleep(1); }
          return painted;
      }   fpj2me_boot() only returns when run() returns — if the VM
          starves paint inside startApp (the AC1 freeze), the loops
          burn all 80 spins with painted == 0 and hostcheck FAILS.
  META-INF/MANIFEST.MF STORED  MIDlet-1: Tst, , Tst

On-target expectation: green box + "fpj2me M1" text (drawn during
startApp via serviceRepaints); any key -> red box;
RSOFT (-7) -> notifyDestroyed -> launcher menu returns (no battery pull).
"""
import struct, sys, zlib, os

def u1(n): return struct.pack('>B', n)
def u2(n): return struct.pack('>H', n)
def u4(n): return struct.pack('>I', n)

def utf8(s):
    b = s.encode()
    return u1(1) + u2(len(b)) + b

def cls(idx):      return u1(7) + u2(idx)
def nat(n, d):     return u1(12) + u2(n) + u2(d)
def mref(c, nt):   return u1(10) + u2(c) + u2(nt)
def fref(c, nt):   return u1(9)  + u2(c) + u2(nt)
def cint(n):       return u1(3) + u4(n)
def cstr(idx):     return u1(8) + u2(idx)

def code_attr(name_idx, max_stack, max_locals, code):
    body = u2(max_stack) + u2(max_locals) + u4(len(code)) + code \
         + u2(0) + u2(0)                    # no exc table, no attrs
    return u2(name_idx) + u4(len(body)) + body

def method(access, name, sig, code):
    return u2(access) + u2(name) + u2(sig) + u2(1) + code

def build_tst_class():
    cp = [None]                       # 1-based
    cp.append(utf8("Tst"))            # 1
    cp.append(cls(1))                 # 2
    cp.append(utf8("javax/microedition/midlet/MIDlet"))  # 3 (SUPER: so
                              # invokevirtual dispatches notifyDestroyed
                              # through the native-only stub chain)
    cp.append(cls(3))                 # 4
    cp.append(utf8("x"))              # 5
    cp.append(utf8("I"))              # 6
    cp.append(utf8("run"))            # 7
    cp.append(utf8("()I"))            # 8
    cp.append(utf8("run2"))           # 9
    cp.append(utf8("startApp"))       # 10
    cp.append(utf8("()V"))            # 11
    cp.append(utf8("Code"))           # 12
    cp.append(utf8("java/lang/Math")) # 13
    cp.append(cls(13))                # 14
    cp.append(utf8("abs"))            # 15
    cp.append(utf8("(I)I"))           # 16
    cp.append(nat(15, 16))            # 17
    cp.append(mref(14, 17))           # 18  Math.abs(I)I
    cp.append(nat(5, 6))              # 19  x I
    cp.append(fref(2, 19))            # 20  Tst.x
    cp.append(utf8("javax/microedition/lcdui/Display"))      # 21
    cp.append(cls(21))                                         # 22
    cp.append(utf8("getDisplay"))                             # 23
    cp.append(utf8("(Ljavax/microedition/midlet/MIDlet;)"
                   "Ljavax/microedition/lcdui/Display;"))    # 24
    cp.append(nat(23, 24))                                     # 25
    cp.append(mref(22, 25))            # 26 Display.getDisplay
    cp.append(utf8("DemoScr"))         # 27
    cp.append(cls(27))                 # 28
    cp.append(utf8("<init>"))          # 29
    cp.append(utf8("()V"))            # 30
    cp.append(nat(29, 30))             # 31
    cp.append(mref(28, 31))            # 32 DemoScr.<init>()V
    cp.append(utf8("setCurrent"))      # 33
    cp.append(utf8("(Ljavax/microedition/lcdui/Displayable;)V")) # 34
    cp.append(nat(33, 34))             # 35
    cp.append(mref(22, 35))            # 36 Display.setCurrent
    cp.append(utf8("inst"))            # 37
    cp.append(utf8("LTst;"))           # 38
    cp.append(nat(37, 38))             # 39
    cp.append(fref(2, 39))             # 40 Tst.inst
    cp.append(utf8("run"))             # 41  DemoScr.run()I (blocking loop)
    cp.append(utf8("()I"))             # 42
    cp.append(nat(41, 42))             # 43
    cp.append(mref(28, 43))            # 44 DemoScr.run()I
    cp.append(utf8("java/lang/String"))    # 45 (missing-native probe)
    cp.append(cls(45))                     # 46
    cp.append(utf8("equalsIgnoreCase"))    # 47
    cp.append(utf8("(Ljava/lang/String;)Z")) # 48
    cp.append(nat(47, 48))                 # 49
    cp.append(mref(46, 49))                # 50 String.equalsIgnore..()Z
    cp.append(utf8("a"))               # 51
    cp.append(cstr(51))                # 52
    cp.append(utf8("A"))               # 53
    cp.append(cstr(53))                # 54
    cp.append(utf8("run3"))            # 55
    cp.append(utf8("y"))               # 56 static int y (clinit test)
    cp.append(nat(56, 6))              # 57  y : I
    cp.append(fref(2, 57))             # 58  Tst.y
    cp.append(utf8("run4"))            # 59  getstatic y (clinit probe)
    cp.append(utf8("run5"))            # 60  lcmp operand-order probe
    cp.append(utf8("<clinit>"))        # 61

    out = u4(0xCAFEBABE) + u2(0) + u2(46)   # magic, minor, major
    out += u2(len(cp)) + b''.join(cp[1:])
    out += u2(0x0021)                        # public super
    out += u2(2) + u2(4)                     # this, super
    out += u2(0)                             # interfaces
    out += u2(3)                             # 3 fields
    out += u2(0x0009) + u2(5) + u2(6) + u2(0)    # public static int x
    out += u2(0x0009) + u2(37) + u2(38) + u2(0)  # public static Tst inst
    out += u2(0x0009) + u2(56) + u2(6) + u2(0)   # public static int y
    out += u2(7)                             # 7 methods (run..run5, startApp, <clinit>)
    # run()I: iconst_2 iconst_3 iadd putstatic#20 getstatic#20 ireturn
    out += method(0x0009, 7, 8,
        code_attr(12, 2, 0,
            u1(0x05) + u1(0x06) + u1(0x60)
            + u1(0xB3) + u2(20) + u1(0xB2) + u2(20) + u1(0xAC)))
    # run2()I: bipush -5; invokestatic#18; ireturn
    out += method(0x0009, 9, 8,
        code_attr(12, 1, 0,
            u1(0x10) + u1(0xFB) + u1(0xB8) + u2(18) + u1(0xAC)))
    # run3()I: return "a".equalsIgnoreCase("A") ? 1 : 0;
    # equalsIgnoreCase is deliberately NOT in the native registry: the
    # no-op fallback must return false (0) WITHOUT panicking.
    #   0: ldc#52; 2: ldc#54; 4: invokevirtual#50
    #   7: ifeq +5 -> 12; 10: iconst_1; 11: ireturn; 12: iconst_0; 13: ireturn
    out += method(0x0009, 55, 8,
        code_attr(12, 2, 0,
            u1(0x12) + u1(52) + u1(0x12) + u1(54)
            + u1(0xB6) + u2(50)
            + u1(0x9A) + u2(5)
            + u1(0x03) + u1(0xAC)
            + u1(0x03) + u1(0xAC)))
    # run4()I: return y;   y is set ONLY by <clinit> -> 41
    out += method(0x0009, 59, 8,
        code_attr(12, 1, 0,
            u1(0xB2) + u2(58) + u1(0xAC)))
    # run5()I: return (100L > 50L) ? 1 : 0;   lcmp OPERAND ORDER: value1
    # is pushed first — an inverted lcmp answers 0 and fails this test
    # (AC1's 100 ms frame limiter waited when it should not and vice
    # versa, repaint-spamming the splash loop forever).
    out += method(0x0009, 60, 8,
        code_attr(12, 4, 0,
            u1(0x10) + u1(100)     # bipush 100
            + u1(0x85)             # i2l          -> value1
            + u1(0x10) + u1(50)    # bipush 50
            + u1(0x85)             # i2l          -> value2
            + u1(0x94)             # lcmp
            + u1(0x9D) + u2(5)     # ifgt -> 12
            + u1(0x03) + u1(0xAC)  # 10: iconst_0; ireturn
            + u1(0x04) + u1(0xAC)))# 12: iconst_1; ireturn
    # <clinit>()V: y = 41;   (runs at first active use — new/getstatic/
    # putstatic/invokestatic — and at the boot's implicit "new")
    out += method(0x0008, 61, 11,
        code_attr(12, 1, 0,
            u1(0x10) + u1(41)      # bipush 41
            + u1(0xB3) + u2(58)    # putstatic Tst.y
            + u1(0xB1)))           # return
    # startApp()V:  (locals: 0=this, 1=d, 2=scr)
    #   inst = this;
    #   Display d = Display.getDisplay(this);
    #   DemoScr scr = new DemoScr();
    #   d.setCurrent(scr);          // -> repaint_req + showNotify
    #   scr.run();                  // Gameloft-style BLOCKING frame loop:
    #                                 paint MUST be delivered inside it
    # receiver for invokevirtual must sit BELOW its args: keep d and scr
    # in locals and load them in call order (the naive dup trick put the
    # RECEIVER at the top and dispatched setCurrent on the wrong class).
    out += method(0x0009, 10, 11,
        code_attr(12, 3, 3,
            u1(0x2A) + u1(0xB3) + u2(40)    # aload_0; putstatic Tst.inst
            + u1(0x2A)                       # aload_0
            + u1(0xB8) + u2(26)              # invokestatic Display.getDisplay
            + u1(0x4C)                       # astore_1 (d)
            + u1(0xBB) + u2(28)              # new DemoScr
            + u1(0x59)                       # dup
            + u1(0xB7) + u2(32)              # invokespecial DemoScr.<init>
            + u1(0x4D)                       # astore_2 (scr)
            + u1(0x2B) + u1(0x2C)            # aload_1; aload_2 -> [d, scr]
            + u1(0xB6) + u2(36)              # invokevirtual Display.setCurrent
            + u1(0x2C)                       # aload_2 -> [scr]
            + u1(0xB6) + u2(44)              # invokevirtual DemoScr.run()I
            + u1(0x57)                       # pop (discard the count)
            + u1(0xB1)))                     # return
    out += u2(0)                             # class attrs
    return out

def build_scr_class():
    """DemoScr extends Canvas (native-only superclass):
        int color = 0x00CC00;
        static int painted, spins, spins2;   (deadlock probes for hostcheck)
        void paint(Graphics g) { clear black; box(color); 2 strings; border;
                                  painted++; }
        void keyPressed(int k) { color = 0xCC0000; repaint(); }
        int run() { ... two-phase BLOCKING loop, see below ... }
    """
    cp = [None]                       # 1-based
    cp.append(utf8("DemoScr"))        # 1
    cp.append(cls(1))                 # 2
    cp.append(utf8("javax/microedition/lcdui/Canvas"))  # 3
    cp.append(cls(3))                 # 4
    cp.append(utf8("color"))          # 5
    cp.append(utf8("I"))              # 6
    cp.append(utf8("<init>"))         # 7
    cp.append(utf8("()V"))            # 8
    cp.append(utf8("paint"))          # 9
    cp.append(utf8("(Ljavax/microedition/lcdui/Graphics;)V"))  # 10
    cp.append(utf8("keyPressed"))     # 11
    cp.append(utf8("(I)V"))           # 12
    cp.append(utf8("Code"))           # 13
    cp.append(utf8("javax/microedition/lcdui/Graphics"))  # 14
    cp.append(cls(14))                # 15
    cp.append(utf8("setColor"))       # 16
    cp.append(utf8("(I)V"))           # 17
    cp.append(nat(16, 17))            # 18
    cp.append(mref(15, 18))           # 19 Graphics.setColor(I)V
    cp.append(utf8("fillRect"))       # 20
    cp.append(utf8("(IIII)V"))        # 21
    cp.append(nat(20, 21))            # 22
    cp.append(mref(15, 22))           # 23 Graphics.fillRect(IIII)V
    cp.append(utf8("drawRect"))       # 24
    cp.append(nat(24, 21))            # 25
    cp.append(mref(15, 25))           # 26 Graphics.drawRect(IIII)V
    cp.append(utf8("drawString"))     # 27
    cp.append(utf8("(Ljava/lang/String;III)V"))  # 28
    cp.append(nat(27, 28))            # 29
    cp.append(mref(15, 29))           # 30 Graphics.drawString
    cp.append(utf8("javax/microedition/lcdui/Canvas"))  # 31
    cp.append(cls(31))                # 32
    cp.append(utf8("repaint"))        # 33
    cp.append(utf8("()V"))            # 34
    cp.append(nat(33, 34))            # 35
    cp.append(mref(32, 35))           # 36 Canvas.repaint()V
    cp.append(nat(5, 6))              # 37 color I
    cp.append(fref(2, 37))            # 38 DemoScr.color
    cp.append(utf8("fpj2me M1"))      # 39
    cp.append(cstr(39))               # 40
    cp.append(utf8("press a key"))    # 41
    cp.append(cstr(41))               # 42
    cp.append(cint(0x00CC00))         # 43 green
    cp.append(cint(0xFFFFFF))         # 44 white
    cp.append(cint(0xAAAAAA))         # 45 gray
    cp.append(cint(0xCC0000))         # 46 red
    cp.append(utf8("javax/microedition/midlet/MIDlet"))      # 47
    cp.append(cls(47))                                         # 48
    cp.append(utf8("notifyDestroyed"))                       # 49
    cp.append(utf8("()V"))                                    # 50
    cp.append(nat(49, 50))                                     # 51
    cp.append(mref(48, 51))            # 52 MIDlet.notifyDestroyed()V
    cp.append(utf8("Tst"))             # 53
    cp.append(cls(53))                                         # 54
    cp.append(utf8("inst"))            # 55
    cp.append(utf8("LTst;"))           # 56
    cp.append(nat(55, 56))                                     # 57
    cp.append(fref(54, 57))            # 58 Tst.inst
    # --- blocking-loop additions (indices 59..80) ---
    cp.append(utf8("run"))             # 59  DemoScr.run()I
    cp.append(utf8("()I"))             # 60
    cp.append(utf8("serviceRepaints")) # 61
    cp.append(utf8("()V"))             # 62
    cp.append(nat(61, 62))             # 63
    cp.append(mref(32, 63))            # 64 Canvas.serviceRepaints()V
    cp.append(utf8("java/lang/Thread"))# 65
    cp.append(cls(65))                 # 66
    cp.append(utf8("sleep"))           # 67
    cp.append(utf8("(J)V"))            # 68
    cp.append(nat(67, 68))             # 69
    cp.append(mref(66, 69))            # 70 Thread.sleep(J)V
    cp.append(utf8("painted"))         # 71
    cp.append(utf8("I"))               # 72 (reuse of "I" text, new index)
    cp.append(nat(71, 72))             # 73
    cp.append(fref(2, 73))             # 74 DemoScr.painted
    cp.append(utf8("spins"))           # 75
    cp.append(nat(75, 72))             # 76
    cp.append(fref(2, 76))             # 77 DemoScr.spins
    cp.append(utf8("spins2"))          # 78
    cp.append(nat(78, 72))             # 79
    cp.append(fref(2, 79))             # 80 DemoScr.spins2
    cp.append(utf8("cl"))              # 81 static int cl (clinit probe)
    cp.append(nat(81, 72))             # 82  cl : I
    cp.append(fref(2, 82))             # 83 DemoScr.cl
    cp.append(utf8("<clinit>"))        # 84
    # extra statics piggyback on "I" descriptors: painted/spins/spins2

    out = u4(0xCAFEBABE) + u2(0) + u2(46)
    out += u2(len(cp)) + b''.join(cp[1:])
    out += u2(0x0020)                 # super (package)
    out += u2(2) + u2(4)             # this, super(Canvas stub)
    out += u2(0)                      # interfaces
    out += u2(5)                      # fields: color + 3 static probes + cl
    out += u2(0x0002) + u2(5) + u2(6) + u2(0)   # private int color
    out += u2(0x0009) + u2(71) + u2(72) + u2(0) # public static int painted
    out += u2(0x0009) + u2(75) + u2(72) + u2(0) # public static int spins
    out += u2(0x0009) + u2(78) + u2(72) + u2(0) # public static int spins2
    out += u2(0x0009) + u2(81) + u2(72) + u2(0) # public static int cl
    out += u2(5)                      # methods (+ <clinit>)

    # <clinit>()V: cl = 9;  — armed ONLY when the "new DemoScr" opcode
    # inside startApp() triggers initialization (the JVM's first-active-
    # use rule the engine used to skip entirely).
    out += method(0x0008, 84, 8,
        code_attr(13, 1, 0,
            u1(0x10) + u1(9)       # bipush 9
            + u1(0xB3) + u2(83)    # putstatic DemoScr.cl
            + u1(0xB1)))           # return

    # <init>()V: this.color = 0x00CC00;
    out += method(0x0000, 7, 8,
        code_attr(13, 2, 1,
            u1(0x2A)                    # aload_0
            + u1(0x12) + u1(43)         # ldc green
            + u1(0xB5) + u2(38)         # putfield color
            + u1(0xB1)))                # return

    # paint(Graphics g): slots 0=this, 1=g   (aload_1 = 0x2B)
    code  = u1(0xB2) + u2(74) + u1(0x04) + u1(0x60) + u1(0xB3) + u2(74)
    #                                 ^ painted++ FIRST: even a broken
    #                                   draw below still counts the frame
    code += u1(0x2B) + u1(0x03)
    code += u1(0xB6) + u2(19)                       # g.setColor(0)
    code += u1(0x2B) + u1(0x03) + u1(0x03)
    code += u1(0x11) + u2(128) + u1(0x11) + u2(160)
    code += u1(0xB6) + u2(23)                       # g.fillRect(0,0,128,160)
    code += u1(0x2B) + u1(0x2A) + u1(0xB4) + u2(38)
    code += u1(0xB6) + u2(19)                       # g.setColor(color)
    code += u1(0x2B) + u1(0x10) + u1(16) + u1(0x10) + u1(16)
    code += u1(0x10) + u1(96) + u1(0x10) + u1(40)
    code += u1(0xB6) + u2(23)                       # g.fillRect(16,16,96,40)
    code += u1(0x2B) + u1(0x12) + u1(44) + u1(0xB6) + u2(19)  # white
    code += u1(0x2B) + u1(0x12) + u1(40) + u1(0x10) + u1(8)
    code += u1(0x10) + u1(60) + u1(0x10) + u1(20)
    code += u1(0xB6) + u2(30)                       # drawString("fpj2me M1",8,60,20)
    code += u1(0x2B) + u1(0x12) + u1(45) + u1(0xB6) + u2(19)  # gray
    code += u1(0x2B) + u1(0x12) + u1(42) + u1(0x10) + u1(8)
    code += u1(0x10) + u1(72) + u1(0x10) + u1(20)
    code += u1(0xB6) + u2(30)                       # drawString("press a key",8,72,20)
    code += u1(0x2B) + u1(0x12) + u1(44) + u1(0xB6) + u2(19)  # white
    code += u1(0x2B) + u1(0x03) + u1(0x03) + u1(0x10) + u1(127)
    code += u1(0x11) + u2(159) + u1(0xB6) + u2(26)  # drawRect(0,0,127,159)
    code += u1(0xB1)                                # return
    out += method(0x0001, 9, 10, code_attr(13, 5, 2, code))

    # keyPressed(int k): slots 0=this, 1=k
    #   if (k == -7) { Tst.inst.notifyDestroyed(); return; }  # RSOFT = exit
    code  = u1(0x10) + u1(0xF9) + u1(0x1B)    # bipush -7; iload_1
    code += u1(0xA0) + u2(10)                 # if_icmpne L (L at +10)
    code += u1(0xB2) + u2(58)                 # getstatic Tst.inst
    code += u1(0xB6) + u2(52)                 # invokevirtual MIDlet.notifyDestroyed
    code += u1(0xB1)                          # return
    # L: color = 0xCC0000; repaint();
    code += u1(0x2A) + u1(0x12) + u1(46) + u1(0xB5) + u2(38)
    code += u1(0x2A) + u1(0xB6) + u2(36)            # repaint()
    code += u1(0xB1)
    out += method(0x0001, 11, 12, code_attr(13, 2, 2, code))

    # run()I: the Gameloft-style blocking frame loop (locals: 0=this, 1=i).
    #   phase 1: while (i < 80 && painted < 3)
    #                { repaint(); serviceRepaints(); Thread.sleep(1L); }
    #   spins = i;  phase 2: while (i < 80 && painted < 6)
    #                { repaint(); Thread.sleep(1L); }   (no sync paint)
    #   spins2 = i; return painted;
    # serviceRepaints must DELIVER paint (phase 1), the pumping sleep
    # must deliver repaints (phase 2) — the VM starved both before.
    # CP: repaint#36(0x24) serviceRepaints#64(0x40) sleep#70(0x46)
    #     painted#74(0x4A) spins#77(0x4D) spins2#80(0x50)
    code  = u1(0x03) + u1(0x3C)               #  0: iconst_0; istore_1
    # L1 (2):                                                       PHASE 1
    code += u1(0x1B)                          #  2: iload_1
    code += u1(0x11) + u2(80)                 #  3: sipush 80
    code += u1(0xA2) + u2(28)                 #  6: if_icmpge L2(34)
    code += u1(0xB2) + u2(74)                 #  9: getstatic painted
    code += u1(0x04)                          # 12: iconst_1
    code += u1(0xA2) + u2(21)                 # 13: if_icmpge L2(34)
    code += u1(0x2A)                          # 16: aload_0
    code += u1(0xB6) + u2(36)                 # 17: invokevirtual repaint
    code += u1(0x2A)                          # 20: aload_0
    code += u1(0xB6) + u2(64)                 # 21: invokevirtual serviceRepaints
    code += u1(0x0A)                          # 24: lconst_1
    code += u1(0xB8) + u2(70)                 # 25: invokestatic Thread.sleep
    code += u1(0x84) + u1(1) + u1(1)          # 28: iinc 1, 1
    code += u1(0xA7) + u2(0xFFE3)             # 31: goto L1(2)
    # L2 (34): spins = i; i = 0;
    code += u1(0x1B)                          # 34: iload_1
    code += u1(0xB3) + u2(77)                 # 35: putstatic spins
    code += u1(0x03)                          # 38: iconst_0
    code += u1(0x3C)                          # 39: istore_1
    # L3 (40):                                                       PHASE 2
    code += u1(0x1B)                          # 40: iload_1
    code += u1(0x11) + u2(80)                 # 41: sipush 80
    code += u1(0xA2) + u2(26)                 # 44: if_icmpge L4(70)
    code += u1(0xB2) + u2(74)                 # 47: getstatic painted
    code += u1(0x11) + u2(6)                  # 50: sipush 6
    code += u1(0xA2) + u2(17)                 # 53: if_icmpge L4(70)
    code += u1(0x2A)                          # 56: aload_0
    code += u1(0xB6) + u2(36)                 # 57: invokevirtual repaint
    code += u1(0x0A)                          # 60: lconst_1
    code += u1(0xB8) + u2(70)                 # 61: invokestatic Thread.sleep
    code += u1(0x84) + u1(1) + u1(1)          # 64: iinc 1, 1
    code += u1(0xA7) + u2(0xFFE5)             # 67: goto L3(40)
    # L4 (70): spins2 = i; return painted;
    code += u1(0x1B)                          # 70: iload_1
    code += u1(0xB3) + u2(80)                 # 71: putstatic spins2
    code += u1(0xB2) + u2(74)                 # 74: getstatic painted
    code += u1(0xAC)                          # 77: ireturn
    out += method(0x0001, 59, 60, code_attr(13, 2, 2, code))

    out += u2(0)                      # class attrs
    return out

def main(path):
    import zipfile
    with zipfile.ZipFile(path, 'w') as z:
        z.writestr('a/b/Hello.class', b'CAFEBABE',
                   compress_type=zipfile.ZIP_STORED)
        z.writestr('res/sheet.png',
                   b'\x89PNG\r\n\x1a\n' + b'\x00' * 4088 + b'\xff',
                   compress_type=zipfile.ZIP_STORED)
        z.writestr('d/deflate.bin', b'deflated-content-' + b'deflated-content',
                   compress_type=zipfile.ZIP_DEFLATED)
        z.writestr('Tst.class', build_tst_class(),
                   compress_type=zipfile.ZIP_DEFLATED)
        z.writestr('DemoScr.class', build_scr_class(),
                   compress_type=zipfile.ZIP_DEFLATED)
        z.writestr('META-INF/MANIFEST.MF',
                   'MIDlet-1: Tst, , Tst\r\nMIDlet-Name: Tst\r\n'
                   'MIDlet-Version: 1.0\r\n',
                   compress_type=zipfile.ZIP_STORED)
    print("wrote", path)

if __name__ == '__main__':
    here = os.path.dirname(os.path.abspath(__file__))
    main(sys.argv[1] if len(sys.argv) > 1
         else os.path.join(here, '..', 'tests', 'test.jar'))
