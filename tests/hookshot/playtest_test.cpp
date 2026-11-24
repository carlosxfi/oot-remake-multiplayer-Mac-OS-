// SPDX-License-Identifier: GPL-3.0-or-later
// The play-test bridge's game-free part (hookshot/src/coop_playtest_core.h): the frame hold driven by a fake frame
// source (a thread calling AtFrame as the game's calc thread does), and every command's checks.
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

#include "coop_playtest_core.h"

using namespace CoopPlaytest;
using namespace std::chrono_literals;

namespace {

// The game's calc thread: a frame hook about every millisecond until stopped.
struct FakeGame {
    Gate& gate;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> updates{0};  // frames whose update ran (past the hook)
    std::thread thread;
    explicit FakeGame(Gate& g) : gate(g) {
        thread = std::thread([this] {
            while (!stop.load()) {
                gate.AtFrame();
                updates.fetch_add(1);
                std::this_thread::sleep_for(1ms);
            }
        });
    }
    ~FakeGame() {
        stop.store(true);
        gate.Resume();
        thread.join();
    }
};

std::optional<Request> Parse(std::string_view line) {
    std::string error;
    return ParseRequest(line, error);
}

std::string ParseError(std::string_view line) {
    std::string error;
    CHECK_FALSE(ParseRequest(line, error));
    return error;
}

}  // namespace

TEST_CASE("playtest: the port and idle switches: off unless a real port is given") {
    CHECK_FALSE(PortFrom(""));
    CHECK_FALSE(PortFrom("0"));
    CHECK_FALSE(PortFrom("80"));
    CHECK_FALSE(PortFrom("65536"));
    CHECK_FALSE(PortFrom("5001x"));
    CHECK_FALSE(PortFrom("-5001"));
    CHECK(PortFrom("5001") == 5001);
    CHECK(PortFrom("5012") == 5012);
    CHECK(IdleFrom("") == kDefaultIdleS);
    CHECK(IdleFrom("abc") == kDefaultIdleS);
    CHECK(IdleFrom("-1") == kDefaultIdleS);
    CHECK(IdleFrom("0") == 0);
    CHECK(IdleFrom("30") == 30);
}

TEST_CASE("playtest: pause holds the game still, resume lets it run") {
    Gate gate(0);
    FakeGame game(gate);
    REQUIRE(gate.Pause(2s));
    CHECK(gate.Held());
    const auto frame = gate.Frame();
    const auto updates = game.updates.load();
    std::this_thread::sleep_for(100ms);
    CHECK(gate.Frame() == frame);          // no frame hook passed
    CHECK(game.updates.load() == updates);  // no update ran
    gate.Resume();
    std::this_thread::sleep_for(50ms);
    CHECK(gate.Frame() > frame);
    CHECK_FALSE(gate.Held());
}

TEST_CASE("playtest: step N runs exactly N frames and holds again, from a hold or from running") {
    Gate gate(0);
    FakeGame game(gate);
    for (const std::uint32_t n : {1u, 2u, 7u, 60u}) {
        std::uint64_t from = 0;
        REQUIRE(gate.Step(n, 2s, &from));
        CHECK(gate.Held());
        CHECK(gate.Frame() - from == n);
        // and nothing slips after the step: still there a while later
        const auto at = gate.Frame();
        const auto updates = game.updates.load();
        std::this_thread::sleep_for(30ms);
        CHECK(gate.Frame() == at);
        CHECK(game.updates.load() == updates);
    }
    gate.Resume();
    std::this_thread::sleep_for(20ms);
    std::uint64_t from = 0;
    REQUIRE(gate.Step(5, 2s, &from));  // (from running: held first, then 5)
    CHECK(gate.Frame() - from == 5);
}

TEST_CASE("playtest: a job runs on the frame thread inside the hold, no frame passing") {
    Gate gate(0);
    FakeGame game(gate);
    REQUIRE(gate.Pause(2s));
    const auto frame = gate.Frame();
    std::thread::id ran_on;
    REQUIRE(gate.RunHeld([&] { ran_on = std::this_thread::get_id(); }, 2s));
    CHECK(ran_on == game.thread.get_id());
    CHECK(gate.Frame() == frame);
    CHECK(gate.Held());
}

TEST_CASE("playtest: the hold ends when the client leaves (Resume) and when the idle time runs out") {
    {
        Gate gate(0);
        FakeGame game(gate);
        REQUIRE(gate.Pause(2s));
        gate.Resume();  // (what the bridge does when its client closes or crashes)
        std::this_thread::sleep_for(30ms);
        CHECK_FALSE(gate.Holding());
        CHECK_FALSE(gate.Held());
    }
    {
        Gate gate(0.1);
        FakeGame game(gate);
        REQUIRE(gate.Pause(2s));
        std::this_thread::sleep_for(400ms);
        CHECK_FALSE(gate.Held());
        CHECK(gate.TakeEndNote() == "idle");
        CHECK(gate.TakeEndNote().empty());
    }
}

TEST_CASE("playtest: no frame source (a load) answers in time instead of hanging") {
    Gate gate(0);
    CHECK_FALSE(gate.Pause(50ms));
    CHECK_FALSE(gate.Step(3, 50ms));
    CHECK_FALSE(gate.RunHeld([] {}, 50ms));
}

TEST_CASE("playtest: the stick's angle and strength as Longshot's axes") {
    auto near = [](Axes a, double x, double y) { return std::fabs(a.x - x) < 1e-6 && std::fabs(a.y - y) < 1e-6; };
    CHECK(near(StickAxes(0, 1), 0, 1));     // up
    CHECK(near(StickAxes(90, 1), 1, 0));    // right
    CHECK(near(StickAxes(180, 0.5), 0, -0.5));
    CHECK(near(StickAxes(-90, 1), -1, 0));
    CHECK(near(StickAxes(45, 2), std::sqrt(0.5), std::sqrt(0.5)));  // strength capped at 1
    CHECK(near(StickAxes(NAN, 1), 0, 0));
}

TEST_CASE("playtest: every command parses, ids come back as given") {
    for (const char* line : {R"({"cmd":"hello"})", R"({"cmd":"state","id":"s1"})", R"({"cmd":"pause"})",
                             R"({"cmd":"resume"})"}) {
        CHECK(Parse(line));
    }
    auto r = Parse(R"({"id":7,"cmd":"step","frames":60})");
    REQUIRE(r);
    CHECK(r->frames == 60);
    CHECK(Reply(r->id, 10, true, "").rfind("{\"id\":7,\"ok\":true,\"frame\":10,\"paused\":true", 0) == 0);
    CHECK(Parse(R"({"cmd":"step"})")->frames == 1);

    r = Parse(R"({"cmd":"input","buttons":["A","ZR"],"stick":[90,1.0],"frames":20})");
    REQUIRE(r);
    CHECK(r->buttons.size() == 2);
    CHECK(r->stick);
    CHECK_FALSE(r->rstick);
    CHECK(r->exclusive);
    const std::string c = ControlInput(*r, 3);
    CHECK(c.find("\"cmd\":\"sequence\"") != std::string::npos);
    CHECK(c.find("\"frames\":20") != std::string::npos);
    CHECK(c.find("\"buttons\":[\"A\",\"ZR\"]") != std::string::npos);
    CHECK(c.find("\"lx\":1.000000,\"ly\":0.000000") != std::string::npos);
    CHECK(c.find("\"wait\":false") != std::string::npos);
    CHECK(c.find("\"exclusive\":true") != std::string::npos);
    // (nothing given: still a step, both sticks centred, so Longshot does not refuse it as empty)
    CHECK(ControlInput(*Parse(R"({"cmd":"input","exclusive":false})"), 4).find("\"lx\":0.000000") != std::string::npos);

    r = Parse(R"({"cmd":"warp","scene":"KokiriForest_Child","entrance":"Kokiri_Start","timeout":30})");
    REQUIRE(r);
    CHECK(r->scene == "KokiriForest_Child");
    CHECK(r->entrance == "Kokiri_Start");
    CHECK(r->timeout_s == 30);

    r = Parse(R"({"cmd":"flag","name":"ForestTemple_Room18_Picture_Shot","value":1})");
    REQUIRE(r);
    CHECK(r->value == "1");
    CHECK(Parse(R"({"cmd":"flag","name":"0x1A2B","value":false})")->value == "0");
    CHECK(Parse(R"({"cmd":"flag","name":"Quest.Step","value":"Done"})")->value == "Done");
    CHECK(Parse(R"({"cmd":"flag","name":"X","value":-3})")->value == "-3");

    CHECK(Parse(R"({"cmd":"give","item":"bombs"})")->item == "bombs");
    CHECK(Parse(R"({"cmd":"dump","tag":"after_chest-2"})")->tag == "after_chest-2");
    CHECK(Parse(R"({"cmd":"shot"})")->tag.empty());
    CHECK(Parse(R"({"cmd":"shot","name":"door_open"})")->tag == "door_open");
}

TEST_CASE("playtest: bad requests are refused with a reason, never acted on") {
    CHECK(ParseError("not json").find("JSON") != std::string::npos);
    CHECK(ParseError("[1,2]").find("object") != std::string::npos);
    CHECK(ParseError(R"({"id":1})").find("cmd") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"format_disk"})").find("cmd") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"step","frames":0})").find("frames") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"step","frames":3601})").find("frames") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"step","frames":1.5})").find("frames") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"step","frames":"10"})").find("frames") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"input","buttons":["A","HOME"]})").find("buttons") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"input","buttons":"A"})").find("buttons") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"input","stick":[90]})").find("stick") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"input","stick":[90,1.5]})").find("stick") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"input","rstick":["up",1]})").find("rstick") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"input","exclusive":1})").find("exclusive") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"warp"})").find("scene") != std::string::npos);
    // (a test command line is built from these: no spaces, ';' or '#' that would make a second step)
    CHECK(ParseError(R"({"cmd":"warp","scene":"A B"})").find("scene") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"warp","scene":"A;give bombs"})").find("scene") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"warp","scene":"A","entrance":"x\ngive"})").find("entrance") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"warp","scene":"A","timeout":0})").find("timeout") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"flag","name":"X"})").find("value") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"flag","name":"X","value":1.5})").find("value") != std::string::npos);
    // (past int32: refused here, never handed to the game's thread; review-01111-b)
    CHECK(ParseError(R"({"cmd":"flag","name":"X","value":2147483648})").find("value") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"flag","name":"X","value":2150000000})").find("value") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"flag","name":"X","value":-2147483649})").find("value") != std::string::npos);
    CHECK(Parse(R"({"cmd":"flag","name":"X","value":2147483647})")->value == "2147483647");
    CHECK(Parse(R"({"cmd":"flag","name":"X","value":-2147483648})")->value == "-2147483648");
    CHECK(ParseError(R"({"cmd":"flag","name":"X Y","value":1})").find("name") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"give"})").find("item") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"give","item":"bombs#x"})").find("item") != std::string::npos);
    // dump and shot name files only inside the bridge's own folder (review-01111-b 1)
    CHECK(ParseError(R"({"cmd":"dump"})").find("tag") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"dump","tag":"../x"})").find("tag") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"dump","tag":"C:\\x"})").find("tag") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"shot","path":"/tmp/x.png"})").find("path") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"shot","name":"../../x"})").find("name") != std::string::npos);
    CHECK_FALSE(ValidTag(std::string(65, 'a')));
    CHECK(ParseError(std::string(kMaxLine + 1, ' ')).find("64 KB") != std::string::npos);
}

TEST_CASE("playtest: every reply is one line: nested values written compact, strings escaped") {
    // (the JSON text has the escape \n inside a string: raw strings keep the backslash)
    const auto v = CoopModJson::Parse(R"({"a":[1, 2.5, {"b":"x\ny"}], "c":{"d":true,"e":null}})");
    REQUIRE(v);
    const std::string c = Compact(*v);
    CHECK(c == R"({"a":[1,2.5,{"b":"x\ny"}],"c":{"d":true,"e":null}})");
    CHECK(c.find('\n') == std::string::npos);
    CHECK(Quote("two\nlines") == R"("two\nlines")");
    const std::string r = Reply(*v, 3, false, "\"state\":" + c);
    CHECK(r.find('\n') == std::string::npos);
    CHECK(CoopModJson::Parse(r));
    CHECK(ErrorReply(*v, 3, true, "failed", "a\nb").find('\n') == std::string::npos);
    CHECK(Reply(CoopModJson::Value::Of(7.0), 10, true, "") == R"({"id":7,"ok":true,"frame":10,"paused":true})");
}

TEST_CASE("playtest: an error reply carries the frame, the hold and a reason") {
    const std::string e = ErrorReply(CoopModJson::Value{}, 42, false, "busy", "another client \"x\"");
    CHECK(e.find("\"id\":null") != std::string::npos);
    CHECK(e.find("\"ok\":false") != std::string::npos);
    CHECK(e.find("\"frame\":42") != std::string::npos);
    CHECK(e.find("\"paused\":false") != std::string::npos);
    CHECK(e.find("\"error\":\"busy\"") != std::string::npos);
    CHECK(CoopModJson::Parse(e));  // valid JSON, the quote escaped
}
