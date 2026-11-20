#!/usr/bin/env python3
"""Live USB log reader for the Oblivion Switch port (libnx usbComms, VID 057e PID 3000).

Setup (once):
    pip install pyusb libusb-package        (--deps: libusb-package ships the libusb DLL, no separate install)
    Zadig: install the WinUSB driver for the Switch device 057E 3000 (see ports/switch/DEBUGGING.md).

Usage:  python usb_log.py
Start it before or after launching the app on the Switch; it reconnects in a loop. Lines are
newline-terminated text; they are printed live and saved to out/switch-logs/usb-<timestamp>.txt
(relative to the current directory; override with --out DIR).
"""
import argparse
import datetime
import os
import sys
import time

if "--deps" in sys.argv:
    print("pip install pyusb libusb-package")
    sys.exit(0)

try:
    import usb.core
    import usb.util
    import usb.backend.libusb1 as libusb1
    try:
        import libusb_package
        BACKEND = libusb1.get_backend(find_library=libusb_package.find_library)
    except ImportError:
        BACKEND = libusb1.get_backend()
except ImportError:
    print("missing deps: pip install pyusb libusb-package")
    sys.exit(1)

VID, PID = 0x057E, 0x3000


def find_device():
    return usb.core.find(idVendor=VID, idProduct=PID, backend=BACKEND)


def bulk_in_endpoint(dev):
    try:
        dev.set_configuration()
    except usb.core.USBError:
        pass  # already configured
    cfg = dev.get_active_configuration()
    for intf in cfg:
        for ep in intf:
            if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_IN and \
                    usb.util.endpoint_type(ep.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK:
                return intf, ep
    return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join("out", "switch-logs"))
    ap.add_argument("--deps", action="store_true", help="print the pip command and exit")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, "usb-%s.txt" % datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    print("saving to", path)
    waiting_msg = False
    with open(path, "a", encoding="utf-8", errors="replace", buffering=1) as out:
        pending = b""
        while True:
            dev = None
            try:
                dev = find_device()
                if dev is None:
                    if not waiting_msg:
                        print("waiting for Switch (USB 057e:3000)...")
                        waiting_msg = True
                    time.sleep(1)
                    continue
                waiting_msg = False
                intf, ep = bulk_in_endpoint(dev)
                if ep is None:
                    print("no bulk IN endpoint found; is the WinUSB driver installed (Zadig)?")
                    time.sleep(2)
                    continue
                print("connected")
                while True:
                    try:
                        data = bytes(dev.read(ep.bEndpointAddress, 4096, timeout=1000))
                    except usb.core.USBTimeoutError:
                        continue
                    pending += data
                    while b"\n" in pending:
                        line, pending = pending.split(b"\n", 1)
                        text = line.decode("utf-8", errors="replace").rstrip("\r")
                        print(text, flush=True)
                        out.write(text + "\n")
            except usb.core.USBError as e:
                print("usb error: %s (reconnecting)" % e)
                time.sleep(1)
            except NotImplementedError as e:
                print("libusb problem: %s (is the Zadig WinUSB driver installed?)" % e)
                time.sleep(2)
            finally:
                if dev is not None:
                    try:
                        usb.util.dispose_resources(dev)
                    except Exception:
                        pass
            pending = b""


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
