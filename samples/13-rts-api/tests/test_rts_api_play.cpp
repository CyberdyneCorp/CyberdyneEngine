// SPDX-License-Identifier: MIT
// integration.rts_api_play — `--play`, the battle a person plays, driven by a written hand.
//
// The host runs as a separate process with `--play --headless`: SDL's event subsystem and no
// window. Every input is a real SDL event the script pushes into SDL's queue
// (`cy::Sdl3EventInjector`), read by the platform's `Sdl3InputSource` into the engine's input
// server — the door a keyboard and a mouse come through — and every decision about it is the Swift
// game's (game/Skirmish.swift). So every assertion here is about what the game did with a person's
// input:
//
//   a box and a right click     the box picked the six swordsmen it was dragged round and the game
//                               outlined them; the right click on a red unit became six attack
//                               commands in the session's log, one a unit, each unit's own group,
//                               and the six went for that unit
//   a camera scroll             D held for thirty frames moved the camera's focus along its own
//                               right, by the distance the game's pan speed says, and not across
//   the log replays             the recorded command log, fed to a fresh session with no player
//                               and no opponent, ends in the digest the played session ended in —
//                               and the same log with one order's target moved by 2^-32 of a metre
//                               does not, so the comparison can tell
//   two sessions agree          the same hand twice writes the same log
//   pause and the menu          P holds the session for as long as it is down, frames still run;
//   Esc
//                               opens the menu, its Resume lets the session go on and its Quit ends
//                               it — the game asks, the host stops
//
// The scripted run without `--play` and its committed digests are `integration.rts_api_sample`'s,
// unchanged.

#include <cy/test/test.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "input_script.h"
#include "letterbox.h"
#include "process.h"

namespace {

[[nodiscard]] std::string line_with(const std::string& output, const char* marker) {
    const std::string::size_type found = output.find(marker);
    if (found == std::string::npos) {
        return {};
    }
    const std::string::size_type end = output.find('\n', found);
    return output.substr(found, end == std::string::npos ? end : end - found);
}

[[nodiscard]] double number_after(const std::string& line, const char* key) {
    const std::string::size_type found = line.find(key);
    if (found == std::string::npos) {
        return -1e30;
    }
    return std::strtod(line.c_str() + found + std::char_traits<char>::length(key), nullptr);
}

[[nodiscard]] std::string word_after(const std::string& line, const char* key) {
    const std::string::size_type found = line.find(key);
    if (found == std::string::npos) {
        return {};
    }
    const std::string::size_type start = found + std::char_traits<char>::length(key);
    return line.substr(start, line.find_first_of(" \n", start) - start);
}

[[nodiscard]] std::string quoted(const std::string& path) {
    return "\"" + path + "\"";
}

[[nodiscard]] std::string script(const char* name) {
    return std::string(CY_RTS_API_PLAY_DIR) + "/" + name;
}

[[nodiscard]] std::string scratch(const char* name) {
    return std::string(CY_TEST_BINARY_DIR) + "/rts-api-play-" + name;
}

[[nodiscard]] cy::test::smoke::ProcessResult play(const std::string& arguments) {
    return cy::test::smoke::run(quoted(CY_SAMPLE_RTS_API) + " --play --headless " + arguments);
}

/// One unit's `unit:health:stance:target kind:target` from the `rts units` line.
struct UnitState {
    unsigned unit = 0;
    unsigned health = 0;
    unsigned stance = 0;
    unsigned target_kind = 0;
    unsigned target = 0;
};

/// Numbers separated by `separator`, as many as `out` has room for; false when the text holds
/// fewer, or anything else.
[[nodiscard]] bool numbers(const std::string& text, char separator, long long* out, int count) {
    const char* cursor = text.c_str();
    for (int index = 0; index < count; ++index) {
        char* end = nullptr;
        out[index] = std::strtoll(cursor, &end, 10);
        const char expected = index + 1 < count ? separator : '\0';
        if (end == cursor || *end != expected) {
            return false;
        }
        cursor = end + (index + 1 < count ? 1 : 0);
    }
    return true;
}

[[nodiscard]] std::vector<UnitState> units(const std::string& output) {
    std::vector<UnitState> out;
    std::istringstream words(line_with(output, "rts units"));
    std::string word;
    while (words >> word) {
        long long fields[5] = {};
        if (numbers(word, ':', fields, 5)) {
            out.push_back(
                UnitState{static_cast<unsigned>(fields[0]), static_cast<unsigned>(fields[1]),
                          static_cast<unsigned>(fields[2]), static_cast<unsigned>(fields[3]),
                          static_cast<unsigned>(fields[4])});
        }
    }
    return out;
}

/// The `order` lines of a recorded log: tick, kind, group, x, z.
struct Order {
    unsigned long long tick = 0;
    unsigned kind = 0;
    unsigned group = 0;
    long long x = 0;
    long long z = 0;
};

[[nodiscard]] std::vector<Order> orders(const std::string& path) {
    std::vector<Order> out;
    std::ifstream file(path);
    std::string line;
    constexpr const char* kOrder = "order ";
    while (std::getline(file, line)) {
        long long fields[5] = {};
        if (line.starts_with(kOrder) && numbers(line.substr(6), ' ', fields, 5)) {
            out.push_back(Order{static_cast<unsigned long long>(fields[0]),
                                static_cast<unsigned>(fields[1]), static_cast<unsigned>(fields[2]),
                                fields[3], fields[4]});
        }
    }
    return out;
}

[[nodiscard]] std::string read_all(const std::string& path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

/// Blue's swordsmen the box was dragged round, by unit: `select-attack.input`'s.
const std::set<unsigned> kBoxed = {2, 3, 4, 7, 8, 9};
/// Red's first swordsman, the one right-clicked.
constexpr unsigned kTarget = 24;
/// The session's ATTACK order and stance.
constexpr unsigned kAttack = 3;
/// game/SkirmishCamera.swift: the camera starts 38 metres out and pans at 0.75 of that a second;
/// D is held for thirty frames of a sixtieth.
constexpr double kPan = 0.75 * 38.0 * 30.0 / 60.0;

}  // namespace

CY_TEST_CASE("rts api play: a box picks the swordsmen and a right click orders them to attack") {
    const std::string log = scratch("select-attack.log");
    const auto run = play("--input " + quoted(script("select-attack.input")) + " --frames 120" +
                          " --record " + quoted(log));
    CY_REQUIRE(run.ran);
    CY_REQUIRE_EQ(run.exit_code, 0);
    CY_TEST_INFO(run.output);

    const std::string session = line_with(run.output, "rts play");
    CY_CHECK_EQ(word_after(session, "ended='"), "frames'");
    CY_CHECK(number_after(session, "injected=") > 0.0);
    CY_CHECK_EQ(number_after(session, "missed_aims="), 0.0);

    // The box: six units picked, six outlined by the engine's selection component.
    const std::string skirmish = line_with(run.output, "rts skirmish");
    CY_CHECK_EQ(number_after(skirmish, "selected="), 6.0);
    CY_CHECK_EQ(number_after(skirmish, "outlined="), 6.0);
    // The right click: one attack command a unit, and nothing else from the player.
    CY_CHECK_EQ(number_after(skirmish, "attacks="), 6.0);
    CY_CHECK_EQ(number_after(skirmish, "moves="), 0.0);
    CY_CHECK_EQ(number_after(skirmish, "attack_moves="), 0.0);

    // The commands, as the session's log holds them: six ATTACKs on one tick, to the six units'
    // own groups, at one point — red's first swordsman's.
    std::set<unsigned> attacked;
    unsigned long long tick = 0;
    long long x = 0;
    for (const Order& order : orders(log)) {
        if (order.kind != kAttack) {
            continue;
        }
        CY_CHECK(attacked.insert(order.group).second);
        tick = attacked.size() == 1 ? order.tick : tick;
        x = attacked.size() == 1 ? order.x : x;
        CY_CHECK_EQ(order.tick, tick);
        CY_CHECK_EQ(order.x, x);
    }
    CY_CHECK(attacked == kBoxed);

    // And the state those commands put the units in: the six are going for red's first swordsman,
    // and no other blue unit was ordered.
    for (const UnitState& unit : units(run.output)) {
        if (kBoxed.contains(unit.unit)) {
            CY_CHECK_EQ(unit.stance, kAttack);
            CY_CHECK_EQ(unit.target, kTarget);
        } else if (unit.unit < kTarget) {
            CY_CHECK_NE(unit.stance, kAttack);
        }
    }

    // The scroll: the focus moved along the camera's right — world +z, the camera looking east —
    // by the pan, and not along its view.
    const double start_x = -8.0 + 14.0;
    const double start_z = -52.0 + 9.0;
    const std::string focus = word_after(skirmish, "focus=");
    char* comma = nullptr;
    const double focus_x = std::strtod(focus.c_str(), &comma);
    CY_REQUIRE(*comma == ',');
    const double focus_z = std::strtod(comma + 1, nullptr);
    CY_CHECK(std::fabs(focus_x - start_x) < 0.05);
    CY_CHECK(std::fabs((focus_z - start_z) - kPan) < 0.05);
}

CY_TEST_CASE("rts api play: the recorded command log replays to the same digest, and only it") {
    const std::string log = scratch("replay.log");
    const auto played = play("--input " + quoted(script("select-attack.input")) + " --frames 240" +
                             " --record " + quoted(log));
    CY_REQUIRE(played.ran);
    CY_REQUIRE_EQ(played.exit_code, 0);
    const std::string recorded = line_with(played.output, "rts battle");
    const std::vector<Order> given = orders(log);
    // The player's six, and the opponent's: red was thinking too.
    CY_REQUIRE(given.size() > 6U);
    const double opponent = number_after(line_with(played.output, "rts skirmish"), "opponent=");
    CY_CHECK_EQ(static_cast<double>(given.size()), 6.0 + opponent);

    // The log alone, in a fresh process: no player's hand and no opponent's orders.
    const auto replayed = play("--replay " + quoted(log));
    CY_REQUIRE(replayed.ran);
    CY_REQUIRE_EQ(replayed.exit_code, 0);
    CY_TEST_INFO(replayed.output);
    const std::string replay = line_with(replayed.output, "rts replay");
    CY_CHECK_EQ(number_after(replay, "match="), 1.0);
    CY_CHECK_EQ(word_after(replay, "digest="), word_after(recorded, "digest="));
    CY_CHECK_EQ(word_after(replay, "follower="), word_after(recorded, "follower="));
    const std::string skirmish = line_with(replayed.output, "rts skirmish");
    CY_CHECK_EQ(number_after(skirmish, "replay="), 1.0);
    CY_CHECK_EQ(number_after(skirmish, "attacks="), 0.0);
    CY_CHECK_EQ(number_after(skirmish, "opponent="), 0.0);

    // THE CONTROL: one order's target moved by the smallest step a `Fixed` has. A comparison that
    // matched whatever it was given would pass here too; this one does not.
    std::string text = read_all(log);
    const Order first = given.front();
    char before[96];
    char after[96];
    std::snprintf(before, sizeof(before), "order %llu %u %u %lld ", first.tick, first.kind,
                  first.group, first.x);
    std::snprintf(after, sizeof(after), "order %llu %u %u %lld ", first.tick, first.kind,
                  first.group, first.x + 1);
    const std::string::size_type at = text.find(before);
    CY_REQUIRE(at != std::string::npos);
    text.replace(at, std::char_traits<char>::length(before), after);
    const std::string tampered = scratch("tampered.log");
    {
        std::ofstream out(tampered);
        out << text;
    }
    const auto moved = play("--replay " + quoted(tampered));
    CY_REQUIRE(moved.ran);
    CY_REQUIRE_EQ(moved.exit_code, 0);
    CY_CHECK_EQ(number_after(line_with(moved.output, "rts replay"), "match="), 0.0);
}

CY_TEST_CASE("rts api play: the same hand twice writes the same command log") {
    const std::string first = scratch("twice-a.log");
    const std::string second = scratch("twice-b.log");
    for (const std::string& log : {first, second}) {
        const auto run = play("--input " + quoted(script("select-attack.input")) +
                              " --frames 180 --record " + quoted(log));
        CY_REQUIRE(run.ran);
        CY_REQUIRE_EQ(run.exit_code, 0);
    }
    CY_CHECK(!read_all(first).empty());
    CY_CHECK_EQ(read_all(first), read_all(second));
}

CY_TEST_CASE("rts api play: P holds the session, and the Esc menu resumes it and quits") {
    const auto run = play("--input " + quoted(script("pause-menu.input")) + " --frames 400");
    CY_REQUIRE(run.ran);
    CY_REQUIRE_EQ(run.exit_code, 0);
    CY_TEST_INFO(run.output);
    const std::string session = line_with(run.output, "rts play");
    // The menu's Quit ended it, long before the frames ran out.
    CY_CHECK_EQ(word_after(session, "ended='"), "game");
    const double frames = number_after(session, "frames=");
    CY_CHECK(frames < 200.0);
    const std::string skirmish = line_with(run.output, "rts skirmish");
    CY_CHECK_EQ(number_after(skirmish, "pauses="), 2.0);
    CY_CHECK_EQ(number_after(skirmish, "menus="), 2.0);
    // Every frame the game held is a frame with no fixed step. A request is honoured from the frame
    // after the one that made it, and the last frame's — the menu open, asking to hold — had no
    // frame after it: the ticks are the frames less one fewer than the game held.
    const double held = number_after(skirmish, "paused_frames=");
    CY_CHECK(held >= 60.0 + 10.0);
    CY_CHECK_EQ(number_after(session, "ticks="), frames - (held - 1.0));
}

CY_TEST_CASE("rts api play: the letterbox keeps the view's aspect, centred, in any window") {
    using sample::rts::letterbox;
    // The same aspect: the whole window.
    const auto same = letterbox(1280.0F, 720.0F, 1920.0F, 1080.0F);
    CY_CHECK_EQ(same.x, 0.0F);
    CY_CHECK_EQ(same.y, 0.0F);
    CY_CHECK_EQ(same.width, 1920.0F);
    CY_CHECK_EQ(same.height, 1080.0F);
    // Taller than the view: bars above and below, the width filled.
    const auto tall = letterbox(1280.0F, 720.0F, 1920.0F, 1200.0F);
    CY_CHECK_EQ(tall.x, 0.0F);
    CY_CHECK_EQ(tall.width, 1920.0F);
    CY_CHECK_EQ(tall.height, 1080.0F);
    CY_CHECK_EQ(tall.y, 60.0F);
    // Wider: bars to the sides.
    const auto wide = letterbox(1280.0F, 720.0F, 2560.0F, 720.0F);
    CY_CHECK_EQ(wide.y, 0.0F);
    CY_CHECK_EQ(wide.height, 720.0F);
    CY_CHECK_EQ(wide.x, 640.0F);
    // No area, no box: a minimised window is not divided by.
    CY_CHECK_EQ(letterbox(1280.0F, 720.0F, 0.0F, 720.0F).width, 0.0F);
}

CY_TEST_CASE("rts api play: an input script's lines, and the ones it refuses") {
    sample::rts::ScriptEvent event;
    bool blank = false;
    CY_REQUIRE(sample::rts::parse_script_line("12 key ctrl down  # Ctrl", event, blank));
    CY_CHECK_FALSE(blank);
    CY_CHECK_EQ(event.frame, 12U);
    CY_CHECK(event.kind == sample::rts::ScriptEvent::Kind::Key);
    CY_CHECK(event.key == cy::input::Key::LeftControl);
    CY_CHECK(event.down);
    CY_REQUIRE(sample::rts::parse_script_line("40 aim unit 9 16 -16", event, blank));
    CY_CHECK(event.kind == sample::rts::ScriptEvent::Kind::AimUnit);
    CY_CHECK_EQ(event.unit, 9U);
    CY_CHECK_EQ(event.y, -16.0F);
    CY_REQUIRE(sample::rts::parse_script_line("41 aim unit 9", event, blank));
    CY_CHECK_EQ(event.x, 0.0F);
    CY_REQUIRE(sample::rts::parse_script_line("   # only a comment", event, blank));
    CY_CHECK(blank);
    // Refused: an unknown key, a button that is not one, a word too many, a verb nobody knows,
    // an aim at nothing and a window with no area.
    for (const char* line : {"1 key banana down", "1 press thumb", "1 move 3 4 5", "1 jump",
                             "1 aim", "1 resize 0 600"}) {
        CY_CHECK_FALSE(sample::rts::parse_script_line(line, event, blank));
    }
}
