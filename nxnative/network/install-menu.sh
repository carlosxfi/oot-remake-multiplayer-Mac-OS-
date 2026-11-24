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
if [ -f "$overlay/../coop_maps.txt" ]; then
  cp "$overlay/../coop_maps.txt" "$pack/"
else
  rm -f "$pack/coop_maps.txt"
fi
launcher="$HOME/Applications/OOT Co-op.app"
if [ -e "$launcher" ] && [ ! -f "$launcher/Contents/Resources/oot-coop-launch.sh" ]; then
  installed_id=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$launcher/Contents/Info.plist" 2>/dev/null || true)
  if [ "$installed_id" != org.squadala.oot-coop-launcher ]; then
    echo "An unrelated app already exists at $launcher." >&2
    exit 2
  fi
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
  <key>NSBluetoothAlwaysUsageDescription</key>
  <string>Use Bluetooth game controllers to play OOT Co-op.</string>
  <key>NSLocalNetworkUsageDescription</key>
  <string>Join and host OOT Co-op games with players on your local network.</string>
</dict>
</plist>
PLIST
cat > "$staging/launcher.m" <<'SOURCE'
#import <AppKit/AppKit.h>

int main(void) {
    @autoreleasepool {
        NSURL *script = [NSBundle.mainBundle URLForResource:@"oot-coop-launch" withExtension:@"sh"];
        if (!script) {
            NSLog(@"OOT Co-op could not find its launch script.");
            return 1;
        }

        [NSApplication sharedApplication];
        [NSApp activateIgnoringOtherApps:YES];
        NSAlert *picker = [[NSAlert alloc] init];
        picker.messageText = @"Co-op inventory profile";
        picker.informativeText = @"Enter the same name each time you join this playthrough. Leave it blank to play without a profile. Use up to 48 letters, digits, spaces, hyphens or underscores.";
        NSTextField *field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 320, 24)];
        field.stringValue = [NSUserDefaults.standardUserDefaults stringForKey:@"InventoryProfile"] ?: @"";
        picker.accessoryView = field;
        picker.window.initialFirstResponder = field;
        [picker addButtonWithTitle:@"Play"];
        [picker addButtonWithTitle:@"Cancel"];
        NSString *profile;
        NSCharacterSet *invalid = [[NSCharacterSet characterSetWithCharactersInString:
            @"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_"] invertedSet];
        for (;;) {
            if ([picker runModal] != NSAlertFirstButtonReturn) return 0;
            profile = [field.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
            if (profile.length <= 48 && [profile rangeOfCharacterFromSet:invalid].location == NSNotFound) break;
            picker.informativeText = @"Use up to 48 letters, digits, spaces, hyphens or underscores.";
        }
        [NSUserDefaults.standardUserDefaults setObject:profile forKey:@"InventoryProfile"];
        NSMutableDictionary *environment = [NSProcessInfo.processInfo.environment mutableCopy];
        environment[@"COOP_NXNATIVE_INVENTORY_PROFILE"] = profile;

        NSTask *game = [[NSTask alloc] init];
        game.executableURL = [NSURL fileURLWithPath:@"/bin/sh"];
        game.arguments = @[script.path];
        game.environment = environment;
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
/usr/bin/xcrun clang -fobjc-arc -framework AppKit "$staging/launcher.m" \
  -o "$bundle/Contents/MacOS/oot-coop-launcher"
printf '%s\n' "$here" "$app" "$overlay" "$module" "$state" > "$resources/oot-coop-paths"
cat > "$resources/oot-coop-launch.sh" <<'LAUNCH'
#!/bin/sh
set -eu
directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
{
  IFS= read -r here
  IFS= read -r app
  IFS= read -r overlay
  IFS= read -r module
  IFS= read -r state
} < "$directory/oot-coop-paths"

unset LICHII_COOP_SERVER LICHII_COOP_PORT LICHII_COOP_PASSWORD
export DM_ROMFS_OVERLAY="$overlay" COOP_NXNATIVE_PUPPET_PREFLIGHT=1
export COOP_NXNATIVE_MENU=1 COOP_NXNATIVE_SHARED_MODULE="$module" COOP_NXNATIVE_SHARED_DIR="$state"
# Identify the supported Mac launcher to the shared startup check.
export OOT_COOP_KIT_START=1
export HOS_COOP_MENU=1 LICHII_COOP_PUPPET=1 LICHII_COOP_HOST_SAVE=0
export HOS_COOP_MODS_CODE=0 HOS_COOP_NULL_READS=0
unset COOP_NXNATIVE_CHARACTER_SELECT
if /usr/bin/grep -Fqx "$(printf '# COOP_CHARACTER_SELECT\t1')" "$app/Contents/Resources/dm/main.fixups.tsv" 2>/dev/null; then
  if ! /usr/bin/plutil -extract characters.mod raw -o - "$overlay/../coop_puppet.json" >/dev/null 2>&1; then
    echo 'Character Select is in the game executable. Choose a prepared ROMFS overlay and puppet pack with its character assets.' >&2
    exit 1
  fi
  export COOP_NXNATIVE_CHARACTER_SELECT=1
fi
# Apply the saved Kokiri colour before nxnative opens the ROMFS. Live texture
# overrides are supplied by Longshot and are unavailable in this nxnative host.
unset LICHII_COOP_SELF_TUNIC
if self_tunic=$(/usr/bin/python3 -I "$here/../../tools/puppet_pack/self_tunic.py" \
    --preset --pack "$overlay/.." --coop-ini "$state/coop.ini") && [ -n "$self_tunic" ]; then
  export LICHII_COOP_SELF_TUNIC="$self_tunic"
fi
unset COOP_NXNATIVE_PAUSE_PACK
if /usr/bin/plutil -extract pause_menu.0 raw -o - "$overlay/../coop_puppet.json" >/dev/null 2>&1; then
  export COOP_NXNATIVE_PAUSE_PACK=1
fi
exec /bin/sh "$here/../probe/run.sh" "$app"
LAUNCH
test -s "$resources/oot-coop-paths"
test -s "$resources/oot-coop-launch.sh"
sh -n "$resources/oot-coop-launch.sh"
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
