#!/usr/bin/env python3
"""Fail-closed Linux launcher for generated Pact executables.

The trusted parent passes a compiled program and a host-only status path. The
program sees its executable, minimal read-only runtime libraries, proc/dev, a
size-limited private tmpfs, and one per-run broker socket for approved typed
mock calls. Systemd owns the cgroup limits.
"""

import os
import platform
import selectors
import shutil
import signal
import subprocess
import sys
import time
import uuid
import ctypes
import socket
import struct
import tempfile

WALL_SECONDS = 30.0
CPU_SECONDS = 5
MEMORY_BYTES = 256 * 1024 * 1024
SCRATCH_BYTES = 64 * 1024 * 1024
OUTPUT_BYTES = 1024 * 1024
PROCESS_COUNT = 16
BROKER_REQUEST = struct.Struct("<4sBBHq")
BROKER_RESPONSE = struct.Struct("<4sB3sq")
BROKER_NAMESPACE_PATH = "/run/pact-broker.sock"
_INTERRUPTED = 0


def read_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            return None
        data.extend(chunk)
    return bytes(data)


def serve_broker_once(server):
    try:
        client, _ = server.accept()
    except BlockingIOError:
        return
    with client:
        client.settimeout(0.25)
        try:
            request = read_exact(client, BROKER_REQUEST.size)
            if request is None or client.recv(1) != b"":
                return
            magic, operation, argc, reserved, value = BROKER_REQUEST.unpack(request)
            if magic != b"PDM1" or operation != 1 or argc != 1 or reserved != 0:
                return
            # Masking in an unsigned domain defines two's-complement int64 wraparound.
            doubled = (value * 2) & 0xffffffffffffffff
            if doubled >= (1 << 63):
                doubled -= 1 << 64
            client.sendall(BROKER_RESPONSE.pack(b"PDR1", 0, b"\0\0\0", doubled))
        except (OSError, socket.timeout):
            return


def remember_signal(signum, _frame):
    global _INTERRUPTED
    _INTERRUPTED = signum


def write_status(path, value):
    with open(path, "w", encoding="utf-8") as stream:
        stream.write(value + "\n")


def terminate(proc):
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        proc.wait(timeout=1.0)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait()


def scope_cgroup_path(unit_name):
    try:
        result = subprocess.run(
            ["systemctl", "--user", "show", "--property=ControlGroup", "--value", unit_name],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
            timeout=0.2,
            text=True,
        )
        group = result.stdout.strip()
        if result.returncode != 0 or not group.startswith("/") or ".." in group.split("/"):
            return None
        root = os.path.realpath("/sys/fs/cgroup")
        path = os.path.realpath(os.path.join(root, group.lstrip("/")))
        if os.path.commonpath((root, path)) != root:
            return None
        return path
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None


def stop_scope(proc, unit_name):
    # Kill every process in the delegated cgroup first, then reap the local
    # systemd-run client. This also cleans up children that outlive Pact.
    cgroup_path = scope_cgroup_path(unit_name)
    try:
        subprocess.run(
            ["systemctl", "--user", "kill", "--kill-whom=all", "--signal=SIGKILL", unit_name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
            timeout=1.0,
        )
    except (OSError, subprocess.TimeoutExpired):
        pass
    terminate(proc)
    # A failed cgroup kill must not be mistaken for successful cancellation.
    # The process-death signal on bwrap is a second cleanup path; this check
    # confirms that the systemd scope no longer contains live processes.
    try:
        subprocess.run(
            ["systemctl", "--user", "stop", unit_name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
            timeout=1.0,
        )
    except (OSError, subprocess.TimeoutExpired):
        pass
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        if cgroup_path is None:
            cgroup_path = scope_cgroup_path(unit_name)
        if cgroup_path:
            try:
                with open(os.path.join(cgroup_path, "cgroup.procs"), encoding="ascii") as stream:
                    if not stream.read().strip():
                        return True
            except FileNotFoundError:
                return True
            except OSError:
                pass
        time.sleep(0.05)
    return False


def read_cgroup_cpu_usec(unit_name):
    """Return the systemd scope CPU usage, or None while it is unavailable."""
    try:
        result = subprocess.run(
            ["systemctl", "--user", "show", "--property=ControlGroup", "--value", unit_name],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
            timeout=0.2,
            text=True,
        )
        group = result.stdout.strip()
        if result.returncode != 0 or not group.startswith("/") or ".." in group.split("/"):
            return None
        cgroup_root = os.path.realpath("/sys/fs/cgroup")
        cgroup_path = os.path.realpath(os.path.join(cgroup_root, group.lstrip("/")))
        if os.path.commonpath((cgroup_root, cgroup_path)) != cgroup_root:
            return None
        with open(os.path.join(cgroup_path, "cpu.stat"), encoding="ascii") as stream:
            for line in stream:
                key, value = line.split()
                if key == "usage_usec":
                    return int(value)
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None
    return None


def main():
    if len(sys.argv) != 4:
        return 2
    program, status_path, parent_pid_text = sys.argv[1:]
    signal.signal(signal.SIGINT, remember_signal)
    signal.signal(signal.SIGTERM, remember_signal)
    if platform.system() != "Linux":
        write_status(status_path, "sandbox_unavailable:Linux is required")
        return 0
    try:
        parent_pid = int(parent_pid_text)
        if ctypes.CDLL(None, use_errno=True).prctl(1, signal.SIGTERM, 0, 0, 0) != 0:
            raise OSError(ctypes.get_errno(), "prctl(PR_SET_PDEATHSIG) failed")
    except (ValueError, OSError) as error:
        write_status(status_path, "sandbox_unavailable:" + str(error).replace("\n", " "))
        return 0
    if os.getppid() != parent_pid:
        write_status(status_path, "cancelled")
        return 0

    bwrap = shutil.which("bwrap")
    systemd_run = shutil.which("systemd-run")
    systemctl = shutil.which("systemctl")
    prlimit = shutil.which("prlimit")
    required_dirs = ("/lib", "/lib64", "/usr/lib")
    if not bwrap or not systemd_run or not systemctl or not prlimit or any(
        not os.path.exists(path) for path in required_dirs
    ):
        write_status(status_path, "sandbox_unavailable:required Linux sandbox tools or runtime files are missing")
        return 0
    if _INTERRUPTED:
        write_status(status_path, "cancelled")
        return 0

    broker_dir = tempfile.mkdtemp(prefix="pact-broker-")
    broker_path = os.path.join(broker_dir, "rpc.sock")
    broker = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        broker.bind(broker_path)
        os.chmod(broker_path, 0o600)
        broker.listen(4)
        broker.setblocking(False)
    except OSError as error:
        broker.close()
        os.unlink(broker_path) if os.path.exists(broker_path) else None
        os.rmdir(broker_dir)
        write_status(status_path, "sandbox_unavailable:math broker setup failed: " + str(error).replace("\n", " "))
        return 0

    bwrap_args = [
        bwrap,
        "--unshare-user",
        "--unshare-pid",
        "--unshare-ipc",
        "--unshare-uts",
        "--unshare-net",
        "--new-session",
        "--die-with-parent",
        "--disable-userns",
        "--assert-userns-disabled",
        "--cap-drop",
        "ALL",
        "--clearenv",
        "--setenv",
        "PACT_BROKER_SOCKET",
        BROKER_NAMESPACE_PATH,
        "--dir",
        "/run",
        "--bind",
        broker_path,
        BROKER_NAMESPACE_PATH,
        "--ro-bind",
        "/lib",
        "/lib",
        "--ro-bind",
        "/lib64",
        "/lib64",
        "--ro-bind",
        "/usr/lib",
        "/usr/lib",
    ]
    if os.path.isfile("/etc/ld.so.cache"):
        bwrap_args += ["--ro-bind", "/etc/ld.so.cache", "/etc/ld.so.cache"]
    bwrap_args += [
        "--dev",
        "/dev",
        "--proc",
        "/proc",
        "--size",
        str(SCRATCH_BYTES),
        "--tmpfs",
        "/tmp",
        "--dir",
        "/tmp/pact",
        "--chdir",
        "/tmp/pact",
        "--ro-bind",
        os.path.realpath(program),
        "/pact-program",
        "--remount-ro",
        "/",
        "--",
        "/pact-program",
    ]
    unit_name = "pact-runner-{}.scope".format(uuid.uuid4().hex)
    command = [
        systemd_run,
        "--user",
        "--scope",
        "--quiet",
        "--unit=" + unit_name,
        "--property=MemoryMax={}".format(MEMORY_BYTES),
        "--property=TasksMax={}".format(PROCESS_COUNT),
        "--property=CPUQuota=100%",
        "--",
        prlimit,
        "--cpu={}:{}".format(CPU_SECONDS, CPU_SECONDS),
        "--core=0:0",
        "--",
    ] + bwrap_args

    captured = bytearray()
    overflow = False
    deadline = time.monotonic() + WALL_SECONDS
    try:
        proc = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            start_new_session=True,
            close_fds=True,
        )
    except (OSError, ValueError) as error:
        broker.close()
        os.unlink(broker_path)
        os.rmdir(broker_dir)
        write_status(status_path, "sandbox_unavailable:" + str(error).replace("\n", " "))
        return 0

    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)
    selector.register(broker, selectors.EVENT_READ)
    timed_out = False
    cpu_limited = False
    cleanup_failed = False
    cpu_usec = None
    cgroup_deadline = time.monotonic() + 2.0

    def stop_for_limit():
        nonlocal cleanup_failed
        if not stop_scope(proc, unit_name):
            cleanup_failed = True

    try:
        while True:
            if _INTERRUPTED:
                stop_for_limit()
                write_status(status_path, "cleanup_failed" if cleanup_failed else "cancelled")
                return 0
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                timed_out = True
                stop_for_limit()
                break
            if cpu_usec is None:
                cpu_usec = read_cgroup_cpu_usec(unit_name)
                if cpu_usec is None and time.monotonic() >= cgroup_deadline and proc.poll() is None:
                    stop_for_limit()
                    outcome = "cleanup_failed" if cleanup_failed else "sandbox_unavailable:cgroup CPU accounting is unavailable"
                    write_status(status_path, outcome)
                    return 0
            elif cpu_usec >= CPU_SECONDS * 1_000_000:
                cpu_limited = True
                stop_for_limit()
                break
            else:
                current_cpu_usec = read_cgroup_cpu_usec(unit_name)
                if current_cpu_usec is not None:
                    cpu_usec = current_cpu_usec
                elif proc.poll() is None:
                    stop_for_limit()
                    outcome = "cleanup_failed" if cleanup_failed else "sandbox_unavailable:cgroup CPU accounting disappeared"
                    write_status(status_path, outcome)
                    return 0
                if cpu_usec >= CPU_SECONDS * 1_000_000:
                    cpu_limited = True
                    stop_for_limit()
                    break
            events = selector.select(min(remaining, 0.25))
            for key, _ in events:
                if key.fileobj is broker:
                    serve_broker_once(broker)
                    continue
                chunk = os.read(key.fd, 65536)
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                room = OUTPUT_BYTES - len(captured)
                if room > 0:
                    captured.extend(chunk[:room])
                if len(chunk) > room:
                    overflow = True
                    stop_for_limit()
                    break
            if overflow:
                break
            if proc.poll() is not None and proc.stdout not in [key.fileobj for key in selector.get_map().values()]:
                break
    finally:
        selector.close()
        broker.close()
        try:
            os.unlink(broker_path)
            os.rmdir(broker_dir)
        except OSError:
            pass
        if proc.poll() is None:
            stop_for_limit()
        if proc.stdout:
            proc.stdout.close()

    if cleanup_failed:
        write_status(status_path, "cleanup_failed")
        return 0
    if timed_out:
        write_status(status_path, "timeout")
        return 0
    if cpu_limited:
        write_status(status_path, "cpu_limit")
        return 0
    if overflow:
        write_status(status_path, "output_limit")
        return 0
    os.write(1, captured)
    write_status(status_path, "ok:{}".format(proc.returncode if proc.returncode is not None else 127))
    return 0


if __name__ == "__main__":
    sys.exit(main())
