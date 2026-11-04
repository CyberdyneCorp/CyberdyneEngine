#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draw the replicated session from `cy_net_session_trace`'s own output. Issue #82.

    build/<profile>/src/networking/tools/cy_net_session_trace trace.csv 400
    python3 src/networking/tools/session_video.py trace.csv session.mp4 still.png

Three views of one world side by side — the dedicated server's and two clients', each client
behind a 200 ms round trip with 5 % loss and reordering — and under them each client's mean
distance from the server's positions. Every dot is a position the engine held at that moment: the
figure is ENGINE OUTPUT, not a diagram, and says so on its face. The still is the frame halfway
through the wandering phase.
"""

import collections
import csv
import math
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.animation as animation  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402


def read(path):
    frames = collections.OrderedDict()
    phases = {}
    with open(path, newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            time = int(row["time_ms"])
            view = int(row["view"])
            frames.setdefault(time, {}).setdefault(view, {})[int(row["id"])] = (
                float(row["x"]),
                float(row["y"]),
            )
            phases[time] = int(row["phase"])
    return frames, phases


def mean_error(server, client):
    distances = [
        math.dist(server[key], client[key]) for key in server.keys() & client.keys()
    ]
    return sum(distances) / len(distances) if distances else 0.0


def main(trace, video, still):
    frames, phases = read(trace)
    times = list(frames.keys())
    errors = {
        view: [mean_error(frames[t].get(0, {}), frames[t].get(view, {})) for t in times]
        for view in (1, 2)
    }
    rest_start = next((t for t in times if phases[t] == 1), times[-1])

    figure = plt.figure(figsize=(12.8, 7.2), dpi=100)
    figure.suptitle(
        "CyberNet: a dedicated server and two clients — 200 ms round trip, 5 % loss, 5 % "
        "reordering — engine output, not a diagram",
        fontsize=12,
        fontweight="bold",
    )
    grid = figure.add_gridspec(2, 3, height_ratios=[3, 1.2])
    titles = ["server (authoritative)", "client 0 (replica)", "client 1 (replica)"]
    colours = ["#1f4e79", "#2e7d32", "#b35c00"]
    panels = []
    for view in range(3):
        axis = figure.add_subplot(grid[0, view])
        axis.set_title(titles[view])
        axis.set_xlim(-430, -230)
        axis.set_ylim(-430, -230)
        axis.set_aspect("equal")
        axis.set_xticks([])
        axis.set_yticks([])
        panels.append(axis.scatter([], [], s=9, color=colours[view]))
    chart = figure.add_subplot(grid[1, :])
    for view in (1, 2):
        chart.plot([t / 1000 for t in times], errors[view], color=colours[view],
                   label=f"client {view - 1}: mean distance from the server's position")
    chart.axvline(rest_start / 1000, color="grey", linestyle="--")
    chart.text(rest_start / 1000 + 0.05, max(max(errors[1]), 0.1) * 0.15, "the world comes to rest",
               color="grey")
    chart.set_xlabel("seconds")
    chart.set_ylabel("metres")
    chart.legend(loc="upper left", fontsize=8)
    cursor = chart.axvline(0, color="black")
    label = figure.text(0.5, 0.36, "", ha="center", fontsize=10)

    def draw(index):
        time = times[index]
        for view in range(3):
            points = list(frames[time].get(view, {}).values())
            panels[view].set_offsets(points if points else [[math.nan, math.nan]])
        cursor.set_xdata([time / 1000, time / 1000])
        phase = "wandering: the clients trail the server by half a round trip" if phases[time] == 0 \
            else "at rest: every snapshot arrives and the replicas equal the server"
        label.set_text(
            f"t = {time / 1000:4.1f} s   {phase}   mean error {errors[1][index]:.2f} m / "
            f"{errors[2][index]:.2f} m"
        )
        return panels + [cursor, label]

    draw(len([t for t in times if t < rest_start]) // 2)
    figure.savefig(still)
    writer = animation.FFMpegWriter(fps=30, bitrate=2400)
    animation.FuncAnimation(figure, draw, frames=len(times), blit=False).save(video, writer=writer)


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit("usage: session_video.py <trace.csv> <video.mp4> <still.png>")
    main(sys.argv[1], sys.argv[2], sys.argv[3])
