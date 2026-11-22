#!/bin/sh
set -eu
umask 077
launcher_only=0
if [ "${1:-}" = "--launcher-only" ]; then
  launcher_only=1
  shift
fi
if [ "$#" -ne 2 ]; then
  echo "usage: $0 [--launcher-only] /path/to/game.app /path/to/puppet-mod/romfs" >&2
  exit 2
fi
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
app=$(CDPATH= cd -- "$1" && pwd)
overlay=$(CDPATH= cd -- "$2" && pwd)
module="$here/build/shared-core/hookshot/hookshot_coop.dylib"
if [ ! -f "$app/Contents/Info.plist" ] ||
   [ ! -s "$overlay/Pack/Actor/CoopPuppet_Child.pack.zs" ] ||
   [ ! -s "$overlay/Pack/Actor/CoopPuppet_Adult.pack.zs" ] ||
   [ ! -f "$overlay/../coop_puppet.json" ]; then
  echo 'Choose a working nxnative app and the romfs folder of a built co-op pack.' >&2
  exit 2
fi
if [ "$launcher_only" -eq 0 ]; then
  sh "$here/build-shared.sh"
fi
if [ ! -f "$module" ] || [ ! -f "$here/../probe/build/libcoop_nxnative_probe.dylib" ]; then
  echo 'Shared co-op module was not built.' >&2
  exit 2
fi

state="$HOME/Library/Application Support/OOT Co-op"
pack="$state/mods/contents/0400486024DEC000/coop-puppet"
mkdir -p "$pack" "$HOME/Applications"
cp "$overlay/../coop_puppet.json" "$pack/"
launcher="$HOME/Applications/OOT Co-op.app"
if [ -e "$launcher" ] && [ ! -f "$launcher/Contents/Resources/oot-coop-launch.sh" ]; then
  echo "An unrelated app already exists at $launcher." >&2
  exit 2
fi

staging=$(mktemp -d "$HOME/Applications/.oot-coop-install.XXXXXX")
trap 'if [ ! -e "$staging/previous.app" ]; then rm -rf "$staging"; fi' EXIT
bundle="$staging/OOT Co-op.app"
resources="$bundle/Contents/Resources"
mkdir -p "$resources" "$bundle/Contents/MacOS"
cat > "$bundle/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleIdentifier</key><string>org.squadala.oot-coop-launcher</string>
  <key>CFBundleName</key><string>OOT Co-op</string>
  <key>CFBundleExecutable</key><string>oot-coop-launcher</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSUIElement</key><true/>
  <key>NSLocalNetworkUsageDescription</key>
  <string>Join and host OOT Co-op games with players on your local network.</string>
</dict>
</plist>
PLIST
cat > "$staging/launcher.m" <<'SOURCE'
#import <Foundation/Foundation.h>

int main(void) {
    @autoreleasepool {
        NSURL *script = [NSBundle.mainBundle URLForResource:@"oot-coop-launch" withExtension:@"sh"];
        if (!script) {
            NSLog(@"OOT Co-op could not find its launch script.");
            return 1;
        }

        NSTask *game = [[NSTask alloc] init];
        game.executableURL = [NSURL fileURLWithPath:@"/bin/sh"];
        game.arguments = @[script.path];
        NSError *error = nil;
        if (![game launchAndReturnError:&error]) {
            NSLog(@"OOT Co-op could not start: %@", error.localizedDescription);
            return 1;
        }
        [game waitUntilExit];
        return game.terminationStatus;
    }
}
SOURCE
/usr/bin/xcrun clang -fobjc-arc -framework Foundation "$staging/launcher.m" \
  -o "$bundle/Contents/MacOS/oot-coop-launcher"
/usr/bin/plutil -lint "$bundle/Contents/Info.plist"
/usr/bin/codesign --force --sign - "$bundle"
/usr/bin/codesign --verify --strict "$bundle"
if [ -e "$launcher" ]; then
  mv "$launcher" "$staging/previous.app"
fi
if ! mv "$bundle" "$launcher"; then
  if [ -e "$staging/previous.app" ]; then
    if ! mv "$staging/previous.app" "$launcher"; then
      trap - EXIT
      echo "Previous launcher saved at $staging/previous.app." >&2
    fi
  fi
  exit 1
fi
rm -rf "$staging/previous.app"
printf 'Open %s in Finder and use the in-game Multiplayer menu.\n' "$launcher"
