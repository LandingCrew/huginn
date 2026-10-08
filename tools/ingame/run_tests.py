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

Which game is "ours": a process whose image is named SkyrimSE.exe and lies
under the chosen instance's folder, created after the launch (5 s of slack
for a clock step; the path and name carry the rest). The runner opens a handle to it the
first time it sees it and keeps it for the whole run, so the PID cannot be
reused under it, and it ends that process through the handle. A game from
another instance, or one that was already running, is never tracked or
killed.

Whatever starts the game closes it. When the run ends, whatever the verdict:
- the game: every game of ours still running 15 s later is ended (through its
  handle). Huginn ends the game itself after DONE, so after a DONE there is
  normally nothing to end; this matters on a timeout, a hang, or an unread
  flag;
- MO2: the ModOrganizer.exe the runner started, if still open 30 s after the
  game is gone, is asked to close (taskkill without /F), and after 30 s more
  is ended with its children (/F /T). The runner starts MO2 only when no MO2
  of this instance is running, so this is never the user's own MO2;
- late games: MO2 may still be starting the game when the run ends (on a
  timeout, or --timeout 0). Such a game's parent (skse64_loader) has already
  exited, so MO2's /T does not reach it. The runner keeps scanning for and
  ending games of ours while it closes MO2, and afterwards until none has
  appeared for 20 s (3 minutes at most), then scans once more.

A game of ours is ended with TerminateProcess on the held handle, and the
runner waits for it to be gone; if it is not, it says so.

Fail-fast: a game of ours that has been up for --log-start-timeout seconds
(default 90) without a Huginn log for this launch ends the run (wrong log
folder, Huginn not loaded) instead of waiting out --timeout. A game that exits
counts as a crash unless this launch's log holds a DONE, and a DONE found when
the game exits is taken at once.

The log folder defaults to <Documents>/My Games/Skyrim.INI/SKSE, with
Documents resolved through the Windows known-folder API (so a OneDrive-
redirected Documents folder is found); --log-dir overrides it.

Reads MO2's ModOrganizer.ini and profile settings; never writes MO2 config.
Does not deploy the DLL: put the Debug Huginn.dll in the list first.

Usage:
    python -I tools/ingame/run_tests.py [--list vanilla+|simonrim|lorerim]
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
import ctypes
import datetime as dt
import os
import hashlib
import io
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from ctypes import wintypes
from pathlib import Path
LOG_NAME = "_Huginn_Debug.log"   # the Debug build's log; only Debug has the suites
FLAG_NAME = "Huginn_TestMode.ini"
GAME_EXE = "SkyrimSE.exe"
MO2_EXE = "ModOrganizer.exe"
DEFAULT_SAVE = "HuginnTest"
# Strings only a DLL with the test harness contains (src/TestHarness.cpp).
HARNESS_MARKERS = (b"Huginn_TestMode.ini", b"[HuginnTest] DONE")
KILL_GRACE_SEC = 15
MO2_GRACE_SEC = 30
# After MO2 is gone, keep watching this long for a late game of ours (MO2 can
# still be starting the game when the run ends, e.g. on --timeout 0); every
# new one resets the watch, up to SHUTDOWN_CAP_SEC in all.
QUIET_SEC = 20
SHUTDOWN_CAP_SEC = 180
# A process created up to this long before the recorded launch still counts
# (a clock step backwards); the instance path and image name decide the rest.
CREATION_SLACK_SEC = 5
LOG_START_TIMEOUT_SEC = 90


@dataclass(frozen=True)
class ModList:
    root: Path
    profile: str
    executable: str   # the MO2 custom-executable title


LISTS = {
    # vanilla+ is the default: the simonrim instance (fast to the menu), on the
    # profile that holds the test character (level 4). "Simonrim Essentials"
    # holds older level-3 saves of that character, but its NEWEST save is a
    # new character still in the opening cinematic, which is what "latest"
    # (the default without a HuginnTest save) would pick there. Both profiles
    # share the instance's overwrite folder, so the same Huginn.dll.
    "vanilla+": ModList(Path("F:/Modlists/simonrim-essentails"), "vanilla+", "SKSE"),
    "simonrim": ModList(Path("F:/Modlists/simonrim-essentails"), "Simonrim Essentials", "SKSE"),
    "lorerim": ModList(Path("F:/Modlists/LoreRim-5"), "Ultra", "LoreRim"),
}
DEFAULT_LIST = "vanilla+"

RE_LAUNCH = re.compile(r"Launch (\d{8}-\d{6}) \(UTC\)")
RE_DONE = re.compile(r"\[HuginnTest\] DONE result=(\w+) (.*)$")
RE_RESULT = re.compile(r"\[HuginnTest\] RESULT phase=(\w+) (.*)$")
RE_SUITE = re.compile(r"\[HuginnTest\] suite (\S+) (\S+) \((\d+) error line\(s\)(?:; skipped: (.*))?\)")
RE_FIELDS = re.compile(r"(\w+)=(\S+)")


def say(msg: str) -> None:
    print(f"[run_tests {dt.datetime.now():%H:%M:%S}] {msg}", flush=True)


class Refused(Exception):
    """Nothing was launched (exit code 2)."""


# --- Windows ------------------------------------------------------------------

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.OpenProcess.restype = wintypes.HANDLE
_k32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
_k32.GetProcessTimes.restype = wintypes.BOOL
_k32.GetProcessTimes.argtypes = (wintypes.HANDLE,) + (ctypes.POINTER(wintypes.FILETIME),) * 4
_k32.QueryFullProcessImageNameW.restype = wintypes.BOOL
_k32.QueryFullProcessImageNameW.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                            ctypes.POINTER(wintypes.DWORD))
_k32.WaitForSingleObject.restype = wintypes.DWORD
_k32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
_k32.TerminateProcess.restype = wintypes.BOOL
_k32.TerminateProcess.argtypes = (wintypes.HANDLE, wintypes.UINT)
_k32.CloseHandle.restype = wintypes.BOOL
_k32.CloseHandle.argtypes = (wintypes.HANDLE,)
_PROCESS_TERMINATE = 0x0001
_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
_SYNCHRONIZE = 0x00100000
_WAIT_TIMEOUT = 0x102
_EPOCH_1601 = dt.datetime(1601, 1, 1, tzinfo=dt.timezone.utc)


def documents_dir() -> Path:
    """The user's Documents folder via SHGetKnownFolderPath(FOLDERID_Documents):
    right even when OneDrive (or a policy) has moved it."""
    class GUID(ctypes.Structure):
        _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD),
                    ("Data3", wintypes.WORD), ("Data4", ctypes.c_ubyte * 8)]
    folder_id = GUID(0xFDD39AD0, 0x238F, 0x46AF,
                     (ctypes.c_ubyte * 8)(0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7))
    path_ptr = ctypes.c_wchar_p()
    try:
        shell32 = ctypes.WinDLL("shell32")
        ole32 = ctypes.WinDLL("ole32")
        hr = shell32.SHGetKnownFolderPath(ctypes.byref(folder_id), 0, None, ctypes.byref(path_ptr))
        if hr == 0 and path_ptr.value:
            found = Path(path_ptr.value)
            ole32.CoTaskMemFree(path_ptr)
            return found
    except OSError:
        pass
    return Path.home() / "Documents"


DEFAULT_LOG_DIR = documents_dir() / "My Games" / "Skyrim.INI" / "SKSE"


class Proc:
    """A process held open by handle from first sight: the PID cannot be
    reused while the handle is open, so the kill always reaches this one."""

    def __init__(self, pid: int, handle: int, created: dt.datetime, image: str):
        self.pid, self.handle, self.created, self.image = pid, handle, created, image

    @staticmethod
    def open(pid: int) -> "Proc | None":
        h = _k32.OpenProcess(_PROCESS_QUERY_LIMITED_INFORMATION | _SYNCHRONIZE | _PROCESS_TERMINATE, False, pid)
        if not h:
            return None
        times = [wintypes.FILETIME() for _ in range(4)]
        size = wintypes.DWORD(1024)
        buf = ctypes.create_unicode_buffer(size.value)
        if not _k32.GetProcessTimes(h, *[ctypes.byref(t) for t in times]) or \
                not _k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
            _k32.CloseHandle(h)
            return None
        ticks = (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
        created = _EPOCH_1601 + dt.timedelta(microseconds=ticks // 10)
        return Proc(pid, h, created, buf.value)

    def alive(self) -> bool:
        return _k32.WaitForSingleObject(self.handle, 0) == _WAIT_TIMEOUT

    def terminate(self) -> bool:
        """End it and wait (up to 10 s) until it has. False when it is still
        there afterwards; the caller reports that."""
        if not _k32.TerminateProcess(self.handle, 1):
            err = ctypes.get_last_error()
            if not self.alive():
                return True
            say(f"TerminateProcess failed for pid {self.pid} (error {err})")
            return False
        if _k32.WaitForSingleObject(self.handle, 10_000) == _WAIT_TIMEOUT:
            say(f"pid {self.pid} is still running 10 s after TerminateProcess")
            return False
        return True

    def close(self) -> None:
        if self.handle:
            _k32.CloseHandle(self.handle)
            self.handle = 0


def under(path: str, root: Path) -> bool:
    a = os.path.normcase(os.path.abspath(path))
    b = os.path.normcase(os.path.abspath(str(root)))
    return a == b or a.startswith(b.rstrip("\\/") + os.sep)


class GameTracker:
    """The games this run started: an image named SkyrimSE.exe under the
    instance folder, created after the launch (less CREATION_SLACK_SEC).
    Everything else is left alone."""

    def __init__(self, root: Path, launched: dt.datetime):
        self.root, self.launched = root, launched
        self.ours: dict[int, Proc] = {}
        self.ignored: set[tuple[int, dt.datetime | None]] = set()
        self.first: Proc | None = None
        self.first_seen: float | None = None   # time.monotonic() at first sight

    def scan(self) -> None:
        for pid in pids_of(GAME_EXE):
            if pid in self.ours:
                continue
            p = Proc.open(pid)
            if p is None:
                continue
            key = (pid, p.created)
            mine = (os.path.basename(p.image).lower() == GAME_EXE.lower()
                    and under(p.image, self.root)
                    and p.created >= self.launched - dt.timedelta(seconds=CREATION_SLACK_SEC))
            if not mine:
                if key not in self.ignored:
                    self.ignored.add(key)
                    say(f"ignoring {GAME_EXE} pid {pid} (created {p.created:%H:%M:%S} UTC, {p.image}): "
                        "not started by this run")
                p.close()
                continue
            self.ours[pid] = p
            if self.first is None:
                self.first, self.first_seen = p, time.monotonic()
                say(f"{GAME_EXE} started (pid {pid}, {p.image})")

    def first_gone(self) -> bool:
        return self.first is not None and not self.first.alive()

    def alive_count(self) -> int:
        return sum(1 for p in self.ours.values() if p.alive())

    def reap(self) -> int:
        """Scan, then end every game of ours still running. Returns how many
        games of ours were seen for the first time in this scan."""
        before = len(self.ours)
        self.scan()
        for p in self.ours.values():
            if p.handle and p.alive():
                say(f"ending {GAME_EXE} pid {p.pid} (ours, still running at the end of the run)")
                p.terminate()
        return len(self.ours) - before

    def close_all(self) -> None:
        for p in self.ours.values():
            if p.alive():
                say(f"WARNING: {GAME_EXE} pid {p.pid} (ours) is still running; close it by hand")
            p.close()


def shut_down(games: GameTracker, mo2_proc: subprocess.Popen) -> None:
    """Whatever starts the game closes it. Give our game KILL_GRACE_SEC to end
    by itself, then end it; close the MO2 this run started; and keep scanning
    for games of ours the whole time, and for QUIET_SEC after MO2 is gone, so
    a game MO2 was still starting when the run ended is not orphaned (its
    parent skse64_loader has exited, so MO2's /T does not reach it)."""
    cap = time.monotonic() + SHUTDOWN_CAP_SEC
    end = time.monotonic() + KILL_GRACE_SEC
    while time.monotonic() < end:
        games.scan()
        if games.alive_count() == 0:
            break
        time.sleep(1)
    games.reap()

    # MO2: wait, ask, force -- reaping games every second throughout.
    stages = [(MO2_GRACE_SEC, None, "closed by itself"),
              (MO2_GRACE_SEC, ["taskkill", "/PID"], "closed"),
              (15, ["taskkill", "/F", "/T", "/PID"], "ended")]
    for wait_sec, command, done in stages:
        if mo2_proc.poll() is not None:
            break
        if command:
            say(f"{'closing' if '/F' not in command else 'ending'} the MO2 this run started (pid {mo2_proc.pid})")
            subprocess.run([*command, str(mo2_proc.pid)], capture_output=True, check=False)
        end = time.monotonic() + wait_sec
        while time.monotonic() < end and mo2_proc.poll() is None:
            games.reap()
            time.sleep(1)
        if mo2_proc.poll() is not None:
            say(f"MO2 (pid {mo2_proc.pid}) {done}")
            break
    else:
        if mo2_proc.poll() is None:
            say(f"WARNING: MO2 (pid {mo2_proc.pid}) is STILL running; close it by hand")

    # Quiet window: no new game of ours for QUIET_SEC.
    quiet_until = time.monotonic() + QUIET_SEC
    while time.monotonic() < quiet_until and time.monotonic() < cap:
        if games.reap():
            quiet_until = time.monotonic() + QUIET_SEC
        time.sleep(1)
    games.reap()
    games.close_all()


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
    ap.add_argument("--list", choices=list(LISTS), default=DEFAULT_LIST)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--save", help="save to load after the main menu (no .ess); 'latest' = newest in the profile")
    g.add_argument("--no-save", action="store_true", help="main-menu suites only")
    ap.add_argument("--timeout", type=int, default=600, help="seconds from launch to the sentinel (default 600)")
    ap.add_argument("--load-timeout", type=int, default=300, help="Huginn's own wait for the save to load")
    ap.add_argument("--log-start-timeout", type=int, default=LOG_START_TIMEOUT_SEC,
                    help="seconds the game may run without this launch's Huginn log (default 90)")
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

    launched_precise = dt.datetime.now(dt.timezone.utc)
    launched_utc = launched_precise.replace(microsecond=0)   # the log's stamp is to the second
    games = GameTracker(ml.root, launched_precise)
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
            games.scan()
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
            elif games.first_seen is not None and \
                    time.monotonic() - games.first_seen > args.log_start_timeout:
                verdict = (1, f"the game has run {args.log_start_timeout}s without a Huginn log for this "
                              f"launch in {log_path.parent} (Huginn not loaded, or the wrong log folder?)")
                break
            if games.first_gone():
                # The game is gone. Only a DONE line in THIS launch's log
                # counts; one found now is taken at once.
                time.sleep(2)
                lines = read_log(log_path)
                fresh = fresh or log_is_fresh(log_path, lines, launched_utc)
                verdict = done_verdict(lines, args.allow_skips) if fresh else None
                if verdict is None:
                    verdict = (1, "the game exited before the sentinel (crash?)"
                                  + ("" if fresh else "; this launch never started a Huginn log"))
                break
        else:
            verdict = (1, f"timeout after {args.timeout}s" + ("" if fresh else " (the log never started)"))
    finally:
        # The flag first: a game MO2 starts late must not enter test mode.
        try:
            flag_path.unlink()
            say("removed the test-mode file (Huginn had not consumed it)")
        except FileNotFoundError:
            pass
        if mo2_proc is not None:
            shut_down(games, mo2_proc)

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
