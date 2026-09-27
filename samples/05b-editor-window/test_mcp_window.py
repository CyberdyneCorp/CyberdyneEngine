#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The sample MCP client must respect its timeout even without a complete reply line."""

from __future__ import annotations

import io
import os
import time
import unittest
from types import SimpleNamespace

from mcp_window import Mcp
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


if __name__ == "__main__":
    unittest.main()
