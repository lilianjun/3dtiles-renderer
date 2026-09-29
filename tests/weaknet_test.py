#!/usr/bin/env python3
"""P18 weak-network resilience gate: slow / cancel / abort / outage.

Runs tiles_demo against a local controllable HTTP server (tests/slow_http_server.py)
and asserts deterministic steady-state behavior. No external network, no root.

Modes:
  slow            - 0.3s/response latency; must settle: loaded==4, failed==0.
  cancel_prevent  - far camera from frame 0; children must never be requested
                    (server access log is ground truth).
  cancel_midload  - zoom out while loads in flight; must exit 0, failed==0,
                    selection follows the camera once in-flight loads resolve.
  abort           - exit on first observed in-flight load; must tear down clean
                    (also run under ASan/UBSan).
  outage          - kill the server mid-load, restart on the same port;
                    must exit 0 (graceful), never crash.
  all             - slow + cancel_prevent + cancel_midload + abort + outage.

Usage:
  python3 tests/weaknet_test.py --mode all [--sanitized] [--suppressions FILE]
Exit 0 on pass, 1 on fail. Prints PASS/FAIL per case with key numbers.
"""

import argparse
import os
import queue
import re
import subprocess
import sys
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.path.join(REPO, "tests", "data", "p3_box_tileset")
SERVER = os.path.join(REPO, "tests", "slow_http_server.py")
RUN_SAN = os.path.join(REPO, "tests", "run_sanitized.py")
DEMO = None  # set from --demo in main()

STAT_RE = re.compile(
    r"\[stats\] frame=(\d+) selected=(-?\d+) loading=(-?\d+) "
    r"loaded=(-?\d+) failed=(-?\d+) bytes=(-?\d+)"
)
SETTLED_RE = re.compile(
    r"\[demo\] settled: frames=(\d+) loaded=(\d+) failed=(\d+)")


def parse_settled(line):
    m = SETTLED_RE.search(line or "")
    if not m:
        return None
    return {"frames": int(m.group(1)), "loaded": int(m.group(2)),
            "failed": int(m.group(3))}

REQ_LOG = "/tmp/weaknet_req.log"


class Server:
    """Controllable local HTTP server. Logs every request path."""

    def __init__(self, delay, port=0):
        self.delay = delay
        self.port = port
        self.proc = None

    def start(self):
        open(REQ_LOG, "w").close()
        self.proc = subprocess.Popen(
            [sys.executable, SERVER, "--directory", DATA_DIR,
             "--delay", str(self.delay), "--port", str(self.port),
             "--log-requests", REQ_LOG],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        for _ in range(100):
            line = self.proc.stdout.readline()
            m = re.match(r"PORT=(\d+)", line or "")
            if m:
                self.port = int(m.group(1))
                return self
        self.stop()
        raise RuntimeError("server did not report a port")

    def requested_paths(self):
        try:
            with open(REQ_LOG) as f:
                return sorted({l.strip() for l in f if l.strip()})
        except OSError:
            return []

    def stop(self):
        if self.proc:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
            self.proc = None


class Demo:
    """tiles_demo with a background stdout reader (non-blocking wait_for)."""

    def __init__(self, tileset_url, args, sanitized=False, suppressions=None):
        cmd = [DEMO, "--tileset", tileset_url] + args
        env = dict(os.environ)
        self._san = None
        if sanitized:
            sys.path.insert(0, os.path.join(REPO, "tests"))
            from sanitizer_common import sanitizer_env, find_reports
            env = sanitizer_env(suppressions)
            self._san = find_reports
        self.proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1, env=env)
        self.lines = queue.Queue()
        self.all_lines = []
        self._thread = threading.Thread(target=self._pump, daemon=True)
        self._thread.start()

    def _pump(self):
        try:
            for line in self.proc.stdout:
                line = line.rstrip("\n")
                self.all_lines.append(line)
                self.lines.put(line)
        except Exception:
            pass

    def sanitizer_reports(self):
        if not self._san:
            return []
        return self._san("\n".join(self.all_lines))

    def wait_for(self, pattern, timeout):
        """First line matching pattern (regex) within timeout, else None."""
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                line = self.lines.get(timeout=0.2)
            except queue.Empty:
                if self.proc.poll() is not None:
                    break
                continue
            if rx.search(line):
                return line
        return None

    def wait_done(self, timeout):
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def drain_stats(self):
        out = []
        while True:
            try:
                line = self.lines.get_nowait()
            except queue.Empty:
                break
            s = parse_stats(line)
            if s:
                out.append(s)
        return out

    def terminate(self):
        if self.proc.poll() is None:
            self.proc.terminate()


def parse_stats(line):
    m = STAT_RE.search(line or "")
    if not m:
        return None
    keys = ("frame", "selected", "loading", "loaded", "failed", "bytes")
    return dict(zip(keys, map(int, m.groups())))


def check(cond, msg, ctx):
    ctx["log"].append(("ok" if cond else "FAIL", msg))
    if not cond:
        ctx["failed"] = True


def srv_url(srv):
    return "http://127.0.0.1:%d/tileset.json" % srv.port


def case_slow(ctx, args):
    srv = Server(delay=0.3).start()
    try:
        d = Demo(srv_url(srv), ["--until-loaded", "3000", "--stats"])
        line = d.wait_for(r"settled|NOT settled", timeout=120)
        rc = d.wait_done(timeout=30)
        s = parse_settled(line)
        check(rc == 0, "exit 0 (rc=%s)" % rc, ctx)
        check(line and "settled" in line and "NOT" not in line,
              "settled within budget: %s" % (line or "<none>"), ctx)
        check(s and s["loaded"] == 4, "loaded==4 (got %s)" %
              (s["loaded"] if s else None), ctx)
        check(s and s["failed"] == 0, "failed==0", ctx)
        d.terminate()
    finally:
        srv.stop()


def case_cancel_prevent(ctx, args):
    # Far camera from frame 0: children must never be requested at all.
    srv = Server(delay=0.3).start()
    try:
        traj = "/tmp/weaknet_far.csv"
        with open(traj, "w") as f:
            f.write("# frame,yaw,pitch,distance\n0,30,18,2000\n300,30,18,2000\n")
        d = Demo(srv_url(srv), ["--trajectory", traj, "--warmup", "0",
                                "--until-loaded", "3000", "--stats"])
        line = d.wait_for(r"settled|NOT settled", timeout=120)
        rc = d.wait_done(timeout=30)
        s = parse_settled(line)
        paths = srv.requested_paths()
        check(rc == 0, "exit 0 (rc=%s)" % rc, ctx)
        check(line and "settled" in line and "NOT" not in line,
              "settled: %s" % (line or "<none>"), ctx)
        check(s and s["loaded"] == 2, "loaded==2 (got %s)" %
              (s["loaded"] if s else None), ctx)
        check("/child_a.glb" not in paths and "/child_b.glb" not in paths,
              "children never requested (server saw: %s)" % paths, ctx)
        check("/root.glb" in paths, "root still requested", ctx)
        d.terminate()
    finally:
        srv.stop()


def case_cancel_midload(ctx, args):
    # Zoom out while loads are in flight: cesium-native keeps in-flight tiles
    # selected until their loads resolve (no popping), then selection follows
    # the camera. Assert graceful steady state, no failures.
    srv = Server(delay=0.3).start()
    try:
        d = Demo(srv_url(srv), ["--zoom-out-on-loading", "--frames", "400",
                                "--stats"])
        line = d.wait_for(r"zoomed out", timeout=60)
        check(line is not None, "zoom fired on in-flight load", ctx)
        rc = d.wait_done(timeout=120)
        check(rc == 0, "exit 0 (rc=%s)" % rc, ctx)
        samples = d.drain_stats()
        check(len(samples) >= 2, "got stats samples (%d)" % len(samples), ctx)
        if samples:
            first, last = samples[0], samples[-1]
            check(last["selected"] == 2,
                  "selection followed camera: selected %d -> %d" %
                  (first["selected"], last["selected"]), ctx)
            check(last["failed"] == 0, "failed==0", ctx)
            steady = all(s["loading"] == last["loading"] and
                         s["loaded"] == last["loaded"]
                         for s in samples[-20:])
            check(steady, "steady state reached (loading=%d loaded=%d)" %
                  (last["loading"], last["loaded"]), ctx)
        d.terminate()
    finally:
        srv.stop()


def case_abort(ctx, args):
    srv = Server(delay=0.3).start()
    try:
        d = Demo(srv_url(srv), ["--exit-on-loading", "--frames", "10000",
                                "--stats"],
                 sanitized=args.sanitized, suppressions=args.suppressions)
        line = d.wait_for(r"abort: load in flight", timeout=60)
        rc = d.wait_done(timeout=60)
        check(line is not None, "abort fired on in-flight load", ctx)
        check(rc == 0, "exit 0 after mid-load teardown (rc=%s)" % rc, ctx)
        if args.sanitized:
            # drain any remaining output so the report scan sees everything
            d.wait_done(timeout=5)
            time.sleep(0.5)
            reports = d.sanitizer_reports()
            check(not reports, "no sanitizer reports (got %s)" % reports, ctx)
        d.terminate()
    finally:
        srv.stop()


def case_outage(ctx, args):
    # Kill the server mid-load, restart on the same port, keep rendering.
    # Must never crash; exit 0.
    srv = Server(delay=0.2).start()
    try:
        d = Demo(srv_url(srv), ["--frames", "600", "--stats"])
        line = d.wait_for(r"\[stats\]", timeout=60)
        s0 = parse_stats(line)
        d2 = d.wait_for(r"loading=([1-9])", timeout=60)
        check(d2 is not None, "saw in-flight load before kill", ctx)
        port = srv.port
        srv.stop()
        time.sleep(3.0)  # dead window: in-flight requests fail, must not hang
        srv = Server(delay=0.2, port=port).start()
        check(srv.port == port, "server restarted on port %d" % port, ctx)
        rc = d.wait_done(timeout=120)
        check(rc == 0, "exit 0 after outage+restart (rc=%s)" % rc, ctx)
        failed_seen = any(s["failed"] > 0 for s in d.drain_stats())
        ctx["log"].append(("ok", "failed>0 observed during outage: %s" %
                           failed_seen))
        d.terminate()
    finally:
        srv.stop()


CASES = {
    "slow": case_slow,
    "cancel_prevent": case_cancel_prevent,
    "cancel_midload": case_cancel_midload,
    "abort": case_abort,
    "outage": case_outage,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="all", choices=list(CASES) + ["all"])
    ap.add_argument("--sanitized", action="store_true")
    ap.add_argument("--suppressions", default="")
    ap.add_argument("--demo", default="",
                    help="tiles_demo binary (default: <repo>/build/linux/tiles_demo)")
    args = ap.parse_args()

    global DEMO
    DEMO = args.demo or os.path.join(REPO, "build", "linux", "tiles_demo")

    if args.sanitized and not args.suppressions:
        print("weaknet: --sanitized requires --suppressions", file=sys.stderr)
        return 2

    modes = list(CASES) if args.mode == "all" else [args.mode]
    t0 = time.time()
    overall_fail = False
    for mode in modes:
        ctx = {"log": [], "failed": False}
        t1 = time.time()
        try:
            CASES[mode](ctx, args)
        except Exception as e:  # never let one case kill the suite
            ctx["log"].append(("FAIL", "exception: %r" % e))
            ctx["failed"] = True
        dt = time.time() - t1
        status = "FAIL" if ctx["failed"] else "PASS"
        print("weaknet [%s] %s (%.1fs)" % (mode, status, dt))
        for ok, msg in ctx["log"]:
            print("    %s %s" % ("ok " if ok == "ok" else "FAIL", msg))
        overall_fail = overall_fail or ctx["failed"]
    print("weaknet suite: %s in %.1fs" %
          ("FAIL" if overall_fail else "PASS", time.time() - t0))
    return 1 if overall_fail else 0


if __name__ == "__main__":
    sys.exit(main())
