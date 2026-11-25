// Tests for emotes and pings (lichii/hook/coop_emote.cpp).
#include <doctest/doctest.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "coop_dbno_body.h"
#include "coop_emote.h"
#include "coop_emote_body.h"
#include "coop_link_sound.h"
#include "coop_puppet.h"
#include "coop_puppet_sound.h"

using CoopEmote::Command;
using CoopEmote::Holds;
using CoopEmote::Limiter;

TEST_CASE("emote: the table has unique ids and names, all with a game command") {
    const auto table = CoopEmote::Table();
    REQUIRE(!table.empty());
    for (std::size_t i = 0; i < table.size(); ++i) {
        CHECK(table[i].id >= 1);
        CHECK(table[i].id <= coop::protocol::kMaxEmoteId);
        CHECK(std::string(table[i].command) != "");
        CHECK(table[i].hold_ms > 0);
        CHECK(table[i].hold_ms <= CoopEmote::kMaxHoldMs);
        for (std::size_t j = i + 1; j < table.size(); ++j) {
            CHECK(table[i].id != table[j].id);
            CHECK(std::string(table[i].name) != table[j].name);
            CHECK(std::string(table[i].name) != table[j].alias);
        }
        CHECK(CoopEmote::ById(table[i].id) == &table[i]);
    }
    CHECK(CoopEmote::ById(0) == nullptr);
    CHECK(CoopEmote::ById(200) == nullptr);
}

TEST_CASE("emote: Parse takes the names, /emote <name> and /emote, and leaves chat and other commands") {
    CHECK(!CoopEmote::Parse("hello"));
    CHECK(!CoopEmote::Parse("/"));
    CHECK(!CoopEmote::Parse("/pvp on"));
    CHECK(!CoopEmote::Parse("/tp Link"));
    CHECK(!CoopEmote::Parse("/help"));
    CHECK(!CoopEmote::Parse("/wavey"));

    const auto wave = CoopEmote::Parse("/wave");
    REQUIRE(wave);
    CHECK(wave->kind == Command::Kind::play);
    CHECK(wave->def == CoopEmote::ByName("wave"));

    const auto same = CoopEmote::Parse("  /WAVE  ");
    REQUIRE(same);
    CHECK(same->def == wave->def);

    const auto alias = CoopEmote::Parse("/hi");
    REQUIRE(alias);
    CHECK(alias->def == wave->def);

    const auto via = CoopEmote::Parse("/emote here");
    REQUIRE(via);
    CHECK(via->kind == Command::Kind::play);
    CHECK(via->def == CoopEmote::ByName("here"));

    const auto list = CoopEmote::Parse("/emote");
    REQUIRE(list);
    CHECK(list->kind == Command::Kind::list);

    const auto bad = CoopEmote::Parse("/emote dance");
    REQUIRE(bad);
    CHECK(bad->kind == Command::Kind::unknown);
    CHECK(bad->name == "dance");
}

TEST_CASE("emote: the list line names every emote") {
    const std::string line = CoopEmote::ListLine();
    for (const auto& d : CoopEmote::Table()) {
        CHECK(line.find(std::string("/") + d.name) != std::string::npos);
    }
}

TEST_CASE("emote: the sender is limited to one per cooldown") {
    Limiter limiter;
    CHECK(limiter.Allow(10'000));
    CHECK(!limiter.Allow(10'000 + CoopEmote::kCooldownMs - 1));
    CHECK(limiter.Allow(10'000 + CoopEmote::kCooldownMs));
    CHECK(!limiter.Allow(10'000 + CoopEmote::kCooldownMs + 1));
}

TEST_CASE("emote: a hold plays the emote until it runs out") {
    Holds holds;
    const auto* wave = CoopEmote::ByName("wave");
    REQUIRE(holds.Start(2, wave->id, 1000));
    CHECK(holds.Apply(2, "Wait", 1000) == wave);
    CHECK(holds.Apply(2, "Wait", 1000 + wave->hold_ms - 1) == wave);
    CHECK(holds.Apply(2, "Wait", 1000 + wave->hold_ms) == nullptr);
    CHECK(holds.Apply(2, "Wait", 1000 + wave->hold_ms + 1) == nullptr);  // gone, not restarted
}

TEST_CASE("emote: the hold ends when the sender's animation changes") {
    Holds holds;
    const auto* wave = CoopEmote::ByName("wave");
    REQUIRE(holds.Start(3, wave->id, 0));
    CHECK(holds.Apply(3, "Wait", 10) == wave);
    CHECK(holds.Apply(3, "Wait", 20) == wave);
    CHECK(holds.Apply(3, "Run", 30) == nullptr);
    CHECK(holds.Apply(3, "Wait", 40) == nullptr);  // stays ended
}

TEST_CASE("emote: holds ignore bad ids and numbers, and are per player") {
    Holds holds;
    CHECK(!holds.Start(2, 0, 0));
    CHECK(!holds.Start(2, 250, 0));
    CHECK(!holds.Start(0, 1, 0));
    CHECK(!holds.Start(200, 1, 0));
    CHECK(holds.Apply(2, "Wait", 0) == nullptr);
    CHECK(holds.Apply(200, "Wait", 0) == nullptr);

    REQUIRE(holds.Start(2, 1, 0));
    CHECK(holds.Apply(3, "Wait", 0) == nullptr);  // another player: nothing
    CHECK(holds.Apply(2, "Wait", 0) != nullptr);
}

TEST_CASE("emote: a new emote replaces the old one, and Clear and ClearAll end them (a player left, a new connection)") {
    Holds holds;
    REQUIRE(holds.Start(2, 1, 0));
    CHECK(holds.Apply(2, "Wait", 0) == CoopEmote::ById(1));
    REQUIRE(holds.Start(2, 2, 100));
    CHECK(holds.Apply(2, "Wait", 100) == CoopEmote::ById(2));
    holds.Clear(2);
    CHECK(holds.Apply(2, "Wait", 100) == nullptr);

    REQUIRE(holds.Start(2, 1, 0));
    REQUIRE(holds.Start(5, 1, 0));
    holds.ClearAll();
    CHECK(holds.Apply(2, "Wait", 0) == nullptr);
    CHECK(holds.Apply(5, "Wait", 0) == nullptr);
}

TEST_CASE("emote: a late joiner has nothing: a fresh Holds plays nothing") {
    Holds late;
    for (std::uint8_t n = 1; n <= coop::protocol::kMaxPlayers; ++n) {
        CHECK(late.Apply(n, "Wait", 5000) == nullptr);
    }
}

TEST_CASE("emote: only a call is read in the others' chat, with the sender's name") {
    const auto* here = CoopEmote::ByName("here");
    REQUIRE(here != nullptr);
    CHECK(CoopEmote::HeardLine(*here, "Saria", 2) == "Saria calls: over here");
    CHECK(CoopEmote::HeardLine(*here, "  Saria ", 2) == "Saria calls: over here");
    CHECK(CoopEmote::HeardLine(*here, "", 3) == "Player 3 calls: over here");   // no name known yet
    for (const auto& d : CoopEmote::Table()) {
        if (&d != here) {
            CHECK(CoopEmote::HeardLine(d, "Saria", 2).empty());   // a gesture is seen, not read
        }
    }
}

// --- Our own Link's gesture and the Navis' calls (coop_emote_body.h) ---

namespace {

class Guest : public CoopPuppet::Guest {
public:
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        auto* o = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = bytes.find(address + i);
            o[i] = it == bytes.end() ? std::uint8_t{0} : it->second;
        }
        return true;
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        const auto* b = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < size; ++i) {
            bytes[address + i] = b[i];
        }
        return true;
    }
    template <typename T>
    void Put(std::uint64_t address, T value) {
        Write(address, &value, sizeof value);
    }
    void PutText(std::uint64_t address, const std::string& text) { Write(address, text.c_str(), text.size() + 1); }
    std::string TextAt(std::uint64_t address) {
        std::string out;
        for (char c = 0; Read(address + out.size(), &c, 1) && c != 0;) {
            out += c;
        }
        return out;
    }
    template <typename T>
    T Get(std::uint64_t address) {
        T value{};
        Read(address, &value, sizeof value);
        return value;
    }
    std::map<std::uint64_t, std::uint8_t> bytes;
};

constexpr std::uint64_t kMain = 0x80000000;
constexpr std::uint64_t kBlock = 0x9000;
constexpr std::uint64_t kLink = 0x50000;
constexpr std::uint64_t kNavi = 0x70000;

// `actor`'s AS slot 0 is on `command`, as the game keeps it.
void PutSlot0(Guest& g, std::uint64_t actor, const std::string& command) {
    namespace pa = CoopPuppet::addr;
    std::uint64_t at = actor;
    std::uint64_t next = actor + 0x1000;
    for (const std::uint64_t step : pa::kControllerPath) {
        g.Put(at + step, next);
        at = next;
        next += 0x400;
    }
    g.Put(at + pa::kSlotCount, std::int32_t{1});
    g.Put(at + pa::kSlots, next);
    g.Put(next, next + 0x400);
    const std::uint64_t slot = next + 0x400;
    g.Put(slot + pa::kSlotName, slot + 0x200);
    g.PutText(slot + 0x200, command);
    g.Put(slot + pa::kSlotFrame, 0.0f);
    g.Put(slot + CoopDbno::Body::addr::kAsSlotRate, 1.0f);
}

// `actor` has a sound user named `name`.
std::uint64_t PutSoundUser(Guest& g, std::uint64_t actor, const std::string& name) {
    namespace pa = CoopPuppetSound::addr;
    const std::uint64_t parts = actor + 0x4000;
    const std::uint64_t xlink = actor + 0x5000;
    const std::uint64_t user = actor + 0x6000;
    const std::uint64_t resource = actor + 0x7000;
    g.Put(actor + pa::kActorParts, parts);
    g.Put(parts + pa::kPartsXLink, xlink);
    g.Put(xlink + pa::kXLinkSoundUser, user);
    g.Put(user + pa::kUserFirstLink, user + pa::kUserEvents);  // an empty event list
    g.Put(user + pa::kUserResource, resource);
    g.Put(resource + pa::kResourceName, resource + 0x100);
    g.PutText(resource + 0x100, name);
    g.Put(resource + CoopLinkSound::addr::kResourceAccessor, resource + 0x200);
    return user;
}

// Frames of the caller, each game call answered as the game would; `log` says what was called.
struct Game {
    Guest guest;
    CoopEmote::Body body;
    std::string slot0 = "Wait";
    bool can_play = true;  // false: the AS has no such command
    std::set<std::string> sounds{"Voice_LockOnEnemy_Hey"};
    std::vector<std::string> log;
    std::vector<std::uint64_t> emitted;  // the users

    void Frame(std::uint64_t now_ms) {
        PutSlot0(guest, kLink, slot0);
        auto call = body.Next(guest, false, 0, now_ms, kMain);
        for (int i = 0; call && i < 16; ++i) {
            std::uint64_t result = 0;
            const std::uint64_t f = call->function - kMain;
            if (f == CoopPuppet::addr::kMalloc) {
                log.push_back("malloc");
                result = kBlock;
            } else if (f == CoopPuppet::addr::kPlayAs) {
                CHECK(guest.Get<std::uint64_t>(call->x[0]) == kLink);
                CHECK(guest.Get<std::int32_t>(call->x[0] + 0x10) == 0);
                CHECK(guest.Get<float>(call->x[0] + 0x14) == 1.0f);
                const std::string name = guest.TextAt(guest.Get<std::uint64_t>(call->x[0] + 8));
                log.push_back("play " + name);
                if (can_play) {
                    slot0 = name;
                }
            } else if (f == CoopLinkSound::addr::kSearchAsset) {
                const std::string name = guest.TextAt(call->x[2]);
                log.push_back("search " + name);
                if (sounds.contains(name)) {
                    guest.Put(call->x[1], std::uint64_t{0x1234});
                    result = 1;
                }
            } else if (f == CoopLinkSound::addr::kEmit) {
                log.push_back("emit");
                CHECK(guest.Get<std::uint64_t>(call->x[1]) == 0x1234);
                emitted.push_back(call->x[0]);
            } else {
                FAIL("an unknown call");
            }
            call = body.Next(guest, true, result, now_ms, kMain);
        }
        PutSlot0(guest, kLink, slot0);
    }
    std::vector<std::string> Take() { return std::exchange(log, {}); }
};

using Log = std::vector<std::string>;

}  // namespace

TEST_CASE("emote body: co-op idle or alone, nothing asked: no game call, no memory") {
    Game g;
    g.body.SetOur({kLink, false});
    for (std::uint64_t t = 0; t < 10'000; t += 16) {
        g.Frame(t);
    }
    CHECK(g.log.empty());
    CHECK(g.body.TakeNotes().empty());
}

TEST_CASE("emote body: our standing Link plays the gesture once, and waits again at its end") {
    Game g;
    const auto* wave = CoopEmote::ByName("wave");
    g.body.SetOur({kLink, false});
    g.body.Play(*wave);
    g.Frame(1000);
    CHECK(g.Take() == Log{"malloc", "play EvtTalkHello"});
    for (std::uint64_t t = 1016; t < 1000 + wave->hold_ms; t += 16) {
        g.Frame(t);
    }
    CHECK(g.log.empty());  // once
    g.Frame(1000 + wave->hold_ms);
    CHECK(g.Take() == Log{"play Wait"});
    g.Frame(1100 + wave->hold_ms);
    CHECK(g.log.empty());
}

TEST_CASE("emote body: the game's own animation is never cut") {
    const auto* wave = CoopEmote::ByName("wave");
    Game g;
    g.body.SetOur({kLink, false});
    SUBCASE("he moves during it") {
        g.body.Play(*wave);
        g.Frame(1000);
        g.Take();
        g.slot0 = "Run";
        for (std::uint64_t t = 1016; t < 6000; t += 16) {
            g.Frame(t);
        }
        CHECK(g.log.empty());
    }
    SUBCASE("he runs when asked, for too long: dropped, not played late") {
        g.slot0 = "Run";
        g.body.Play(*wave);
        for (std::uint64_t t = 1000; t < 2000; t += 16) {
            g.Frame(t);
        }
        g.slot0 = "Wait";
        g.Frame(2000);
        CHECK(g.Take() == Log{"malloc"});
    }
    SUBCASE("an event, or downed") {
        g.body.SetOur({kLink, true});
        g.body.Play(*wave);
        for (std::uint64_t t = 1000; t < 2000; t += 16) {
            g.Frame(t);
        }
        CHECK(g.Take() == Log{"malloc"});
    }
    SUBCASE("an event takes him during it: not set back") {
        g.body.Play(*wave);
        g.Frame(1000);
        g.Take();
        g.body.SetOur({kLink, true});
        g.Frame(1000 + wave->hold_ms);
        CHECK(g.log.empty());
    }
    SUBCASE("another Link (a scene, an age change)") {
        g.body.Play(*wave);
        g.Frame(1000);
        g.Take();
        g.body.SetOur({0, false});
        g.Frame(1016);
        g.body.SetOur({kLink, false});
        g.Frame(1000 + wave->hold_ms);
        CHECK(g.log.empty());
    }
    SUBCASE("no Link at all") {
        g.body.SetOur({});
        g.body.Play(*wave);
        g.Frame(1000);
        g.Frame(2000);
        CHECK(g.Take() == Log{"malloc"});
    }
}

TEST_CASE("emote body: a command his animation set lacks is tried once") {
    Game g;
    g.can_play = false;
    g.body.SetOur({kLink, false});
    g.body.Play(*CoopEmote::ByName("wow"));
    for (std::uint64_t t = 1000; t < 6000; t += 16) {
        g.Frame(t);
    }
    CHECK(g.Take() == Log{"malloc", "play EvtSurprise"});
}

TEST_CASE("emote body: a second gesture during the first keeps what he was on before") {
    Game g;
    g.body.SetOur({kLink, false});
    g.body.Play(*CoopEmote::ByName("wave"));
    g.Frame(1000);
    g.body.Play(*CoopEmote::ByName("yes"));
    g.Frame(2500);
    g.Frame(2500 + CoopEmote::ByName("yes")->hold_ms);
    CHECK(g.Take() == Log{"malloc", "play EvtTalkHello", "play EvtTalkYes", "play Wait"});
}

TEST_CASE("emote body: only a call has a voice, on the caller's Navi") {
    const auto* here = CoopEmote::ByName("here");
    CHECK(std::string(here->navi) == "Voice_LockOnEnemy_Hey");
    Game g;
    const std::uint64_t user = PutSoundUser(g.guest, kNavi, "Dm_Npc_Navi");
    int asked_for = -1;
    g.body.SetNaviActor([&](CoopPuppet::Guest&, std::uint8_t number) {
        asked_for = number;
        return kNavi;
    });
    for (const auto& d : CoopEmote::Table()) {
        if (&d != here) {
            CHECK(std::string(d.navi).empty());
            g.body.Say(2, d, 1000);
        }
    }
    g.Frame(1000);
    CHECK(g.Take().empty());

    g.body.Say(3, *here, 1000);
    g.Frame(1016);
    CHECK(g.Take() == Log{"malloc", "search Voice_LockOnEnemy_Hey", "emit"});
    CHECK(g.emitted == std::vector<std::uint64_t>{user});
    CHECK(asked_for == 3);
    g.Frame(1032);
    CHECK(g.Take().empty());  // once
    CHECK(g.body.TakeNotes() == Log{"call: P3's Navi says 'Voice_LockOnEnemy_Hey' (sound user 'Dm_Npc_Navi')"});

    g.body.Say(0, *here, 3000);  // our own
    g.Frame(3000);
    CHECK(g.Take() == Log{"search Voice_LockOnEnemy_Hey", "emit"});
    CHECK(asked_for == 0);
}

TEST_CASE("emote body: a Navi that is not here or lacks the sound is silent, and holds up no other") {
    const auto* here = CoopEmote::ByName("here");
    Game g;
    PutSoundUser(g.guest, kNavi, "Dm_Npc_Navi");
    std::uint64_t second = 0;
    g.body.SetNaviActor([&](CoopPuppet::Guest&, std::uint8_t number) { return number == 2 ? second : kNavi; });
    g.body.Say(2, *here, 1000);  // not here yet
    g.body.Say(3, *here, 1000);
    g.Frame(1000);
    CHECK(g.Take() == Log{"malloc", "search Voice_LockOnEnemy_Hey", "emit"});
    SUBCASE("she comes in time") {
        second = kNavi;
        g.Frame(1500);
        CHECK(g.Take() == Log{"search Voice_LockOnEnemy_Hey", "emit"});
    }
    SUBCASE("she does not") {
        g.Frame(1000 + CoopEmote::Body::kVoiceWaitMs);
        second = kNavi;
        g.Frame(1100 + CoopEmote::Body::kVoiceWaitMs);
        CHECK(g.Take().empty());
    }
    SUBCASE("no such sound") {
        second = kNavi;
        g.sounds.clear();
        g.Frame(1500);
        CHECK(g.Take() == Log{"search Voice_LockOnEnemy_Hey"});
    }
}

TEST_CASE("emote: a hold is kept, not started again, while the sender's own Link shows the gesture") {
    Holds holds;
    const auto* wave = CoopEmote::ByName("wave");
    REQUIRE(holds.Start(2, wave->id, 1000));
    CHECK(holds.Apply(2, "Wait", 1000) == wave);
    CHECK(holds.Apply(2, wave->command, 1100) == wave);                        // their Link plays it
    CHECK(holds.Apply(2, wave->command, 1000 + wave->hold_ms + 200) == wave);  // the samples trail
    CHECK(holds.Apply(2, "Wait", 1000 + wave->hold_ms + 216) == nullptr);      // theirs ended: so does it
    CHECK(holds.Apply(2, wave->command, 1000 + wave->hold_ms + 232) == nullptr);

    REQUIRE(holds.Start(3, wave->id, 1000));
    CHECK(holds.Apply(3, wave->command, 1000 + CoopEmote::kMaxHoldMs - 1) == wave);
    CHECK(holds.Apply(3, wave->command, 1000 + CoopEmote::kMaxHoldMs) == nullptr);  // never longer
}
