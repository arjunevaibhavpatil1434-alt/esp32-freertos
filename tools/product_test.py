#!/usr/bin/env python3
"""Final product test for the ESP32 headset (hfp_mic_test).

Run from an ESP-IDF shell (so `idf.py` and pyserial are available):

    . ~/esp/esp-idf/export.sh
    python tools/product_test.py                 # automatic, no person needed
    python tools/product_test.py --interactive   # + phone, music, touch, call

Stages:
  1. build       hfp_mic_test and pipeline_check build with no warnings in main/
  2. hardware    pipeline_check: DHT11, OLED, mic signal, amp pins toggling
                 (+ "did you hear the beep?" when interactive)
  3. boot        hfp_mic_test: one clean boot, display, touch calibrated,
                 all Bluetooth profiles up, first DHT11 reading
  4. phone       (interactive only) connect, music + title, touch pause/play,
                 incoming call, touch answer, audio both ways, touch hang up

The board is left running hfp_mic_test. A Markdown report and the raw
serial log of each stage are written to test_reports/ (or next to
--report). Exit code 0 only if nothing failed.
"""

import argparse
import datetime
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: run this from an ESP-IDF shell (. export.sh)")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRODUCT = os.path.join(REPO, "hfp_mic_test")
HWCHECK = os.path.join(REPO, "pipeline_check")

PASS, FAIL, SKIP, INFO = "PASS", "FAIL", "SKIP", "INFO"

LOG_BASE = None     # report path without .md; each stage's serial log is saved next to it


def redact(text):
    """Reports go into a public repo: mask Bluetooth addresses and phone
    numbers. The raw .log files keep them and are git-ignored."""
    text = re.sub(r"\b([0-9a-f]{2}):[0-9a-f]{2}(:[0-9a-f]{2}){4}\b", r"\1:xx:xx:xx:xx:xx", text, flags=re.I)
    return re.sub(r"\+?\d{6,}(\d{2})\b", r"****\1", text)


def save_log(con, stage):
    path = f"{LOG_BASE}_{stage}.log"
    with open(path, "w") as f:
        f.write("\n".join(con.lines) + "\n")
    return os.path.relpath(path, REPO)


class Report:
    def __init__(self):
        self.rows = []  # (stage, check, result, detail)

    def add(self, stage, check, result, detail=""):
        self.rows.append((stage, check, result, detail))
        mark = {PASS: "\033[32mPASS\033[0m", FAIL: "\033[31mFAIL\033[0m"}.get(result, result)
        print(f"  [{mark}] {check}" + (f"  ({detail})" if detail else ""))

    @property
    def failed(self):
        return [r for r in self.rows if r[2] == FAIL]

    def write(self, path, args, started):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        rev = subprocess.run(["git", "-C", REPO, "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", REPO, "status", "--porcelain", "--untracked-files=no"],
                               capture_output=True, text=True).stdout.strip()
        with open(path, "w") as f:
            f.write("# Product test report\n\n")
            f.write("Bluetooth addresses and phone numbers are masked; the .log files beside\n"
                    "this report (kept locally, not in git) have the full serial output.\n\n")
            f.write(f"- Date: {started:%Y-%m-%d %H:%M}\n")
            f.write(f"- Firmware: `{rev}`{' (with uncommitted changes)' if dirty else ''}\n")
            f.write(f"- Mode: {'interactive' if args.interactive else 'automatic'}\n")
            f.write(f"- Port: `{args.port}`\n")
            f.write(f"- Result: **{'FAIL' if self.failed else 'PASS'}** "
                    f"({sum(r[2] == PASS for r in self.rows)} passed, {len(self.failed)} failed, "
                    f"{sum(r[2] == SKIP for r in self.rows)} skipped)\n\n")
            f.write("| Stage | Check | Result | Detail |\n| --- | --- | --- | --- |\n")
            for stage, check, result, detail in self.rows:
                detail = redact(detail).replace("|", "\\|")
                f.write(f"| {stage} | {check} | {result} | {detail} |\n")
            logs = sorted(p for p in os.listdir(os.path.dirname(path))
                          if p.startswith(os.path.basename(LOG_BASE)) and p.endswith(".log"))
            if logs:
                f.write("\nSerial logs: " + ", ".join(f"[{l}]({l})" for l in logs) + "\n")
        print(f"\nReport written to {os.path.relpath(path, REPO)}")


# ---------------------------------------------------------------- build/flash

def idf(project, *args, log=None):
    cmd = ["idf.py", *args]
    res = subprocess.run(cmd, cwd=project, capture_output=True, text=True)
    if log:
        with open(log, "w") as f:
            f.write(res.stdout + res.stderr)
    return res


def build(project, report, stage):
    name = os.path.basename(project)
    res = idf(project, "build")
    out = res.stdout + res.stderr
    ok = res.returncode == 0 and "Project build complete" in out
    warnings = [l for l in out.splitlines() if "warning:" in l and "/main/" in l]
    size = re.search(rf"{name}\.bin binary size (0x[0-9a-f]+) bytes\..*?\((\d+)%\) free", out)
    detail = f"app {int(size.group(1), 16) // 1024} KB, {size.group(2)}% of partition free" if size else ""
    report.add(stage, f"{name} builds", PASS if ok else FAIL, detail if ok else out.strip()[-200:])
    if ok:
        report.add(stage, f"{name}: no warnings in main/", PASS if not warnings else FAIL,
                   f"{len(warnings)} warning(s)" if warnings else "")
    return ok


def flash(project, port, report, stage, tries=3):
    name = os.path.basename(project)
    for i in range(1, tries + 1):
        # 115200 is the rate that survives this board's flaky USB link
        res = idf(project, "-p", port, "-b", "115200", "flash")
        if res.returncode == 0:
            report.add(stage, f"flash {name}", PASS, f"try {i}")
            return True
        time.sleep(2)
    err = [l for l in (res.stdout + res.stderr).splitlines() if "fatal" in l.lower()]
    report.add(stage, f"flash {name}", FAIL, err[0] if err else "see idf.py output")
    return False


# ---------------------------------------------------------------- serial

class Console:
    """Reads the board's log in a thread; lets tests wait for patterns."""

    def __init__(self, port):
        self.ser = serial.Serial()
        self.ser.port, self.ser.baudrate, self.ser.timeout = port, 115200, 0.2
        self.lines = []
        self.q = queue.Queue()
        self.stop = False

    def open_and_reset(self):
        self.ser.dtr = False
        self.ser.rts = True        # EN low: hold in reset
        self.ser.open()
        time.sleep(0.1)
        self.ser.rts = False       # release: fresh boot, logged from the start
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        buf = b""
        while not self.stop:
            try:
                buf += self.ser.read(1024)
            except serial.SerialException:
                self.q.put(None)
                return
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode(errors="replace").strip("\r")
                self.lines.append(line)
                self.q.put(line)

    def wait_for(self, pattern, timeout):
        """First new line matching `pattern` within `timeout` s, else None."""
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            try:
                line = self.q.get(timeout=0.5)
            except queue.Empty:
                continue
            if line is None:
                return None
            if rx.search(line):
                return line
        return None

    def collect(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            try:
                self.q.get(timeout=0.5)
            except queue.Empty:
                pass

    def drain(self):
        while not self.q.empty():
            self.q.get_nowait()

    def close(self):
        self.stop = True
        time.sleep(0.3)
        self.ser.close()

    def count(self, pattern):
        rx = re.compile(pattern)
        return sum(1 for l in self.lines if rx.search(l))

    def last(self, pattern):
        rx = re.compile(pattern)
        for l in reversed(self.lines):
            if rx.search(l):
                return l
        return None


def ask(question):
    while True:
        a = input(f"  ? {question} [y/n] ").strip().lower()
        if a in ("y", "n"):
            return a == "y"


def step(text):
    print(f"\n  >>> {text}")


# ---------------------------------------------------------------- stages

def stage_hardware(args, report):
    stage = "hardware"
    print("\n== Stage 2: hardware check (pipeline_check) ==")
    if not flash(HWCHECK, args.port, report, stage):
        return
    con = Console(args.port)
    con.open_and_reset()
    con.collect(25)
    con.close()
    save_log(con, "hardware")

    pins = [l for l in con.lines if "[SPK pin]" in l]
    for name in ("BCLK", "LRC", "DIN"):
        l = next((p for p in pins if f"] {name}" in p), None)
        ok = l is not None and "toggling OK" in l
        report.add(stage, f"amp {name} pin toggling", PASS if ok else FAIL,
                   l.split("->")[-1].strip() if l else "no reading")
    dht = con.count(r"DHT OK T=")
    report.add(stage, "DHT11 reads", PASS if dht >= 3 else FAIL, f"{dht} good report lines")
    report.add(stage, "OLED at 0x3C", PASS if con.count(r"\[OLED\] init OK") else FAIL)
    sig = con.count(r"MIC SIGNAL")
    report.add(stage, "mic signal", PASS if sig >= 3 else FAIL, f"{sig} report lines with signal")
    loop = con.last(r"\[LOOPBACK\]")
    report.add(stage, "speaker -> mic loopback", INFO,
               loop.split("->")[-1].strip() if loop else "no result (needs mic near speaker)")
    if args.interactive:
        report.add(stage, "1 kHz beep heard (1 s on / 1 s off)",
                   PASS if ask("Did you hear a beep, 1 s on and 1 s off?") else FAIL)
    else:
        report.add(stage, "1 kHz beep heard", SKIP, "needs a listener: run with --interactive")


def stage_boot(args, report):
    stage = "boot"
    print("\n== Stage 3: product boot (hfp_mic_test) ==")
    if not flash(PRODUCT, args.port, report, stage):
        return None
    con = Console(args.port)
    con.open_and_reset()
    con.collect(15)

    boots = con.count(r"main_task: Calling app_main")
    report.add(stage, "single clean boot", PASS if boots == 1 else FAIL, f"{boots} boot(s) in 15 s")
    crash = con.last(r"Guru Meditation|abort\(\)|Backtrace:|rst:0x[^1]")
    report.add(stage, "no crash / reset", PASS if not crash else FAIL, crash or "")
    for pat, check in [
        (r"HF PROF STATE: Init Complete", "HFP (calls) up"),
        (r"A2DP PROF STATE: Init Complete", "A2DP (music) up"),
        (r"AVRCP CT PROF STATE: Init Complete", "AVRCP (titles/control) up"),
        (r"OLED: status display on", "display on"),
        (r"TOUCH: pad on GPIO13 ready", "touch pad calibrated"),
        (r"DHT11: first reading", "temperature sensor reading"),
    ]:
        l = con.last(pat)
        detail = ""
        if l and "TOUCH" in l:
            detail = l.split("ready,")[-1].strip()
        elif l and "first reading" in l:
            detail = l.split("first reading")[-1].strip()
        report.add(stage, check, PASS if l else FAIL, detail)
    tgt = con.last(r"Reconnect target|No paired phone")
    report.add(stage, "reconnect logic runs", PASS if tgt else FAIL,
               tgt.split("BT_HF: ")[-1] if tgt else "")
    return con


def stage_phone(args, report, con):
    stage = "phone"
    print("\n== Stage 4: phone, music, touch, call (interactive) ==")

    def expect(check, pattern, timeout, instruction=None):
        if instruction:
            step(instruction)
        con.drain()
        l = con.wait_for(pattern, timeout)
        report.add(stage, check, PASS if l else FAIL, "" if l else f"not seen within {timeout} s")
        return l is not None

    if con.last(r"slc_connected"):
        report.add(stage, "phone connected (automatically)", PASS)
    elif not expect("phone connected", r"connection state slc_connected", 180,
                    "Connect the phone: Bluetooth settings -> tap ESP_HFP_HF (waiting up to 3 min)"):
        report.add(stage, "remaining phone tests", SKIP, "no phone connected")
        return

    if expect("music streams to the board", r"A2DP audio state: Started", 120,
              "Play a song on the phone (waiting up to 2 min)"):
        # the phone may send the title just before the stream starts
        title = con.last(r"AVRCP Title: (?!unknow)") or con.wait_for(r"AVRCP Title: (?!unknow)", 10)
        report.add(stage, "song title received", PASS if title else FAIL,
                   title.split("Title:")[-1].strip() if title else "")
        report.add(stage, "music heard from the speaker",
                   PASS if ask("Do you hear the song from the board's speaker?") else FAIL)
        report.add(stage, "title shown on the display",
                   PASS if ask("Does the display show MUSIC and the song title?") else FAIL)
        expect("touch tap pauses music", r"TOUCH: tap -> pause music", 60,
               "TAP the touch pad once (touch and lift within 1 s)")
        expect("touch tap plays music", r"TOUCH: tap -> play music", 60,
               "TAP the touch pad again to resume")

    step("Pause the music, then CALL this phone from another phone (waiting up to 3 min)")
    con.drain()
    ring = con.wait_for(r"Call setup indicator INCOMING", 180)
    report.add(stage, "incoming call seen", PASS if ring else FAIL)
    if not ring:
        report.add(stage, "call tests", SKIP, "no incoming call")
        return
    clip = con.wait_for(r"clip number|caller (found|not) in contacts", 8)
    report.add(stage, "caller number received", PASS if clip else FAIL)
    name = con.last(r"caller (found|not) in contacts")
    report.add(stage, "caller name lookup", INFO, name.split("BT_HF: ")[-1] if name else "no result")
    report.add(stage, "display shows INCOMING CALL and the caller",
               PASS if ask("Does the display show INCOMING CALL and the number or name?") else FAIL)

    if expect("touch tap answers", r"TOUCH: tap -> answer call", 30,
              "TAP the touch pad to ANSWER"):
        audio = con.wait_for(r"audio state connected", 10)
        report.add(stage, "call audio link up", PASS if audio else FAIL,
                   audio.split("audio state")[-1].strip() if audio else "")
        report.add(stage, "caller heard on the speaker",
                   PASS if ask("Can you hear the caller from the board's speaker?") else FAIL)
        report.add(stage, "caller hears the board's mic",
                   PASS if ask("Can the caller hear you through the board's mic?") else FAIL)
        expect("touch tap hangs up", r"TOUCH: tap -> hang up", 60,
               "TAP the touch pad to HANG UP")
        ended = con.wait_for(r"Call indicator NO call in progress", 10)
        report.add(stage, "call ended", PASS if ended else FAIL)

    step("CALL this phone once more; this time LONG-PRESS the pad (1 s) to reject (waiting up to 3 min)")
    con.drain()
    if con.wait_for(r"Call setup indicator INCOMING", 180):
        expect("touch long press rejects", r"TOUCH: long press -> reject call", 30)
    else:
        report.add(stage, "touch long press rejects", SKIP, "no second call")


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--interactive", action="store_true", help="also run the phone/listening tests")
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--skip-hardware", action="store_true", help="skip the pipeline_check stage")
    ap.add_argument("--report", help="report path (default test_reports/product_test_<date>.md)")
    args = ap.parse_args()

    if not shutil.which("idf.py"):
        sys.exit("idf.py not found: run `. <esp-idf>/export.sh` first")
    if not os.path.exists(args.port):
        sys.exit(f"{args.port} not found: is the board plugged in?")

    started = datetime.datetime.now()
    report = Report()
    global LOG_BASE
    path = args.report or os.path.join(REPO, "test_reports", f"product_test_{started:%Y%m%d_%H%M}.md")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    LOG_BASE = os.path.splitext(path)[0]
    print(f"Product test, {started:%Y-%m-%d %H:%M}, port {args.port}")

    if not args.skip_build:
        print("\n== Stage 1: build ==")
        ok = build(PRODUCT, report, "build") & build(HWCHECK, report, "build")
        if not ok:
            args.skip_hardware = True
    if not args.skip_hardware:
        stage_hardware(args, report)
    con = stage_boot(args, report)
    if con:
        if args.interactive:
            stage_phone(args, report, con)
        else:
            report.add("phone", "phone, music, touch, call", SKIP, "run with --interactive")
        con.close()
        save_log(con, "product")

    report.write(path, args, started)
    print(f"RESULT: {'FAIL' if report.failed else 'PASS'} "
          f"({len(report.failed)} failed)")
    for stage, check, _, detail in report.failed:
        print(f"  - {stage}: {check}" + (f" ({detail})" if detail else ""))
    sys.exit(1 if report.failed else 0)


if __name__ == "__main__":
    main()
