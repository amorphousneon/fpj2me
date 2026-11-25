#!/usr/bin/env python3
"""
usbcast_host.py — fpj2me Round 10/11: the LINUX side of USB screen casting.

Turns a CachyOS / KDE Plasma (Wayland) laptop into a sender for the
itel i2163 feature phone running fpj2me's UsbCast.jar MIDlet. The phone
enumerates as fpdoom's raw vendor-class USB device:

    VID 0x1782 / PID 0x4D00, class 0xFF, 64-byte bulk packets,
    EP OUT 0x02 (phone RX) / EP IN 0x83 (phone TX)

Frame protocol (self-syncing):
    "FPJ2" | u16le width=128 | u16le height=160 | u8 flags=0 (RGB565)
    | width*height*2 bytes little-endian RGB565
Backpressure: after each frame the phone sends a 1-byte ACK over EP IN:
    'K' (0x4B) = frame displayed, send the next one
    'Q' (0x51) = the MIDlet exited (user pressed the exit key) — stop
    'R' (0x52) = hello, sent once when UsbCast boots
"""
import argparse
import os
import subprocess
import sys
import time

W, H = 128, 160
FRAME_RGB565 = W * H * 2
HEADER = b"FPJ2" + W.to_bytes(2, "little") + H.to_bytes(2, "little") + b"\x00"
VID, PID = 0x1782, 0x4D00
EP_OUT, EP_IN = 0x02, 0x83
ACK_TIMEOUT_MS = 150

try:
    import numpy as _np
except ImportError:
    _np = None

if _np is not None:
    def rgb24_to_rgb565(rgb):
        a = _np.frombuffer(rgb, dtype=_np.uint8).reshape(-1, 3)
        r = (a[:, 0].astype(_np.uint16) >> 3) << 11
        g = (a[:, 1].astype(_np.uint16) >> 2) << 5
        b = a[:, 2].astype(_np.uint16) >> 3
        return (r | g | b).astype("<u2").tobytes()
else:
    _R = [(r >> 3) << 11 for r in range(256)]
    _G = [(g >> 2) << 5 for g in range(256)]
    _B = [b >> 3 for b in range(256)]

    def rgb24_to_rgb565(rgb):
        out = bytearray(W * H * 2)
        j = 0
        lut_r, lut_g, lut_b = _R, _G, _B
        for i in range(0, W * H * 3, 3):
            v = lut_r[rgb[i]] | lut_g[rgb[i + 1]] | lut_b[rgb[i + 2]]
            out[j] = v & 0xFF
            out[j + 1] = v >> 8
            j += 2
        return bytes(out)


def rotate90(rgb, src_w, src_h, cw=True):
    if _np is not None:
        a = _np.frombuffer(rgb, dtype=_np.uint8).reshape(src_h, src_w, 3)
        r = _np.rot90(a, k=-1 if cw else 1)
        return r.tobytes(), src_h, src_w
    row = src_w * 3
    out = bytearray(len(rgb))
    o = 0
    if cw:
        for yo in range(src_w):
            sp = yo * 3 + (src_h - 1) * row
            for _ in range(src_h):
                out[o:o + 3] = rgb[sp:sp + 3]
                o += 3
                sp -= row
    else:
        for yo in range(src_w):
            sp = (src_w - 1 - yo) * 3
            for _ in range(src_h):
                out[o:o + 3] = rgb[sp:sp + 3]
                o += 3
                sp += row
    return bytes(out), src_h, src_w


def demo_frame(t):
    buf = bytearray(W * H * 3)
    bands = 8
    for y in range(H):
        band = (y * bands // H + int(t * 2)) % bands
        base = 255 * (band + 1) // (bands + 1)
        for x in range(W):
            i = (y * W + x) * 3
            buf[i] = base if band % 3 == 0 else 0
            buf[i + 1] = base if band % 3 == 1 else 0
            buf[i + 2] = base if band % 3 == 2 else 0
    bx = int((W - 20) * (0.5 + 0.5 * ((t * 0.7) % 2 - 1))) if (t * 0.7) % 2 < 1 \
        else int((W - 20) * (0.5 - 0.5 * (((t * 0.7) % 2) - 1)))
    by = int((H - 20) * (0.5 + 0.5 * ((t * 0.35) % 2 - 1))) if (t * 0.35) % 2 < 1 \
        else int((H - 20) * (0.5 - 0.5 * (((t * 0.35) % 2) - 1)))
    for y in range(by, by + 20):
        for x in range(bx, bx + 20):
            i = (y * W + x) * 3
            edge = x in (bx, bx + 19) or y in (by, by + 19)
            v = 0 if edge else 255
            buf[i] = buf[i + 1] = buf[i + 2] = v
    return bytes(buf)


class PhoneLink:
    def __init__(self, vid=VID, pid=PID):
        import usb.core
        import usb.util
        self.usb = usb.core
        self.util = usb.util
        self.claimed = False
        dev = usb.core.find(idVendor=vid, idProduct=pid)
        if dev is None:
            sys.exit("usbcast: no phone found (VID %04x:%04x). Is fpj2me "
                     "running? Is the cable in? (see --help)" % (vid, pid))
        self.dev = dev

        for c in dev:
            for intf in c:
                try:
                    if dev.is_kernel_driver_active(intf.bInterfaceNumber):
                        dev.detach_kernel_driver(intf.bInterfaceNumber)
                except (NotImplementedError, ValueError, usb.core.USBError):
                    pass

        try:
            cfg = dev.get_active_configuration()
        except usb.core.USBError:
            cfg = None

        if cfg is None:
            try:
                dev.set_configuration()
            except usb.core.USBError as e:
                if getattr(e, "errno", None) != 16:
                    raise
            cfg = dev.get_active_configuration()

        try:
            usb.util.claim_interface(dev, 0)
            self.claimed = True
        except usb.core.USBError as e:
            if getattr(e, "errno", None) == 16:
                sys.exit("usbcast: USB interface busy — close any other "
                         "usbcast_host.py instance and re-plug the cable.")
            raise

        intf = cfg[(0, 0)]
        self.ep_out = usb.util.find_descriptor(intf, custom_match=lambda e:
                                               e.bEndpointAddress == EP_OUT)
        self.ep_in = usb.util.find_descriptor(intf, custom_match=lambda e:
                                              e.bEndpointAddress == EP_IN)
        if not (self.ep_out and self.ep_in):
            self.close()
            sys.exit("usbcast: endpoints 0x%02x/0x%02x not found — wrong "
                     "device?" % (EP_OUT, EP_IN))

    def send_frame(self, payload):
        self.ep_out.write(HEADER + payload, ACK_TIMEOUT_MS)

    def wait_ack(self):
        try:
            data = self.ep_in.read(1, ACK_TIMEOUT_MS)
        except Exception:
            return b""
        return bytes(data[:1])

    def hello_wait(self, secs=3.0):
        t0 = time.monotonic()
        while time.monotonic() - t0 < secs:
            b = self.wait_ack()
            if b == b"Q":
                return b"Q"
            if b == b"R":
                return b"R"
            if not b:
                continue
        return b""

    def close(self):
        if self.claimed:
            try:
                self.util.release_interface(self.dev, 0)
            except Exception:
                pass
            self.claimed = False
        try:
            self.util.dispose_resources(self.dev)
        except Exception:
            pass


def fps_line(t0, n):
    if not n:
        return
    dt = time.monotonic() - t0
    if dt > 0:
        sys.stderr.write("\r usbcast: %4d frames, %5.1f fps   "
                         % (n, n / dt))
        sys.stderr.flush()


def cast_loop(link, frame_iter, args):
    n = 0
    t0 = time.monotonic()
    last_report = t0
    period = 1.0 / args.fps if args.fps else 0.0
    next_t = time.monotonic()
    try:
        for rgb in frame_iter:
            if period:
                now = time.monotonic()
                if now < next_t:
                    time.sleep(next_t - now)
                next_t += period
            if len(rgb) != W * H * 3:
                sys.stderr.write("\nusbcast: skipping short frame "
                                 "(%d bytes)\n" % len(rgb))
                continue
            link.send_frame(rgb24_to_rgb565(rgb))
            ack = link.wait_ack()
            if ack == b"Q":
                sys.stderr.write("\nusbcast: phone exited (exit key). "
                                 "Bye.\n")
                return
            if not ack:
                sys.stderr.write("\nusbcast: ACK timeout — resending\n")
                continue
            n += 1
            now = time.monotonic()
            if now - last_report > 0.5:
                fps_line(t0, n)
                last_report = now
    except KeyboardInterrupt:
        sys.stderr.write("\nusbcast: interrupted\n")
    fps_line(t0, n)
    if n:
        sys.stderr.write("\n")


def demo_iter(args):
    t0 = time.monotonic()
    period = 1.0 / args.fps if args.fps else 0.0
    while True:
        yield demo_frame(time.monotonic() - t0)
        if period:
            time.sleep(period)


def stream_iter(stream, args):
    sw, sh = (H, W) if args.rotate else (W, H)
    need = sw * sh * 3
    while True:
        buf = stream.read(need)
        while len(buf) < need:
            more = stream.read(need - len(buf))
            if not more:
                if buf:
                    sys.stderr.write("usbcast: partial frame at EOF, "
                                     "dropped\n")
                return
            buf += more
        if args.rotate:
            rot, _, _ = rotate90(buf, sw, sh, cw=(args.rotate == "cw"))
            buf = rot
        yield buf


def kde_iter(args):
    try:
        import dbus
        from dbus.mainloop.glib import DBusGMainLoop
        from gi.repository import GLib
    except ImportError as e:
        sys.exit(f"usbcast: --kde requires python-dbus and python-gobject ({e})\n"
                 "Install on CachyOS: sudo pacman -S python-dbus python-gobject gst-plugin-pipewire")

    sw, sh = (H, W) if args.rotate else (W, H)
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    portal = bus.get_object("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop")
    sc = dbus.Interface(portal, "org.freedesktop.portal.ScreenCast")
    sender = bus.get_unique_name()[1:].replace(".", "_")
    loop = GLib.MainLoop()

    state = {"session": None, "node_id": None, "fd": None, "err": None}

    def listen(token, cb):
        path = f"/org/freedesktop/portal/desktop/request/{sender}/{token}"
        def handler(code, res):
            if code != 0:
                state["err"] = f"Portal cancelled or failed (code {code})"
                loop.quit()
                return
            cb(res)
        bus.add_signal_receiver(handler, "Response", "org.freedesktop.portal.Request", path=path)

    def on_started(res):
        state["node_id"] = int(res["streams"][0][0])
        state["fd"] = sc.OpenPipeWireRemote(state["session"], dbus.Dictionary({}, signature="sv")).take()
        loop.quit()

    def on_selected(_res):
        listen("t3", on_started)
        sc.Start(state["session"], "", {"handle_token": "t3"})

    def on_created(res):
        state["session"] = res["session_handle"]
        listen("t2", on_selected)
        sc.SelectSources(state["session"], {
            "handle_token": "t2",
            "types": dbus.UInt32(3),
            "cursor_mode": dbus.UInt32(2),
        })

    listen("t1", on_created)
    sc.CreateSession({"handle_token": "t1", "session_handle_token": "s1"})
    sys.stderr.write("usbcast: select a screen, virtual output, or window in KDE...\n")
    loop.run()

    if state["err"] or state["fd"] is None:
        sys.exit(f"usbcast: {state['err'] or 'no PipeWire stream returned'}")

    fd = state["fd"]
    node_id = state["node_id"]
    gst_cmd = [
        "gst-launch-1.0", "-q",
        "pipewiresrc", f"fd={fd}", f"path={node_id}", "do-timestamp=true", "keepalive-time=1000",
        "!", "video/x-raw",
        "!", "videoconvert",
        "!", "videoscale",
        "!", f"video/x-raw,format=RGB,width={sw},height={sh}",
        "!", "fdsink", "fd=1",
    ]
    gst_proc = subprocess.Popen(gst_cmd, stdout=subprocess.PIPE, pass_fds=(fd,))
    os.close(fd)
    try:
        yield from stream_iter(gst_proc.stdout, args)
    finally:
        if gst_proc.stdout:
            gst_proc.stdout.close()
        if gst_proc.poll() is None:
            gst_proc.terminate()
            try:
                gst_proc.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                gst_proc.kill()


def selftest():
    ok = True
    assert HEADER == b"FPJ2" + bytes([128, 0, 160, 0, 0]), "header layout"
    print("selftest: header = %s (w=%d h=%d flags=0, %d-byte payload)"
          % (HEADER[:4].hex().upper(), W, H, FRAME_RGB565))
    cases = [((255, 0, 0), 0xF800), ((0, 255, 0), 0x07E0),
             ((0, 0, 255), 0x001F), ((0, 0, 0), 0x0000),
             ((255, 255, 255), 0xFFFF), ((8, 8, 8), 0x0841)]
    px = b"".join(bytes(c) for c, _ in cases)
    got = rgb24_to_rgb565(px)
    for (c, want), i in zip(cases, range(len(cases))):
        v = got[2 * i] | got[2 * i + 1] << 8
        state = "ok" if v == want else "MISMATCH"
        if v != want:
            ok = False
        print("selftest: rgb%s -> %04x (%s)" % (c, v, state))

    A, B, C, D = b"\x01\x02\x03", b"\x04\x05\x06", \
                 b"\x07\x08\x09", b"\x0a\x0b\x0c"
    grid2 = A + B + C + D
    cw2, w, h = rotate90(grid2, 2, 2, cw=True)
    ccw2, _, _ = rotate90(grid2, 2, 2, cw=False)
    if (w, h) != (2, 2) or cw2 != C + A + D + B:
        print("selftest: rotate cw 2x2 -> %s FAIL" % cw2.hex())
        ok = False
    else:
        print("selftest: rotate cw 2x2 -> [C A; D B] ok")
    if ccw2 != B + D + A + C:
        print("selftest: rotate ccw 2x2 -> %s FAIL" % ccw2.hex())
        ok = False
    else:
        print("selftest: rotate ccw 2x2 -> [B D; A C] ok")

    sw, sh = 160, 128
    src = bytes((i * 7 + 3) & 0xFF for i in range(sw * sh * 3))
    cw, w1, h1 = rotate90(src, sw, sh, cw=True)
    back, w2, h2 = rotate90(cw, w1, h1, cw=False)
    if (w2, h2) != (sw, sh):
        print("selftest: rotate shape %dx%d != %dx%d FAIL" % (w2, h2, sw, sh))
        ok = False
    elif back != src:
        diffs = sum(1 for a, b in zip(back, src) if a != b)
        print("selftest: rotate cw+ccw roundtrip: %d/%d bytes differ FAIL"
              % (diffs, len(src)))
        ok = False
    else:
        print("selftest: rotate cw+ccw roundtrip = identity (160x128) ok")

    if _np is not None:
        saved = _np
        _self = sys.modules[__name__]
        _self._np = None
        try:
            pp_cw, _, _ = _self.rotate90(grid2, 2, 2, cw=True)
            ok_pp = (pp_cw == C + A + D + B)
            rt, _, _ = _self.rotate90(src, sw, sh, cw=True)
            rb, _, _ = _self.rotate90(rt, 128, 160, cw=False)
            ok_pp = ok_pp and (rb == src)
            print("selftest: pure-python rotate parity: %s"
                  % ("ok" if ok_pp else "FAIL"))
            if not ok_pp:
                ok = False
        finally:
            _self._np = saved

    f = demo_frame(1.234)
    assert len(f) == W * H * 3
    t0 = time.perf_counter()
    for _ in range(10):
        rgb24_to_rgb565(f)
    dt = (time.perf_counter() - t0) / 10
    print("selftest: RGB24->RGB565 encode: %.2f ms/frame (%s path)"
          % (dt * 1000, "numpy" if _np is not None else "pure-python"))
    print("selftest: %s" % ("ALL OK" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(
        prog="usbcast_host.py",
        description="Send live 128x160 RGB565 frames to an fpj2me phone "
                    "(itel i2163 + UsbCast.jar) over raw-USB bulk.")
    ap.add_argument("--demo", action="store_true",
                    help="animated color-bar/moving-box/FPS test pattern")
    ap.add_argument("--stdin", action="store_true",
                    help="read raw RGB24 frames (128x160, or 160x128 with --rotate) from stdin")
    ap.add_argument("--kde", action="store_true",
                    help="open KDE Plasma Wayland ScreenCast picker and stream directly")
    ap.add_argument("--portrait", action="store_true",
                    help="with --kde, capture upright 128x160 without rotating")
    ap.add_argument("--rotate", choices=["cw", "ccw"], default=None,
                    help="rotate a 160x128 landscape stream 90 deg to 128x160")
    ap.add_argument("--fps", type=float, default=0,
                    help="cap send rate (0 = as fast as phone ACKs)")
    ap.add_argument("--device", metavar="VID:PID", default=None,
                    help="override device id (default 1782:4d00)")
    ap.add_argument("--no-hello", action="store_true",
                    help="don't wait for phone 'R' ready byte")
    ap.add_argument("--selftest", action="store_true",
                    help="run protocol/encoder self-tests (no USB)")
    args = ap.parse_args()

    if args.selftest:
        sys.exit(selftest())

    modes = sum([bool(args.demo), bool(args.stdin), bool(args.kde)])
    if modes == 0:
        ap.print_help()
        sys.exit(2)
    if modes > 1:
        sys.exit("pick only one of --demo / --stdin / --kde")

    if args.kde and not args.portrait and args.rotate is None:
        args.rotate = "cw"

    vid, pid = VID, PID
    if args.device:
        try:
            a, b = args.device.split(":")
            vid, pid = int(a, 16), int(b, 16)
        except ValueError:
            sys.exit("--device wants VID:PID hex, e.g. 1782:4d00")

    link = PhoneLink(vid, pid)
    sys.stderr.write("usbcast: phone on vid %04x:%04x, EP out 0x%02x / "
                     "in 0x%02x\n" % (vid, pid, EP_OUT, EP_IN))
    if not args.no_hello:
        sys.stderr.write("usbcast: waiting for UsbCast to boot ('R')...\n")
        hello = link.hello_wait()
        if hello == b"Q":
            sys.stderr.write("usbcast: phone is exiting; bye.\n")
            link.close()
            return
        if hello == b"R":
            sys.stderr.write("usbcast: UsbCast ready, casting\n")
        else:
            sys.stderr.write("usbcast: no 'R' (is UsbCast.jar running?) "
                             "— frames will self-sync anyway\n")

    try:
        if args.demo:
            cast_loop(link, demo_iter(args), args)
        elif args.kde:
            cast_loop(link, kde_iter(args), args)
        else:
            cast_loop(link, stream_iter(sys.stdin.buffer, args), args)
    finally:
        link.close()


if __name__ == "__main__":
    main()
