"""P6: shared sanitizer helpers for the test suite.

The sanitizer runtimes are enabled purely through environment variables, so
these helpers work for any binary built with -DTILES_SANITIZE=ON (the
linux-asan preset), without any test-code changes.
"""
import os

# Output markers that prove a sanitizer fired. Any one of them in the
# captured output fails the test, even if the process exit code is 0
# (UBSan's default recover mode prints and continues).
SANITIZER_MARKERS = (
    "ERROR: AddressSanitizer",
    "SUMMARY: AddressSanitizer",
    "ERROR: LeakSanitizer",
    "SUMMARY: LeakSanitizer",
    "SUMMARY: UndefinedBehaviorSanitizer",
    "runtime error:",  # UBSan recover-mode prefix (kept as a backstop)
)


def sanitizer_env(suppressions_path=None):
    """Environment enabling ASan+LSan+UBSan with fail-fast behavior."""
    env = dict(os.environ)
    # ASan (includes LSan on Linux). abort_on_error is already the default;
    # set explicitly so a changed default can't silently weaken the gate.
    env["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1"
    # UBSan: the build also passes -fno-sanitize-recover=all; this is the
    # matching run-time backstop.
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    lsan = "exitcode=23"
    if suppressions_path:
        lsan += ":suppressions=" + suppressions_path
    env["LSAN_OPTIONS"] = lsan
    return env


def find_reports(text):
    """Return the sanitizer markers present in text (empty = clean)."""
    return [m for m in SANITIZER_MARKERS if m in text]


def maybe_wrap_xvfb(cmd):
    """Prepend xvfb-run when no X display is present (headless CI)."""
    if not os.environ.get("DISPLAY"):
        return ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24"] + cmd
    return cmd
