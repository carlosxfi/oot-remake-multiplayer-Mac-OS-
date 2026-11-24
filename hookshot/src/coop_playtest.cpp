// SPDX-License-Identifier: GPL-3.0-or-later
// The play-test bridge in the game (coop_playtest.h; docs/playtest.md, "Bridge protocol").
//
// One thread of its own: it listens on 127.0.0.1:<LICHII_COOP_PLAYTEST_PORT>, serves one client at a time, and
// answers each request in order. What it does through others:
// - the hold: the Gate (coop_playtest_core.h), which the game's calc thread waits in at the frame hook;
// - input, shots and Link's snapshot: Longshot's own control bridge (HOS_CONTROL), as a client of it, the
//   controller's own path (an input lands at the game's controller read, the frame after it is sent);
// - warp, flag, give: the module's test commands (CoopTestInput::PlayNow), the game's own sequence and flag
//   setters and the cheats panel's give, on the game's calc thread inside the hold;
// - dump: the shared flags (CoopTestInput::SharedFlags) and the state, as one JSON file in <game>/playtest/dumps.
// A client that leaves (closed, crashed) lets a held game go at once.
#include "coop_playtest.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>


#include "common/logging.h"
#include "coop_cheats.h"
#include "coop_hook.h"
#include "coop_playtest_core.h"
#include "coop_testinput.h"
#include "core/core.h"

// (the platform's sockets and pipe after our own headers, as coop_lan_net.cpp: no Windows macro reaches them)
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;
#endif

namespace CoopPlaytest {
namespace {

using namespace std::chrono_literals;
using CoopModJson::Value;

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_stop{false};
std::unique_ptr<Gate> g_gate;
std::thread g_thread;
Core::System* g_system = nullptr;
std::filesystem::path g_dir;  // <game>/playtest
SocketHandle g_listener = kNoSocket;

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

void CloseSocket(SocketHandle s) {
    if (s == kNoSocket) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

bool SendAll(SocketHandle s, const std::string& text) {
    std::size_t sent = 0;
    while (sent < text.size()) {
#ifdef _WIN32
        const int n = send(s, text.data() + sent, static_cast<int>(text.size() - sent), 0);
#else
        const ssize_t n = send(s, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
#endif
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// 1: readable, 0: not within ms, -1: error.
int WaitReadable(SocketHandle s, int ms) {
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    const int r = select(0, &set, nullptr, nullptr, &tv);
#else
    pollfd p{s, POLLIN, 0};
    const int r = poll(&p, 1, ms);
#endif
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}

// ---- Longshot's control bridge, as its client ----

class Control {
public:
    ~Control() { Close(); }

    bool Open() {
        if (Connected()) return true;
        const std::string where = Env("HOS_CONTROL");
        if (where.empty()) return false;
#ifdef _WIN32
        const std::string name = where.rfind("\\\\.\\pipe\\", 0) == 0 ? where : "\\\\.\\pipe\\" + where;
        pipe_ = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe_ == INVALID_HANDLE_VALUE) return false;
#else
        sock_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock_ < 0) return false;
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        if (where.size() >= sizeof a.sun_path) return Close(), false;
        std::snprintf(a.sun_path, sizeof a.sun_path, "%s", where.c_str());
        if (connect(sock_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return Close(), false;
#endif
        buffer_.clear();
        return true;
    }

    bool Connected() const {
#ifdef _WIN32
        return pipe_ != INVALID_HANDLE_VALUE;
#else
        return sock_ >= 0;
#endif
    }

    void Close() {
#ifdef _WIN32
        if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
#else
        if (sock_ >= 0) close(sock_);
        sock_ = -1;
#endif
    }

    // Sends a request; its id is returned (replies are matched by it).
    std::optional<int> Send(const std::string& body_without_id_open) {
        if (!Open()) return std::nullopt;
        const int id = ++next_id_;
        const std::string line = "{\"id\":" + std::to_string(id) + "," + body_without_id_open + "\n";
#ifdef _WIN32
        DWORD n = 0;
        if (!WriteFile(pipe_, line.data(), static_cast<DWORD>(line.size()), &n, nullptr) || n != line.size()) {
            Close();
            return std::nullopt;
        }
#else
        std::size_t sent = 0;
        while (sent < line.size()) {
            const ssize_t n = send(sock_, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) return Close(), std::nullopt;
            sent += static_cast<std::size_t>(n);
        }
#endif
        return id;
    }

    // Sends a whole request line someone made (ControlInput) under its own id.
    bool SendLine(const std::string& line) {
        if (!Open()) return false;
        const std::string l = line + "\n";
#ifdef _WIN32
        DWORD n = 0;
        if (!WriteFile(pipe_, l.data(), static_cast<DWORD>(l.size()), &n, nullptr) || n != l.size()) return Close(), false;
#else
        if (send(sock_, l.data(), l.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(l.size())) return Close(), false;
#endif
        return true;
    }

    int NextId() { return ++next_id_; }

    // The reply to `id` (event lines and other replies skipped), within ms.
    std::optional<Value> Await(int id, int ms) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (Connected()) {
            for (std::size_t nl; (nl = buffer_.find('\n')) != std::string::npos;) {
                const std::string line = buffer_.substr(0, nl);
                buffer_.erase(0, nl + 1);
                auto v = CoopModJson::Parse(line);
                if (v && v->Number("id") && static_cast<int>(*v->Number("id")) == id) return v;
            }
            if (std::chrono::steady_clock::now() >= until) return std::nullopt;
            if (!ReadSome(20)) return std::nullopt;
        }
        return std::nullopt;
    }

    std::optional<Value> Call(const std::string& body, int ms = 2000) {
        const auto id = Send(body);
        if (!id) return std::nullopt;
        return Await(*id, ms);
    }

private:
    // Reads what is there (waiting up to ms); false when the connection broke.
    bool ReadSome(int ms) {
        char buf[16384];
#ifdef _WIN32
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &avail, nullptr)) return Close(), false;
        if (avail == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms > 5 ? 5 : ms));
            return true;
        }
        DWORD n = 0;
        if (!ReadFile(pipe_, buf, avail < sizeof buf ? avail : sizeof buf, &n, nullptr)) return Close(), false;
        buffer_.append(buf, n);
#else
        const int r = WaitReadable(sock_, ms);
        if (r < 0) return Close(), false;
        if (r == 0) return true;
        const ssize_t n = recv(sock_, buf, sizeof buf, 0);
        if (n <= 0) return Close(), false;
        buffer_.append(buf, static_cast<std::size_t>(n));
#endif
        if (buffer_.size() > 8 * 1024 * 1024) buffer_.clear();
        return true;
    }

#ifdef _WIN32
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
#else
    int sock_ = -1;
#endif
    std::string buffer_;
    int next_id_ = 0;
};

Control g_control;  // (the bridge's thread only)

// ---- the state ----

std::string Field(const Value* game, const char* key) {
    const Value* v = game ? game->Get(key) : nullptr;
    return v ? Compact(*v) : std::string("null");
}

// The state's members (no braces): Longshot's Link snapshot, then the module's equips, inventory and co-op.
std::string StateMembers() {
    std::string out;
    const auto status = g_control.Call("\"cmd\":\"status\"}", 1500);
    const Value* game = status ? status->Get("game") : nullptr;
    if (game && game->kind != Value::Kind::Object) game = nullptr;
    out += "\"scene\":" + (status && status->Get("map") ? Compact(*status->Get("map")) : Field(game, "map"));
    out += ",\"room\":null";
    for (const char* k : {"pos", "facing", "camera", "life", "max_life", "rupees", "age", "event", "loading", "prompt"}) {
        out += ",\"" + std::string(k) + "\":" + Field(game, k);
    }
    // (Longshot's present count when the snapshot was taken; our frame count is the reply's `frame`)
    out += ",\"snapshot_present\":" + Field(game, "frame");
    out += ",\"control\":" + std::string(status ? "true" : "false");
    Core::System& sys = *g_system;
    out += ",\"equips\":{";
    bool first = true;
    for (const char* slot : {"weapon", "shield", "head", "armor", "boots"}) {
        const auto flag = CoopTestInput::WearFlag(slot);
        const auto v = flag ? CoopTestInput::FlagText(sys, "PlayerStatus." + *flag) : std::nullopt;
        out += std::string(first ? "" : ",") + "\"" + slot + "\":" + (v ? Quote(*v) : "null");
        first = false;
    }
    out += "},\"inventory\":{";
    first = true;
    for (const auto& item : CoopCheats::Items()) {
        const auto v = CoopTestInput::FlagInt(sys, item);
        if (!v) continue;
        out += std::string(first ? "" : ",") + Quote(item) + ":" + std::to_string(*v);
        first = false;
    }
    out += "},\"coop\":{\"players\":[";
    first = true;
    for (const auto& p : CoopHook::PlayerList()) {
        out += std::string(first ? "" : ",") + "{\"number\":" + std::to_string(p.number) + ",\"name\":" +
               Quote(p.name) + ",\"self\":" + (p.self ? "true" : "false") + ",\"state\":" + Quote(p.state) +
               ",\"scene\":" + (p.scene ? Quote(*p.scene) : "null") + "}";
        first = false;
    }
    out += "]}";
    return out;
}

std::string StateObject() { return "{" + StateMembers() + "}"; }

// ---- the commands ----

constexpr auto kHoldWait = 5000ms;  // a hold (or a held job) that does not come in this time: timeout

std::string Handle(const Request& r) {
    Gate& gate = *g_gate;
    gate.Touch();
    const auto ok = [&](const std::string& rest) { return Reply(r.id, gate.Frame(), gate.Held(), rest); };
    const auto fail = [&](std::string_view code, std::string_view why) {
        return ErrorReply(r.id, gate.Frame(), gate.Held(), code, why);
    };
    const auto played = [&](const std::string& lines, std::string& error) {
        bool done = false;
        const bool ran = gate.RunHeld([&] { done = CoopTestInput::PlayNow(*g_system, lines, error); }, kHoldWait);
        if (!ran) error = "the game was not held in time (a load, or no frame hook)";
        return ran && done;
    };

    if (r.cmd == "hello") {
        const bool control = g_control.Open();
        return ok("\"protocol\":" + std::to_string(kProtocol) + ",\"control\":" + (control ? "true" : "false"));
    }
    if (r.cmd == "state") return ok(StateMembers());
    if (r.cmd == "pause") {
        return gate.Pause(kHoldWait) ? ok("") : fail("timeout", "no frame came to hold at (a load, or the title)");
    }
    if (r.cmd == "resume") {
        gate.Resume();
        return ok("");
    }
    if (r.cmd == "step") {
        std::uint64_t from = 0;
        if (!gate.Step(r.frames, kHoldWait, &from)) return fail("timeout", "the frames did not run in time");
        return ok("\"from\":" + std::to_string(from));
    }
    if (r.cmd == "input") {
        if (!gate.Pause(kHoldWait)) return fail("timeout", "no frame came to hold at");
        if (!g_control.SendLine(ControlInput(r, g_control.NextId()))) return fail("no_control", "HOS_CONTROL unset or unreachable");
        std::uint64_t from = 0;
        if (!gate.Step(r.frames, kHoldWait, &from)) return fail("timeout", "the frames did not run in time");
        return ok("\"from\":" + std::to_string(from) + ",\"state\":" + StateObject());
    }
    if (r.cmd == "warp") {
        std::string error;
        const std::string line = "warp " + r.scene + (r.entrance.empty() ? "" : " " + r.entrance);
        if (!played(line, error)) return fail("failed", error);
        // Frames run (in steps of 10, held between them) until Link is in that scene with no event or load.
        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(r.timeout_s);
        while (std::chrono::steady_clock::now() < until) {
            if (!gate.Step(10, kHoldWait)) return fail("timeout", "the frames did not run in time");
            const auto s = g_control.Call("\"cmd\":\"status\"}", 1500);
            const Value* game = s ? s->Get("game") : nullptr;
            const auto map = s ? s->Text("map") : std::nullopt;
            if (map && *map == r.scene && game && game->Bool("event") == false && game->Bool("loading") == false) {
                return ok("\"arrived\":true,\"state\":" + StateObject());
            }
        }
        return ErrorReply(r.id, gate.Frame(), gate.Held(), "timeout",
                          "not in " + r.scene + " after " + std::to_string(static_cast<int>(r.timeout_s)) +
                              " s; held; the state: " + StateObject());
    }
    if (r.cmd == "flag") {
        std::string error;
        const bool number = !r.value.empty() && (r.value[0] == '-' || (r.value[0] >= '0' && r.value[0] <= '9'));
        const auto was = CoopTestInput::FlagInt(*g_system, r.name);
        const std::string line = (number ? "setflag " : "setenum ") + r.name + " " + r.value;
        if (!played(line, error)) return fail("failed", error);
        const auto now = CoopTestInput::FlagInt(*g_system, r.name);
        return ok("\"name\":" + Quote(r.name) + ",\"was\":" + (was ? std::to_string(*was) : "null") +
                  ",\"value\":" + (number ? (now ? std::to_string(*now) : "null") : Quote(r.value)));
    }
    if (r.cmd == "give") {
        std::string error;
        if (!played("give " + r.item, error)) return fail("failed", error);
        return ok("\"item\":" + Quote(r.item) + ",\"note\":\"queued: in the inventory after the next frame\"");
    }
    if (r.cmd == "dump") {
        std::string body, error;
        std::size_t count = 0;
        const bool ran = gate.RunHeld([&] {
            const auto flags = CoopTestInput::SharedFlags(*g_system);
            count = flags.size();
            body = "\"flags\":{";
            for (std::size_t i = 0; i < flags.size(); ++i) {
                body += (i ? "," : "") + Quote(flags[i].first) + ":" + std::to_string(flags[i].second);
            }
            body += "}";
        }, kHoldWait);
        if (!ran) return fail("timeout", "the game was not held in time");
        std::error_code ec;
        std::filesystem::create_directories(g_dir / "dumps", ec);
        const auto path = g_dir / "dumps" / (r.tag + ".json");
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << "{\"frame\":" << gate.Frame() << ",\"tag\":" << Quote(r.tag) << ",\"state\":" << StateObject() << ","
          << body << "}\n";
        if (!f) return fail("failed", "the dump file could not be written");
        LOG_INFO(Core, "coop: playtest: dump {} at frame {}: {} flags", r.tag, gate.Frame(), count);
        return ok("\"path\":" + Quote(path.generic_string()) + ",\"flags\":" + std::to_string(count));
    }
    if (r.cmd == "shot") {
        std::error_code ec;
        std::filesystem::create_directories(g_dir / "shots", ec);
        const std::string name = r.tag.empty() ? "shot-f" + std::to_string(gate.Frame()) : r.tag;
        const auto path = g_dir / "shots" / (name + ".png");
        const auto id = g_control.Send("\"cmd\":\"shot\",\"path\":" + Quote(path.generic_string()) + "}");
        if (!id) return fail("no_control", "HOS_CONTROL unset or unreachable");
        // A held game presents nothing: let one frame at a time run until the shot is taken (at most 5).
        std::uint32_t advanced = 0;
        std::optional<Value> reply = g_control.Await(*id, gate.Holding() ? 300 : 3000);
        while (!reply && gate.Holding() && advanced < 5) {
            if (!gate.Step(1, kHoldWait)) break;
            ++advanced;
            reply = g_control.Await(*id, 500);
        }
        if (!reply || reply->Bool("ok") != true) return fail("failed", "no shot came back");
        return ok("\"path\":" + Quote(path.generic_string()) + ",\"width\":" + Field(&*reply, "width") +
                  ",\"height\":" + Field(&*reply, "height") + ",\"advanced\":" + std::to_string(advanced));
    }
    return fail("bad_request", "unknown cmd");
}

// ---- the socket ----

void Serve(SocketHandle client) {
    std::string buffer;
    bool skipping = false;  // (a line over 64 KB: the rest of it)
    char chunk[8192];
    while (!g_stop.load()) {
        const int w = WaitReadable(client, 200);
        if (w < 0) break;
        if (w == 0) {
            // (a second client meanwhile: one busy reply and closed)
            if (WaitReadable(g_listener, 0) > 0) {
                const SocketHandle other = accept(g_listener, nullptr, nullptr);
                if (other != kNoSocket) {
                    SendAll(other, ErrorReply(Value{}, g_gate->Frame(), g_gate->Held(), "busy",
                                              "another client has this game's bridge") + "\n");
                    CloseSocket(other);
                }
            }
            continue;
        }
        const auto n = recv(client, chunk, sizeof chunk, 0);
        if (n <= 0) break;
        buffer.append(chunk, static_cast<std::size_t>(n));
        for (std::size_t nl; (nl = buffer.find('\n')) != std::string::npos;) {
            std::string line = buffer.substr(0, nl);
            buffer.erase(0, nl + 1);
            if (skipping) {
                skipping = false;
                continue;
            }
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::string error;
            const auto req = ParseRequest(line, error);
            const std::string reply =
                req ? Handle(*req) : ErrorReply(Value{}, g_gate->Frame(), g_gate->Held(), "bad_request", error);
            if (!SendAll(client, reply + "\n")) return;
        }
        if (buffer.size() > kMaxLine) {
            buffer.clear();
            skipping = true;
            if (!SendAll(client, ErrorReply(Value{}, g_gate->Frame(), g_gate->Held(), "bad_request",
                                            "the line is longer than 64 KB") + "\n")) {
                return;
            }
        }
    }
}

void Run() {
    while (!g_stop.load()) {
        if (WaitReadable(g_listener, 200) <= 0) {
            if (const std::string note = g_gate->TakeEndNote(); !note.empty()) {
                LOG_WARNING(Core, "coop: playtest: the hold ended by itself ({}): the game runs again", note);
            }
            continue;
        }
        const SocketHandle client = accept(g_listener, nullptr, nullptr);
        if (client == kNoSocket) continue;
        LOG_INFO(Core, "coop: playtest: a client connected");
        Serve(client);
        CloseSocket(client);
        // The client left (closed or crashed): a held game goes on (review-01111-b: never frozen by a lost tester).
        const bool was = g_gate->Holding();
        g_gate->Resume();
        LOG_INFO(Core, "coop: playtest: the client left{}", was ? ": the game runs again" : "");
    }
}

}  // namespace

void Start(Core::System& system, const std::filesystem::path& game_dir) {
    const std::string port_text = Env("LICHII_COOP_PLAYTEST_PORT");
    const auto port = PortFrom(port_text);
    if (!port) {
        if (!port_text.empty() && port_text != "0") {
            LOG_WARNING(Core, "coop: playtest: off (LICHII_COOP_PLAYTEST_PORT={} is not a port 1024 to 65535)", port_text);
        }
        return;  // off: nothing listens
    }
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOG_WARNING(Core, "coop: playtest: off (no Winsock)");
        return;
    }
#endif
    const SocketHandle s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) {
        LOG_WARNING(Core, "coop: playtest: off (no socket)");
        return;
    }
#ifdef _WIN32
    BOOL excl = TRUE;  // (no other program may take the same port beside us)
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(*port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 only, never another address
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(s, 4) != 0) {
        LOG_WARNING(Core, "coop: playtest: off (127.0.0.1:{} is taken)", *port);
        CloseSocket(s);
        return;
    }
    g_listener = s;
    g_system = &system;
    g_dir = game_dir / "playtest";
    g_gate = std::make_unique<Gate>(IdleFrom(Env("LICHII_COOP_PLAYTEST_IDLE")));
    g_enabled.store(true);
    g_thread = std::thread(Run);
    LOG_INFO(Core, "coop: playtest: listening on 127.0.0.1:{} (control {}; dumps and shots in {})", *port,
             Env("HOS_CONTROL").empty() ? "off: HOS_CONTROL unset" : "on", g_dir.generic_string());
}

void AtFrame() {
    if (!g_enabled.load(std::memory_order_acquire)) return;
    g_gate->AtFrame();
}

void Stop() {
    if (!g_enabled.exchange(false)) return;
    g_stop.store(true);
    g_gate->Resume();
    if (g_thread.joinable()) g_thread.join();
    CloseSocket(g_listener);
    g_listener = kNoSocket;
}

}  // namespace CoopPlaytest
