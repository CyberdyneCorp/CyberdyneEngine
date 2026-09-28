#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The sample MCP client must respect its timeout even without a complete reply line."""

from __future__ import annotations

import io
import base64
import os
import sys
import time
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest.mock import Mock, patch

from mcp_window import Mcp, Session, capture_scene, main, require_pillow, wait_for_runtime
from artefact import Failed


class McpTimeoutTests(unittest.TestCase):
    def setUp(self) -> None:
        reader, self.writer = os.pipe()
        self.stream = os.fdopen(reader, "r")
        self.mcp = Mcp(SimpleNamespace(stdin=io.StringIO(), stdout=self.stream))

    def tearDown(self) -> None:
        self.stream.close()
        os.close(self.writer)

    def test_no_reply_times_out(self) -> None:
        started = time.monotonic()
        with self.assertRaisesRegex(Failed, "no answer within"):
            self.mcp.call("resources/read", seconds=0.05)
        self.assertLess(time.monotonic() - started, 0.5)

    def test_partial_reply_times_out(self) -> None:
        os.write(self.writer, b'{"jsonrpc":"2.0","id":1')
        started = time.monotonic()
        with self.assertRaisesRegex(Failed, "no answer within"):
            self.mcp.call("resources/read", seconds=0.05)
        self.assertLess(time.monotonic() - started, 0.5)

    def test_buffered_reply_is_returned(self) -> None:
        os.write(
            self.writer,
            b'{"jsonrpc":"2.0","id":99,"result":{}}\n'
            b'{"jsonrpc":"2.0","id":1,"result":{"ready":true}}\n',
        )
        self.assertEqual(self.mcp.call("resources/read", seconds=0.5), {"ready": True})

    def test_capture_retries_a_window_waiting_for_its_first_frame(self) -> None:
        pixels = b"captured PNG payload"
        image = Mock()
        image.convert.return_value = image
        image.getpixel.return_value = (255, 0, 0)
        pillow = SimpleNamespace(Image=SimpleNamespace(open=Mock(return_value=image)))
        replies = iter([
            {"content": [{"text": "the editor window presented no frame within 2000 ms"}]},
            {"contents": [
                {"mimeType": "image/png", "blob": base64.b64encode(pixels).decode()},
                {"text": "first frame"},
            ]},
        ])
        self.mcp.call = lambda method, params: next(replies)
        with patch.dict(sys.modules, {"PIL": pillow}), patch("mcp_window.time.sleep") as pause:
            image, description = self.mcp.capture("editor:window")
        self.assertEqual(image.getpixel((0, 0)), (255, 0, 0))
        self.assertEqual(description, "first frame")
        self.assertEqual(pillow.Image.open.call_args.args[0].getvalue(), pixels)
        image.convert.assert_called_once_with("RGB")
        pause.assert_called_once()


class SceneCaptureTests(unittest.TestCase):
    class Image:
        def __init__(self, colour: tuple[int, int, int]) -> None:
            self.colour = colour

        def resize(self, size: tuple[int, int]) -> "SceneCaptureTests.Image":
            return self

        def getdata(self) -> list[tuple[int, int, int]]:
            return [self.colour]

        def save(self, path: Path) -> None:
            path.write_bytes(b"captured editor window")

    def test_saves_a_live_window_after_the_viewport_is_visible(self) -> None:
        window = self.Image((255, 0, 0))
        session = SimpleNamespace(
            viewport=lambda: (window, "engine frame"),
            mcp=SimpleNamespace(capture=lambda uri: (window, "editor window")),
        )
        with TemporaryDirectory() as directory:
            target = Path(directory) / "screenshots" / "scene.png"
            detail = capture_scene(session, target)
            self.assertEqual(target.read_bytes(), b"captured editor window")
            self.assertIn("engine frame", detail)

    def test_refuses_a_black_viewport_without_saving_a_screenshot(self) -> None:
        viewport = self.Image((0, 0, 0))
        session = SimpleNamespace(
            viewport=lambda: (viewport, "null frame"),
            mcp=SimpleNamespace(capture=lambda uri: self.fail("window capture after refusal")),
        )
        with TemporaryDirectory() as directory, patch(
            "mcp_window.settle", side_effect=lambda read, predicate: read()
        ):
            target = Path(directory) / "scene.png"
            with self.assertRaisesRegex(Failed, "viewport is neutral"):
                capture_scene(session, target)
            self.assertFalse(target.exists())

    def test_reports_missing_pillow_before_starting_the_runtime(self) -> None:
        with patch.dict(sys.modules, {"PIL": None}):
            with self.assertRaisesRegex(Failed, "Pillow is required"):
                require_pillow()

    def test_runtime_exit_is_reported_without_waiting_for_socket_timeout(self) -> None:
        with TemporaryDirectory() as directory:
            viewport = str(Path(directory) / "viewport.sock")
            host = str(Path(directory) / "host.sock")
            process = SimpleNamespace(poll=lambda: 7)
            started = time.monotonic()
            with self.assertRaisesRegex(Failed, "exited with status 7"):
                wait_for_runtime(process, viewport, host)
            self.assertLess(time.monotonic() - started, 0.5)

    def test_runtime_startup_failure_closes_process_and_log(self) -> None:
        process = Mock()
        process.poll.return_value = None
        with TemporaryDirectory() as directory, patch(
            "mcp_window.subprocess.Popen", return_value=process
        ) as launch, patch("mcp_window.wait_for_runtime", side_effect=Failed("startup failed")):
            work = Path(directory)
            with self.assertRaisesRegex(Failed, "startup failed"):
                Session(Path("editor"), Path("runtime"), work, work, "worlds/empty.cyworld", "")
            self.assertTrue(launch.call_args.kwargs["stdout"].closed)
            process.terminate.assert_called_once()
            process.wait.assert_called_once()

    def test_capture_command_reports_runtime_startup_failure(self) -> None:
        with TemporaryDirectory() as directory:
            root = Path(directory) / "project"
            (root / "worlds").mkdir(parents=True)
            stderr = io.StringIO()
            with patch.object(sys, "argv", ["mcp_window.py", "--display", "display",
                                            "--work", directory, "--capture", "scene.png"]), \
                    patch("mcp_window.require_pillow"), \
                    patch("mcp_window.binaries", return_value=(Path(__file__), Path(__file__))), \
                    patch("mcp_window.prepare", return_value=(root, root, root)), \
                    patch("mcp_window.Session", side_effect=Failed("runtime exited")), \
                    redirect_stderr(stderr), redirect_stdout(io.StringIO()):
                self.assertEqual(main(), 1)
            self.assertIn("runtime exited", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
