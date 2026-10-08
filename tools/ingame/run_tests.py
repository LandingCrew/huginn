"""Unattended in-game run of Huginn's Debug test suites (roadmap R1).

Launches the game through Mod Organizer 2's command line, lets Huginn run its
main-menu suites, auto-load a save and run the after-load suites, then waits
for the sentinel line Huginn logs before it ends the game:

    [HuginnTest] DONE result=PASS suites=19 passed=19 failed=0 skipped=0 fail_lines=0 failed_suites=- skipped_suites=- reason=-

Exit code: 0 on PASS (and no skipped suite, unless --allow-skips); 1 on a
failed suite, a skipped suite, or a run that never reached the sentinel
(timeout, crash, unread flag); 2 when it refused to launch.

How Huginn is told to test (src/TestHarness.h): a one-shot file
Huginn_TestMode.ini in the SKSE log folder, written here and deleted by Huginn
when it reads it (and here again, in any case, when the run ends). It carries
an expiry, so a file left behind by a killed runner cannot turn a later
ordinary launch into a test. An environment variable would not do: when MO2 is
already running, the shortcut is handed to that process and runs with its
environment, not ours.

Pre-flight (refuses, exit 2, before writing or launching anything):
- The list's deployed overwrite/SKSE/Plugins/Huginn.dll must carry the test
  harness (Debug, 0.23.9 or later): it is searched for the harness's strings,
  which a Release build or an older Debug build does not contain. Its MD5 is
  printed. Without this a run would only time out.
- SkyrimSE.exe must not be running.
- No ModOrganizer.exe may be running: the shortcut would be handed to it, and
  an MO2 of the same instance open on another profile would launch THAT
  profile. --multiple passes MO2's own (unsupported) --multiple, and is still
  refused when a running MO2 belongs to this instance or its path is unknown.
- The MO2 executable title, the profile, and the save must exist.

Whatever starts the game closes it. When the run ends, whatever the verdict:
- the game: any SkyrimSE.exe still running 15 s later is killed. None was
  running when the runner launched (it refuses otherwise), so any that runs
  now is the one its launch started. Huginn ends the game itself after DONE, so
  after a DONE there is normally nothing to kill; the kill matters on a
  timeout, a hang, or an unread flag;
- MO2: the ModOrganizer.exe the runner started, if still open 30 s after the
  game is gone, is asked to close (taskkill without /F), and after 30 s more
  is ended with its children (/F /T). The runner starts MO2 only when no MO2
  is running, so this is never the user's own MO2.

Reads MO2's ModOrganizer.ini and profile settings; never writes MO2 config.
Does not deploy the DLL: put the Debug Huginn.dll in the list first.

Usage:
    python -I tools/ingame/run_tests.py [--list simonrim|lorerim]
        [--save NAME | --save latest | --no-save] [--timeout 600]
        [--allow-skips] [--multiple] [--dry-run]

A dedicated save is best: in game, open the console and type `save HuginnTest`
(it lands in the profile's saves folder as HuginnTest.ess). Without --save the
runner uses HuginnTest if it exists, else the newest save in the profile.
"""

from __future__ import annotations

import argparse
import configparser
import csv
import datetime as dt
import hashlib
import io
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

DEFAULT_LOG_DIR = Path.home() / "Documents" / "My Games" / "Skyrim.INI" / "SKSE"
LOG_NAME = "_Huginn_Debug.log"   # the Debug build's log; only Debug has the suites
FLAG_NAME = "Huginn_TestMode.ini"
GAME_EXE = "SkyrimSE.exe"
MO2_EXE = "ModOrganizer.exe"
DEFAULT_SAVE = "HuginnTest"
# Strings only a DLL with the test harness contains (src/TestHarness.cpp).
HARNESS_MARKERS = (b"Huginn_TestMode.ini", b"[HuginnTest] DONE")
KILL_GRACE_SEC = 15
MO2_GRACE_SEC = 30


@dataclass(frozen=True)
class ModList:
    root: Path
    profile: str
    executable: str   # the MO2 custom-executable title


LISTS = {
    # simonrim first: it reaches the main menu fastest.
    "simonrim": ModList(Path("F:/Modlists/simonrim-essentails"), "Simonrim Essentials", "SKSE"),
    "lorerim": ModList(Path("F:/Modlists/LoreRim-5"), "Ultra", "LoreRim"),
}

RE_LAUNCH = re.compile(r"Launch (\d{8}-\d{6}) \(UTC\)")
RE_DONE = re.compile(r"\[HuginnTest\] DONE result=(\w+) (.*)$")
RE_RESULT = re.compile(r"\[HuginnTest\] RESULT phase=(\w+) (.*)$")
RE_SUITE = re.compile(r"\[HuginnTest\] suite (\S+) (\S+) \((\d+) error line\(s\)(?:; skipped: (.*))?\)")
RE_FIELDS = re.compile(r"(\w+)=(\S+)")


def say(msg: str) -> None:
    print(f"[run_tests {dt.datetime.now():%H:%M:%S}] {msg}", flush=True)


class Refused(Exception):
    """Nothing was launched (exit code 2)."""


# --- processes ---------------------------------------------------------------

def pids_of(image: str) -> list[int]:
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {image}", "/NH", "/FO", "CSV"],
                         capture_output=True, text=True, check=False).stdout
    pids = []
    for row in csv.reader(io.StringIO(out)):
        if len(row) >= 2 and row[0].lower() == image.lower():
            pids.append(int(row[1]))
    return pids


def game_pids() -> list[int]:
    return pids_of(GAME_EXE)


def mo2_processes() -> list[tuple[int, Path | None]]:
    """Every running ModOrganizer.exe with its path; None when the path cannot
    be read (an elevated process hides it). tasklist is the source of truth for
    which processes exist, so a process CIM leaves out is still listed."""
    cmd = ("Get-CimInstance Win32_Process -Filter \"Name='ModOrganizer.exe'\" | "
           "ForEach-Object { \"$($_.ProcessId)|$($_.ExecutablePath)\" }")
    out = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", cmd],
                         capture_output=True, text=True, check=False).stdout
    paths: dict[int, Path | None] = {}
    for line in out.splitlines():
        pid, _, path = line.strip().partition("|")
        if pid.isdigit():
            paths[int(pid)] = Path(path) if path.strip() else None
    return [(pid, paths.get(pid)) for pid in pids_of(MO2_EXE)]


def kill(pid: int) -> None:
    subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True, check=False)


def close_mo2(proc: subprocess.Popen) -> None:
    """Close the MO2 this run started: politely, then by force."""
    try:
        proc.wait(timeout=MO2_GRACE_SEC)
        say(f"MO2 (pid {proc.pid}) closed by itself")
        return
    except subprocess.TimeoutExpired:
        pass
    say(f"closing the MO2 this run started (pid {proc.pid})")
    subprocess.run(["taskkill", "/PID", str(proc.pid)], capture_output=True, check=False)
    try:
        proc.wait(timeout=MO2_GRACE_SEC)
        say("MO2 closed")
        return
    except subprocess.TimeoutExpired:
        pass
    say(f"MO2 (pid {proc.pid}) did not close; ending it and its children")
    subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)], capture_output=True, check=False)
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        say(f"MO2 (pid {proc.pid}) is STILL running; close it by hand")


# --- pre-flight (read only) --------------------------------------------------

def read_ini(path: Path) -> configparser.RawConfigParser:
    ini = configparser.RawConfigParser(strict=False, interpolation=None)
    ini.optionxform = str
    ini.read(path, encoding="utf-8-sig")
    return ini


def check_executable(ml: ModList) -> None:
    ini = read_ini(ml.root / "ModOrganizer.ini")
    titles = []
    if ini.has_section("customExecutables"):
        for key, val in ini.items("customExecutables"):
            if key.endswith("\\title"):
                titles.append(val)
    if ml.executable not in titles:
        raise Refused(f"no MO2 executable titled '{ml.executable}' in "
                      f"{ml.root / 'ModOrganizer.ini'} (have: {titles})")


def check_deployed_dll(ml: ModList) -> None:
    """The DLL the list will load carries the test harness."""
    dll = ml.root / "overwrite" / "SKSE" / "Plugins" / "Huginn.dll"
    if not dll.is_file():
        raise Refused(f"no {dll}: deploy the Debug Huginn.dll there first "
                      "(this runner checks only the list's overwrite folder)")
    data = dll.read_bytes()
    md5 = hashlib.md5(data).hexdigest()
    mtime = dt.datetime.fromtimestamp(dll.stat().st_mtime)
    say(f"deployed DLL: {dll} md5={md5} size={len(data)} modified={mtime:%Y-%m-%d %H:%M:%S}")
    missing = [m.decode() for m in HARNESS_MARKERS if m not in data]
    if missing:
        raise Refused(f"the deployed Huginn.dll has no test harness (missing {missing}): it is a "
                      "Release build or older than 0.23.9, and a run would only time out. "
                      "Deploy a Debug build of 0.23.9 or later")


def check_mo2(ml: ModList, multiple: bool) -> list[str]:
    """MO2 arguments to add, or Refused."""
    running = mo2_processes()
    if not running:
        return []
    shown = [f"pid {pid}: {path if path else '(path unknown)'}" for pid, path in running]
    if not multiple:
        raise Refused(f"MO2 is running ({shown}). The shortcut would go to it, and an MO2 of this "
                      "instance on another profile would launch that profile. Close it, or pass "
                      "--multiple if it is another instance")
    own = (ml.root / MO2_EXE).resolve()
    risky = [s for (pid, path), s in zip(running, shown) if path is None or path.resolve() == own]
    if risky:
        raise Refused(f"--multiple refused: {risky} is this instance's MO2 or cannot be identified; "
                      "MO2 must never run twice on one instance")
    return ["--multiple"]


def profile_saves_dir(ml: ModList) -> Path | None:
    """The profile's own saves folder when it uses local saves, else None."""
    prof = ml.root / "profiles" / ml.profile
    if not prof.is_dir():
        raise Refused(f"no MO2 profile folder {prof}")
    settings = read_ini(prof / "settings.ini")
    local = settings.get("General", "LocalSaves", fallback="false").strip().lower() == "true"
    return prof / "saves" if local else None


def resolve_save(ml: ModList, wanted: str | None) -> str | None:
    saves = profile_saves_dir(ml)
    if wanted == "":
        return None
    if saves is None:
        if wanted in (None, "latest"):
            raise Refused("the profile does not use local saves; name the save with --save")
        say(f"cannot check that save '{wanted}' exists (profile has no local saves); trying it anyway")
        return wanted
    if wanted is None:
        if (saves / f"{DEFAULT_SAVE}.ess").is_file():
            return DEFAULT_SAVE
        say(f"no {DEFAULT_SAVE}.ess in {saves}; using the newest save")
        wanted = "latest"
    if wanted == "latest":
        found = sorted(saves.glob("*.ess"), key=lambda p: p.stat().st_mtime)
        if not found:
            raise Refused(f"no .ess saves in {saves}")
        return found[-1].stem
    name = wanted[:-4] if wanted.lower().endswith(".ess") else wanted
    if not (saves / f"{name}.ess").is_file():
        raise Refused(f"no save {name}.ess in {saves}")
    return name


# --- the log -----------------------------------------------------------------

def read_log(path: Path) -> list[str]:
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []


def log_is_fresh(path: Path, lines: list[str], since_utc: dt.datetime) -> bool:
    """The log was started by THIS launch: its first line's launch stamp (UTC,
    to the second) is no earlier than our launch, and so is its mtime."""
    if not lines:
        return False
    m = RE_LAUNCH.search(lines[0])
    if not m:
        return False
    stamp = dt.datetime.strptime(m.group(1), "%Y%m%d-%H%M%S").replace(tzinfo=dt.timezone.utc)
    floor = since_utc - dt.timedelta(seconds=2)
    try:
        mtime = dt.datetime.fromtimestamp(path.stat().st_mtime, dt.timezone.utc)
    except OSError:
        return False
    return stamp >= floor and mtime >= floor


def fields(text: str) -> dict[str, str]:
    return dict(RE_FIELDS.findall(text))


def done_verdict(lines: list[str], allow_skips: bool) -> tuple[int, str] | None:
    for line in lines:
        m = RE_DONE.search(line)
        if m:
            f = fields(m.group(2))
            ok = m.group(1) == "PASS"
            skipped = int(f.get("skipped", "0"))
            if ok and skipped and not allow_skips:
                return (1, f"{skipped} suite(s) skipped: {f.get('skipped_suites', '?')} "
                           "(pass --allow-skips to accept)")
            return (0 if ok else 1, line.split("]: ", 1)[-1])
    return None


# --- main --------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", choices=sorted(LISTS), default="simonrim")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--save", help="save to load after the main menu (no .ess); 'latest' = newest in the profile")
    g.add_argument("--no-save", action="store_true", help="main-menu suites only")
    ap.add_argument("--timeout", type=int, default=600, help="seconds from launch to the sentinel (default 600)")
    ap.add_argument("--load-timeout", type=int, default=300, help="Huginn's own wait for the save to load")
    ap.add_argument("--log-dir", type=Path, default=DEFAULT_LOG_DIR)
    ap.add_argument("--allow-skips", action="store_true", help="a skipped suite does not fail the run")
    ap.add_argument("--multiple", action="store_true",
                    help="launch even when ANOTHER instance's MO2 is running (MO2's unsupported --multiple)")
    ap.add_argument("--dry-run", action="store_true", help="check everything, print the command, launch nothing")
    args = ap.parse_args()

    ml = LISTS[args.list]
    mo2 = ml.root / MO2_EXE
    if not mo2.is_file():
        raise Refused(f"{mo2} not found")
    check_executable(ml)
    check_deployed_dll(ml)
    save = resolve_save(ml, "" if args.no_save else args.save)

    running = game_pids()
    if running:
        raise Refused(f"{GAME_EXE} is already running (pid {running}); not launching, not killing")
    mo2_args = check_mo2(ml, args.multiple)
    cmd = [str(mo2), *mo2_args, "-p", ml.profile, f"moshortcut://:{ml.executable}"]

    log_path = args.log_dir / LOG_NAME
    flag_path = args.log_dir / FLAG_NAME
    say(f"list={args.list} save={save or '(none: main-menu suites only)'} timeout={args.timeout}s")
    say(f"log: {log_path}")
    say("command: " + subprocess.list2cmdline(cmd))
    if args.dry_run:
        return 0

    launched_utc = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
    expires = int(time.time()) + args.timeout + 120
    flag_path.write_text(
        "; written by tools/ingame/run_tests.py; Huginn deletes it when it reads it\n"
        "[Test]\n"
        "bEnabled=1\n"
        f"sSaveName={save or ''}\n"
        f"iExpiresUnix={expires}\n"
        f"iLoadTimeoutSec={args.load_timeout}\n",
        encoding="utf-8")

    deadline = time.monotonic() + args.timeout
    game_pid: int | None = None
    verdict: tuple[int, str] | None = None
    lines: list[str] = []
    fresh = False
    mo2_proc = None
    try:
        mo2_proc = subprocess.Popen(cmd, cwd=str(ml.root))
        say(f"launched MO2 (pid {mo2_proc.pid}); waiting for the game and the log")
        build_seen = False
        while time.monotonic() < deadline:
            time.sleep(2)
            pids = game_pids()
            if game_pid is None and pids:
                game_pid = pids[0]
                say(f"{GAME_EXE} started (pid {game_pid})")
            lines = read_log(log_path)
            if not fresh:
                fresh = log_is_fresh(log_path, lines, launched_utc)
                if fresh:
                    say("Huginn log started for this launch: " + lines[0].split("]: ", 1)[-1])
            if fresh and not build_seen:
                ver = next((l.split("]: ", 1)[-1] for l in lines[:10] if " Loading" in l), None)
                if ver:
                    build_seen = True
                    say("build: " + ver)
            if fresh:
                verdict = done_verdict(lines, args.allow_skips)
                if verdict:
                    break
                if any("[HuginnTest] RESULT phase=menu" in l for l in lines) and \
                        not any("[HuginnTest] test mode ON" in l for l in lines):
                    verdict = (1, "Huginn ran its suites without test mode: the flag file was not read")
                    break
            if game_pid is not None and game_pid not in pids:
                # The game is gone. Only a DONE line in THIS launch's log counts.
                time.sleep(2)
                lines = read_log(log_path)
                fresh = fresh or log_is_fresh(log_path, lines, launched_utc)
                if fresh and done_verdict(lines, args.allow_skips):
                    continue   # parsed on the next pass
                verdict = (1, "the game exited before the sentinel (crash?)"
                              + ("" if fresh else "; this launch never started a Huginn log"))
                break
        else:
            verdict = (1, f"timeout after {args.timeout}s" + ("" if fresh else " (the log never started)"))
    finally:
        if mo2_proc is not None and game_pids():
            # None ran before the launch (checked), so these are the launch's.
            end = time.monotonic() + KILL_GRACE_SEC
            while time.monotonic() < end and game_pids():
                time.sleep(1)
            for pid in game_pids():
                say(f"killing {GAME_EXE} pid {pid} (still running at the end of the run)")
                kill(pid)
        try:
            flag_path.unlink()
            say("removed the test-mode file (Huginn had not consumed it)")
        except FileNotFoundError:
            pass
        if mo2_proc is not None:
            close_mo2(mo2_proc)

    # Report: only from this launch's log.
    if fresh:
        for line in lines:
            m = RE_SUITE.search(line)
            if m:
                extra = f"; skipped: {m.group(4)}" if m.group(4) else ""
                print(f"  {m.group(2):8} {m.group(1)}  ({m.group(3)} error lines{extra})")
        for line in lines:
            m = RE_RESULT.search(line)
            if m:
                print(f"  RESULT {m.group(1)}: {m.group(2)}")
    code, why = verdict or (1, "no verdict")
    say(("PASS: " if code == 0 else "FAIL: ") + why)
    return code


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Refused as e:
        say(f"refused: {e}")
        sys.exit(2)
