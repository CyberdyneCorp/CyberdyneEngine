#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Install, run, measure, and capture the iOS Metal sample on physical hardware."""

from __future__ import annotations

import argparse
import json
import os
import queue
import re
import statistics
import subprocess
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional

if Path("/Applications/Xcode.app/Contents/Developer").is_dir():
    os.environ.setdefault("DEVELOPER_DIR", "/Applications/Xcode.app/Contents/Developer")


CONTRACT = (
    "CY_IOS_CONTRACT platform=ios window=single desktop_ops=unsupported "
    "surface=metal touch_events=4"
)
READY = re.compile(r"CY_IOS_READY backend=(\S+) device=(.+) size=(\d+)x(\d+)")
QUALITY = re.compile(
    r"CY_IOS_QUALITY native=(\d+)x(\d+) drawable=(\d+)x(\d+) "
    r"scale=([0-9]+(?:\.[0-9]+)?) terrain_octaves=(\d+) march_steps=(\d+)"
)
FPS = re.compile(r"CY_IOS_FPS fps=([0-9]+(?:\.[0-9]+)?) frames=(\d+) seconds=([0-9]+(?:\.[0-9]+)?)")
COMPUTE = re.compile(
    r"CY_IOS_COMPUTE skin_models=(\d+) skin_vertices=(\d+) skin_bones=(\d+) "
    r"vfx_emitters=(\d+) vfx_capacity=(\d+) vfx_dispatches=(\d+) "
    r"gpu_particle_instances=(\d+) cpu_particle_readback=(\d+)"
)


@dataclass(frozen=True)
class Device:
    selector: str
    name: str
    model: str
    os_version: str
    udid: str


@dataclass(frozen=True)
class Measurement:
    backend: str
    gpu: str
    width: int
    height: int
    native_width: int
    native_height: int
    scale: float
    terrain_octaves: int
    march_steps: int
    skin_models: int
    skin_vertices: int
    skin_bones: int
    vfx_emitters: int
    vfx_capacity: int
    vfx_dispatches: int
    gpu_particle_instances: int
    fps: tuple[float, ...]


@dataclass(frozen=True)
class Quality:
    native_width: int
    native_height: int
    drawable_width: int
    drawable_height: int
    scale: float
    terrain_octaves: int
    march_steps: int


def _nested(value: dict[str, Any], *keys: str, default: Any = "") -> Any:
    current: Any = value
    for key in keys:
        if not isinstance(current, dict):
            return default
        current = current.get(key, default)
    return current


def select_physical_iphone(document: dict[str, Any], requested: str) -> Device:
    """Find the requested connected physical iPhone and reject simulators loudly."""
    candidates = _nested(document, "result", "devices", default=[])
    for candidate in candidates:
        hardware = _nested(candidate, "properties", "hardware", default={})
        state = _nested(candidate, "properties", "state", default={})
        connection = _nested(candidate, "properties", "connection", default={})
        legacy_hardware = candidate.get("hardwareProperties", {})
        legacy_device = candidate.get("deviceProperties", {})
        udid = str(hardware.get("udid") or legacy_hardware.get("udid") or "")
        name = str(state.get("name") or legacy_device.get("name") or "")
        aliases = {str(candidate.get("identifier", "")), udid, name}
        if requested not in aliases:
            continue
        reality = str(hardware.get("reality") or legacy_hardware.get("reality") or "")
        platform = str(hardware.get("platform") or legacy_hardware.get("platform") or "")
        connected = str(connection.get("state") or "")
        if reality != "physical" or platform != "iOS":
            raise RuntimeError(f"{requested!r} is {reality or 'unknown'} {platform or 'device'}, not a physical iPhone")
        if connected != "connected":
            raise RuntimeError(f"{name or requested} is paired but {connected or 'not connected'}; unlock and connect it")
        software = _nested(candidate, "properties", "software", "osVersionNumber", default={})
        os_version = str(software.get("stringValue") or legacy_device.get("osVersionNumber") or "unknown")
        model = str(hardware.get("marketingName") or legacy_hardware.get("marketingName") or "iPhone")
        return Device(requested, name, model, os_version, udid)
    raise RuntimeError(f"no device matches {requested!r}; check `xcrun devicectl list devices`")


def parse_ready(lines: list[str]) -> tuple[str, str, int, int]:
    ready = next((match for line in lines if (match := READY.search(line))), None)
    if ready is None:
        raise RuntimeError("the app never emitted CY_IOS_READY after creating its Metal swapchain")
    backend = ready.group(1)
    if backend != "metal":
        raise RuntimeError(f"the app selected {backend!r}, expected the native Metal backend")
    return backend, ready.group(2), int(ready.group(3)), int(ready.group(4))


def parse_quality(lines: list[str]) -> Quality:
    quality = next((match for line in lines if (match := QUALITY.search(line))), None)
    if quality is None:
        raise RuntimeError("the app never emitted CY_IOS_QUALITY for its mobile workload")
    native_width, native_height = int(quality.group(1)), int(quality.group(2))
    drawable_width, drawable_height = int(quality.group(3)), int(quality.group(4))
    scale = float(quality.group(5))
    if not 0.0 < scale < 1.0:
        raise RuntimeError("the mobile workload must use a render scale between zero and one")
    if abs(drawable_width - round(native_width * scale)) > 1:
        raise RuntimeError(
            "reported drawable dimensions do not match the native dimensions and render scale"
        )
    if abs(drawable_height - round(native_height * scale)) > 1:
        raise RuntimeError(
            "reported drawable dimensions do not match the native dimensions and render scale"
        )
    return Quality(
        native_width,
        native_height,
        drawable_width,
        drawable_height,
        scale,
        int(quality.group(6)),
        int(quality.group(7)),
    )


def parse_fps(
    lines: list[str], minimum_samples: int, minimum_median_fps: float
) -> tuple[float, ...]:
    samples = tuple(float(match.group(1)) for line in lines if (match := FPS.search(line)))
    if len(samples) < minimum_samples:
        raise RuntimeError(f"only {len(samples)} FPS samples arrived; expected at least {minimum_samples}")
    if any(sample <= 0.0 for sample in samples):
        raise RuntimeError("an FPS sample was zero; frames were not continuously presented")
    median_fps = statistics.median(samples)
    if median_fps < minimum_median_fps:
        raise RuntimeError(
            f"median {median_fps:.2f} FPS is below the required {minimum_median_fps:.2f} FPS"
        )
    return samples


def parse_compute(
    lines: list[str], expected_skin_models: int = 0, expected_vfx_emitters: int = 0
) -> tuple[int, int, int, int, int, int, int]:
    marker = next((match for line in lines if (match := COMPUTE.search(line))), None)
    if marker is None:
        raise RuntimeError("the app never emitted CY_IOS_COMPUTE after presenting the combined scene")
    values = tuple(int(marker.group(index)) for index in range(1, 9))
    models, skin_vertices, skin_bones, emitters, capacity, dispatches, instances, readback = values
    if min(models, skin_vertices, skin_bones, emitters, capacity, dispatches, instances) <= 0:
        raise RuntimeError("the combined compute marker contains an empty workload")
    if expected_skin_models and models != expected_skin_models:
        raise RuntimeError(
            f"the run reported {models} skin models; expected {expected_skin_models}"
        )
    if expected_vfx_emitters and emitters != expected_vfx_emitters:
        raise RuntimeError(
            f"the run reported {emitters} VFX emitters; expected {expected_vfx_emitters}"
        )
    if instances != capacity:
        raise RuntimeError("GPU particle instance count must cover the device-resident capacity")
    if readback != 0:
        raise RuntimeError("the presentation path enabled CPU particle readback")
    return models, skin_vertices, skin_bones, emitters, capacity, dispatches, instances


def parse_measurement(
    lines: list[str], minimum_samples: int, minimum_median_fps: float = 0.0,
    expected_skin_models: int = 0, expected_vfx_emitters: int = 0,
) -> Measurement:
    """Require the platform, Metal, quality, and measured-frame evidence contracts."""
    if not any(CONTRACT in line for line in lines):
        raise RuntimeError("the app never emitted the iOS platform contract marker")
    backend, gpu, ready_width, ready_height = parse_ready(lines)
    quality = parse_quality(lines)
    if (quality.drawable_width, quality.drawable_height) != (ready_width, ready_height):
        raise RuntimeError("CY_IOS_READY and CY_IOS_QUALITY disagree about drawable dimensions")
    models, skin_vertices, skin_bones, emitters, capacity, dispatches, instances = parse_compute(
        lines, expected_skin_models, expected_vfx_emitters
    )
    samples = parse_fps(lines, minimum_samples, minimum_median_fps)
    return Measurement(
        backend=backend,
        gpu=gpu,
        width=quality.drawable_width,
        height=quality.drawable_height,
        native_width=quality.native_width,
        native_height=quality.native_height,
        scale=quality.scale,
        terrain_octaves=quality.terrain_octaves,
        march_steps=quality.march_steps,
        skin_models=models,
        skin_vertices=skin_vertices,
        skin_bones=skin_bones,
        vfx_emitters=emitters,
        vfx_capacity=capacity,
        vfx_dispatches=dispatches,
        gpu_particle_instances=instances,
        fps=samples,
    )


def devicectl(*arguments: str, capture: bool = False) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["xcrun", "devicectl", *arguments],
        check=True,
        text=True,
        capture_output=capture,
    )


def discover(requested: str) -> Device:
    listed = devicectl("list", "devices", "--json-output", "-", capture=True)
    return select_physical_iphone(json.loads(listed.stdout), requested)


def collect_console(
    process: subprocess.Popen[str], received: queue.Queue[Optional[str]], seconds: float
) -> list[str]:
    lines: list[str] = []
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            line = received.get(timeout=min(0.25, max(0.01, deadline - time.monotonic())))
        except queue.Empty:
            if process.poll() is not None:
                break
            continue
        if line is None:
            break
        print(line, flush=True)
        lines.append(line)
    while not received.empty():
        line = received.get_nowait()
        if line is not None:
            print(line, flush=True)
            lines.append(line)
    return lines


def stop_process(process: subprocess.Popen[str], reader: threading.Thread) -> None:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
    reader.join(timeout=1)


def measure(
    device: Device,
    bundle_id: str,
    seconds: float,
    minimum_samples: int,
    minimum_median_fps: float,
    expected_skin_models: int,
    expected_vfx_emitters: int,
    screenshot: Path,
    log: Path,
) -> Measurement:
    command = [
        "xcrun", "devicectl", "device", "process", "launch",
        "--device", device.selector,
        "--console", "--terminate-existing", bundle_id,
    ]
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    received: queue.Queue[Optional[str]] = queue.Queue()
    def read_console() -> None:
        assert process.stdout is not None
        for line in process.stdout:
            received.put(line.rstrip())
        received.put(None)

    reader = threading.Thread(target=read_console, daemon=True)
    reader.start()
    try:
        lines = collect_console(process, received, seconds)
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text("\n".join(lines) + "\n", encoding="utf-8")
        measurement = parse_measurement(
            lines, minimum_samples, minimum_median_fps,
            expected_skin_models, expected_vfx_emitters,
        )
        if process.poll() is not None:
            raise RuntimeError("the app exited before its physical display could be captured")
        screenshot.parent.mkdir(parents=True, exist_ok=True)
        devicectl(
            "device", "capture", "screenshot", "--device", device.selector,
            "--destination", str(screenshot.resolve()),
        )
        if not screenshot.is_file() or screenshot.stat().st_size < 10_000:
            raise RuntimeError("physical-device screenshot is missing or implausibly small")
        return measurement
    finally:
        stop_process(process, reader)


def write_evidence(path: Path, screenshot: Path, device: Device, measurement: Measurement) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    relative_screenshot = os.path.relpath(screenshot, path.parent)
    samples = ", ".join(f"{sample:.2f}" for sample in measurement.fps)
    path.write_text(
        "\n".join(
            [
                "# Physical iPhone Metal validation",
                "",
                f"- Captured: {datetime.now(timezone.utc).isoformat(timespec='seconds')}",
                f"- Hardware: {device.model}",
                f"- OS: iOS {device.os_version}",
                f"- Backend: `{measurement.backend}`",
                f"- GPU reported by RHI: `{measurement.gpu}`",
                f"- Native display: {measurement.native_width} × {measurement.native_height}",
                f"- Render drawable: {measurement.width} × {measurement.height}",
                f"- Linear render scale: {measurement.scale:.3f}",
                f"- Terrain quality: {measurement.terrain_octaves} octaves, "
                f"{measurement.march_steps} maximum march steps",
                f"- GPU skinning: {measurement.skin_models} models, "
                f"{measurement.skin_vertices} vertices, {measurement.skin_bones} shared animated bones",
                f"- GPU VFX: {measurement.vfx_emitters} independent emitters, "
                f"{measurement.vfx_capacity} device-resident slots, "
                f"{measurement.vfx_dispatches} simulation dispatches, "
                f"{measurement.gpu_particle_instances} fixed-capacity draw instances",
                "- CPU particle readback: disabled",
                f"- FPS samples: {samples}",
                f"- Median FPS: {statistics.median(measurement.fps):.2f}",
                f"- Range: {min(measurement.fps):.2f}–{max(measurement.fps):.2f} FPS",
                "- Contract: platform `ios`, one fullscreen window, desktop operations rejected, "
                "Metal surface, four touch events",
                "",
                f"![iOS Metal compute presentation on {device.model}]({relative_screenshot})",
                "",
            ]
        ),
        encoding="utf-8",
    )


def self_test() -> int:
    physical = {
        "result": {"devices": [{
            "identifier": "core-device-id",
            "properties": {
                "connection": {"state": "connected"},
                "hardware": {"reality": "physical", "platform": "iOS", "udid": "PHONE", "marketingName": "iPhone 16"},
                "software": {"osVersionNumber": {"stringValue": "27.0"}},
                "state": {"name": "Test iPhone"},
            },
        }]},
    }
    assert select_physical_iphone(physical, "PHONE").model == "iPhone 16"
    simulated = json.loads(json.dumps(physical))
    simulated["result"]["devices"][0]["properties"]["hardware"]["reality"] = "simulated"
    try:
        select_physical_iphone(simulated, "PHONE")
        raise AssertionError("a simulator was accepted as hardware evidence")
    except RuntimeError as error:
        assert "not a physical iPhone" in str(error)
    good = [
        CONTRACT,
        "CY_IOS_READY backend=metal device=Apple A18 GPU size=1534x707",
        "CY_IOS_QUALITY native=2556x1179 drawable=1534x707 scale=0.600 terrain_octaves=4 march_steps=64",
        "CY_IOS_COMPUTE skin_models=500 skin_vertices=18000 skin_bones=5 vfx_emitters=100 "
        "vfx_capacity=51200 vfx_dispatches=400 gpu_particle_instances=51200 "
        "cpu_particle_readback=0",
        "CY_IOS_FPS fps=59.80 frames=60 seconds=1.003",
        "CY_IOS_FPS fps=60.00 frames=60 seconds=1.000",
        "CY_IOS_FPS fps=59.90 frames=60 seconds=1.002",
    ]
    parsed = parse_measurement(good, 3, 55.0, 500, 100)
    assert parsed.fps == (59.8, 60.0, 59.9)
    assert parsed.scale == 0.6 and parsed.terrain_octaves == 4 and parsed.march_steps == 64
    assert parsed.skin_models == 500 and parsed.skin_vertices == 18000
    assert parsed.vfx_emitters == 100 and parsed.vfx_dispatches == 400
    broken_runs = (
        good[1:],
        [line for line in good if "CY_IOS_QUALITY" not in line],
        [line for line in good if "CY_IOS_COMPUTE" not in line],
        [line.replace("skin_models=500", "skin_models=499") for line in good],
        [line.replace("vfx_emitters=100", "vfx_emitters=99") for line in good],
        [line.replace("skin_vertices=18000", "skin_vertices=0") for line in good],
        [line.replace("cpu_particle_readback=0", "cpu_particle_readback=1") for line in good],
        [line.replace("drawable=1534x707", "drawable=2556x1179") for line in good],
        [line.replace("scale=0.600", "scale=1.000") for line in good],
        [line.replace("backend=metal", "backend=null") for line in good],
        [line.replace("fps=59.80", "fps=20.00").replace("fps=60.00", "fps=20.00")
              .replace("fps=59.90", "fps=20.00") for line in good],
    )
    for broken in broken_runs:
        try:
            parse_measurement(broken, 3, 55.0, 500, 100)
            raise AssertionError("invalid device evidence was accepted")
        except RuntimeError:
            pass
    print("iOS physical-device runner self-test PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--device", help="physical iPhone name, CoreDevice identifier, or UDID")
    parser.add_argument("--app", type=Path)
    parser.add_argument("--bundle-id")
    parser.add_argument("--seconds", type=float, default=12.0)
    parser.add_argument("--minimum-samples", type=int, default=8)
    parser.add_argument("--minimum-median-fps", type=float, default=55.0)
    parser.add_argument("--expected-skin-models", type=int, default=0)
    parser.add_argument("--expected-vfx-emitters", type=int, default=0)
    parser.add_argument("--screenshot", type=Path)
    parser.add_argument("--log", type=Path)
    parser.add_argument("--evidence", type=Path)
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    required = ("device", "app", "bundle_id", "screenshot", "log", "evidence")
    missing = [name for name in required if getattr(args, name) is None]
    if missing:
        parser.error("required for a device run: " + ", ".join(f"--{name.replace('_', '-')}" for name in missing))
    if args.seconds < 3.0 or args.minimum_samples < 1:
        parser.error("--seconds must be at least 3 and --minimum-samples must be positive")
    if not args.app.is_dir():
        parser.error(f"app bundle does not exist: {args.app}")

    device = discover(args.device)
    print(f"physical device: {device.name} — {device.model}, iOS {device.os_version}")
    devicectl("device", "install", "app", "--device", args.device, str(args.app))
    measurement = measure(
        device,
        args.bundle_id,
        args.seconds,
        args.minimum_samples,
        args.minimum_median_fps,
        args.expected_skin_models,
        args.expected_vfx_emitters,
        args.screenshot,
        args.log,
    )
    write_evidence(args.evidence, args.screenshot, device, measurement)
    print(f"iOS physical-device PASS: median {statistics.median(measurement.fps):.2f} FPS")
    print(f"screenshot: {args.screenshot}")
    print(f"evidence: {args.evidence}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
