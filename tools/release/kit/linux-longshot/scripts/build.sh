#!/usr/bin/env bash
# Builds the co-op game on this PC from your own copy of the game (start-squadala.sh runs it; the
# Linux twin of the Windows kit's scripts/hookshot-build.ps1). Longshot (Team MagicBean, GPL-3.0,
# git.magicbean.dev/MagicBeanDev/Longshot) is a static recompilation of the game: it is compiled
# here, from your files, and nothing of the game leaves this PC.
#
#   bash scripts/build.sh                       finds your .nup (or asks), builds once, puts co-op in
#   bash scripts/build.sh --game FILE.nup       no questions (or --game <romfs folder> --exefs <exefs folder>)
#   bash scripts/build.sh --rebuild             build again even when nothing changed
#   bash scripts/build.sh --check-only          get the tools and check your game files; build nothing
#   bash scripts/build.sh --jobs N              at most N compilers at once
#   bash scripts/build.sh --dlss                with NVIDIA DLSS: asks you to accept NVIDIA's license once, then every
#                                               build fetches NVIDIA's DLSS SDK and builds with it (data/dlss.txt)
#   bash scripts/build.sh --no-dlss             without DLSS again (the game is built again without it)
#   bash scripts/build.sh --renderer-only DIR   (CI, no game) steps 1 to 3, then compile Longshot's patched
#                                               Linux renderer and our host files into DIR
#
# 1. Longshot's own Linux builder for the release in data/longshot/LINUX.txt (its AppImage, SHA-256
#    checked), unpacked into data/builder/image (no FUSE needed): its Python and its source.
# 2. That source copied to data/builder/source with our patches (data/longshot/patches) applied
#    (apply_patch.py --refresh: a later patch change puts back only what the old ones changed).
# 3. Longshot's build tools for Linux (its tools.lock: clang/lld, the C library to link against,
#    FFmpeg, SDL2, the Vulkan headers; about 170 MB, SHA-256 checked by Longshot's own
#    builder/linux/linux_tools.py) into data/builder/home.
# 4. Longshot's nx2/make_game.py on your game: 15 to 60 minutes and about 60 GB free while it runs.
#    Its renderer is compiled here from the patched source (not the AppImage's prebuilt one, which has
#    no co-op drawing). The work folder data/work is kept: an update rebuilds only what changed.
# 5. Co-op into data/game: hookshot_coop.so beside the game, coop.ini, and the puppet pack built from
#    your game files in data/game/mods.
# Steps 1 to 4 are skipped when nothing they depend on changed, so most starts only check.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

game='' exefs='' rebuild=0 check_only=0 jobs='' renderer_out='' dlss=0 no_dlss=0
while (($#)); do
    case $1 in
        --game) game=$2; shift ;;
        --exefs) exefs=$2; shift ;;
        --rebuild) rebuild=1 ;;
        --check-only) check_only=1 ;;
        --dlss) dlss=1 ;;
        --no-dlss) no_dlss=1 ;;
        --jobs) jobs=$2; shift ;;
        --renderer-only) renderer_out=$2; shift ;;
        *) fail "build.sh: unknown option $1" ;;
    esac
    shift
done

LS=$DATA/longshot
B=$DATA/builder
WORK=$DATA/work
mkdir -p "$B" "$LOGS"
[[ -f $LS/LINUX.txt && -f $LS/PIN ]] || fail "data/longshot/LINUX.txt or PIN is missing: unzip the kit again."

# --- 1: Longshot's builder ----------------------------------------------------------------------
tag=$(pin_get "$LS/LINUX.txt" tag) || fail 'data/longshot/LINUX.txt has no tag.'
name=$(pin_get "$LS/LINUX.txt" appimage) || fail 'data/longshot/LINUX.txt has no appimage.'
url=$(pin_get "$LS/LINUX.txt" url) || fail 'data/longshot/LINUX.txt has no url.'
want=$(pin_get "$LS/LINUX.txt" sha256) || fail 'data/longshot/LINUX.txt has no sha256.'
[[ $(pin_get "$LS/PIN" tag) == "$tag" ]] || fail "data/longshot/LINUX.txt ($tag) and PIN ($(pin_get "$LS/PIN" tag)) name different Longshot releases."
[[ $name =~ ^[A-Za-z0-9._-]+$ && $url == https://git.magicbean.dev/MagicBeanDev/Longshot/releases/download/* ]] ||
    [[ -n ${OOT_COOP_FETCH_LOCAL-} ]] || fail "data/longshot/LINUX.txt: not a Longshot release download ($url)."
# The co-op mirror (kit-mirror): our GitHub release with the same AppImage, tried after Longshot's server (the SHA-256
# above decides either way). OOT_COOP_MIRROR (tests) points it elsewhere.
mirror=${OOT_COOP_MIRROR:-$(pin_get "$LS/LINUX.txt" mirror || true)}
mirror=${mirror%/}
[[ -z $mirror || $mirror == https://* || -n ${OOT_COOP_FETCH_LOCAL-} ]] || fail "data/longshot/LINUX.txt: mirror=$mirror is not an https:// address."
IMG=$B/image
PY=$IMG/usr/python/bin/python3
if [[ $(cat "$B/image.stamp" 2>/dev/null) != "$want" || ! -x $PY ]]; then
    step "Getting Longshot $tag's builder (about 95 MB, once)"
    rm -rf "$IMG" "$B/image.stamp" "$B/squashfs-root"
    if [[ ! -f $B/$name || $(sha256_of "$B/$name") != "$want" ]]; then
        say "  downloading $name ..."
        fetch_verified "$B/$name" "$want" "$url" ${mirror:+"$mirror/$name"} ||
            fail "$(fetch_fail_text "$name" "$url" "$([[ -n $mirror ]] && echo 1)")"
    fi
    chmod +x "$B/$name"
    # (--appimage-extract is the AppImage runtime's own unpacker: no FUSE, no root)
    (cd "$B" && "./$name" --appimage-extract >/dev/null) || fail "$name could not be unpacked (is this an x86-64 PC?)."
    mv "$B/squashfs-root" "$IMG"
    [[ -x $PY && -f $IMG/usr/share/longshot/nx2/make_game.py ]] || fail "$name has no Python or no nx2/make_game.py: not the builder we expect."
    printf '%s' "$want" >"$B/image.stamp"
    rm -f "$B/$name"   # (unpacked: the 95 MB file is not needed any more)
fi

# --- 2: the patched source ----------------------------------------------------------------------
SRC=$B/source
patches=("$LS"/patches/*.patch)
[[ -f ${patches[0]} ]] || fail 'data/longshot/patches has no patches: unzip the kit again.'
src_stamp=$want
for p in "${patches[@]}"; do src_stamp+=" ${p##*/}:$(sha256_of "$p")"; done
if [[ $(cat "$SRC/.coop-stamp" 2>/dev/null) != "$src_stamp" ]]; then
    step "Longshot's source with co-op's ${#patches[@]} patches"
    if [[ $(cat "$SRC/.coop-image" 2>/dev/null) != "$want" ]]; then
        rm -rf "$SRC" "$B/source-orig"
        cp -a "$IMG/usr/share/longshot" "$SRC"
        printf '%s' "$want" >"$SRC/.coop-image"
    fi
    "$PY" -I -B "$KIT/scripts/apply_patch.py" --refresh "$B/source-orig" "$SRC" "${patches[@]}" ||
        fail "Co-op's patches do not apply to Longshot $tag (said why above). This kit is damaged: unzip it again."
    printf '%s' "$src_stamp" >"$SRC/.coop-stamp"
fi

# --- 3: Longshot's build tools ------------------------------------------------------------------
# (linux_tools.py prints 'export NAME=value' lines: EMSDK, LONGSHOT_SYSROOT, LONGSHOT_FFMPEG,
# LONGSHOT_SDL2, RX_CPM. Its downloads go to LONGSHOT_BUILDER_HOME: kept inside the kit.)
export LONGSHOT_BUILDER_HOME=$B/home
step "Longshot's build tools (about 170 MB, once)"
tools_env=$(env -u LONGSHOT_TOOLS "$PY" -I "$SRC/builder/linux/linux_tools.py" env --install) ||
    fail "Longshot's build tools could not be fetched (said why above). Start again to retry."
eval "$tools_env"
# Longshot's own variables for its build, and nothing of a launcher or test run: LONGSHOT_TOOLS
# unset keeps the game in one folder (no bin/), LONGSHOT_VK_PREBUILT unset compiles the patched
# renderer, LONGSHOT_ROMFS_FROM unset (a launcher's, Longshot rc.6 #360: game files hard-linked from
# another folder) unpacks the game files the kit names. Co-op's OOT_COOP_* ones go too (Longshot's lift
# key hashed every A64_/OOT_/RX_ variable before rc.6, and they are no business of the build's).
language=${OOT_COOP_LANGUAGE:-en-US}   # (read before the OOT_COOP_* variables go below)
fetch_local=${OOT_COOP_FETCH_LOCAL-}   # (tests: a local OOTRR in step 5, after they go)
offline=${OOT_COOP_OFFLINE-}           # (tests: 1 = no internet, for step 5)
unset LONGSHOT_TOOLS LONGSHOT_VK_PREBUILT LONGSHOT_BUILDER_EXE LONGSHOT_NGX_SDK LONGSHOT_FFX_SDK LONGSHOT_ROMFS_FROM
while IFS='=' read -r v _; do [[ $v == OOT_COOP_* ]] && unset "$v"; done < <(env)
export PYTHONIOENCODING=utf-8

if [[ -n $renderer_out ]]; then
    # (CI: no game. Longshot's renderer from the patched source, as its scripts/linux/appimage.sh builds it)
    step 'The patched renderer (longshot_vk.so), no game'
    mkdir -p "$renderer_out"
    (cd "$SRC" && OOT_WORK=$renderer_out RX_VK_ONLY=1 RX_RELEASE=1 "$PY" -I nx2/build.py --standalone --jobs "${jobs:-$(nproc)}")
    [[ -f $renderer_out/sa/build/longshot_vk.so ]] || fail 'longshot_vk.so was not built.'
    say "built $renderer_out/sa/build/longshot_vk.so"
    exit 0
fi

# --- 4: the game --------------------------------------------------------------------------------
make_game() {
    # (its "@@<word> ..." lines are for Longshot's launcher window: stages and errors shown, the rest left out)
    (cd "$SRC" && "$PY" -I nx2/make_game.py "$@") 2>&1 | while IFS= read -r line; do
        case $line in
            '@@stage '*) line=${line#@@stage }; printf '  -- %s\n' "${line#* }" ;;
            '@@error '*) printf '  %s\n' "${line#@@error }" ;;
            '@@'*) ;;
            *) printf '  %s\n' "$line" ;;
        esac
    done
    return "${PIPESTATUS[0]}"
}

game_stamp=$GAME/coop-built-from.txt
ready=0
[[ -n $(kit_game_exe) && $(cat "$game_stamp" 2>/dev/null) == "$src_stamp" ]] && ready=1

# NVIDIA DLSS (#234), as hookshot-build.ps1: only in a game built with NVIDIA's DLSS SDK, which nobody may pass on, so
# each build downloads it once NVIDIA's license is accepted. --dlss asks once and data/dlss.txt keeps the answer (the
# builds after an update keep DLSS); --no-dlss takes it back. Not in coop-built-from.txt (release, pin and patches).
# A game with DLSS has its runtime in data/game/ngx (make_game.py's place_ngx_runtime; gone again without the SDK).
dlss_note=$DATA/dlss.txt
if ((no_dlss)); then
    rm -f "$dlss_note"
elif ((dlss)) && [[ ! -f $dlss_note ]]; then
    kit_tty || fail 'NVIDIA DLSS needs NVIDIA'"'"'s license accepted: run bash scripts/build.sh --dlss from a terminal.'
    say ''
    warn "NVIDIA DLSS (NVIDIA RTX graphics cards): the build downloads NVIDIA's DLSS SDK, under NVIDIA's own license:"
    # (the link as the pinned Longshot's scripts/ngx_sdk.py names it, so a pin that moves NVIDIA's commit moves it too)
    ngx_commit=$(sed -n 's/^COMMIT *= *"\([0-9a-f]\{40\}\)".*/\1/p' "$SRC/scripts/ngx_sdk.py" 2>/dev/null | head -n 1 || true)
    say "  https://github.com/NVIDIA/DLSS/blob/${ngx_commit:-main}/LICENSE.txt (the build prints NVIDIA's notice too)."
    say "  The game folder then holds NVIDIA's files: never share it, and never post builds made with them."
    read -r -p "  Accept NVIDIA's license and build with DLSS? (y/n) " answer || answer=''
    [[ ${answer,,} =~ ^(y|yes|j|ja)$ ]] || fail "NVIDIA's license not accepted: nothing changed (the game stays as it is, without DLSS)."
    printf 'nvidia-license=accepted\n' >"$dlss_note"
fi
want_dlss=0
((!no_dlss)) && [[ -f $dlss_note ]] && want_dlss=1
game_dlss=0
compgen -G "$GAME/ngx/libnvidia-ngx-dlss.so.*" >/dev/null && game_dlss=1
if ((ready && want_dlss != game_dlss && (want_dlss || no_dlss))); then
    if ((want_dlss)); then say 'NVIDIA DLSS: the game has no DLSS yet, so it is built again with it (much shorter than the first build).'
    else say 'NVIDIA DLSS off: the game is built again without it.'; fi
    ready=0
fi
remembered=$DATA/game-files.txt

if [[ -z $game ]] && ((!ready || rebuild || check_only)); then
    if [[ -f $remembered ]]; then
        IFS= read -r line <"$remembered" || true
        [[ -n $line && -e $line ]] && { game=$line; say "Your game (from the last time): $game"; }
    fi
fi
if [[ -z $game ]] && ((!ready || rebuild || check_only)); then
    # As Longshot's launcher: a .nup in Downloads, Desktop, Documents, the home folder or beside the kit.
    mapfile -t found < <(find "$HOME/Downloads" "$HOME/Desktop" "$HOME/Documents" "$HOME" "$(dirname "$KIT")" \
        -maxdepth 2 -iname '*.nup' -type f 2>/dev/null | sort -u)
    if ((${#found[@]} == 1)); then
        game=${found[0]}
        say "Found your game: $game"
    elif ((${#found[@]} > 1)) && kit_tty; then
        say 'Several game files found:'
        for i in "${!found[@]}"; do say "  $((i + 1))) ${found[$i]}"; done
        read -r -p 'Which one is the game (number)? ' n || n=''
        [[ $n =~ ^[0-9]+$ && $n -ge 1 && $n -le ${#found[@]} ]] || fail 'No game chosen. Start again.'
        game=${found[$((n - 1))]}
    fi
fi
if [[ -z $game ]] && ((!ready || rebuild || check_only)); then
    kit_tty || fail 'The game is not built yet and needs your game file. Start start-squadala.sh from a terminal once (Steam Deck: in Desktop Mode, right-click it, "Run in Konsole"), then it starts from anywhere.'
    say ''
    say "Drag your game's .nup file (or its romfs folder) into this window and press Enter:"
    read -r game || game=''
    game=${game#\'} game=${game%\'} game=${game#\"} game=${game%\"}
    game=${game#file://}
    [[ -n $game ]] || fail 'No game file given. Start again.'
fi

game_args=()
if [[ -n $game ]]; then
    [[ -e $game ]] || fail "Not found: $game"
    game=$(cd "$(dirname "$game")" && printf '%s/%s' "$(pwd)" "$(basename "$game")")
    if [[ -d $game ]]; then
        if [[ -z $exefs ]]; then
            if [[ -f $(dirname "$game")/exefs/main ]]; then exefs=$(dirname "$game")/exefs
            elif kit_tty; then
                read -r -p 'A romfs folder needs its exefs folder too. Drag the exefs folder in and press Enter: ' exefs || exefs=''
                exefs=${exefs#\'} exefs=${exefs%\'}
            fi
        fi
        [[ -f $exefs/main ]] || fail "$exefs is not an exefs folder (no main in it)."
        game_args=(--romfs "$game" --exefs "$(cd "$exefs" && pwd)")
    else
        game_args=(--nup "$game")
    fi
fi

if ((ready && !rebuild && !check_only)); then
    step 'The game: built already'
else
    ((${#game_args[@]})) || fail 'No game files given.'
    step 'Checking your game files'
    make_game "${game_args[@]}" --check ||
        fail 'Longshot cannot build from these files (it said why above). Co-op needs the game program 0400486024DEC000, update v131072, build e0f3ccfa: dump that version of your game and give its .nup.'
    if ((check_only)); then say ''; say 'Your game files are right. Start again without --check-only to build.'; exit 0; fi
    printf '%s\n' "$game" >"$remembered"
    free_kb=$(df -Pk "$DATA" | awk 'NR == 2 { print $4 }')
    if [[ -n $free_kb && $free_kb -lt $((60 * 1024 * 1024)) && ! -d $WORK/sa ]]; then
        warn "only $((free_kb / 1024 / 1024)) GB free here; the first build wants about 60 GB."
    fi
    step 'Building the game (15 to 60 minutes, once; the PC stays usable)'
    build_args=("${game_args[@]}" --out "$GAME" --work "$WORK" --language "$language" --log "$LOGS/build.log")
    [[ -n $jobs ]] && build_args+=(--jobs "$jobs")
    # (NVIDIA DLSS: make_game.py fetches NVIDIA's SDK into data/work/ngx-sdk, only what is missing, and builds the
    # renderer with it; the license was accepted above, data/dlss.txt)
    ((want_dlss)) && build_args+=(--fetch-ngx-sdk --accept-nvidia-license)
    mkdir -p "$GAME"
    ((want_dlss)) && export LONGSHOT_NGX_SDK=$WORK/ngx-sdk
    # (make_game.py's own words for a failed SDK download are the Builder's, "untick NVIDIA DLSS": the kit's way is --no-dlss)
    no_dlss_hint=''
    ((want_dlss)) && no_dlss_hint=" This build was with NVIDIA DLSS: if NVIDIA's SDK could not be downloaded (offline?), bash start-squadala.sh --no-dlss builds the game without it."
    make_game "${build_args[@]}" || fail "The build stopped. Its log: $LOGS/build.log$no_dlss_hint"
    [[ -n $(kit_game_exe) ]] || fail "The build finished but there is no game in $GAME. Its log: $LOGS/build.log"
    printf '%s' "$src_stamp" >"$game_stamp"
fi

# --- 5: co-op into the game ---------------------------------------------------------------------
step 'Co-op into the game'
[[ -f $LS/hookshot_coop.so ]] || fail 'data/longshot/hookshot_coop.so is missing: unzip the kit again.'
cp -f "$LS/hookshot_coop.so" "$GAME/hookshot_coop.so"
say "  hookshot_coop.so -> $GAME"
# The randomizer, OOTRR (data/longshot/OOTRR.txt: its release and SHA-256), into data/game/tools/ootrr where the
# module finds it with no setting; it reads the game's own files beside it (data/game/romfs) and never changes
# them. A failed download only warns: co-op plays without it, and the next start tries again.
ootrr_pin=$LS/OOTRR.txt
if [[ -f $ootrr_pin ]]; then
    o_rel=$(pin_get "$ootrr_pin" release || true) o_url=$(pin_get "$ootrr_pin" url || true)
    o_file=$(pin_get "$ootrr_pin" linux || true) o_sha=$(pin_get "$ootrr_pin" linux_sha256 || true)
    o_dir=$GAME/tools/ootrr o_want="$o_rel $o_sha"
    if [[ -x $o_dir/ootrr && $(cat "$o_dir/coop-ootrr.txt" 2>/dev/null) == "$o_want" ]]; then
        say "  randomizer: OOTRR $o_rel -> $o_dir (there already)"
    elif [[ ! $o_file =~ ^[A-Za-z0-9._-]+$ || ! $o_sha =~ ^[0-9a-f]{64}$ || -z $o_url ]]; then
        warn 'data/longshot/OOTRR.txt is incomplete: the randomizer is left out.'
    else
        mkdir -p "$B"
        o_tgz=$B/$o_file o_why=''
        o_get=0
        [[ ! -f $o_tgz || $(sha256_of "$o_tgz") != "$o_sha" ]] && o_get=1
        if ((o_get)) && { [[ $offline == 1 || $o_url =~ ^https?:// ]] && kit_offline "$offline"; }; then
            # (no internet: no download try, which would only wait for its timeout, on every start)
            o_why=offline
        elif ((o_get)); then
            say "  downloading $o_file ..."
            # (kit-mirror: with retries; a file with another SHA-256 is deleted)
            OOT_COOP_FETCH_LOCAL=$fetch_local fetch_verified "$o_tgz" "$o_sha" "$o_url/$o_file" || o_why="the download failed ($KIT_FETCH_WHY)"
        fi
        o_had=$(cat "$o_dir/coop-ootrr.txt" 2>/dev/null || true)
        o_new=$GAME/tools/ootrr.new o_old=$GAME/tools/ootrr.old
        # (a swap stopped between its two moves: the old folder goes back first)
        [[ ! -d $o_dir && -f $o_old/ootrr ]] && mv "$o_old" "$o_dir"
        if [[ -z $o_why ]]; then
            # All or nothing (Windows: kit-paths.ps1 Install-KitOotrr): unpacked beside the old one, tried, then swapped,
            # so a failure keeps the OOTRR that was there; the old way deleted it first (player report 2026-11-19).
            rm -rf "$o_new" && mkdir -p "$o_new"
            # (one folder, ootrr-<version>-linux-x86_64/: ootrr, LICENSE, NOTICE, README.md, install.sh, ootrr.desktop)
            if ! tar -xzf "$o_tgz" -C "$o_new" --strip-components=1 || [[ ! -f $o_new/ootrr ]]; then
                o_why="unpacking $o_file failed"
            else
                chmod +x "$o_new/ootrr"
                # ('shuffle 1 1' prints one number at once and touches no file: the program runs on this system)
                if ! timeout 30 "$o_new/ootrr" shuffle 1 1 >/dev/null 2>&1; then
                    o_why="ootrr did not run here (a 1-second test failed)"
                else
                    printf '%s\n' "$o_want" >"$o_new/coop-ootrr.txt"
                    rm -rf "$o_old"
                    if { [[ ! -e $o_dir ]] || mv "$o_dir" "$o_old"; } && mv "$o_new" "$o_dir"; then
                        rm -rf "$o_old"
                    else
                        [[ ! -e $o_dir && -d $o_old ]] && mv "$o_old" "$o_dir"
                        o_why="it could not be put in place: close the game and start again"
                    fi
                fi
            fi
            rm -rf "$o_new"
        fi
        if [[ -z $o_why ]]; then
            # Another release than the one before: its seeds are other worlds and the old seed files do not build
            # with it, so every built seed goes and the game makes it again at its next start
            # (kit-paths.ps1 Remove-KitRandoSeeds on Windows).
            if [[ -n $o_had && ${o_had%% *} != "$o_rel" ]]; then
                for seed_dir in "$GAME"/mods/randomizer-*/; do
                    [[ -f ${seed_dir}coop-seed.txt ]] || continue
                    seed_name=$(basename "$seed_dir")
                    rm -rf "$seed_dir" "$GAME/mods/oot-remake-randomizer/$seed_name.ootrr" \
                        "$GAME/mods/oot-remake-randomizer/$seed_name.spoiler.txt"
                    say "  randomizer: seed $seed_name removed for OOTRR $o_rel: the game makes it again at its next start (another world: start a new save)"
                done
            fi
        fi
        # (how it went, for the game's chat and collect-logs: data/game/tools/ootrr-setup.txt, as on Windows)
        mkdir -p "$GAME/tools"
        if [[ $o_why == offline ]]; then
            printf 'failed %s: this PC had no internet at the last start\n' "$o_rel" >"$GAME/tools/ootrr-setup.txt"
            say "  offline: using the tools already here (the randomizer, OOTRR $o_rel, is set up at the next start with internet)"
        elif [[ -n $o_why ]]; then
            printf 'failed %s: %s\n' "$o_rel" "$o_why" >"$GAME/tools/ootrr-setup.txt"
            warn "the randomizer (OOTRR $o_rel) was not set up: $o_why. Co-op works without it; start again to retry."
        else
            printf 'ok %s\n' "$o_rel" >"$GAME/tools/ootrr-setup.txt"
            say "  randomizer: OOTRR $o_rel -> $o_dir"
        fi
    fi
fi
# coop.ini beside the game (the module reads it there): the Multiplayer tab fills in the rest.
if [[ ! -f $GAME/coop.ini ]]; then
    sed -e 's/^\(server\|name\|password\) = @[A-Z]*@$/# \1 =/' -e 's/@PORT@/27510/' "$KIT/scripts/coop.ini.template" >"$GAME/coop.ini"
fi
# The puppet pack (the other players' Link, and the title menu's rows), from the same game files,
# where the module and Longshot's mods loader both look: data/game/mods/contents/<title>/coop-puppet.
pack_from=$game
[[ -n $pack_from ]] || { [[ -f $remembered ]] && IFS= read -r pack_from <"$remembered" || true; }
mod=$GAME/mods/contents/0400486024DEC000/coop-puppet
# (the pack tool's own files too: a pack made by another version of the tool is made again, as the
# Windows kit's Test-KitPuppetPackStale does; the look files and the copies' marks change with it)
pack_stamp="$src_stamp menus:1 tool:$(cd "$DATA/tools/puppet_pack" && LC_ALL=C sha256sum -- *.py | sha256sum | cut -d' ' -f1)"
# A file the pack's manifest names that is gone (a pack tool stopped half way, a hand-copied pack): the look files
# (coop_look/gear.bin, a colour's .bin) and each tunic's files, as the Windows kit's Get-KitPuppetPackGaps (pack-retry).
pack_gap() {
    [[ -f $mod/coop_puppet.json ]] || return 0
    "$PY" -I -B -c '
import json, os, sys
d = sys.argv[1]
try:
    m = json.load(open(os.path.join(d, "coop_puppet.json"), encoding="utf-8"))
except Exception:
    print("coop_puppet.json"); sys.exit()
want = list((m.get("look_files") or {}).values())
want += ["romfs/" + f for fs in (m.get("tunics") or {}).values() for f in (fs or [])]
gone = [f for f in want if f and not os.path.isfile(os.path.join(d, f))]
if gone:
    print(gone[0])
' "$mod" 2>/dev/null || true
}
# The game file gone or never named (pack-retry: players whose pack was never made): the usual places, the folder
# the remembered file was in first; only a file dumpcheck.py says is the game (its header), the remembered name first.
# Without dumpcheck.py nothing is taken: an unchecked file is never used (as on Windows).
find_pack_game() {
    local places=() f same='' good=()
    [[ -f $DATA/tools/puppet_pack/dumpcheck.py ]] || return 0
    [[ -n $pack_from ]] && places+=("$(dirname -- "$pack_from")")
    places+=("$KIT" "$(dirname "$KIT")" "$HOME/Downloads" "$HOME/Desktop" "$HOME/Documents")
    while IFS= read -r f; do
        "$PY" -I -B "$DATA/tools/puppet_pack/dumpcheck.py" --user-dir "$DATA" --romfs "$f" >/dev/null 2>&1 || continue
        good+=("$f")
        [[ -n $pack_from && $(basename -- "$f") == "$(basename -- "$pack_from")" ]] && same=$f
    done < <(find "${places[@]}" -maxdepth 2 -iname '*.nup' -type f 2>/dev/null | sort -u)
    if [[ -n $same ]]; then printf '%s\n' "$same"
    elif ((${#good[@]} == 1)); then printf '%s\n' "${good[0]}"
    fi
}
# The drag prompt is asked once, not at every start: data/pack-asked.txt keeps what it asked about (the game file named
# and the pack tool's hash), so a new game file or tool asks again; start-squadala.sh --pack removes it and asks.
pack_asked=$DATA/pack-asked.txt
ask_key="${pack_from:-}|${pack_stamp##* tool:}"
if [[ ! -e ${pack_from:-} ]] && ! [[ -f $mod/coop_puppet.json && $(cat "$mod/.coop-stamp" 2>/dev/null) == "$pack_stamp" && -z $(pack_gap) ]]; then
    found=$(find_pack_game)
    if [[ -n $found ]]; then
        say "  Found your game file: $found (saved in data/game-files.txt)"
        pack_from=$found
        printf '%s\n' "$pack_from" >"$remembered"
    elif [[ $(cat "$pack_asked" 2>/dev/null) == "$ask_key" ]]; then
        say '  (Not asked again at every start: bash start-squadala.sh --pack asks for your game file and makes the pack.)'
    elif kit_tty && [[ -z ${CI-} ]]; then
        say "  The puppet pack (how you see the other players) is made from your game file, and the kit cannot find it$([[ -n $pack_from ]] && printf ' (not at %s any more)' "$pack_from")."
        a=''
        read -r -t 120 -p "  Drag your game file (.nup) onto this window and press Enter (Enter alone: start without the pack): " a || a=''
        a=${a#\'} a=${a%\'} a=${a#\"} a=${a%\"} a=${a#file://}
        a=${a%"${a##*[![:space:]]}"}
        if [[ -n $a && -e $a ]]; then
            pack_from=$a
            printf '%s\n' "$pack_from" >"$remembered"
            rm -f "$pack_asked"
        else
            [[ -z $a ]] || warn "not found: $a"
            printf '%s\n' "$ask_key" >"$pack_asked"
        fi
    fi
fi
# The pack with the title's Host/Join/Co-op Settings rows and the pause menu's Multiplayer tab (--title-rows
# --pause-tab, as the Windows kit's Get-KitPackArgs). They make the pack replace files the game reads at boot (each
# language's Mals archive, the Title and Common layout archives) with larger ones; start.sh's merge_tables.py takes
# them out of play when the size table the game reads cannot hold them (another mod's table wins).
gap=$(pack_gap)
if [[ -f $mod/coop_puppet.json && $(cat "$mod/.coop-stamp" 2>/dev/null) == "$pack_stamp" && -z $gap ]]; then
    say '  puppet pack: built already'
elif [[ -n $pack_from && -e $pack_from ]]; then
    [[ -n $gap && -f $mod/coop_puppet.json ]] && say "  puppet pack: incomplete ($gap is missing): made again"
    say '  Making the puppet pack (how you see the other players; about a minute) ...'
    if "$PY" -I -B "$DATA/tools/puppet_pack/build.py" --romfs "$pack_from" --mod-dir "$mod" --title-rows --pause-tab 2>&1 | sed 's/^/    /'; then
        printf '%s' "$pack_stamp" >"$mod/.coop-stamp"
        rm -f "$pack_asked"
    else
        rm -f "$mod/.coop-stamp"
        warn 'the puppet pack could not be made (the lines above say why): the game starts, and the other players show as names only until it is made. Every start tries again.'
    fi
else
    if [[ -n $pack_from ]]; then
        warn "the puppet pack was not made: your game file is not at $pack_from any more (data/game-files.txt names it; moved, renamed or its drive not mounted). Put it back, or start again with --game <your .nup>. Until then the other players show as names only."
    else
        warn 'the puppet pack was not made: the kit does not know your game file (no data/game-files.txt). Start again with --game <your .nup>. Until then the other players show as names only.'
    fi
fi
# Randomizer seeds built before this puppet pack: the game uses one size table, the seed's, and without the pack's
# entries it aborts while it loads. Built again with every other mod's table beside it (as the module does,
# coop_rando StageSizeTables), from the seed file the game kept; removed when that cannot be done (the game builds
# it at its next start). (kit-paths.ps1 Update-KitRandoSeeds on Windows)
rando_tables() {   # the size tables of the mods the game loads besides the seeds
    local t rel
    while IFS= read -r t; do
        rel=${t#"$GAME/mods/"}
        [[ $rel =~ ^(randomizer-[^/]*|oot-remake-randomizer)/ || /$rel =~ /[-_] ]] || printf '%s\n' "$t"
    done < <(find "$GAME/mods" -mindepth 4 -maxdepth 7 -path '*/romfs/System/Resource/*.rsizetable.zs' -type f 2>/dev/null)
}
pack_table=$(ls -t "$mod"/romfs/System/Resource/*.rsizetable.zs 2>/dev/null | head -n 1 || true)
for seed_dir in "$GAME"/mods/randomizer-*/; do
    [[ -n $pack_table && -f ${seed_dir}coop-seed.txt ]] || continue
    seed_name=$(basename "$seed_dir")
    seed_table=$(ls "${seed_dir}"romfs/System/Resource/*.rsizetable.zs 2>/dev/null | head -n 1 || true)
    [[ -n $seed_table && ! $pack_table -nt $seed_table ]] && continue
    control=$GAME/mods/oot-remake-randomizer stage=$GAME/mods/oot-remake-randomizer/_seed-build
    rm -rf "$stage"
    ok=''
    if [[ -x $GAME/tools/ootrr/ootrr && -f $control/$seed_name.ootrr && -d $GAME/romfs/System ]]; then
        n=0
        while IFS= read -r t; do
            [[ -n $t ]] || continue
            mkdir -p "$stage/$n/romfs/System/Resource" && cp "$t" "$stage/$n/romfs/System/Resource/" && n=$((n + 1))
        done < <(rando_tables)
        if "$GAME/tools/ootrr/ootrr" build "$control/$seed_name.ootrr" "$GAME/romfs" "$stage" "$seed_name" --host hookshot \
            >"$control/last-build.txt" 2>&1 && [[ -f $stage/$seed_name/coop-seed.txt ]]; then
            # the game's own files OOTRR's build does not write: the preset, and the spoiler log and settings
            # string the players share (the same seed file: the same seed)
            for keep in coop-rando.txt spoiler.txt settings.txt; do
                [[ -f ${seed_dir}$keep ]] && cp "${seed_dir}$keep" "$stage/$seed_name/"
            done
            rm -rf "$seed_dir" && mv "$stage/$seed_name" "$GAME/mods/$seed_name" && ok=1
        fi
    fi
    rm -rf "$stage"
    if [[ -n $ok ]]; then say "  randomizer: seed $seed_name built again for the new puppet pack"
    else rm -rf "$seed_dir"; say "  randomizer: seed $seed_name removed (could not build it again for the new puppet pack): the game builds it at its next start"
    fi
done
say ''
say 'The game is ready.'
