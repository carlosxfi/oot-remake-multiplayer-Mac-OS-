// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_playtest_core.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <utility>

namespace CoopPlaytest {

std::optional<std::uint16_t> PortFrom(std::string_view value) {
    unsigned port = 0;
    const char* end = value.data() + value.size();
    const auto r = std::from_chars(value.data(), end, port);
    if (value.empty() || r.ec != std::errc{} || r.ptr != end || port < 1024 || port > 65535) return std::nullopt;
    return static_cast<std::uint16_t>(port);
}

double IdleFrom(std::string_view value) {
    double s = 0;
    const char* end = value.data() + value.size();
    const auto r = std::from_chars(value.data(), end, s);
    if (value.empty() || r.ec != std::errc{} || r.ptr != end || !(s >= 0) || s > 86400) return kDefaultIdleS;
    return s;
}

bool ValidTag(std::string_view tag) {
    if (tag.empty() || tag.size() > 64) return false;
    for (const char c : tag) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                        c == '_';
        if (!ok) return false;
    }
    return true;
}

// ---- the hold ----
// No lock of the module's is held here: the frame hook calls AtFrame first (coop_hookshot.cpp Frame), before any
// of co-op's own work, and the only lock taken is the gate's own, which the wait gives up (condition_variable).

void Gate::AtFrame() {
    std::unique_lock lock(mu_);
    ++frame_;
    if (budget_ > 0 && --budget_ > 0) return;  // a step still running
    if (!want_hold_) return;
    held_ = true;
    cv_.notify_all();
    while (want_hold_ && budget_ == 0) {
        while (!jobs_.empty()) {
            std::function<void()>* job = jobs_.front();
            jobs_.pop_front();
            lock.unlock();
            (*job)();
            lock.lock();
            *job = nullptr;  // (done: RunHeld waits for this)
            cv_.notify_all();
        }
        if (idle_s_ > 0 && std::chrono::duration<double>(Clock::now() - touched_).count() >= idle_s_) {
            want_hold_ = false;
            end_note_ = "idle";
            break;
        }
        cv_.wait_for(lock, std::chrono::milliseconds(50));
    }
    held_ = false;
    cv_.notify_all();
}

std::uint64_t Gate::Frame() const {
    std::lock_guard lock(mu_);
    return frame_;
}

bool Gate::Held() const {
    std::lock_guard lock(mu_);
    return held_;
}

bool Gate::Holding() const {
    std::lock_guard lock(mu_);
    return want_hold_;
}

bool Gate::Pause(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mu_);
    touched_ = Clock::now();
    want_hold_ = true;
    if (!held_) budget_ = 0;  // (a step not yet begun is cancelled: the hold comes at the next frame)
    return cv_.wait_for(lock, timeout, [&] { return held_ && budget_ == 0; });
}

void Gate::Resume() {
    std::lock_guard lock(mu_);
    want_hold_ = false;
    budget_ = 0;
    cv_.notify_all();
}

bool Gate::Step(std::uint32_t frames, std::chrono::milliseconds timeout, std::uint64_t* from) {
    if (frames == 0) return false;
    if (!Pause(timeout)) return false;
    std::unique_lock lock(mu_);
    const std::uint64_t start = frame_;
    if (from) *from = start;
    budget_ = frames;
    cv_.notify_all();
    // Held again at exactly start + frames: AtFrame holds at the hook where the budget reaches 0.
    // (a Resume meanwhile, the client gone or the idle time out, ends the wait: not a step done)
    cv_.wait_for(lock, timeout + std::chrono::milliseconds(frames * 100),
                 [&] { return !want_hold_ || (held_ && budget_ == 0 && frame_ == start + frames); });
    return want_hold_ && held_ && budget_ == 0 && frame_ == start + frames;
}

bool Gate::RunHeld(std::function<void()> job, std::chrono::milliseconds timeout) {
    if (!Pause(timeout)) return false;
    std::unique_lock lock(mu_);
    std::function<void()> slot = std::move(job);
    jobs_.push_back(&slot);
    cv_.notify_all();
    if (cv_.wait_for(lock, timeout, [&] { return !slot; })) return true;
    // Not run in time (the hold ended): take it back so it never runs after we return.
    for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
        if (*it == &slot) {
            jobs_.erase(it);
            return false;
        }
    }
    cv_.wait(lock, [&] { return !slot; });  // (running now: wait for it)
    return true;
}

void Gate::Touch() {
    std::lock_guard lock(mu_);
    touched_ = Clock::now();
}

std::string Gate::TakeEndNote() {
    std::lock_guard lock(mu_);
    return std::exchange(end_note_, {});
}

// ---- the requests ----

Axes StickAxes(double angle_deg, double strength) {
    if (!std::isfinite(angle_deg) || !std::isfinite(strength)) return {};
    strength = strength < 0 ? 0 : strength > 1 ? 1 : strength;
    const double a = angle_deg * 3.14159265358979323846 / 180.0;
    Axes out{std::sin(a) * strength, std::cos(a) * strength};
    if (std::fabs(out.x) < 1e-9) out.x = 0;
    if (std::fabs(out.y) < 1e-9) out.y = 0;
    return out;
}

namespace {

constexpr std::string_view kButtons[] = {"A",  "B",    "X",     "Y",  "L",  "R",  "ZL",   "ZR",
                                         "PLUS", "MINUS", "LS", "RS", "UP", "DOWN", "LEFT", "RIGHT"};

bool KnownButton(std::string_view b) {
    for (const auto k : kButtons) {
        if (k == b) return true;
    }
    return false;
}

std::optional<std::uint32_t> Frames(const CoopModJson::Value& v, std::string& error) {
    const auto n = v.Number("frames");
    if (!v.Get("frames")) return 1u;
    if (!n || *n != std::floor(*n) || *n < 1 || *n > kMaxFrames) {
        error = "frames: a whole number, 1 to 3600";
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*n);
}

bool Stick(const CoopModJson::Value& v, std::string_view key, std::optional<Axes>& out, std::string& error) {
    const CoopModJson::Value* s = v.Get(key);
    if (!s || s->kind == CoopModJson::Value::Kind::Null) return true;
    if (s->kind != CoopModJson::Value::Kind::Array || s->array.size() != 2 ||
        s->array[0].kind != CoopModJson::Value::Kind::Number || s->array[1].kind != CoopModJson::Value::Kind::Number ||
        !std::isfinite(s->array[0].number) || !(s->array[1].number >= 0 && s->array[1].number <= 1)) {
        error = std::string(key) + ": [angle in degrees, strength 0 to 1]";
        return false;
    }
    out = StickAxes(s->array[0].number, s->array[1].number);
    return true;
}

bool PlainText(std::string_view s, std::size_t max) {
    if (s.empty() || s.size() > max) return false;
    for (const char c : s) {
        if (static_cast<unsigned char>(c) < 0x21 || c == ';' || c == '#' || c == 0x7f) return false;
    }
    return true;
}

}  // namespace

std::optional<Request> ParseRequest(std::string_view line, std::string& error) {
    if (line.size() > kMaxLine) {
        error = "the line is longer than 64 KB";
        return std::nullopt;
    }
    std::string why;
    const auto v = CoopModJson::Parse(line, &why);
    if (!v || v->kind != CoopModJson::Value::Kind::Object) {
        error = "not a JSON object" + (why.empty() ? std::string() : ": " + why);
        return std::nullopt;
    }
    Request r;
    if (const auto* id = v->Get("id")) r.id = *id;
    const auto cmd = v->Text("cmd");
    if (!cmd) {
        error = "cmd: a command name";
        return std::nullopt;
    }
    r.cmd = *cmd;
    const std::string& c = r.cmd;
    if (c == "hello" || c == "state" || c == "pause" || c == "resume") return r;
    if (c == "step") {
        const auto f = Frames(*v, error);
        if (!f) return std::nullopt;
        r.frames = *f;
        return r;
    }
    if (c == "input") {
        const auto f = Frames(*v, error);
        if (!f) return std::nullopt;
        r.frames = *f;
        if (const auto* b = v->Get("buttons"); b && b->kind != CoopModJson::Value::Kind::Null) {
            if (b->kind != CoopModJson::Value::Kind::Array || b->array.size() > 16) {
                error = "buttons: a list of names";
                return std::nullopt;
            }
            for (const auto& x : b->array) {
                if (x.kind != CoopModJson::Value::Kind::String || !KnownButton(x.string)) {
                    error = "buttons: A B X Y L R ZL ZR PLUS MINUS LS RS UP DOWN LEFT RIGHT";
                    return std::nullopt;
                }
                r.buttons.push_back(x.string);
            }
        }
        if (!Stick(*v, "stick", r.stick, error) || !Stick(*v, "rstick", r.rstick, error)) return std::nullopt;
        if (const auto* e = v->Get("exclusive"); e && e->kind != CoopModJson::Value::Kind::Bool) {
            error = "exclusive: true or false";
            return std::nullopt;
        }
        r.exclusive = v->Bool("exclusive").value_or(true);
        return r;
    }
    if (c == "warp") {
        const auto scene = v->Text("scene");
        if (!scene || !PlainText(*scene, 64)) {
            error = "scene: a map name";
            return std::nullopt;
        }
        r.scene = *scene;
        if (const auto* e = v->Get("entrance"); e && e->kind != CoopModJson::Value::Kind::Null) {
            if (e->kind != CoopModJson::Value::Kind::String || !PlainText(e->string, 64)) {
                error = "entrance: a start position name";
                return std::nullopt;
            }
            r.entrance = e->string;
        }
        if (v->Get("timeout")) {
            const auto t = v->Number("timeout");
            if (!t || !(*t >= 1 && *t <= 600)) {
                error = "timeout: 1 to 600 seconds";
                return std::nullopt;
            }
            r.timeout_s = *t;
        }
        return r;
    }
    if (c == "flag") {
        const auto name = v->Text("name");
        if (!name || !PlainText(*name, 128)) {
            error = "name: a flag name or a 0x hash";
            return std::nullopt;
        }
        r.name = *name;
        const auto* val = v->Get("value");
        using K = CoopModJson::Value::Kind;
        if (!val) {
            error = "value: a number, true or false, or an enum value's name";
            return std::nullopt;
        }
        if (val->kind == K::Bool) {
            r.value = val->boolean ? "1" : "0";
        } else if (val->kind == K::Number && val->number == std::floor(val->number) && val->number >= -2147483648.0 &&
                   val->number <= 2147483647.0) {  // (int32, as the game's Int flags; review-01111-b)
            r.value = std::to_string(static_cast<long long>(val->number));
        } else if (val->kind == K::String && PlainText(val->string, 128)) {
            r.value = val->string;  // an enum value's name: setenum
        } else {
            error = "value: a whole number from -2147483648 to 2147483647, true or false, or an enum value's name";
            return std::nullopt;
        }
        return r;
    }
    if (c == "give") {
        const auto item = v->Text("item");
        if (!item || !PlainText(*item, 128)) {
            error = "item: bombs, bow, hookshot or an item flag name";
            return std::nullopt;
        }
        r.item = *item;
        return r;
    }
    if (c == "dump" || c == "shot") {
        const auto tag = v->Text(c == "dump" ? "tag" : "name");
        if (c == "dump" && (!tag || !ValidTag(*tag))) {
            error = "tag: 1 to 64 letters, digits, - and _";
            return std::nullopt;
        }
        if (c == "shot" && v->Get("name") && (!tag || !ValidTag(*tag))) {
            error = "name: 1 to 64 letters, digits, - and _ (the shot goes in the bridge's shots folder)";
            return std::nullopt;
        }
        if (c == "shot" && v->Get("path")) {
            error = "path: not taken; give a name (the shot goes in the bridge's shots folder)";
            return std::nullopt;
        }
        r.tag = tag.value_or("");
        return r;
    }
    error = "cmd: hello, state, pause, resume, step, input, warp, flag, give, dump or shot";
    return std::nullopt;
}

std::string Quote(std::string_view text) {
    return Compact(CoopModJson::Value::Of(std::string(text)));
}

std::string Compact(const CoopModJson::Value& value) {
    using K = CoopModJson::Value::Kind;
    switch (value.kind) {
    case K::Array: {
        std::string out = "[";
        for (std::size_t i = 0; i < value.array.size(); ++i) out += (i ? "," : "") + Compact(value.array[i]);
        return out + "]";
    }
    case K::Object: {
        std::string out = "{";
        for (std::size_t i = 0; i < value.object.size(); ++i) {
            out += (i ? "," : "") + Quote(value.object[i].first) + ":" + Compact(value.object[i].second);
        }
        return out + "}";
    }
    default: {
        // (a scalar: the file writer's text is one token, then its newline; control characters are escaped)
        std::string out = CoopModJson::Write(value);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        return out;
    }
    }
}

namespace {
std::string Num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6f", v);
    return buf;
}
}  // namespace

std::string ControlInput(const Request& r, int control_id) {
    std::string step = "{\"at\":0,\"frames\":" + std::to_string(r.frames);
    if (!r.buttons.empty()) {
        step += ",\"buttons\":[";
        for (std::size_t i = 0; i < r.buttons.size(); ++i) step += (i ? "," : "") + Quote(r.buttons[i]);
        step += "]";
    }
    // Both sticks always set (centred when not given), so the step is never empty and an exclusive input holds
    // the sticks still.
    const Axes l = r.stick.value_or(Axes{}), rr = r.rstick.value_or(Axes{});
    step += ",\"lx\":" + Num(l.x) + ",\"ly\":" + Num(l.y) + ",\"rx\":" + Num(rr.x) + ",\"ry\":" + Num(rr.y) + "}";
    return "{\"id\":" + std::to_string(control_id) + ",\"cmd\":\"sequence\",\"steps\":[" + step +
           "],\"exclusive\":" + (r.exclusive ? "true" : "false") + ",\"wait\":false}";
}

std::string Reply(const CoopModJson::Value& id, std::uint64_t frame, bool paused, const std::string& rest) {
    std::string out = "{\"id\":" + Compact(id) + ",\"ok\":true,\"frame\":" + std::to_string(frame) +
                      ",\"paused\":" + (paused ? "true" : "false");
    if (!rest.empty()) out += "," + rest;
    return out + "}";
}

std::string ErrorReply(const CoopModJson::Value& id, std::uint64_t frame, bool paused, std::string_view code,
                       std::string_view message) {
    return "{\"id\":" + Compact(id) + ",\"ok\":false,\"frame\":" + std::to_string(frame) +
           ",\"paused\":" + (paused ? "true" : "false") + ",\"error\":" + Quote(code) + ",\"message\":" +
           Quote(message) + "}";
}

}  // namespace CoopPlaytest
