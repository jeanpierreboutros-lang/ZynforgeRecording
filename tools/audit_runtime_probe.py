#!/usr/bin/env python3
"""Opt-in, isolated runtime audit probes; never targets an existing process.

  python3 tools/audit_runtime_probe.py signals --daemon /absolute/candidate/ZynforgeCapture
  python3 tools/audit_runtime_probe.py disk-full --app /absolute/candidate/App.app/Contents/MacOS/App

Signals use an empty --no-device take: this verifies signal handling and file
finalization, not audio continuity. Disk-full creates a disposable 32 MiB HFS+
image, observes real ENOSPC inside it, and runs only the opt-in export suite.
Artifacts/logs are retained in a printed temporary directory. Run sequentially
with other audit tests. The disk image is detached on success or failure.
"""

import argparse
import errno
import hashlib
import hmac
import json
import os
from pathlib import Path
import plistlib
import re
import secrets
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time
import wave

PROTOCOL = 4


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class Wire:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=2)
        self.pending = b""
        self.sequence = 0
        challenge = self.receive_matching(lambda m: m.get("type") == "challenge", 3)
        require(challenge.get("version") == PROTOCOL, "candidate protocol is not version 4")
        nonce = challenge.get("nonce", "")
        require(re.fullmatch("[0-9a-f]{64}", nonce), "invalid server nonce")
        folder = Path(f"/tmp/zynforge-capture-{os.geteuid()}")
        key_path = folder / f"{port}.key"
        folder_stat = folder.lstat()
        require(stat.S_ISDIR(folder_stat.st_mode) and folder_stat.st_uid == os.geteuid()
                and stat.S_IMODE(folder_stat.st_mode) == 0o700, "unsafe endpoint directory")
        fd = os.open(key_path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        try:
            info = os.fstat(fd)
            require(stat.S_ISREG(info.st_mode) and info.st_uid == os.geteuid()
                    and info.st_nlink == 1 and stat.S_IMODE(info.st_mode) == 0o600,
                    "unsafe endpoint key")
            key = os.read(fd, 65)
        finally:
            os.close(fd)
        require(re.fullmatch(b"[0-9a-f]{64}", key), "invalid endpoint key")
        client_nonce = secrets.token_hex(32)
        transcript = f"{nonce}:{client_nonce}:{PROTOCOL}"
        proof = hmac.new(key, ("client:" + transcript).encode(), hashlib.sha256).hexdigest()
        reply = self.request("hello", authNonce=client_nonce, authProof=proof)
        expected = hmac.new(key, ("server:" + transcript).encode(), hashlib.sha256).hexdigest()
        require(reply.get("ok") and reply.get("version") == PROTOCOL
                and hmac.compare_digest(reply.get("authProof", ""), expected),
                "daemon authentication failed")

    def receive_matching(self, predicate, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if b"\n" in self.pending:
                line, self.pending = self.pending.split(b"\n", 1)
                message = json.loads(line)
                if predicate(message):
                    return message
                continue
            self.socket.settimeout(max(0.001, deadline - time.monotonic()))
            data = self.socket.recv(4096)
            require(data, "daemon disconnected before expected response")
            self.pending += data
            require(len(self.pending) <= 1024 * 1024, "oversized protocol frame")
        raise RuntimeError("protocol response timed out")

    def request(self, action, **fields):
        self.sequence += 1
        command = {"type": "cmd", "action": action, "version": PROTOCOL,
                   "id": self.sequence, **fields}
        self.socket.settimeout(2)
        self.socket.sendall((json.dumps(command, separators=(",", ":")) + "\n").encode())
        return self.receive_matching(lambda m: m.get("type") == "reply"
                                     and m.get("id") == self.sequence)

    def close(self):
        self.socket.close()


def stop_owned_child(process):
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    time.sleep(0.15)
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()  # Only our disposable no-device subprocess, never an existing app.
        process.wait(timeout=5)


def signal_probe(args, artifacts):
    daemon = Path(args.daemon).resolve(strict=True)
    chosen_signal = signal.SIGTERM if args.signal == "TERM" else signal.SIGINT
    results = []
    for rolling in (False, True):
        case = "rolling" if rolling else "idle"
        log_path = artifacts / f"signal-{args.signal.lower()}-{case}.log"
        with log_path.open("wb") as log:
            process = subprocess.Popen([str(daemon), "--no-device", "--inputs", "1", "--port", "0"],
                                       stdout=log, stderr=subprocess.STDOUT)
            wire = None
            try:
                deadline = time.monotonic() + 8
                port = None
                while time.monotonic() < deadline:
                    require(process.poll() is None, "owned daemon exited during startup; inspect its log")
                    match = re.search(r"\[capture\] listening on 127\.0\.0\.1:(\d+)",
                                      log_path.read_text(errors="replace"))
                    if match:
                        port = int(match.group(1))
                        break
                    time.sleep(0.05)
                require(port is not None, "owned daemon did not report its ephemeral listening port")
                wire = Wire(port)
                session = artifacts / f"empty-take-{args.signal.lower()}"
                if rolling:
                    configuration = {"sampleRate": 48000, "preRoll": 0, "backup": "",
                                     "backupFormat": 1, "mirrors": [], "tracks": [
                                         {"input": 0, "armed": True, "stereo": False, "bus": False,
                                          "name": "Audit no-device", "uid": "audit-no-device"}]}
                    require(wire.request("configureCapture", configuration=configuration).get("ok"),
                            "configuration refused")
                    require(wire.request("startRecording", sessionDir=str(session)).get("ok"),
                            "synthetic empty take failed to start")
                process.send_signal(chosen_signal)
                if rolling:
                    # Longer than the main loop period; then a fresh correlated
                    # Quit refusal proves the first signal preserved the take.
                    time.sleep(0.4)
                    require(process.poll() is None, "first signal terminated a rolling take")
                    refused = wire.request("quit")
                    require(not refused.get("ok") and "mid-take" in refused.get("error", ""),
                            "daemon no longer considered the take rolling after the first signal")
                    process.send_signal(chosen_signal)
                exit_code = process.wait(timeout=10)
                require(exit_code == 0, f"daemon exited with {exit_code}")
                result = {"case": case, "signal": args.signal, "exitCode": exit_code}
                if rolling:
                    report = json.loads((session / "session.report.json").read_text())
                    require(report.get("totalSamples") == 0, "no-device take unexpectedly received samples")
                    require(not report.get("primaryFailed") and not report.get("captureDeviceLost"),
                            "synthetic take reported a primary write/device failure")
                    audio = list((session / "Audio Files").glob("*.wav"))
                    require(len(audio) == 1, "expected one finalized empty WAV")
                    with wave.open(str(audio[0]), "rb") as wav:
                        require(wav.getnchannels() == 1 and wav.getframerate() == 48000
                                and wav.getnframes() == 0, "finalized WAV header does not match the empty take")
                    result.update(firstSignalPreservedTake=True, finalizedWav=True,
                                  totalSamples=0, sha256Pending=report.get("sha256Pending"))
                results.append(result)
            finally:
                if wire is not None:
                    wire.close()
                stop_owned_child(process)
    return {"probe": "signals", "results": results,
            "limit": "No audio callbacks were fed; this verifies signal semantics and empty-file finalization only."}


def disk_full_probe(args, artifacts):
    require(sys.platform == "darwin", "disk-image probe currently requires macOS hdiutil")
    app = Path(args.app).resolve(strict=True)
    image = artifacts / "bounded-volume.dmg"
    mount = artifacts / "volume"
    mount.mkdir()
    nonce = secrets.token_hex(16)
    subprocess.run(["/usr/bin/hdiutil", "create", "-size", "32m", "-fs", "HFS+", "-volname",
                    "ZFAudit-" + nonce[:8], str(image)], check=True, timeout=60,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    attached = False
    try:
        attached = True  # Cleanup also covers an attach that times out after mounting.
        result = subprocess.run(["/usr/bin/hdiutil", "attach", "-nobrowse", "-mountpoint", str(mount),
                                 "-plist", str(image)], check=True, timeout=30, stdout=subprocess.PIPE)
        entities = plistlib.loads(result.stdout).get("system-entities", [])
        require(any(Path(item.get("mount-point", "/")) == mount for item in entities),
                "hdiutil did not mount the requested disposable image")
        require(mount.stat().st_dev != artifacts.stat().st_dev, "refusing to fill a host-volume directory")
        volume = os.statvfs(mount)
        require(0 < volume.f_blocks * volume.f_frsize <= 64 * 1024 * 1024,
                "refusing a filesystem larger than 64 MiB")
        (mount / "previous.wav").write_text("previous deliverable")
        observed_enospc = False
        bytes_written = 0
        with (mount / "audit-filler").open("wb", buffering=0) as filler:
            block = b"\0" * 65536
            while bytes_written <= 64 * 1024 * 1024:
                try:
                    count = filler.write(block)
                    require(count and count > 0, "bounded fill made no forward progress")
                    bytes_written += count
                except OSError as error:
                    require(error.errno == errno.ENOSPC, f"unexpected fill failure: {error}")
                    observed_enospc = True
                    break
            require(observed_enospc, "bounded fill did not encounter ENOSPC")
            require(bytes_written > 128 * 1024, "volume too small for the controlled headroom")
            # Leave room for a WAV header and initial data, then force a real
            # mid-export kernel write failure. The source lives outside here.
            filler.truncate(bytes_written - 96 * 1024)
            os.fsync(filler.fileno())
        marker = {"nonce": nonce, "kernelEnospcObserved": True}
        (mount / "audit-volume.json").write_text(json.dumps(marker))
        environment = os.environ.copy()
        environment["ZYNFORGE_AUDIT_FULL_VOLUME"] = str(mount)
        environment["ZYNFORGE_AUDIT_VOLUME_NONCE"] = nonce
        report = artifacts / "disk-full-tests.log"
        stdout = artifacts / "disk-full-stdout.log"
        with stdout.open("wb") as output:
            tested = subprocess.run([str(app), "--run-tests", "--isolated-settings",
                                     "--test-filter=Audit bounded volume export", f"--test-report={report}"],
                                    env=environment, stdout=output, stderr=subprocess.STDOUT, timeout=90)
        require(tested.returncode == 0, "bounded-volume export tests failed; inspect retained logs")
        return {"probe": "disk-full", "kernelErrno": "ENOSPC", "fillerBytesBeforeHeadroom": bytes_written,
                "testReport": str(report), "limit": "Synthetic PCM export only; no production recording or hardware audio."}
    finally:
        if attached:
            # Never recursively delete a mountpoint if detachment fails.
            detached = subprocess.run(["/usr/bin/hdiutil", "detach", str(mount)],
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            require(detached.returncode == 0, f"temporary image may remain mounted at {mount}; detach this exact path")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    modes = parser.add_subparsers(dest="mode", required=True)
    signals = modes.add_parser("signals")
    signals.add_argument("--daemon", required=True)
    signals.add_argument("--signal", choices=("TERM", "INT"), default="TERM")
    disk = modes.add_parser("disk-full")
    disk.add_argument("--app", required=True)
    args = parser.parse_args()
    artifacts = Path(tempfile.mkdtemp(prefix="zynforge-audit-runtime-")).resolve()
    print(f"Artifacts: {artifacts}", flush=True)
    try:
        result = signal_probe(args, artifacts) if args.mode == "signals" else disk_full_probe(args, artifacts)
        (artifacts / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2))
    except Exception as error:
        (artifacts / "failure.txt").write_text(str(error) + "\n")
        print(f"FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
