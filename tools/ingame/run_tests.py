"""Unattended in-game run of Huginn's Debug test suites (roadmap R1).

Launches the game through Mod Organizer 2's command line, lets Huginn run its
main-menu suites, auto-load a save and run the after-load suites, then waits
for the sentinel line Huginn logs before it ends the game:

    [HuginnTest] DONE result=PASS suites=19 passed=19 failed=0 skipped=0 fail_lines=0 failed_suites=- reason=-

Exit code: 0 on PASS (and no skipped suite, unless --allow-skips); 1 on a
failed suite, a skipped suite, or a run that never reached the sentinel
(timeout, crash, Release build, stale log); 2 when it refused to launch.

How Huginn is told to test (src/TestHarness.h): a one-shot file
Huginn_TestMode.ini in the SKSE log folder, written here and deleted by Huginn
when it reads it (and here again, in any case, when the run ends). It carries
an expiry, so a file left behind by a killed runner cannot turn a later
ordinary launch into a test. An environment variable would not do: when MO2 is
already running, the shortcut is handed to that process and runs with its
environment, not ours.

Safety:
- Refuses to launch while SkyrimSE.exe is running. Kills only the SkyrimSE.exe
  it saw start after its own launch, and only on a timeout.
- Refuses when a ModOrganizer.exe from another MO2 instance is running (the
  shortcut would go to it); --multiple passes MO2's own --multiple instead.
- Reads MO2's ModOrganizer.ini and profile settings; never writes MO2 config.
- Does not deploy the DLL: put the Debug Huginn.dll in the list first.

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
import io
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

DEFAULT_LOG_DIR = Path.home() / "Documents" / "My Games" / "Skyrim.INI" / "SKSE"
LOG_NAME = "_Huginn_Debug.log"   # the Debug build's log (Release writes Huginn.log)
FLAG_NAME = "Huginn_TestMode.ini"
GAME_EXE = "SkyrimSE.exe"
DEFAULT_SAVE = "HuginnTest"


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
RE_SUITE = re.compile(r"\[HuginnTest\] suite (\S+) (\S+) \((\d+) error line")
RE_FIELDS = re.compile(r"(\w+)=(\S+)")


def say(msg: str) -> None:
    print(f"[run_tests {dt.datetime.now():%H:%M:%S}] {msg}", flush=True)


class Refused(Exception):
    """Nothing was launched (exit code 2)."""


# --- processes ---------------------------------------------------------------

def game_pids() -> list[int]:
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {GAME_EXE}", "/NH", "/FO", "CSV"],
                         capture_output=True, text=True, check=False).stdout
    pids = []
    for row in csv.reader(io.StringIO(out)):
        if len(row) >= 2 and row[0].lower() == GAME_EXE.lower():
            pids.append(int(row[1]))
    return pids


def mo2_paths() -> list[Path]:
    """Executable paths of every running ModOrganizer.exe."""
    cmd = ("Get-CimInstance Win32_Process -Filter \"Name='ModOrganizer.exe'\" | "
           "ForEach-Object { $_.ExecutablePath }")
    out = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", cmd],
                         capture_output=True, text=True, check=False).stdout
    return [Path(line.strip()) for line in out.splitlines() if line.strip()]


def kill(pid: int) -> None:
    subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True, check=False)


# --- MO2 config (read only) --------------------------------------------------

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
                    help="launch even when another MO2 instance is running (MO2's unsupported --multiple)")
    ap.add_argument("--dry-run", action="store_true", help="check everything, print the command, launch nothing")
    args = ap.parse_args()

    ml = LISTS[args.list]
    mo2 = ml.root / "ModOrganizer.exe"
    if not mo2.is_file():
        raise Refused(f"{mo2} not found")
    check_executable(ml)
    save = resolve_save(ml, "" if args.no_save else args.save)

    running = game_pids()
    if running:
        say(f"refused: {GAME_EXE} is already running (pid {running}); not launching, not killing")
        return 2

    mo2_args = []
    others = [p for p in mo2_paths() if p.resolve() != mo2.resolve()]
    if others:
        if not args.multiple:
            say(f"refused: another MO2 instance is running ({[str(p) for p in others]}); "
                "it would receive the shortcut. Close it, or pass --multiple")
            return 2
        mo2_args.append("--multiple")
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
    mo2_proc = None
    try:
        mo2_proc = subprocess.Popen(cmd, cwd=str(ml.root))
        say(f"launched MO2 (pid {mo2_proc.pid}); waiting for the game and the log")
        fresh = False
        build_checked = False
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
            if fresh and not build_checked:
                head = "\n".join(lines[:10])
                if "[RELEASE]" in head:
                    verdict = (1, "the deployed Huginn is a Release build; the suites exist only in Debug")
                    break
                if "[DEBUG BUILD]" in head:
                    build_checked = True
                    ver = next((l.split("]: ", 1)[-1] for l in lines[:10] if "[DEBUG BUILD]" in l), "")
                    say("build: " + ver)
            if fresh:
                for line in lines:
                    m = RE_DONE.search(line)
                    if m:
                        f = fields(m.group(2))
                        ok = m.group(1) == "PASS"
                        skipped = int(f.get("skipped", "0"))
                        if ok and skipped and not args.allow_skips:
                            verdict = (1, f"{skipped} suite(s) skipped (pass --allow-skips to accept)")
                        else:
                            verdict = (0 if ok else 1, line.split("]: ", 1)[-1])
                        break
                if verdict:
                    break
                if any("[HuginnTest] RESULT phase=menu" in l for l in lines) and \
                        not any("[HuginnTest] test mode ON" in l for l in lines):
                    verdict = (1, "Huginn ran its suites without test mode: the flag file was not read")
                    break
            if game_pid is not None and game_pid not in pids:
                time.sleep(2)
                lines = read_log(log_path)
                if not any(RE_DONE.search(l) for l in lines):
                    verdict = (1, "the game exited before the sentinel (crash?)")
                    break
        else:
            verdict = (1, f"timeout after {args.timeout}s" + ("" if fresh else " (the log never started)"))
    finally:
        if game_pid is not None and verdict is not None and verdict[0] != 0 and game_pid in game_pids():
            say(f"killing {GAME_EXE} pid {game_pid}")
            kill(game_pid)
        try:
            flag_path.unlink()
            say("removed the test-mode file (Huginn had not consumed it)")
        except FileNotFoundError:
            pass
        if mo2_proc is not None:
            try:
                mo2_proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                say(f"MO2 (pid {mo2_proc.pid}) is still running after the game; left open")

    # Report.
    for line in lines:
        m = RE_SUITE.search(line)
        if m:
            print(f"  {m.group(2):8} {m.group(1)}  ({m.group(3)} error lines)")
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
