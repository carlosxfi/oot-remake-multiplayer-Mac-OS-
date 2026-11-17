// SPDX-License-Identifier: GPL-3.0-or-later
// CoopClipboard (hookshot/src/coop_clipboard_posix.h), the module's POSIX clipboard, with stand-in
// desktop clipboard tools on PATH that keep the text in a file. It is the one POSIX reader:
// CoopFiles::ReadClipboard (the answer paste, the Join paste) is this.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <optional>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include "coop_clipboard_posix.h"

namespace {

namespace fs = std::filesystem;

std::string Slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

void Script(const fs::path& p, const std::string& body) {
    {
        std::ofstream f(p, std::ios::binary);
        f << "#!/bin/sh\n" << body;
    }
    fs::permissions(p, fs::perms::owner_all);
}

std::string Get(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

void Put(const char* name, const std::string& value) {
    if (value.empty()) {
        unsetenv(name);
    } else {
        setenv(name, value.c_str(), 1);
    }
}

// A folder of stand-in tools, and PATH only that folder (plus the few programs the stand-ins use, linked in), so
// the runner's own wl-paste, xclip or xsel never answer; the desktop variables set as asked. Undone at the end.
struct Desktop {
    fs::path dir;
    std::string path, wayland, display;
    Desktop(const char* wayland_display, const char* x_display)
        : path(Get("PATH")), wayland(Get("WAYLAND_DISPLAY")), display(Get("DISPLAY")) {
        dir = fs::temp_directory_path() / ("coop-clip-test-" + std::to_string(getpid()) + "-" +
                                           std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(dir);
        const std::string clip = (dir / "clip.txt").string(), fds = (dir / "fds.txt").string();
        // (the copy tools also list their own open descriptors, to see that none of the game's came along)
        Script(dir / "wl-copy", "cat > '" + clip + "'\nls /proc/$$/fd > '" + fds + "'\n");
        Script(dir / "wl-paste", "[ \"$1\" = -n ] || exit 3\ncat '" + clip + "'\necho\n");
        Script(dir / "pbcopy", "cat > '" + clip + "'\n");
        Script(dir / "pbpaste", "cat '" + clip + "'\n");
        Script(dir / "xclip",
               "[ \"$1 $2\" = '-selection clipboard' ] || exit 3\n"
               "if [ \"$3\" = -i ]; then cat > '" + clip + "'; ls /proc/$$/fd > '" + fds + "'; "
               "else cat '" + clip + "'; fi\n");
        for (const char* tool : {"cat", "ls", "timeout", "yes", "touch"}) {
            for (const char* bin : {"/usr/bin/", "/bin/"}) {
                std::error_code ec;
                if (fs::exists(std::string(bin) + tool, ec)) {
                    fs::create_symlink(std::string(bin) + tool, dir / tool, ec);
                    break;
                }
            }
        }
        Put("PATH", dir.string());
        Put("WAYLAND_DISPLAY", wayland_display);
        Put("DISPLAY", x_display);
    }
    ~Desktop() {
        Put("PATH", path);
        Put("WAYLAND_DISPLAY", wayland);
        Put("DISPLAY", display);
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string Clip() const { return Slurp(dir / "clip.txt"); }
    std::string Fds() const { return Slurp(dir / "fds.txt"); }
    void Remove(const char* tool) const { fs::remove(dir / tool); }
    void Tool(const char* tool, const std::string& body) const { Script(dir / tool, body); }
};

}  // namespace

#ifndef __APPLE__
TEST_CASE("Linux clipboard: Wayland's wl-copy and wl-paste") {
    Desktop d("wayland-0", "");
    CHECK(CoopClipboard::Copy("ABCD-EFGH-1234"));
    CHECK(d.Clip() == "ABCD-EFGH-1234");
    CHECK(CoopClipboard::Paste() == "ABCD-EFGH-1234");   // (wl-paste's line end dropped)
}

TEST_CASE("Linux clipboard: X11's xclip when there is no Wayland") {
    Desktop d("", ":7");
    CHECK(CoopClipboard::Copy("code with spaces and 'quotes'\n"));
    CHECK(d.Clip() == "code with spaces and 'quotes'\n");
    CHECK(CoopClipboard::Paste() == "code with spaces and 'quotes'");
}

TEST_CASE("Linux clipboard: no desktop, no tool: nothing, and the game goes on") {
    Desktop d("", "");
    CHECK_FALSE(CoopClipboard::Copy("x"));
    CHECK_FALSE(CoopClipboard::Paste().has_value());   // (nothing: "install wl-clipboard or xclip")
    CHECK(d.Clip().empty());
}

TEST_CASE("Linux clipboard: the tool gets none of the game's descriptors") {
    Desktop d("wayland-0", "");
    const int game_fd = open("/dev/null", O_RDONLY);   // (as a socket of the game: no O_CLOEXEC)
    REQUIRE(game_fd >= 0);
    REQUIRE(dup2(game_fd, 200) == 200);
    CHECK(CoopClipboard::Copy("x"));
    close(200);
    close(game_fd);
    std::istringstream fds(d.Fds());
    std::string fd;
    int count = 0;
    while (fds >> fd) {
        ++count;
        CHECK_MESSAGE(fd != "200", "the clipboard tool kept a descriptor of the game");
    }
    CHECK(count >= 3);   // (0, 1, 2 and the shell's own script)
}

TEST_CASE("Linux clipboard: a long paste is cut at 4096 bytes") {
    Desktop d("wayland-0", "");
    REQUIRE(CoopClipboard::Copy(std::string(10000, 'a')));
    CHECK(CoopClipboard::Paste().value_or("").size() == 4096);
}

TEST_CASE("Linux clipboard: an empty clipboard is empty, not 'no tool'") {
    Desktop d("wayland-0", "");
    d.Tool("wl-paste", "echo 'Nothing is copied' >&2\nexit 1\n");   // (as wl-paste and xclip do)
    const auto got = CoopClipboard::Paste();
    REQUIRE(got.has_value());
    CHECK(got->empty());
}

TEST_CASE("Linux clipboard: the tools in order: Wayland's first, then xclip, then xsel") {
    Desktop d("wayland-0", ":7");
    d.Tool("wl-paste", "printf wayland\n");
    d.Tool("xclip", "printf xclip\n");
    d.Tool("xsel", "[ \"$1 $2\" = '-b -o' ] || exit 3\nprintf xsel\n");
    CHECK(CoopClipboard::Paste() == "wayland");
    d.Remove("wl-paste");   // (a Wayland session without wl-clipboard: X11's tools through XWayland)
    CHECK(CoopClipboard::Paste() == "xclip");
    d.Remove("xclip");
    CHECK(CoopClipboard::Paste() == "xsel");
    d.Remove("xsel");
    CHECK_FALSE(CoopClipboard::Paste().has_value());
}

TEST_CASE("Linux clipboard: a tool that prints without end: 64 KiB read, 4096 bytes kept") {
    Desktop d("wayland-0", "");
    d.Tool("wl-paste", "exec yes abcdefg\n");
    const auto got = CoopClipboard::Paste();
    REQUIRE(got.has_value());
    CHECK(got->size() == 4096);
    CHECK(got->rfind("abcdefg\nabcdefg", 0) == 0);
}

TEST_CASE("Linux clipboard: the clipboard's text never reaches a shell") {
    Desktop d("", ":7");
    const fs::path marker = d.dir / "ran";
    const std::string evil = "$(touch '" + marker.string() + "')`touch '" + marker.string() + "'`; touch '" +
                             marker.string() + "' #\n";
    REQUIRE(CoopClipboard::Copy(evil));
    CHECK(d.Clip() == evil);
    CHECK(CoopClipboard::Paste() == evil.substr(0, evil.size() - 1));
    CHECK_FALSE(fs::exists(marker));
}
#else
TEST_CASE("macOS clipboard: pbcopy and pbpaste") {
    Desktop d("", "");
    CHECK(CoopClipboard::Copy("ABCD-EFGH-1234"));
    CHECK(d.Clip() == "ABCD-EFGH-1234");
    CHECK(CoopClipboard::Paste() == "ABCD-EFGH-1234");
}

TEST_CASE("macOS clipboard: no native tools means no clipboard") {
    Desktop d("", "");
    d.Remove("pbcopy");
    d.Remove("pbpaste");
    CHECK_FALSE(CoopClipboard::Copy("x"));
    CHECK_FALSE(CoopClipboard::Paste().has_value());
}

TEST_CASE("macOS clipboard: a long paste is cut at 4096 bytes") {
    Desktop d("", "");
    REQUIRE(CoopClipboard::Copy(std::string(10000, 'a')));
    CHECK(CoopClipboard::Paste().value_or("").size() == 4096);
}

TEST_CASE("macOS clipboard: clipboard text never reaches a shell") {
    Desktop d("", "");
    const fs::path marker = d.dir / "ran";
    const std::string evil = "$(touch '" + marker.string() + "')`touch '" + marker.string() + "'`; touch '" +
                             marker.string() + "' #\n";
    REQUIRE(CoopClipboard::Copy(evil));
    CHECK(d.Clip() == evil);
    CHECK(CoopClipboard::Paste() == evil.substr(0, evil.size() - 1));
    CHECK_FALSE(fs::exists(marker));
}
#endif

TEST_CASE("Linux clipboard: a paste on SDL's thread takes SDL's text first, then a tool's, then SDL's empty one") {
    int asked = 0;
    const auto tool = [&asked](std::optional<std::string> answer) {
        return [&asked, answer]() {
            ++asked;
            return answer;
        };
    };
    // SDL has text (Steam Deck Game Mode: no tool at all): that, and no tool is run
    CHECK(CoopClipboard::PickPaste(std::string("ABCD-EFGH"), tool(std::nullopt)) == "ABCD-EFGH");
    CHECK(asked == 0);
    // SDL's "" is empty or unreadable: a tool's answer wins, else the empty clipboard stands
    CHECK(CoopClipboard::PickPaste(std::string(), tool(std::string("from wl-paste"))) == "from wl-paste");
    CHECK(CoopClipboard::PickPaste(std::string(), tool(std::nullopt)) == "");
    // not on SDL's thread, or no SDL: the tools alone; nothing only when neither can read
    CHECK(CoopClipboard::PickPaste(std::nullopt, tool(std::string("from xclip"))) == "from xclip");
    CHECK_FALSE(CoopClipboard::PickPaste(std::nullopt, tool(std::nullopt)).has_value());
    CHECK(asked == 4);
}

TEST_CASE("POSIX clipboard: without the game's SDL, SDL's thread pastes through the tools as before") {
    CHECK(CoopClipboard::GameSdl("SDL_GetClipboardText") == nullptr);   // (the tests load no SDL)
    Desktop d("wayland-7", "");
    CHECK(CoopClipboard::Copy("ABCD-EFGH-5678"));
    const CoopClipboard::OnSdlThread sdl_thread;
    CHECK(CoopClipboard::Paste() == "ABCD-EFGH-5678");
    {
        const CoopClipboard::OnSdlThread nested;
    }
#ifdef __APPLE__
    d.Remove("pbpaste");
#else
    d.Remove("wl-paste");
    d.Remove("xclip");
#endif
    CHECK_FALSE(CoopClipboard::Paste().has_value());   // (said in the tab: "no clipboard tool ... press Ctrl+V")
}

