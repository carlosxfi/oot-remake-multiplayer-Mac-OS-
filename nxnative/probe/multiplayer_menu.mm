#import <AppKit/AppKit.h>
#import <QuartzCore/CALayer.h>

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cstdint>
#include <cstdio>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <ifaddrs.h>
#include <net/if.h>

#include "../../hookshot/src/hos_coop_host.h"
#include "../../hookshot/src/hos_coop_draw.h"
#include "../../hookshot/src/hos_coop_mac_peers.h"
#include "multiplayer_menu.hpp"

namespace {

struct SessionApi {
  hos_coop_host_start_fn host;
  hos_coop_join_fn join;
  hos_coop_leave_fn leave;
  hos_coop_status_fn status;
  void (*client_status)(char*, std::uint32_t);
  hos_coop_mac_peers_fn peers;
  int (*key)(std::uint32_t, int);
  int (*character)(std::uint32_t);
  void (*chat)(char*, std::size_t);
  hos_coop_draw_fn markers;
};

NSString* lan_addresses(std::uint16_t port) {
  NSMutableArray<NSString*>* addresses = [NSMutableArray array];
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0) return @"";
  for (const auto* item = interfaces; item; item = item->ifa_next) {
    if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
        !(item->ifa_flags & IFF_UP) || (item->ifa_flags & (IFF_LOOPBACK | IFF_POINTOPOINT))) continue;
    const auto* address = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
    char ip[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &address->sin_addr, ip, sizeof(ip)))
      [addresses addObject:[NSString stringWithFormat:@"%s:%u", ip, static_cast<unsigned>(port)]];
  }
  freeifaddrs(interfaces);
  return [addresses componentsJoinedByString:@", "];
}

NSButton* button(NSString* title, id target, SEL action) {
  return [NSButton buttonWithTitle:title target:target action:action];
}

void style_material(NSVisualEffectView* view, NSVisualEffectMaterial kind) {
  view.translatesAutoresizingMaskIntoConstraints = NO;
  view.material = kind;
  view.blendingMode = NSVisualEffectBlendingModeWithinWindow;
  view.state = NSVisualEffectStateActive;
  view.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
  view.wantsLayer = YES;
  view.layer.cornerRadius = 12;
  view.layer.masksToBounds = YES;
}

NSString* heart_count(std::uint16_t quarters) {
  NSArray<NSString*>* fractions = @[@"", @"¼", @"½", @"¾"];
  if (quarters < 4 && quarters > 0) return fractions[quarters];
  return [NSString stringWithFormat:@"%u%@", static_cast<unsigned>(quarters / 4),
                                   fractions[quarters % 4]];
}

NSColor* marker_color(std::uint32_t rgba) {
  return [NSColor colorWithSRGBRed:(rgba & 255) / 255.0
                             green:((rgba >> 8) & 255) / 255.0
                              blue:((rgba >> 16) & 255) / 255.0
                             alpha:(rgba >> 24) / 255.0];
}

}  // namespace

@interface NxnativePeersView : NSVisualEffectView
@end

@implementation NxnativePeersView
- (NSView*)hitTest:(NSPoint)point {
  (void)point;
  return nil;
}
@end

@interface NxnativeMarkerView : NSView
@property(nonatomic, assign) hos_coop_draw_fn drawMarkers;
@end

@implementation NxnativeMarkerView
- (BOOL)isFlipped { return YES; }
- (NSView*)hitTest:(NSPoint)point { (void)point; return nil; }
- (void)drawRect:(NSRect)dirtyRect {
  (void)dirtyRect;
  if (!_drawMarkers) return;
  HosCoopDraw draw{};
  draw.width = self.bounds.size.width;
  draw.height = self.bounds.size.height;
  draw.circle = [](void*, float x, float y, float radius, std::uint32_t color, float thickness) {
    auto* path = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(x-radius, y-radius, 2*radius, 2*radius)];
    [marker_color(color) set];
    if (thickness > 0) {
      path.lineWidth = thickness;
      [path stroke];
    } else {
      [path fill];
    }
  };
  draw.triangle = [](void*, float x0, float y0, float x1, float y1, float x2, float y2, std::uint32_t color) {
    auto* path = [NSBezierPath bezierPath];
    [path moveToPoint:NSMakePoint(x0, y0)];
    [path lineToPoint:NSMakePoint(x1, y1)];
    [path lineToPoint:NSMakePoint(x2, y2)];
    [path closePath];
    [marker_color(color) setFill];
    [path fill];
  };
  draw.text = [](void*, float x, float y, float size, std::uint32_t color, const char* text) {
    NSString* label = [NSString stringWithUTF8String:text];
    [label drawAtPoint:NSMakePoint(x, y) withAttributes:@{
      NSFontAttributeName: [NSFont systemFontOfSize:size weight:NSFontWeightSemibold],
      NSForegroundColorAttributeName: marker_color(color)
    }];
  };
  draw.text_width = [](void*, float size, const char* text) {
    NSString* label = [NSString stringWithUTF8String:text];
    return static_cast<float>([label sizeWithAttributes:@{
      NSFontAttributeName: [NSFont systemFontOfSize:size weight:NSFontWeightSemibold]
    }].width);
  };
  _drawMarkers(&draw);
}
@end

@interface NxnativeMultiplayerMenu : NSObject
- (instancetype)initWithApi:(SessionApi)api;
- (void)installWithPanel:(bool)showPanel;
- (BOOL)handleKey:(NSEvent*)event;
@end

@implementation NxnativeMultiplayerMenu {
  SessionApi _api;
  dispatch_queue_t _queue;
  NSVisualEffectView* _panel;
  NxnativePeersView* _peers;
  NSTextField* _peerLabel;
  NxnativePeersView* _chat;
  NxnativeMarkerView* _markers;
  NSTextField* _chatLabel;
  __weak NSView* _hudContainer;
  __weak NSResponder* _beforePanel;
  NSTextField* _name;
  NSTextField* _address;
  NSTextField* _port;
  NSTextField* _password;
  NSStackView* _passwordRow;
  NSTextField* _status;
  NSButton* _host;
  NSButton* _join;
  NSButton* _leave;
  NSButton* _copy;
  BOOL _working;
  NSString* _inputError;
}

- (instancetype)initWithApi:(SessionApi)api {
  self = [super init];
  if (self) {
    _api = api;
    _queue = dispatch_queue_create("org.ootcoop.mac.session", DISPATCH_QUEUE_SERIAL);
  }
  return self;
}

- (void)installWithPanel:(bool)showPanel {
  if (showPanel) [self attachToGameMenu];
  auto* timer = [NSTimer timerWithTimeInterval:0.5 repeats:YES block:^(NSTimer*) {
    if (showPanel) [self attachToGameMenu];
    if (self->_panel.window) [self refresh];
    [self refreshPeers];
    [self refreshChat];
  }];
  [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
  if (_api.markers) {
    auto* markerTimer = [NSTimer timerWithTimeInterval:1.0 / 30.0 repeats:YES block:^(NSTimer*) {
      NSWindow* window = NSApp.mainWindow ?: NSApp.keyWindow;
      NSView* content = window.isVisible ? window.contentView : nil;
      if (self->_markers.superview != content) {
        [self->_markers removeFromSuperview];
        if (content) {
          self->_markers = [[NxnativeMarkerView alloc] initWithFrame:content.bounds];
          self->_markers.drawMarkers = self->_api.markers;
          self->_markers.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
          [content addSubview:self->_markers positioned:NSWindowAbove relativeTo:nil];
        }
      }
      self->_markers.hidden = self->_panel.window == window;
      if (!self->_markers.hidden) self->_markers.needsDisplay = YES;
    }];
    [NSRunLoop.mainRunLoop addTimer:markerTimer forMode:NSRunLoopCommonModes];
  }

  using KeyFilter = int (*)(void*, void*);
  using SetKeyFilter = void (*)(KeyFilter, void*);
  const auto setFilter = reinterpret_cast<SetKeyFilter>(dlsym(RTLD_DEFAULT, "dm_coop_set_key_filter"));
  if (setFilter && _api.key && _api.character && _api.chat) {
    setFilter([](void* context, void* event) -> int {
      auto* menu = (__bridge NxnativeMultiplayerMenu*)context;
      return [menu handleKey:(__bridge NSEvent*)event] ? 1 : 0;
    }, (__bridge void*)self);
    std::fprintf(stderr, "[coop-nxnative] chat keyboard bridge installed\n");
  } else {
    std::fprintf(stderr, "[coop-nxnative] chat keyboard bridge unavailable\n");
    if (showPanel) {
      [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent*(NSEvent* event) {
        if (_panel.window && event.window == _panel.window && [self handleKey:event]) return nil;
        return event;
      }];
    }
  }
}

- (BOOL)handleKey:(NSEvent*)event {
  const auto modifiers = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
  if (!modifiers && [event.charactersIgnoringModifiers isEqualToString:@"\033"] &&
      _panel.window && event.window == _panel.window) {
    [self closePanel:nil];
    return YES;
  }
  NSWindow* window = NSApp.mainWindow ?: NSApp.keyWindow;
  if (!_api.key || !_api.character || !_api.chat ||
      !window || event.window != window || _panel.window == window ||
      [window.firstResponder isKindOfClass:NSTextView.class] ||
      (modifiers & (NSEventModifierFlagCommand | NSEventModifierFlagControl))) return NO;
  const unichar ch = event.charactersIgnoringModifiers.length
      ? [event.charactersIgnoringModifiers characterAtIndex:0] : 0;
  std::uint32_t key = ch;
  if (ch >= 'a' && ch <= 'z') key = ch - 'a' + 'A';
  else if (ch == 127) key = 8;
  else if (ch == 3) key = 13;
  else if (ch >= NSF1FunctionKey && ch <= NSF12FunctionKey) key = 0x70 + ch - NSF1FunctionKey;
  else if (ch < 0x20 && ch != 8 && ch != 13 && ch != 27) key = 0;
  else if (ch >= 0x80) key = 0;
  if (!_api.key(key, event.isARepeat)) return NO;
  for (NSUInteger i = 0; i < event.characters.length; ++i) {
    const unichar c = [event.characters characterAtIndex:i];
    if (c >= 0x20 && c < 0x7f) _api.character(c);
  }
  [self refreshChat];
  return YES;
}

- (void)attachToGameMenu {
  NSMenu* bar = NSApp.mainMenu;
  if (!bar) return;
  for (NSMenuItem* item in bar.itemArray) {
    if ([item.identifier isEqualToString:@"org.ootcoop.mac.multiplayer"]) return;
  }
  NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Multiplayer"];
  auto* open = [menu addItemWithTitle:@"Host or Join…" action:@selector(show:)
                        keyEquivalent:@"m"];
  open.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
  open.target = self;
  auto* item = [bar addItemWithTitle:@"Multiplayer" action:nil keyEquivalent:@""];
  item.identifier = @"org.ootcoop.mac.multiplayer";
  item.submenu = menu;
}

- (void)closePanel:(id)sender {
  (void)sender;
  NSWindow* window = _panel.window;
  [window endEditingFor:nil];
  [_panel removeFromSuperview];
  [window makeFirstResponder:_beforePanel];
  _beforePanel = nil;
  [self refreshPeers];
}

- (void)refreshPeers {
  NSWindow* window = NSApp.mainWindow ?: NSApp.keyWindow;
  NSView* content = window.isVisible ? window.contentView : nil;
  if (content != _hudContainer) {
    [_peers removeFromSuperview];
    _hudContainer = content;
  }
  if (!content || !_api.peers) return;

  std::array<HosCoopMacPeer, 16> peers{};
  const auto count = std::min<std::size_t>(_api.peers(peers.data(), peers.size()), peers.size());
  if (!count || _panel.window == window) {
    _peers.hidden = YES;
    return;
  }

  if (!_peers) {
    _peers = [[NxnativePeersView alloc] initWithFrame:NSZeroRect];
    style_material(_peers, NSVisualEffectMaterialHUDWindow);
    _peerLabel = [NSTextField wrappingLabelWithString:@""];
    _peerLabel.translatesAutoresizingMaskIntoConstraints = NO;
    [_peers addSubview:_peerLabel];
    [NSLayoutConstraint activateConstraints:@[
      [_peerLabel.leadingAnchor constraintEqualToAnchor:_peers.leadingAnchor constant:12],
      [_peerLabel.trailingAnchor constraintEqualToAnchor:_peers.trailingAnchor constant:-12],
      [_peerLabel.topAnchor constraintEqualToAnchor:_peers.topAnchor constant:8],
      [_peerLabel.bottomAnchor constraintEqualToAnchor:_peers.bottomAnchor constant:-8],
    ]];
  }

  NSMutableArray<NSString*>* rows = [NSMutableArray arrayWithCapacity:count];
  for (std::size_t i = 0; i < count; ++i) {
    const auto& peer = peers[i];
    NSString* name = [NSString stringWithUTF8String:peer.name] ?: @"Player";
    NSString* hp = peer.max_life
        ? [NSString stringWithFormat:@"♥ %@ / %@", heart_count(peer.life), heart_count(peer.max_life)]
        : @"♥ —";
    [rows addObject:[NSString stringWithFormat:@"%@   %@", name, hp]];
  }
  NSString* roster = [rows componentsJoinedByString:@"\n"];
  if (![_peerLabel.stringValue isEqualToString:roster]) _peerLabel.stringValue = roster;
  if (!_peers.superview) {
    [content addSubview:_peers positioned:NSWindowAbove relativeTo:nil];
    [NSLayoutConstraint activateConstraints:@[
      [_peers.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:18],
      [_peers.topAnchor constraintEqualToAnchor:content.topAnchor constant:86],
      [_peers.widthAnchor constraintLessThanOrEqualToConstant:315],
      [_peers.trailingAnchor constraintLessThanOrEqualToAnchor:content.trailingAnchor constant:-18],
    ]];
  }
  _peers.hidden = NO;
}

- (void)refreshChat {
  NSWindow* window = NSApp.mainWindow ?: NSApp.keyWindow;
  NSView* content = window.isVisible && _panel.window != window ? window.contentView : nil;
  if (_chat.superview != content) [_chat removeFromSuperview];
  if (!content || !_api.chat) return;

  std::array<char, 4096> snapshot{};
  _api.chat(snapshot.data(), snapshot.size());
  NSString* text = [NSString stringWithUTF8String:snapshot.data()] ?: @"";
  if (!text.length) {
    _chat.hidden = YES;
    return;
  }
  if (!_chat) {
    _chat = [[NxnativePeersView alloc] initWithFrame:NSZeroRect];
    style_material(_chat, NSVisualEffectMaterialHUDWindow);
    _chatLabel = [NSTextField wrappingLabelWithString:@""];
    _chatLabel.maximumNumberOfLines = 10;
    _chatLabel.translatesAutoresizingMaskIntoConstraints = NO;
    [_chat addSubview:_chatLabel];
    [NSLayoutConstraint activateConstraints:@[
      [_chatLabel.leadingAnchor constraintEqualToAnchor:_chat.leadingAnchor constant:12],
      [_chatLabel.trailingAnchor constraintEqualToAnchor:_chat.trailingAnchor constant:-12],
      [_chatLabel.topAnchor constraintEqualToAnchor:_chat.topAnchor constant:8],
      [_chatLabel.bottomAnchor constraintEqualToAnchor:_chat.bottomAnchor constant:-8],
    ]];
  }
  if (![_chatLabel.stringValue isEqualToString:text]) _chatLabel.stringValue = text;
  if (!_chat.superview) {
    [content addSubview:_chat positioned:NSWindowAbove relativeTo:nil];
    [NSLayoutConstraint activateConstraints:@[
      [_chat.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:18],
      [_chat.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-36],
      [_chat.widthAnchor constraintLessThanOrEqualToConstant:420],
      [_chat.trailingAnchor constraintLessThanOrEqualToAnchor:content.trailingAnchor constant:-18],
    ]];
  }
  _chat.hidden = NO;
}

- (void)show:(id)sender {
  (void)sender;
  if (_panel.window) {
    [self closePanel:nil];
    return;
  }
  NSWindow* window = NSApp.mainWindow ?: NSApp.keyWindow;
  NSView* content = window.contentView;
  if (!window.isVisible || !content) return;
  if (!_panel) {
    _panel = [[NSVisualEffectView alloc] initWithFrame:NSZeroRect];
    style_material(_panel, NSVisualEffectMaterialPopover);
    auto* card = _panel;
    auto* stack = [[NSStackView alloc] initWithFrame:NSZeroRect];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 8;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [card addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
      [stack.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:20],
      [stack.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-20],
      [stack.topAnchor constraintEqualToAnchor:card.topAnchor constant:19],
      [stack.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-19],
    ]];

    auto* heading = [NSTextField labelWithString:@"Multiplayer"];
    heading.font = [NSFont systemFontOfSize:19 weight:NSFontWeightSemibold];
    [stack addArrangedSubview:heading];
    _name = [NSTextField textFieldWithString:@"MacLink"];
    _address = [NSTextField textFieldWithString:@""];
    _address.placeholderString = @"192.168.1.10, or paste the invite line";
    _port = [NSTextField textFieldWithString:@"27510"];
    for (NSArray* row in @[@[@"Nickname", _name], @[@"Host address (to join)", _address], @[@"Port", _port]]) {
      [stack addArrangedSubview:[NSTextField labelWithString:row[0]]];
      NSTextField* field = row[1];
      [stack addArrangedSubview:field];
      [field.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    }
    [stack addArrangedSubview:[NSTextField labelWithString:@"Room password"]];
    _password = [[NSSecureTextField alloc] initWithFrame:NSZeroRect];
    _password.placeholderString = @"Required to join; leave blank to generate one when hosting";
    auto* show = [NSButton checkboxWithTitle:@"Show password" target:self action:@selector(togglePassword:)];
    [show setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    _passwordRow = [NSStackView stackViewWithViews:@[_password, show]];
    [stack addArrangedSubview:_passwordRow];
    [_passwordRow.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    _host = button(@"Host LAN", self, @selector(host:));
    _join = button(@"Join", self, @selector(join:));
    _leave = button(@"Leave", self, @selector(leave:));
    _copy = button(@"Copy invite", self, @selector(copyInvite:));
    auto* close = button(@"Close", self, @selector(closePanel:));
    auto* actions = [NSStackView stackViewWithViews:@[_host, _join, _leave, _copy, close]];
    [stack addArrangedSubview:actions];
    _status = [NSTextField wrappingLabelWithString:@""];
    _status.selectable = YES;
    [stack addArrangedSubview:_status];
    [_status.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
  }
  [content addSubview:_panel positioned:NSWindowAbove relativeTo:nil];
  auto* width = [_panel.widthAnchor constraintEqualToConstant:490];
  width.priority = NSLayoutPriorityDefaultHigh;
  [NSLayoutConstraint activateConstraints:@[
    [_panel.centerXAnchor constraintEqualToAnchor:content.centerXAnchor],
    [_panel.centerYAnchor constraintEqualToAnchor:content.centerYAnchor],
    [_panel.leadingAnchor constraintGreaterThanOrEqualToAnchor:content.leadingAnchor constant:16],
    width,
  ]];
  _beforePanel = window.firstResponder;
  [window makeFirstResponder:_name];
  [self refresh];
  [self refreshPeers];
}

- (void)togglePassword:(NSButton*)sender {
  [_panel.window endEditingFor:nil];
  const bool visible = sender.state == NSControlStateValueOn;
  NSTextField* field = visible ? [[NSTextField alloc] initWithFrame:NSZeroRect]
                               : [[NSSecureTextField alloc] initWithFrame:NSZeroRect];
  field.editable = YES;
  field.selectable = YES;
  field.bezeled = YES;
  field.drawsBackground = YES;
  field.placeholderString = _password.placeholderString;
  field.stringValue = _password.stringValue;
  [_passwordRow removeArrangedSubview:_password];
  [_password removeFromSuperview];
  _password = field;
  [_passwordRow insertArrangedSubview:_password atIndex:0];
}

- (void)refresh {
  HosCoopStatus status{};
  status.size = sizeof(status);
  _api.status(&status);
  const bool idle = status.state == HOS_COOP_IDLE || status.state == HOS_COOP_FAILED;
  _host.enabled = !_working && idle;
  _join.enabled = !_working && idle;
  _leave.enabled = !_working && !idle;
  _copy.enabled = !_working && status.state == HOS_COOP_HOSTING && status.password[0];
  if (_working) {
    _status.stringValue = @"Updating session…";
  } else if (_inputError) {
    _status.stringValue = _inputError;
  } else if (status.state == HOS_COOP_HOSTING) {
    auto* address = lan_addresses(status.port);
    _status.stringValue = [NSString stringWithFormat:@"Hosting LAN · %u player(s)\n%@",
        static_cast<unsigned>(status.players), address.length ? address : @"No active LAN address found."];
  } else if (status.state == HOS_COOP_CONNECTED && _api.client_status) {
    char detail[256]{};
    _api.client_status(detail, sizeof(detail));
    _status.stringValue = detail[0] ? [NSString stringWithUTF8String:detail] : @"Connecting to the host…";
  } else if (status.message[0]) {
    _status.stringValue = [NSString stringWithUTF8String:status.message] ?: @"Session error";
  } else {
    const char* message = status.state == HOS_COOP_CONNECTED ? "Join requested. Return to the game to play."
                        : !idle ? "Connecting…"
                                : "Host a LAN session or join a friend's game.";
    _status.stringValue = [NSString stringWithUTF8String:message];
  }
}

- (void)run:(dispatch_block_t)job {
  if (_working) return;
  _inputError = nil;
  _working = YES;
  [self refresh];
  dispatch_async(_queue, ^{
    job();
    dispatch_async(dispatch_get_main_queue(), ^{
      self->_working = NO;
      [self refresh];
    });
  });
}

- (int)selectedPort {
  NSString* port = [_port.stringValue stringByTrimmingCharactersInSet:
      NSCharacterSet.whitespaceAndNewlineCharacterSet];
  if (!port.length) return 27510;
  if ([port rangeOfCharacterFromSet:
      [NSCharacterSet characterSetWithCharactersInString:@"0123456789"].invertedSet].location != NSNotFound ||
      port.integerValue < 1 || port.integerValue > 65535) {
    _inputError = @"Enter a port from 1 to 65535.";
    [self refresh];
    return 0;
  }
  return port.intValue;
}

- (void)host:(id)sender {
  (void)sender;
  NSString* name = _name.stringValue;
  NSString* password = _password.stringValue;
  const int port = [self selectedPort];
  if (!port) return;
  [self run:^{
    HosCoopHostOptions options{};
    options.size = sizeof(options);
    options.name = name.UTF8String;
    options.password = password.UTF8String;
    options.port = static_cast<std::uint16_t>(port);
    self->_api.host(&options);
  }];
}

- (void)join:(id)sender {
  (void)sender;
  NSString* name = _name.stringValue;
  NSString* address = [_address.stringValue stringByTrimmingCharactersInSet:
      NSCharacterSet.whitespaceAndNewlineCharacterSet];
  NSString* password = _password.stringValue;
  const auto space = [address rangeOfCharacterFromSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
  if (space.location != NSNotFound) {
    if (!password.length) {
      password = [[address substringFromIndex:space.location] stringByTrimmingCharactersInSet:
          NSCharacterSet.whitespaceAndNewlineCharacterSet];
    }
    address = [address substringToIndex:space.location];
  }
  if (!address.length) {
    _inputError = @"Enter the Windows host's address.";
    [self refresh];
    return;
  }
  if ([address rangeOfString:@":"].location == NSNotFound &&
      [address rangeOfString:@"@"].location == NSNotFound && password.length) {
    const int port = [self selectedPort];
    if (!port) return;
    address = [NSString stringWithFormat:@"%@:%d", address, port];
  }
  [self run:^{
    HosCoopJoinOptions options{sizeof(HosCoopJoinOptions), name.UTF8String,
                              address.UTF8String, password.UTF8String};
    self->_api.join(&options);
  }];
}

- (void)leave:(id)sender {
  (void)sender;
  [self run:^{ self->_api.leave(); }];
}

- (void)copyInvite:(id)sender {
  (void)sender;
  HosCoopStatus status{};
  status.size = sizeof(status);
  _api.status(&status);
  if (status.state != HOS_COOP_HOSTING || !status.password[0]) return;
  NSString* first = [lan_addresses(status.port) componentsSeparatedByString:@", "].firstObject;
  if (!first.length) return;
  NSString* invite = [NSString stringWithFormat:@"%@ %s", first, status.password];
  auto* clipboard = NSPasteboard.generalPasteboard;
  [clipboard clearContents];
  [clipboard setString:invite forType:NSPasteboardTypeString];
}

@end

void install_multiplayer_menu(void* module, bool show_panel) {
  const SessionApi api{
      reinterpret_cast<hos_coop_host_start_fn>(dlsym(module, "hos_coop_host_start")),
      reinterpret_cast<hos_coop_join_fn>(dlsym(module, "hos_coop_join")),
      reinterpret_cast<hos_coop_leave_fn>(dlsym(module, "hos_coop_leave")),
      reinterpret_cast<hos_coop_status_fn>(dlsym(module, "hos_coop_status")),
      reinterpret_cast<void (*)(char*, std::uint32_t)>(dlsym(module, "hos_coop_client_status")),
      reinterpret_cast<hos_coop_mac_peers_fn>(dlsym(module, "hos_coop_mac_peers")),
      reinterpret_cast<int (*)(std::uint32_t, int)>(dlsym(module, "hos_coop_key")),
      reinterpret_cast<int (*)(std::uint32_t)>(dlsym(module, "hos_coop_char")),
      reinterpret_cast<void (*)(char*, std::size_t)>(dlsym(module, "hos_coop_mac_chat")),
      reinterpret_cast<hos_coop_draw_fn>(dlsym(module, "hos_coop_mac_draw_markers")),
  };
  if (!api.host || !api.join || !api.leave || !api.status) return;
  dispatch_async(dispatch_get_main_queue(), ^{
    static NxnativeMultiplayerMenu* menu = [[NxnativeMultiplayerMenu alloc] initWithApi:api];
    [menu installWithPanel:show_panel];
  });
}
