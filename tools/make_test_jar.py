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
      static int run6()     { boxing: Integer.valueOf (int, String) and
                              new Integer round-trips incl. negatives }
                              -> -1085   (Round 8, AC2 sound-prompt freeze)
      static int run7()     { Boolean.valueOf(true) + new Boolean(false) }
                              -> 2
      static int run8()     { Long.valueOf(-1234567890123L).longValue()
                              == -1234567890123L } -> 1
      static int run9()     { Byte.valueOf(-100) + valueOf(90) } -> -10
      static int run10()    { Short.valueOf(-20000) + 12345 } -> -7655
      static int run11()    { Character.valueOf(65) + 0x4E2D } -> 20078
      static String run12() { Integer.valueOf(-456).toString(); } -> "-456"
      static String run13() { Long.valueOf(-9876543210L).toString(); }
      static String run14() { Boolean.valueOf(false).toString(); } -> "false"
      static String run15() { Character.valueOf(65).toString(); } -> "A"
      static String run16() { Byte.valueOf(-3).toString(); } -> "-3"
      static String run17() { Short.valueOf(12345).toString(); } -> "12345"
      static int run18()    { Integer(-9).equals(new Integer(-9)) and
                              !Integer(5).equals(Short(5)) } -> 2
      static int run19()    { Boolean.valueOf(true) == valueOf(true) }
                              -> 1  (TRUE/FALSE singleton identity)
      static int run20()   { Integer.valueOf("2A", 16).intValue(); } -> 42
                              (AC2's sound-prompt overload: radix)
      static String run21(){ Integer.toString(255, 16); } -> "ff"
      static int run22()   { Byte.parseByte("-100"); } -> -100
      static int run23()   { b={'f','p','j','2','m'}; new String(b,0,5)
                              .length() + (charAt(0)=='f'?100:0) +
                              new String(b,1,2).length()*2; } -> 109
                              (handle rebind: GLLib text ctor path)
      static int run24()   { new StringBuffer().append(true)
                              .append(false).append(Integer.valueOf(7))
                              .length(); } -> 10  ("truefalse7")
      static int run25()   { Math.max/min over (I)I and the (J)J
                              overloads WWG's boot tripped on } -> 1
      static int run26()   { sqrt/max(D)/max(F)/sin/cos/ceil through
                              the interpreter's d2i/f2i/dcmpl } -> 454
      static int run27()   { new IndexOutOfBounds-family exceptions +
                              getMessage() through the stub chain } -> 27
      static int run28()   { try { a[5] } catch (IndexOutOfBounds) } -> 7
                              (first method with a nonzero exc table)
      static int run33()   { comm:usb GCF roundtrip: bad URL throws
                              ConnectionNotFoundException (caught);
                              open("comm:usb") -> StreamConnection ->
                              InputStream/OutputStream; read 10 fed bytes,
                              write 'K' ACK, read 'Z' -> 110 (Round 10)
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

    # ---- boxing round-trip additions (Round 8: wrapper valueOf) -------
    # indices are computed, not hand-numbered: 14 new methods share one
    # block of constant-pool entries appended after index 61. CONSTANT_
    # Long/Double occupy TWO cp slots (JVMS 4.4.5) — the emitter's slot
    # counter and the pool COUNT must both account for the phantom slot.
    state = {'next': 62}               # next free cp slot (1-based)
    def CPe(e):                         # append -> 1-based cp slot index
        cp.append(e)
        idx = state['next']
        state['next'] = idx + (2 if e[0:1] in (b'\x05', b'\x06') else 1)
        return idx

    n_Integer   = CPe(utf8("java/lang/Integer"));   c_Integer = CPe(cls(n_Integer))
    n_valueOf   = CPe(utf8("valueOf"))
    s_val_I     = CPe(utf8("(I)Ljava/lang/Integer;"))
    m_Int_valOf = CPe(mref(c_Integer, CPe(nat(n_valueOf, s_val_I))))
    n_intValue  = CPe(utf8("intValue"))
    s_ret_I     = CPe(utf8("()I"))
    m_Int_intVal= CPe(mref(c_Integer, CPe(nat(n_intValue, s_ret_I))))
    n_init      = CPe(utf8("<init>"))
    s_ctor_I    = CPe(utf8("(I)V"))
    m_Int_init  = CPe(mref(c_Integer, CPe(nat(n_init, s_ctor_I))))
    s_val_S     = CPe(utf8("(Ljava/lang/String;)Ljava/lang/Integer;"))
    m_Int_valOfS= CPe(mref(c_Integer, CPe(nat(n_valueOf, s_val_S))))
    n_str_m99   = CPe(utf8("-99"))
    c_str_m99   = CPe(cstr(n_str_m99))

    n_Boolean   = CPe(utf8("java/lang/Boolean"));   c_Boolean = CPe(cls(n_Boolean))
    s_val_Z     = CPe(utf8("(Z)Ljava/lang/Boolean;"))
    m_Bln_valOf = CPe(mref(c_Boolean, CPe(nat(n_valueOf, s_val_Z))))
    s_ctor_Z    = CPe(utf8("(Z)V"))
    m_Bln_init  = CPe(mref(c_Boolean, CPe(nat(n_init, s_ctor_Z))))
    n_blnValue  = CPe(utf8("booleanValue"))
    s_ret_Z     = CPe(utf8("()Z"))
    m_Bln_blnVal= CPe(mref(c_Boolean, CPe(nat(n_blnValue, s_ret_Z))))

    n_Long      = CPe(utf8("java/lang/Long"));      c_Long = CPe(cls(n_Long))
    s_val_J     = CPe(utf8("(J)Ljava/lang/Long;"))
    m_Lng_valOf = CPe(mref(c_Long, CPe(nat(n_valueOf, s_val_J))))
    n_lngValue  = CPe(utf8("longValue"))
    s_ret_J     = CPe(utf8("()J"))
    m_Lng_lngVal= CPe(mref(c_Long, CPe(nat(n_lngValue, s_ret_J))))
    def clong(v): return u1(5) + struct.pack('>q', v)   # 2 cp slots
    c_lng_A     = CPe(clong(-1234567890123))
    c_lng_B     = CPe(clong(-9876543210))
    s_ctor_J    = CPe(utf8("(J)V"))
    m_Lng_init  = CPe(mref(c_Long, CPe(nat(n_init, s_ctor_J))))

    n_Byte      = CPe(utf8("java/lang/Byte"));       c_Byte = CPe(cls(n_Byte))
    s_val_B     = CPe(utf8("(B)Ljava/lang/Byte;"))
    m_Byt_valOf = CPe(mref(c_Byte, CPe(nat(n_valueOf, s_val_B))))
    n_bytValue  = CPe(utf8("byteValue"))
    s_ret_B     = CPe(utf8("()B"))
    m_Byt_bytVal= CPe(mref(c_Byte, CPe(nat(n_bytValue, s_ret_B))))

    n_Short     = CPe(utf8("java/lang/Short"));      c_Short = CPe(cls(n_Short))
    s_val_S2    = CPe(utf8("(S)Ljava/lang/Short;"))
    m_Sht_valOf = CPe(mref(c_Short, CPe(nat(n_valueOf, s_val_S2))))
    n_shtValue  = CPe(utf8("shortValue"))
    s_ret_S     = CPe(utf8("()S"))
    m_Sht_shtVal= CPe(mref(c_Short, CPe(nat(n_shtValue, s_ret_S))))

    n_Char      = CPe(utf8("java/lang/Character"));  c_Char = CPe(cls(n_Char))
    s_val_C     = CPe(utf8("(C)Ljava/lang/Character;"))
    m_Chr_valOf = CPe(mref(c_Char, CPe(nat(n_valueOf, s_val_C))))
    n_chrValue  = CPe(utf8("charValue"))
    s_ret_C     = CPe(utf8("()C"))
    m_Chr_chrVal= CPe(mref(c_Char, CPe(nat(n_chrValue, s_ret_C))))

    n_toString  = CPe(utf8("toString"))
    s_ret_Str   = CPe(utf8("()Ljava/lang/String;"))
    m_Int_toStr = CPe(mref(c_Integer, CPe(nat(n_toString, s_ret_Str))))
    m_Lng_toStr = CPe(mref(c_Long,    CPe(nat(n_toString, s_ret_Str))))
    m_Bln_toStr = CPe(mref(c_Boolean, CPe(nat(n_toString, s_ret_Str))))
    m_Chr_toStr = CPe(mref(c_Char,    CPe(nat(n_toString, s_ret_Str))))
    m_Byt_toStr = CPe(mref(c_Byte,    CPe(nat(n_toString, s_ret_Str))))
    m_Sht_toStr = CPe(mref(c_Short,   CPe(nat(n_toString, s_ret_Str))))

    s_arg_Obj   = CPe(utf8("(Ljava/lang/Object;)Z"))
    m_Int_equals= CPe(mref(c_Integer, CPe(nat(CPe(utf8("equals")), s_arg_Obj))))

    # method-name pool
    m_run6  = CPe(utf8("run6"));  m_run7  = CPe(utf8("run7"))
    m_run8  = CPe(utf8("run8"));  m_run9  = CPe(utf8("run9"))
    m_run10 = CPe(utf8("run10")); m_run11 = CPe(utf8("run11"))
    m_run12 = CPe(utf8("run12")); m_run13 = CPe(utf8("run13"))
    m_run14 = CPe(utf8("run14")); m_run15 = CPe(utf8("run15"))
    m_run16 = CPe(utf8("run16")); m_run17 = CPe(utf8("run17"))
    m_run18 = CPe(utf8("run18")); m_run19 = CPe(utf8("run19"))
    m_run20 = CPe(utf8("run20")); m_run21 = CPe(utf8("run21"))
    m_run22 = CPe(utf8("run22")); m_run23 = CPe(utf8("run23"))
    m_run24 = CPe(utf8("run24"))

    # ---- AC2-driven additions (its real GLLib call sites) -------------
    n_StrCls  = CPe(utf8("java/lang/String"));  c_StrCls = CPe(cls(n_StrCls))
    s_Sinit_B = CPe(utf8("([BII)V"))
    m_Str_initB = CPe(mref(c_StrCls, CPe(nat(n_init, s_Sinit_B))))
    n_length  = CPe(utf8("length"))
    m_Str_len = CPe(mref(c_StrCls, CPe(nat(n_length, s_ret_I))))
    n_charAt  = CPe(utf8("charAt"))
    s_arg_C   = CPe(utf8("(I)C"))
    m_Str_chAt= CPe(mref(c_StrCls, CPe(nat(n_charAt, s_arg_C))))

    n_StrBfr  = CPe(utf8("java/lang/StringBuffer")); c_StrBfr = CPe(cls(n_StrBfr))
    s_V       = CPe(utf8("()V"))
    m_SB_init = CPe(mref(c_StrBfr, CPe(nat(n_init, s_V))))
    n_append  = CPe(utf8("append"))
    s_SB_appZ = CPe(utf8("(Z)Ljava/lang/StringBuffer;"))
    m_SB_appZ = CPe(mref(c_StrBfr, CPe(nat(n_append, s_SB_appZ))))
    s_SB_appO = CPe(utf8("(Ljava/lang/Object;)Ljava/lang/StringBuffer;"))
    m_SB_appO = CPe(mref(c_StrBfr, CPe(nat(n_append, s_SB_appO))))
    m_SB_len  = CPe(mref(c_StrBfr, CPe(nat(n_length, s_ret_I))))

    s_val_SR  = CPe(utf8("(Ljava/lang/String;I)Ljava/lang/Integer;"))
    m_Int_valOfSR = CPe(mref(c_Integer, CPe(nat(n_valueOf, s_val_SR))))
    s_toStr_II= CPe(utf8("(II)Ljava/lang/String;"))
    m_Int_toStrR = CPe(mref(c_Integer, CPe(nat(n_toString, s_toStr_II))))
    n_parseByte = CPe(utf8("parseByte"))
    s_pB      = CPe(utf8("(Ljava/lang/String;)B"))
    m_Byt_parseB = CPe(mref(c_Byte, CPe(nat(n_parseByte, s_pB))))

    n_str_2A  = CPe(utf8("2A"));  c_str_2A = CPe(cstr(n_str_2A))
    n_str_m100= CPe(utf8("-100")); c_str_m100 = CPe(cstr(n_str_m100))

    # ---- Round 9 additions: complete Math + exception ctor probes ---
    # (WWG's boot diag froze on "m: java/lang/Math.max" — the (JJ)J
    # overload — and its catch blocks instantiate the IndexOutOfBounds
    # family with messages, which used to resolve only through the
    # Throwable stub chain)
    def cdbl(v): return u1(6) + struct.pack('>d', v)   # 2 cp slots
    def cflt(v): return u1(4) + struct.pack('>f', v)   # 1 slot
    n_str_boom = CPe(utf8("boom")); c_str_boom = CPe(cstr(n_str_boom))
    n_str_re   = CPe(utf8("re"));   c_str_re   = CPe(cstr(n_str_re))
    n_str_ix   = CPe(utf8("ix"));   c_str_ix   = CPe(cstr(n_str_ix))

    c_Math    = CPe(cls(CPe(utf8("java/lang/Math"))))
    n_max     = CPe(utf8("max"));  n_min  = CPe(utf8("min"))
    n_sqrt    = CPe(utf8("sqrt")); n_sin  = CPe(utf8("sin"))
    n_cos     = CPe(utf8("cos"));  n_ceil = CPe(utf8("ceil"))
    s_II      = CPe(utf8("(II)I"))
    s_JJ      = CPe(utf8("(JJ)J"))
    s_DD      = CPe(utf8("(DD)D"))
    s_FF      = CPe(utf8("(FF)F"))
    s_D_D     = CPe(utf8("(D)D"))
    m_Math_maxII = CPe(mref(c_Math, CPe(nat(n_max, s_II))))
    m_Math_minII = CPe(mref(c_Math, CPe(nat(n_min, s_II))))
    m_Math_maxJJ = CPe(mref(c_Math, CPe(nat(n_max, s_JJ))))
    m_Math_minJJ = CPe(mref(c_Math, CPe(nat(n_min, s_JJ))))
    m_Math_maxDD = CPe(mref(c_Math, CPe(nat(n_max, s_DD))))
    m_Math_maxFF = CPe(mref(c_Math, CPe(nat(n_max, s_FF))))
    m_Math_sqrt  = CPe(mref(c_Math, CPe(nat(n_sqrt, s_D_D))))
    m_Math_sin   = CPe(mref(c_Math, CPe(nat(n_sin, s_D_D))))
    m_Math_cos   = CPe(mref(c_Math, CPe(nat(n_cos, s_D_D))))
    m_Math_ceil  = CPe(mref(c_Math, CPe(nat(n_ceil, s_D_D))))
    d_16 = CPe(cdbl(16.0)); d_0 = CPe(cdbl(0.0)); d_1 = CPe(cdbl(1.0))
    d_15 = CPe(cdbl(1.5));  d_2 = CPe(cdbl(2.0)); d_25 = CPe(cdbl(2.5))
    f_25 = CPe(cflt(2.5));  f_15 = CPe(cflt(1.5))
    c_lng_m2 = CPe(clong(-2)); c_lng_5 = CPe(clong(5))

    # exception stub classes + their <init> methodrefs (Round 9)
    def exc_cls(name):
        return CPe(cls(CPe(utf8(name))))
    c_SIOOBE = exc_cls("java/lang/StringIndexOutOfBoundsException")
    c_IAE    = exc_cls("java/lang/IllegalArgumentException")
    c_RE     = exc_cls("java/lang/RuntimeException")
    c_IOOBE  = exc_cls("java/lang/IndexOutOfBoundsException")
    c_Thr    = exc_cls("java/lang/Throwable")
    s_msg    = CPe(utf8("(Ljava/lang/String;)V"))
    def exc_init(c):
        return CPe(mref(c, CPe(nat(n_init, s_msg))))
    m_SIOOBE_init = exc_init(c_SIOOBE)
    m_IAE_init    = exc_init(c_IAE)
    m_RE_init     = exc_init(c_RE)
    m_IOOBE_init  = exc_init(c_IOOBE)
    m_IAE_init0   = CPe(mref(c_IAE, CPe(nat(n_init, s_V))))
    n_getMsg  = CPe(utf8("getMessage"))
    m_Thr_getMsg = CPe(mref(c_Thr, CPe(nat(n_getMsg, s_ret_Str))))
    n_equals  = CPe(utf8("equals"))
    m_StrEq   = CPe(mref(c_StrCls, CPe(nat(n_equals, s_arg_Obj))))

    m_run25 = CPe(utf8("run25")); m_run26 = CPe(utf8("run26"))
    m_run27 = CPe(utf8("run27")); m_run28 = CPe(utf8("run28"))
    m_run29 = CPe(utf8("run29")); m_run30 = CPe(utf8("run30"))
    m_run31 = CPe(utf8("run31")); m_run32 = CPe(utf8("run32"))


    # ---- Round 10 (UsbCast): the comm:usb GCF roundtrip ----------------
    # hostcheck feeds "FPJ2hello" + 'Z' into the mock USB and checks the
    # 'K' ACK in the mock TX capture. The FIRST open() must throw the
    # MIDP-mandated ConnectionNotFoundException (caught via the Round 9
    # exception machinery + the new io stub chain).
    c_Conn = CPe(cls(CPe(utf8("javax/microedition/io/Connector"))))
    c_CNFE = CPe(cls(CPe(utf8(
        "javax/microedition/io/ConnectionNotFoundException"))))
    c_SC   = CPe(cls(CPe(utf8("javax/microedition/io/StreamConnection"))))
    c_In   = CPe(cls(CPe(utf8("java/io/InputStream"))))
    c_Out  = CPe(cls(CPe(utf8("java/io/OutputStream"))))
    s_open = CPe(utf8("(Ljava/lang/String;)"
                      "Ljavax/microedition/io/StreamConnection;"))
    m_open = CPe(mref(c_Conn, CPe(nat(CPe(utf8("open")), s_open))))
    m_sc_in = CPe(mref(c_SC, CPe(nat(
        CPe(utf8("openInputStream")),
        CPe(utf8("()Ljava/io/InputStream;"))))))
    m_sc_out = CPe(mref(c_SC, CPe(nat(
        CPe(utf8("openOutputStream")),
        CPe(utf8("()Ljava/io/OutputStream;"))))))
    m_rd3 = CPe(mref(c_In, CPe(nat(
        CPe(utf8("read")), CPe(utf8("([BII)I"))))))
    m_rd1 = CPe(mref(c_In, CPe(nat(
        CPe(utf8("read")), CPe(utf8("()I"))))))
    m_wrI = CPe(mref(c_Out, CPe(nat(
        CPe(utf8("write")), CPe(utf8("(I)V"))))))
    c_http = CPe(cstr(CPe(utf8("http:zz"))))
    c_usb  = CPe(cstr(CPe(utf8("comm:usb"))))
    m_run33 = CPe(utf8("run33"))

    n_boxing_methods = [
      # run6()I: Integer round-trip incl. negatives + the String overload
      #   valueOf(-1234)+valueOf(127)+valueOf(128)+valueOf(0)
      #   + new Integer(-7) + valueOf("-99")  -> -1085
      (m_run6, s_ret_I,
       u1(0x11) + u2(0xFB2E)     #  0: sipush -1234 (0xFB2E)
       + u1(0xB8) + u2(m_Int_valOf) + u1(0xB6) + u2(m_Int_intVal)
       + u1(0x11) + u2(127) + u1(0xB8) + u2(m_Int_valOf)
       + u1(0xB6) + u2(m_Int_intVal) + u1(0x60)
       + u1(0x11) + u2(128) + u1(0xB8) + u2(m_Int_valOf)
       + u1(0xB6) + u2(m_Int_intVal) + u1(0x60)
       + u1(0x03) + u1(0xB8) + u2(m_Int_valOf)
       + u1(0xB6) + u2(m_Int_intVal) + u1(0x60)
       + u1(0xBB) + u2(c_Integer) + u1(0x59)          # new Integer; dup
       + u1(0x10) + u1(0xF9) + u1(0xB7) + u2(m_Int_init)   # bipush -7
       + u1(0xB6) + u2(m_Int_intVal) + u1(0x60)
       + u1(0x12) + u1(c_str_m99) + u1(0xB8) + u2(m_Int_valOfS)
       + u1(0xB6) + u2(m_Int_intVal) + u1(0x60)
       + u1(0xAC), 4, 0),
      # run7()I: Boolean round-trip: valueOf(true) AND new Boolean(false)
      #   -> 1 + 1 = 2 when both paths work
      (m_run7, s_ret_I,
       u1(0x04) + u1(0xB8) + u2(m_Bln_valOf) + u1(0xB6) + u2(m_Bln_blnVal)
       + u1(0x99) + u2(7)                          #  7: ifeq -> 14
       + u1(0x04) + u1(0xA7) + u2(4)               # 10: iconst_1; goto -> 15
       + u1(0x03)                                   # 14: iconst_0
       + u1(0xBB) + u2(c_Boolean) + u1(0x59)       # 15: new Boolean; dup
       + u1(0x03) + u1(0xB7) + u2(m_Bln_init)      #     iconst_0; <init>(Z)
       + u1(0xB6) + u2(m_Bln_blnVal)
       + u1(0x99) + u2(7)                          # 26: ifeq -> 33
       + u1(0x03) + u1(0xA7) + u2(4)               # 29: iconst_0; goto -> 34
       + u1(0x04)                                   # 33: iconst_1
       + u1(0x60) + u1(0xAC), 4, 0),                # 34: iadd; ireturn
      # run8()I: Long round-trip (negative): valueOf(J).longValue()==J -> 1
      (m_run8, s_ret_I,
       u1(0x14) + u2(c_lng_A) + u1(0xB8) + u2(m_Lng_valOf)
       + u1(0xB6) + u2(m_Lng_lngVal)
       + u1(0x14) + u2(c_lng_A) + u1(0x94)          # ldc2_w; lcmp
       + u1(0x9A) + u2(7)                           # 13: ifne -> 20
       + u1(0x04) + u1(0xA7) + u2(4)                # 16: iconst_1; goto -> 21
       + u1(0x03) + u1(0xAC), 4, 0),                # 20: iconst_0; ireturn
      # run9()I: Byte round-trip: valueOf(-100).byteValue()
      #   + valueOf(90).byteValue() -> -10
      (m_run9, s_ret_I,
       u1(0x10) + u1(0x9C) + u1(0xB8) + u2(m_Byt_valOf)
       + u1(0xB6) + u2(m_Byt_bytVal)
       + u1(0x10) + u1(90) + u1(0xB8) + u2(m_Byt_valOf)
       + u1(0xB6) + u2(m_Byt_bytVal) + u1(0x60) + u1(0xAC), 4, 0),
      # run10()I: Short round-trip: valueOf(-20000).shortValue()
      #   + valueOf(12345).shortValue() -> -7655
      (m_run10, s_ret_I,
       u1(0x11) + u2(0xB1E0) + u1(0xB8) + u2(m_Sht_valOf)
       + u1(0xB6) + u2(m_Sht_shtVal)
       + u1(0x11) + u2(12345) + u1(0xB8) + u2(m_Sht_valOf)
       + u1(0xB6) + u2(m_Sht_shtVal) + u1(0x60) + u1(0xAC), 4, 0),
      # run11()I: Character round-trip incl. a non-ASCII char:
      #   valueOf(65).charValue() + valueOf(0x4E2D).charValue() -> 20078
      (m_run11, s_ret_I,
       u1(0x10) + u1(65) + u1(0xB8) + u2(m_Chr_valOf)
       + u1(0xB6) + u2(m_Chr_chrVal)
       + u1(0x11) + u2(0x4E2D) + u1(0xB8) + u2(m_Chr_valOf)
       + u1(0xB6) + u2(m_Chr_chrVal) + u1(0x60) + u1(0xAC), 4, 0),
      # run12..run17: instance toString() of every wrapper, checked as
      # strings by hostcheck (areturn leaves the String ref in ret[0])
      (m_run12, s_ret_Str,
       u1(0x11) + u2(0xFE38) + u1(0xB8) + u2(m_Int_valOf)   # sipush -456
       + u1(0xB6) + u2(m_Int_toStr) + u1(0xB0), 4, 0),
      (m_run13, s_ret_Str,
       u1(0x14) + u2(c_lng_B) + u1(0xB8) + u2(m_Lng_valOf)
       + u1(0xB6) + u2(m_Lng_toStr) + u1(0xB0), 4, 0),
      (m_run14, s_ret_Str,
       u1(0x03) + u1(0xB8) + u2(m_Bln_valOf)
       + u1(0xB6) + u2(m_Bln_toStr) + u1(0xB0), 4, 0),
      (m_run15, s_ret_Str,
       u1(0x10) + u1(65) + u1(0xB8) + u2(m_Chr_valOf)
       + u1(0xB6) + u2(m_Chr_toStr) + u1(0xB0), 4, 0),
      (m_run16, s_ret_Str,
       u1(0x10) + u1(0xFD) + u1(0xB8) + u2(m_Byt_valOf)     # bipush -3
       + u1(0xB6) + u2(m_Byt_toStr) + u1(0xB0), 4, 0),
      (m_run17, s_ret_Str,
       u1(0x11) + u2(12345) + u1(0xB8) + u2(m_Sht_valOf)
       + u1(0xB6) + u2(m_Sht_toStr) + u1(0xB0), 4, 0),
      # run18()I: value-based equals: Integer(-9).equals(new Integer(-9))
      #   true, Integer(5).equals(Short(5)) false -> 1 + 1 = 2
      (m_run18, s_ret_I,
       u1(0x11) + u2(0xFFF7) + u1(0xB8) + u2(m_Int_valOf)   # sipush -9
       + u1(0xBB) + u2(c_Integer) + u1(0x59)
       + u1(0x11) + u2(0xFFF7) + u1(0xB7) + u2(m_Int_init)
       + u1(0xB6) + u2(m_Int_equals)
       + u1(0x99) + u2(7)                          # 19: ifeq -> 26
       + u1(0x04) + u1(0xA7) + u2(4)               # 22: iconst_1; goto -> 27
       + u1(0x03)                                   # 26: iconst_0
       + u1(0x08) + u1(0xB8) + u2(m_Int_valOf)     # 27: iconst_5; valueOf(5)
       + u1(0x08) + u1(0xB8) + u2(m_Sht_valOf)     #       valueOf((short)5)
       + u1(0xB6) + u2(m_Int_equals)
       + u1(0x99) + u2(7)                          # 37: ifeq -> 44
       + u1(0x03) + u1(0xA7) + u2(4)               # 40: iconst_0; goto -> 45
       + u1(0x04)                                   # 44: iconst_1
       + u1(0x60) + u1(0xAC), 4, 0),                # 45: iadd; ireturn
      # run19()I: Boolean TRUE/FALSE singleton identity:
      #   valueOf(true) == valueOf(true) -> 1
      (m_run19, s_ret_I,
       u1(0x04) + u1(0xB8) + u2(m_Bln_valOf)
       + u1(0x04) + u1(0xB8) + u2(m_Bln_valOf)
       + u1(0xA6) + u2(7)                           #  8: if_acmpne -> 15
       + u1(0x04) + u1(0xA7) + u2(4)                # 11: iconst_1; goto -> 16
       + u1(0x03) + u1(0xAC), 4, 0),                # 15: iconst_0; ireturn
      # ---- AC2's real call sites (constant-pool scan of GloftASC2) ---
      # run20()I: Integer.valueOf("2A", 16).intValue() -> 42
      #   (THE sound-prompt overload: valueOf(String, radix))
      (m_run20, s_ret_I,
       u1(0x12) + u1(c_str_2A)                      #  0: ldc "2A"
       + u1(0x10) + u1(16)                          #  2: bipush 16
       + u1(0xB8) + u2(m_Int_valOfSR)               #  4: valueOf(String,I)
       + u1(0xB6) + u2(m_Int_intVal)                #  7: intValue
       + u1(0xAC), 4, 0),                           # 10: ireturn
      # run21()Ljava/lang/String;: Integer.toString(255, 16) -> "ff"
      (m_run21, s_ret_Str,
       u1(0x11) + u2(255)                           #  0: sipush 255
       + u1(0x10) + u1(16)                          #  3: bipush 16
       + u1(0xB8) + u2(m_Int_toStrR)                #  5: toString(II)
       + u1(0xB0), 4, 0),                           #  8: areturn
      # run22()I: Byte.parseByte("-100") -> -100
      (m_run22, s_ret_I,
       u1(0x12) + u1(c_str_m100)                    #  0: ldc "-100"
       + u1(0xB8) + u2(m_Byt_parseB)                #  2: parseByte(String)
       + u1(0xAC), 4, 0),                           #  5: ireturn
      # run23()I: new String(bytes, off, len) — the GLLib text path:
      #   b = {'f','p','j','2','m'}; s = new String(b, 0, 5);
      #   return s.length() + (s.charAt(0)=='f' ? 100 : 0)
      #        + new String(b, 1, 2).length() * 2;  -> 5+100+4 = 109
      # locals: 0 = b, 1 = s
      (m_run23, s_ret_I,
       u1(0x08) + u1(0xBC) + u1(8)                  #  0: iconst_5; newarray byte
       + u1(0x59) + u1(0x03) + u1(0x10) + u1(102) + u1(0x54)   # b[0]='f'
       + u1(0x59) + u1(0x04) + u1(0x10) + u1(112) + u1(0x54)   # b[1]='p'
       + u1(0x59) + u1(0x05) + u1(0x10) + u1(106) + u1(0x54)   # b[2]='j'
       + u1(0x59) + u1(0x06) + u1(0x10) + u1(50)  + u1(0x54)   # b[3]='2'
       + u1(0x59) + u1(0x07) + u1(0x10) + u1(109) + u1(0x54)   # b[4]='m'
       + u1(0x4B)                                   # 28: astore_0
       + u1(0xBB) + u2(c_StrCls) + u1(0x59)         # 29: new String; dup
       + u1(0x1A) + u1(0x03) + u1(0x08)             # 33: aload_0; 0; 5
       + u1(0xB7) + u2(m_Str_initB)                 # 36: <init>([BII)
       + u1(0x4C)                                   # 39: astore_1
       + u1(0x1B) + u1(0xB6) + u2(m_Str_len)        # 40: s.length()
       + u1(0x1B) + u1(0x03)                        # 44: s; index 0
       + u1(0xB6) + u2(m_Str_chAt)                  # 46: s.charAt(0)
       + u1(0x10) + u1(102)                         # 49: 'f'
       + u1(0xA0) + u2(8)                           # 51: if_icmpne -> 59
       + u1(0x10) + u1(100)                         # 54: bipush 100
       + u1(0xA7) + u2(4)                           # 56: goto -> 60
       + u1(0x03)                                   # 59: iconst_0
       + u1(0x60)                                   # 60: iadd
       + u1(0xBB) + u2(c_StrCls) + u1(0x59)         # 61: new String; dup
       + u1(0x1A) + u1(0x04) + u1(0x05)             # 65: aload_0; 1; 2
       + u1(0xB7) + u2(m_Str_initB)                 # 68: <init>([BII)
       + u1(0xB6) + u2(m_Str_len)                   # 71: length() = 2
       + u1(0x05)                                   # 74: iconst_2
       + u1(0x68)                                   # 75: imul  (2 * 2)
       + u1(0x60) + u1(0xAC), 5, 2),               # 76: iadd; ireturn
      # run24()I: StringBuffer append(Z) + append(Object):
      #   sb = new StringBuffer(); sb.append(true); sb.append(false);
      #   sb.append(Integer.valueOf(7)); return sb.length();  -> 10
      # locals: 0 = sb
      (m_run24, s_ret_I,
       u1(0xBB) + u2(c_StrBfr) + u1(0x59)           #  0: new SB; dup
       + u1(0xB7) + u2(m_SB_init)                   #  4: <init>()V
       + u1(0x4B)                                   #  7: astore_0
       + u1(0x1A) + u1(0x04)                        #  8: aload_0; true
       + u1(0xB6) + u2(m_SB_appZ) + u1(0x57)        # 11: append(Z); pop
       + u1(0x1A) + u1(0x03)                        # 15: aload_0; false
       + u1(0xB6) + u2(m_SB_appZ) + u1(0x57)        # 18: append(Z); pop
       + u1(0x1A)                                   # 22: aload_0 (receiver!)
       + u1(0x10) + u1(7)                           # 23: bipush 7
       + u1(0xB8) + u2(m_Int_valOf)                 # 25: Integer.valueOf(7)
       + u1(0xB6) + u2(m_SB_appO) + u1(0x57)        # 28: append(Object); pop
       + u1(0x1A) + u1(0xB6) + u2(m_SB_len)         # 32: sb.length()
       + u1(0xAC), 3, 1),                          # 35: ireturn
      # ---- Round 9 probes (WWG) --------------------------------------
      # run25()I: Math max/min int + long overload — the (JJ)J pair the
      # WWG boot actually tripped on (m: java/lang/Math.max):
      #   max(-5,3) + min(-5,3) + (int)max(-2L,5L) + (int)min(-2L,5L)
      #   = 3 - 5 + 5 - 2  -> 1
      (m_run25, s_ret_I,
       u1(0x10) + u1(0xFB)                          #  0: bipush -5
       + u1(0x06)                                   #  2: iconst_3
       + u1(0xB8) + u2(m_Math_maxII)                #  3: Math.max(II)I
       + u1(0x10) + u1(0xFB)                        #  6: bipush -5
       + u1(0x06)                                   #  8: iconst_3
       + u1(0xB8) + u2(m_Math_minII)                #  9: Math.min(II)I
       + u1(0x60)                                   # 12: iadd (-2)
       + u1(0x14) + u2(c_lng_m2)                    # 13: ldc2_w -2L
       + u1(0x14) + u2(c_lng_5)                     # 16: ldc2_w 5L
       + u1(0xB8) + u2(m_Math_maxJJ)                # 19: Math.max(JJ)J
       + u1(0x88)                                   # 22: l2i (5)
       + u1(0x14) + u2(c_lng_m2)                    # 23: ldc2_w -2L
       + u1(0x14) + u2(c_lng_5)                     # 26: ldc2_w 5L
       + u1(0xB8) + u2(m_Math_minJJ)                # 29: Math.min(JJ)J
       + u1(0x88)                                   # 32: l2i (-2)
       + u1(0x60) + u1(0x60) + u1(0xAC), 4, 0),     # 33: iadd; iadd; ireturn
      # run26()I: Math double/float + sqrt/sin/cos/ceil through the
      # REAL interpreter marshaling (d2i/f2i + dcmpl); ldc_w because
      # the pool is past 255 entries now:
      #   (int)sqrt(16) + (int)max(2.5,1.5) + (int)max(2.5f,1.5f)
      #   + [sin(0)==0]*6 + [cos(0)==1]*40 + [ceil(1.5)==2]*400
      #   = 4 + 2 + 2 + 6 + 40 + 400  -> 454
      (m_run26, s_ret_I,
       u1(0x14) + u2(d_16)                          #  0: ldc2_w 16.0
       + u1(0xB8) + u2(m_Math_sqrt)                 #  3: Math.sqrt(D)D
       + u1(0x8E)                                   #  6: d2i (4)
       + u1(0x14) + u2(d_25)                        #  7: ldc2_w 2.5
       + u1(0x14) + u2(d_15)                        # 10: ldc2_w 1.5
       + u1(0xB8) + u2(m_Math_maxDD)                # 13: Math.max(DD)D
       + u1(0x8E)                                   # 16: d2i (2)
       + u1(0x60)                                   # 17: iadd (6)
       + u1(0x13) + u2(f_25)                        # 18: ldc_w 2.5f
       + u1(0x13) + u2(f_15)                        # 21: ldc_w 1.5f
       + u1(0xB8) + u2(m_Math_maxFF)                # 24: Math.max(FF)F
       + u1(0x8B)                                   # 27: f2i (2)
       + u1(0x60)                                   # 28: iadd (8)
       + u1(0x14) + u2(d_0)                         # 29: ldc2_w 0.0
       + u1(0xB8) + u2(m_Math_sin)                  # 32: Math.sin(D)D
       + u1(0x14) + u2(d_0)                         # 35: ldc2_w 0.0
       + u1(0x97)                                   # 38: dcmpl
       + u1(0x9A) + u2(7)                           # 39: ifne -> 46
       + u1(0x11) + u2(6)                           # 42: sipush 6
       + u1(0x60)                                   # 45: iadd
       + u1(0x14) + u2(d_0)                         # 46: ldc2_w 0.0
       + u1(0xB8) + u2(m_Math_cos)                  # 49: Math.cos(D)D
       + u1(0x14) + u2(d_1)                         # 52: ldc2_w 1.0
       + u1(0x97)                                   # 55: dcmpl
       + u1(0x9A) + u2(7)                           # 56: ifne -> 63
       + u1(0x11) + u2(40)                          # 59: sipush 40
       + u1(0x60)                                   # 62: iadd
       + u1(0x14) + u2(d_15)                        # 63: ldc2_w 1.5
       + u1(0xB8) + u2(m_Math_ceil)                 # 66: Math.ceil(D)D
       + u1(0x14) + u2(d_2)                         # 69: ldc2_w 2.0
       + u1(0x97)                                   # 72: dcmpl
       + u1(0x9A) + u2(7)                           # 73: ifne -> 80
       + u1(0x11) + u2(400)                         # 76: sipush 400
       + u1(0x60)                                   # 79: iadd
       + u1(0xAC), 4, 0),                           # 80: ireturn
      # run27()I: exception stub ctors end-to-end through the 'new'
      # opcode + invokespecial + inherited getMessage:
      #   new StringIndexOutOfBoundsException("boom").getMessage()
      #     .equals("boom")                            -> +1
      #   new IllegalArgumentException().getMessage()
      #     == null                                   -> +2
      #   new RuntimeException("re").getMessage()
      #     .equals("re")                             -> +8
      #   new IndexOutOfBoundsException("ix").getMessage()
      #     .equals("ix")                             -> +16
      # locals: 1..4 = the four exception objects; sum on the stack
      (m_run27, s_ret_I,
       u1(0xBB) + u2(c_SIOOBE) + u1(0x59)           #  0: new SIOOBE; dup
       + u1(0x13) + u2(c_str_boom)                  #  3: ldc_w "boom"
       + u1(0xB7) + u2(m_SIOOBE_init)               #  6: <init>(String)
       + u1(0x4C)                                   #  9: astore_1
       + u1(0x13) + u2(c_str_boom)                  # 10: ldc_w "boom"
       + u1(0x2B)                                   # 13: aload_1
       + u1(0xB6) + u2(m_Thr_getMsg)                # 14: getMessage()
       + u1(0xB6) + u2(m_StrEq)                     # 17: String.equals
       + u1(0x99) + u2(7)                           # 20: ifeq -> 27 (fail)
       + u1(0x04)                                   # 23: iconst_1
       + u1(0xA7) + u2(4)                           # 24: goto -> 28
       + u1(0x03)                                   # 27: iconst_0 [fail]
       + u1(0xBB) + u2(c_IAE) + u1(0x59)            # 28: new IAE; dup
       + u1(0xB7) + u2(m_IAE_init0)                 # 31: <init>()V
       + u1(0x4D)                                   # 34: astore_2
       + u1(0x2C)                                   # 35: aload_2
       + u1(0xB6) + u2(m_Thr_getMsg)                # 36: getMessage()
       + u1(0xC7) + u2(8)                           # 39: ifnonnull -> 47
       + u1(0x10) + u1(2)                           # 42: bipush 2
       + u1(0xA7) + u2(4)                           # 44: goto -> 48
       + u1(0x03)                                   # 47: iconst_0 [fail]
       + u1(0xBB) + u2(c_RE) + u1(0x59)             # 48: new RE; dup
       + u1(0x13) + u2(c_str_re)                    # 51: ldc_w "re"
       + u1(0xB7) + u2(m_RE_init)                   # 54: <init>(String)
       + u1(0x3E)                                   # 57: astore_3
       + u1(0x13) + u2(c_str_re)                    # 58: ldc_w "re"
       + u1(0x2D)                                   # 61: aload_3
       + u1(0xB6) + u2(m_Thr_getMsg)                # 62: getMessage()
       + u1(0xB6) + u2(m_StrEq)                     # 65: String.equals
       + u1(0x99) + u2(8)                           # 68: ifeq -> 79 (fail)
       + u1(0x10) + u1(8)                           # 71: bipush 8
       + u1(0xA7) + u2(4)                           # 73: goto -> 80
       + u1(0x03)                                   # 79: iconst_0 [fail]
       + u1(0xBB) + u2(c_IOOBE) + u1(0x59)          # 80: new IOOBE; dup
       + u1(0x13) + u2(c_str_ix)                    # 83: ldc_w "ix"
       + u1(0xB7) + u2(m_IOOBE_init)                # 86: <init>(String)
       + u1(0x3A) + u1(4)                           # 89: astore 4
       + u1(0x13) + u2(c_str_ix)                    # 91: ldc_w "ix"
       + u1(0x19) + u1(4)                           # 94: aload 4
       + u1(0xB6) + u2(m_Thr_getMsg)                # 96: getMessage()
       + u1(0xB6) + u2(m_StrEq)                     # 99: String.equals
       + u1(0x99) + u2(8)                           # 102: ifeq -> 111 (fail)
       + u1(0x10) + u1(16)                          # 105: bipush 16
       + u1(0xA7) + u2(4)                           # 107: goto -> 112
       + u1(0x03)                                   # 111: iconst_0 [fail]
       + u1(0x60) + u1(0x60) + u1(0x60)             # 112: iadd x3
       + u1(0xAC), 6, 5),                           # 115: ireturn
      # run29()I: ISOLATED check B — new IAE() + getMessage()==null -> 2
      (m_run29, s_ret_I,
       u1(0xBB) + u2(c_IAE) + u1(0x59)              #  0: new IAE; dup
       + u1(0xB7) + u2(m_IAE_init0)                 #  3: <init>()V
       + u1(0x4D)                                   #  6: astore_2
       + u1(0x2C)                                   #  7: aload_2
       + u1(0xB6) + u2(m_Thr_getMsg)                #  9: getMessage()
       + u1(0xC7) + u2(7)                           # 12: ifnonnull -> 19
       + u1(0x05)                                   # 15: iconst_2
       + u1(0xA7) + u2(4)                           # 16: goto -> 20
       + u1(0x03)                                   # 19: iconst_0
       + u1(0xAC), 3, 3),                           # 20: ireturn
      # run30()I: ISOLATED sqrt — (int)Math.sqrt(16.0) -> 4
      (m_run30, s_ret_I,
       u1(0x14) + u2(d_16)                          #  0: ldc2_w 16.0
       + u1(0xB8) + u2(m_Math_sqrt)                 #  3: sqrt(D)D
       + u1(0x8E)                                   #  6: d2i
       + u1(0xAC), 2, 0),                           #  7: ireturn
      # run31()I: ISOLATED sin check — sin(0)==0 -> 6 else 0
      (m_run31, s_ret_I,
       u1(0x14) + u2(d_0)                           #  0: ldc2_w 0.0
       + u1(0xB8) + u2(m_Math_sin)                  #  3: sin(D)D
       + u1(0x14) + u2(d_0)                         #  6: ldc2_w 0.0
       + u1(0x97)                                   #  9: dcmpl
       + u1(0x9A) + u2(9)                           # 10: ifne -> 19 (fail)
       + u1(0x11) + u2(6)                           # 13: sipush 6
       + u1(0xA7) + u2(4)                           # 16: goto -> 20
       + u1(0x03)                                   # 19: iconst_0
       + u1(0xAC), 4, 0),                           # 20: ireturn
      # run32()I: ISOLATED maxFF — (int)Math.max(2.5f, 1.5f) -> 2
      (m_run32, s_ret_I,
       u1(0x13) + u2(f_25)                          #  0: ldc_w 2.5f
       + u1(0x13) + u2(f_15)                        #  3: ldc_w 1.5f
       + u1(0xB8) + u2(m_Math_maxFF)                #  6: max(FF)F
       + u1(0x8B)                                   #  9: f2i
       + u1(0xAC), 2, 0),                           # 10: ireturn
    ]

    out = u4(0xCAFEBABE) + u2(0) + u2(46)   # magic, minor, major
    out += u2(state['next']) + b''.join(cp[1:])   # count incl. phantoms
    out += u2(0x0021)                        # public super
    out += u2(2) + u2(4)                     # this, super
    out += u2(0)                             # interfaces
    out += u2(3)                             # 3 fields
    out += u2(0x0009) + u2(5) + u2(6) + u2(0)    # public static int x
    out += u2(0x0009) + u2(37) + u2(38) + u2(0)  # public static Tst inst
    out += u2(0x0009) + u2(56) + u2(6) + u2(0)   # public static int y
    out += u2(8 + len(n_boxing_methods) + 1)  # + boxing probes (run6..)
                                             # + run28 (exc table) + run33
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
    # ---- boxing round-trip methods (run6..run27) ----------------------
    for mn, msig, mcode, mstack, mlocals in n_boxing_methods:
        out += method(0x0009, mn, msig,
                      code_attr(12, mstack, mlocals, mcode))

    # run28()I — the full throw/catch round trip (Round 9): the
    # interpreter's array-bounds check raises ArrayIndexOutOfBounds-
    # Exception, the unwinder matches the catch (IndexOutOfBounds-
    # Exception — a SUPERCLASS, so the stub-chain walk is exercised)
    # and lands in the handler. The first test method to carry a
    # nonzero exception_table.
    #   0: iconst_3; newarray int; iconst_5; iaload  <- RAISES
    #   5: ireturn (unreachable)
    #   6: astore_1; bipush 7; ireturn                <- handler
    #   exc table: [0, 6) -> 6, catch java/lang/IndexOutOfBoundsException
    r28 = (u1(0x06) + u1(0xBC) + u1(10)          #   0: iconst_3; newarray
           + u1(0x08) + u1(0x2E)                 #   3: iconst_5; iaload
           + u1(0xAC)                            #   5: ireturn
           + u1(0x4C)                            #   6: astore_1 (handler)
           + u1(0x10) + u1(7) + u1(0xAC))        #   7: bipush 7; ireturn
    r28_body = u2(3) + u2(2) + u4(len(r28)) + r28
    r28_body += u2(1)                            # 1 exception-table entry
    r28_body += u2(0) + u2(6) + u2(6) + u2(c_IOOBE)
    r28_body += u2(0)                            # no code attributes
    out += (u2(0x0009) + u2(m_run28) + u2(s_ret_I) + u2(1)
            + u2(12) + u4(len(r28_body)) + r28_body)

    # ---- Round 10 (UsbCast) cp entries: moved ABOVE the pool dump --

    # static method, locals 0..5 (0 unused to keep the numbering simple)
    r33 = (u1(0x13) + u2(c_http)                 #  0: ldc_w "http:zz"
           + u1(0xB8) + u2(m_open)               #  3: invokestatic open
           + u1(0x57)                            #  6: pop
           + u1(0x11) + u2(900)                  #  7: sipush 900
           + u1(0xAC)                            # 10: ireturn
           + u1(0x57)                            # 11: pop [catch handler]
           + u1(0x13) + u2(c_usb)                # 12: ldc_w "comm:usb"
           + u1(0xB8) + u2(m_open)               # 15: invokestatic open
           + u1(0x4C)                            # 18: astore_1 (conn)
           + u1(0x2B) + u1(0xB6) + u2(m_sc_in)   # 19: openInputStream
           + u1(0x4D)                            # 23: astore_2 (in)
           + u1(0x2B) + u1(0xB6) + u2(m_sc_out)  # 24: openOutputStream
           + u1(0x3E)                            # 28: astore_3 (out)
           + u1(0x10) + u1(10)                   # 29: bipush 10
           + u1(0xBC) + u1(4)                    # 31: newarray byte
           + u1(0x3A) + u1(4)                    # 33: astore 4 (b)
           + u1(0x2C) + u1(0x19) + u1(4)         # 35: aload_2; aload 4
           + u1(0x03)                            # 38: iconst_0
           + u1(0x10) + u1(10)                   # 39: bipush 10
           + u1(0xB6) + u2(m_rd3)                # 41: read([BII)I
           + u1(0x36) + u1(5)                    # 44: istore 5 (n)
           + u1(0x2D)                            # 46: aload_3
           + u1(0x10) + u1(75)                   # 47: bipush 75 'K'
           + u1(0xB6) + u2(m_wrI)                # 49: write(I)V -> ACK
           + u1(0x2C) + u1(0xB6) + u2(m_rd1)     # 52: read()I
           + u1(0x10) + u1(90)                   # 56: bipush 90 'Z'
           + u1(0xA0) + u2(9)                    # 58: if_icmpne -> 67
           + u1(0x15) + u1(5)                    # 61: iload 5
           + u1(0x10) + u1(100)                  # 63: bipush 100
           + u1(0x60)                            # 65: iadd
           + u1(0xAC)                            # 66: ireturn n+100
           + u1(0x15) + u1(5)                    # 67: iload 5 (fail path)
           + u1(0xAC))                           # 69: ireturn n
    r33_body = u2(4) + u2(6) + u4(len(r33)) + r33
    r33_body += u2(1)                            # 1 exception-table entry
    r33_body += u2(0) + u2(11) + u2(11) + u2(c_CNFE)
    r33_body += u2(0)                            # no code attributes
    out += (u2(0x0009) + u2(m_run33) + u2(s_ret_I) + u2(1)
            + u2(12) + u4(len(r33_body)) + r33_body)

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
