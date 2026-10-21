#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Chart the GI's live-cache cost: the beauty sample's `gi live` lines, the caches on the host
against the caches on the device, scene by scene.

    python3 tools/docs/plot_gi_live_cost.py \
        --timing openspec/changes/finish-gi-live-caches-and-streaming/evidence/live-timing.txt \
        --out docs/design/images/gi-live-cost.png

The timing file is what the measurement wrote: a `== <scene> <device|host> <tier>` line before each
run's output. Only the hardware tier's runs are charted.
"""

import argparse
import pathlib
import re
import sys

LIVE = re.compile(
    r"host update median (?P<median>[\d.]+), p95 (?P<p95>[\d.]+), worst [\d.]+ ms; "
    r"device median resolve (?P<resolve>[\d.]+), cards (?P<cards>[\d.]+), gather (?P<gather>[\d.]+)"
)
HEADER = re.compile(r"^== (?P<scene>\w+) (?P<caches>device|host) (?P<tier>\w+)")
SCENES = (("interior", "the sunlit room"), ("hall", "the workshop hall"),
          ("colonnade", "the Colonnade"))


def parse(text):
    """{(scene, caches): {median, p95, resolve, cards, gather}} for the hardware tier."""
    runs = {}
    key = None
    for line in text.splitlines():
        header = HEADER.match(line)
        if header:
            key = (header["scene"], header["caches"]) if header["tier"] == "hardware" else None
            continue
        live = LIVE.search(line)
        if live and key is not None:
            runs[key] = {name: float(value) for name, value in live.groupdict().items()}
    return runs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timing", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    runs = parse(pathlib.Path(args.timing).read_text())
    missing = [s for s, _ in SCENES for c in ("host", "device") if (s, c) not in runs]
    if missing:
        print(f"plot_gi_live_cost: no hardware-tier run for {missing}", file=sys.stderr)
        return 1

    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    labels = [label for _, label in SCENES]
    host = [runs[(scene, "host")]["median"] for scene, _ in SCENES]
    device = [runs[(scene, "device")]["median"] for scene, _ in SCENES]
    resolve = [runs[(scene, "device")]["resolve"] for scene, _ in SCENES]
    caches = [runs[(scene, "device")]["cards"] + runs[(scene, "device")]["gather"]
              for scene, _ in SCENES]

    figure, (left, right) = plt.subplots(1, 2, figsize=(12, 4.2), dpi=120)
    positions = range(len(labels))
    width = 0.38
    left.bar([p - width / 2 for p in positions], host, width, color="#b0b4ba",
             label="caches on the host")
    left.bar([p + width / 2 for p in positions], device, width, color="#2f7ed8",
             label="caches on the device")
    for p, value in zip(positions, host):
        left.text(p - width / 2, value + 0.4, f"{value:.1f}", ha="center", fontsize=9)
    for p, value in zip(positions, device):
        left.text(p + width / 2, value + 0.4, f"{value:.2f}", ha="center", fontsize=9)
    left.set_xticks(list(positions), labels)
    left.set_ylabel("ms of processor time a frame (median)")
    left.set_title("The host's GI update, caches live")
    left.set_ylim(0.0, max(host) * 1.2)
    left.legend(frameon=False, loc="upper right")
    left.spines[["top", "right"]].set_visible(False)

    right.bar(list(positions), resolve, 0.55, color="#2f7ed8", label="resolve")
    right.bar(list(positions), caches, 0.55, bottom=resolve, color="#f0a030",
              label="card shading + probe gather")
    for p, base, top in zip(positions, resolve, caches):
        right.text(p, base + top + 0.08, f"{base + top:.2f}", ha="center", fontsize=9)
    right.set_xticks(list(positions), labels)
    right.set_ylabel("ms of GPU time a frame (median)")
    right.set_title("The GI on the device, caches live (RTX 5060, 1080p)")
    right.set_ylim(0.0, max(b + t for b, t in zip(resolve, caches)) * 1.3)
    right.legend(frameon=False, loc="upper center", ncol=2)
    right.spines[["top", "right"]].set_visible(False)

    figure.suptitle("CyberGI with a turning sun and a moving block: 240 frames a scene",
                    fontsize=11)
    figure.tight_layout()
    figure.savefig(args.out)
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
