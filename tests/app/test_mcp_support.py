#!/usr/bin/env python3
"""Process isolation and cleanup when an integration scenario fails."""

import os
import select
import signal
import subprocess
import sys
import unittest

from mcp_support import McpTest, stop_process


class CleanupTests(unittest.TestCase):
    def test_failure_cleans_owned_processes(self):
        command = [sys.executable, "-c", "import time; time.sleep(60)"]
        unrelated = subprocess.Popen(command)
        detached = subprocess.Popen(command)
        try:
            with self.assertRaisesRegex(RuntimeError, "scenario failed"):
                with McpTest("simxr-cleanup-", windowless=True) as test:
                    root = test.root
                    child = test.start(command)
                    test.track(detached.pid)
                    self.assertNotEqual(test.env["SIMXR_DATA_DIR"], os.environ.get("SIMXR_DATA_DIR"))
                    self.assertNotIn("XR_RUNTIME_JSON", test.env)
                    self.assertNotIn("SIMXR_MCP_TOKEN", test.env)
                    self.assertNotIn("DISPLAY", test.env)
                    raise RuntimeError("scenario failed")
            self.assertIsNotNone(child.poll())
            self.assertIsNotNone(detached.poll())
            self.assertIsNone(unrelated.poll())
            self.assertFalse(root.exists())
        finally:
            stop_process(detached)
            stop_process(unrelated)

    def test_cleanup_after_process_exits(self):
        with McpTest("simxr-cleanup-exit-") as test:
            child = test.start([sys.executable, "-c", "import sys; sys.stdin.read()"], stdin=subprocess.PIPE)
            test.track(child.pid)
            child.stdin.close()
            self.assertEqual(child.wait(timeout=10), 0)

    @unittest.skipIf(os.name == "nt", "POSIX process groups")
    def test_cleanup_before_descendant_registration(self):
        handle = None
        try:
            with McpTest("simxr-cleanup-group-") as test:
                source = ("import subprocess, sys, time; "
                          "child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)']); "
                          "print(child.pid, flush=True); time.sleep(60)")
                parent = test.start([sys.executable, "-c", source], start_new_session=True,
                                    stdout=subprocess.PIPE, text=True)
                handle = os.pidfd_open(int(parent.stdout.readline()))
            self.assertTrue(select.select([handle], [], [], 5)[0], "Descendant survived cleanup")
        finally:
            if handle is not None:
                try:
                    signal.pidfd_send_signal(handle, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                os.close(handle)


if __name__ == "__main__":
    unittest.main()
