#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""samples/16-menus/tools/session.py — write the editor session that authors the menus.

The menus' documents are made in the editor: every widget is placed, named, classed, laid out and
bound by the UI layout tools (`ui.document.*`, `ui.widget.*`, `ui.theme.save`), the same commands
the UI Layout panel invokes. This writes that session, one `tools/call` a line, to
`project/ui/session.jsonl`; `cy-editor-mcp`'s `the_menus_sample_is_authored_by_the_ui_layout_tools`
replays it over MCP into a fresh project and holds every file it produces to the committed one.
Run it after changing the menus, then the test with `CY_MENUS_UPDATE=1` to write the documents.
"""

from __future__ import annotations

import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
CALLS: list[dict] = []


def call(tool: str, **arguments) -> None:
    CALLS.append({"name": tool, "arguments": arguments})


class Document:
    """One document being authored: every call names it."""

    def __init__(self, reference: str, root_id: str, root_classes: str = "screen") -> None:
        self.reference = reference
        call("ui.document.create", reference=reference)
        call("ui.document.stylesheet", reference=reference, sheet="ui/menus.cyss")
        call("ui.widget.identity", reference=reference, widget="#root", id=root_id,
             classes=root_classes)

    def add(self, kind: str, parent: str, id: str = "", text: str = "") -> str:
        arguments = {"reference": self.reference, "kind": kind, "parent": parent}
        if id:
            arguments["id"] = id
        if text:
            arguments["text"] = text
        call("ui.widget.add", **arguments)
        return f"#{id}" if id else ""

    def set(self, widget: str, key: str, value: str) -> None:
        call("ui.widget.set", reference=self.reference, widget=widget, key=key, value=value)

    def classes(self, widget: str, id: str, classes: str) -> None:
        call("ui.widget.identity", reference=self.reference, widget=widget, id=id,
             classes=classes)

    def centre(self, widget: str, width: float, height: float) -> None:
        call("ui.widget.layout", reference=self.reference, widget=widget,
             anchor=[0.5, 0.5, 0.5, 0.5],
             offset=[-width / 2, -height / 2, width / 2, height / 2])

    def bind(self, widget: str, name: str, mode: str = "two-way") -> None:
        call("ui.widget.bind", reference=self.reference, widget=widget, name=name, mode=mode)


def sheets() -> None:
    for name, source in (("ui/menus.cyss", "menus.cyss"),
                         ("ui/themes/high-contrast.cyss", "high-contrast.cyss"),
                         ("ui/themes/ember.cyss", "ember.cyss")):
        call("ui.theme.save", reference=name, source=(HERE / source).read_text())


def main_menu() -> None:
    d = Document("ui/main_menu.cyui", "main-menu")
    menu = d.add("column", "#main-menu", id="menu")
    d.classes(menu, "menu", "menu-panel")
    d.centre(menu, 380, 330)
    d.add("label", menu, id="title", text="CYBERDYNE")
    d.classes("#title", "title", "title")
    d.add("label", menu, id="subtitle", text="Menus made in the editor")
    d.classes("#subtitle", "subtitle", "subtitle")
    for button_id, caption, extra in (("new-game", "New game", "primary"),
                                      ("continue", "Continue", ""),
                                      ("settings", "Settings", ""),
                                      ("quit", "Quit", "danger")):
        d.add("button", menu, id=button_id, text=caption)
        d.classes(f"#{button_id}", button_id, f"menu-button {extra}".strip())
    d.add("label", menu, id="main-hint", text="Arrows or D-pad to move - Enter or A to choose")
    d.classes("#main-hint", "main-hint", "hint")
    d.set("#continue", "tooltip", '"Pick up where the last session saved"')


def name_label(d: Document, row_id: str, text: str) -> None:
    """A row's first column: the setting's name, at the column's width."""
    label_id = f"{row_id}-name"
    d.add("label", f"#{row_id}", id=label_id, text=text)
    d.classes(f"#{label_id}", label_id, "setting-name")


def settings() -> None:
    d = Document("ui/settings.cyui", "settings-screen")
    panel = d.add("column", "#settings-screen", id="settings")
    d.classes(panel, "settings", "menu-panel")
    d.centre(panel, 660, 500)
    d.add("label", panel, id="settings-title", text="Settings")
    d.classes("#settings-title", "settings-title", "title")
    tabs = d.add("tabs", panel, id="settings-tabs")
    d.set(tabs, "flex-grow", "1")

    graphics = d.add("column", tabs, id="graphics")
    d.set(graphics, "title", '"Graphics"')
    d.set(graphics, "gap", "8")
    for row_id, label, widget_id, items, selected in (
            ("resolution-row", "Resolution", "resolution",
             '["1280x720", "1920x1080", "2560x1440"]', "0"),
            ("quality-row", "Quality preset", "quality",
             '["Low", "Medium", "High", "Ultra"]', "2"),
            ("upscaler-row", "Upscaler", "upscaler",
             '["Native", "Quality", "Balanced", "Performance"]', "0"),
            ("gi-row", "Global illumination", "gi-quality",
             '["Low", "Medium", "High", "Epic"]', "2")):
        d.add("row", graphics, id=row_id)
        d.classes(f"#{row_id}", row_id, "setting")
        name_label(d, row_id, label)
        d.add("dropdown", f"#{row_id}", id=widget_id)
        d.set(f"#{widget_id}", "items", items)
        d.set(f"#{widget_id}", "selected", selected)
    d.add("row", graphics, id="fullscreen-row")
    d.classes("#fullscreen-row", "fullscreen-row", "setting")
    name_label(d, "fullscreen-row", "Fullscreen")
    d.add("toggle", "#fullscreen-row", id="fullscreen")

    audio = d.add("column", tabs, id="audio")
    d.set(audio, "title", '"Audio"')
    d.set(audio, "gap", "8")
    for bus, label in (("master", "Master"), ("music", "Music"), ("sfx", "Effects"),
                       ("voice", "Voice")):
        d.add("row", audio, id=f"{bus}-row")
        d.classes(f"#{bus}-row", f"{bus}-row", "setting")
        name_label(d, f"{bus}-row", label)
        d.add("slider", f"#{bus}-row", id=f"volume-{bus}")
        d.set(f"#volume-{bus}", "min", "0")
        d.set(f"#volume-{bus}", "max", "1")
        d.set(f"#volume-{bus}", "step", "0.05")
        d.set(f"#volume-{bus}", "value", "1")
        d.bind(f"#volume-{bus}", f"volume.{bus}")

    controls = d.add("column", tabs, id="controls")
    d.set(controls, "title", '"Controls"')
    d.set(controls, "gap", "6")
    d.add("row", controls, id="controls-header")
    d.classes("#controls-header", "controls-header", "setting")
    name_label(d, "controls-header", "Action")
    for column in ("Keyboard", "Gamepad"):
        d.add("label", "#controls-header", id=f"column-{column.lower()}", text=column)
        d.classes(f"#column-{column.lower()}", f"column-{column.lower()}", "setting-column")
    for action, label in (("jump", "Jump"), ("interact", "Interact"),
                          ("inventory", "Inventory"), ("sprint", "Sprint")):
        d.add("row", controls, id=f"{action}-row")
        d.classes(f"#{action}-row", f"{action}-row", "setting")
        name_label(d, f"{action}-row", label)
        for scheme in ("keyboard", "gamepad"):
            d.add("button", f"#{action}-row", id=f"bind-{action}-{scheme}", text="-")
            d.classes(f"#bind-{action}-{scheme}", f"bind-{action}-{scheme}", "binding")
    d.add("button", controls, id="reset-bindings", text="Reset to defaults")

    actions = d.add("row", panel, id="settings-actions")
    d.set(actions, "gap", "8")
    d.set(actions, "justify-content", "end")
    d.add("button", actions, id="apply", text="Apply")
    d.classes("#apply", "apply", "primary")
    d.add("button", actions, id="back", text="Back")

    dialog = d.add("dialog", "#settings-screen", id="rebind-dialog")
    d.set(dialog, "title", '"Rebind"')
    d.add("label", dialog, id="rebind-prompt", text="Press a key or a button")
    d.add("label", dialog, id="rebind-hint", text="Escape or Back cancels")
    d.classes("#rebind-hint", "rebind-hint", "hint")


def pause() -> None:
    d = Document("ui/pause.cyui", "pause-screen", root_classes="overlay")
    panel = d.add("column", "#pause-screen", id="pause")
    d.classes(panel, "pause", "menu-panel")
    d.centre(panel, 340, 330)
    d.add("label", panel, id="pause-title", text="Paused")
    d.classes("#pause-title", "pause-title", "title")
    for button_id, caption, extra in (("resume", "Resume", "primary"),
                                      ("open-inventory", "Inventory", ""),
                                      ("pause-settings", "Settings", ""),
                                      ("to-main-menu", "Main menu", "")):
        d.add("button", panel, id=button_id, text=caption)
        d.classes(f"#{button_id}", button_id, f"menu-button {extra}".strip())


def inventory() -> None:
    d = Document("ui/inventory.cyui", "inventory-screen", root_classes="overlay")
    panel = d.add("column", "#inventory-screen", id="inventory")
    d.classes(panel, "inventory", "menu-panel")
    d.centre(panel, 640, 350)
    d.add("label", panel, id="inventory-title", text="Inventory")
    d.classes("#inventory-title", "inventory-title", "title")
    body = d.add("row", panel, id="inventory-body")
    d.set(body, "gap", "18")
    grid = d.add("grid", body, id="slots")
    d.set(grid, "columns", "6")
    d.set(grid, "gap", "6")
    for index in range(18):
        d.add("button", grid, id=f"slot-{index}")
        d.classes(f"#slot-{index}", f"slot-{index}", "slot")
    details = d.add("column", body, id="details")
    d.classes(details, "details", "details")
    d.add("label", details, id="item-name", text="Nothing held")
    d.classes("#item-name", "item-name", "item-name")
    d.add("rich-text", details, id="item-description", text="Choose a slot to see what is in it.")
    d.add("label", panel, id="inventory-hint",
          text="Drag an item, or Enter / A to pick it up and again to put it down")
    d.classes("#inventory-hint", "inventory-hint", "hint")
    actions = d.add("row", panel, id="inventory-actions")
    d.set(actions, "justify-content", "end")
    d.add("button", actions, id="inventory-back", text="Back")


def playing() -> None:
    d = Document("ui/playing.cyui", "playing-screen", root_classes="")
    hud = d.add("column", "#playing-screen", id="hud")
    d.classes(hud, "hud", "menu-panel")
    call("ui.widget.layout", reference=d.reference, widget="#hud", anchor=[0, 0, 0, 0],
         offset=[24, 24, 344, 104])
    d.add("label", hud, id="hud-title", text="Playing")
    d.classes("#hud-title", "hud-title", "item-name")
    d.add("label", hud, id="hud-hint", text="Escape or Start pauses")
    d.classes("#hud-hint", "hud-hint", "hint")


def main() -> int:
    sheets()
    main_menu()
    settings()
    pause()
    inventory()
    playing()
    out = HERE.parent / "project" / "ui" / "session.jsonl"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("".join(json.dumps(entry, separators=(",", ":")) + "\n" for entry in CALLS))
    print(f"{len(CALLS)} calls -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
