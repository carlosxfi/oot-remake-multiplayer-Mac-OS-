#import <AppKit/AppKit.h>

#include <arpa/inet.h>
#include <cstdint>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <ifaddrs.h>
#include <net/if.h>

#include "../../hookshot/src/hos_coop_host.h"
#include "multiplayer_menu.hpp"

namespace {

struct SessionApi {
  hos_coop_host_start_fn host;
  hos_coop_join_fn join;
  hos_coop_leave_fn leave;
  hos_coop_status_fn status;
  void (*client_status)(char*, std::uint32_t);
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

}  // namespace

@interface NxnativeMultiplayerMenu : NSObject
- (instancetype)initWithApi:(SessionApi)api;
- (void)install;
@end

@implementation NxnativeMultiplayerMenu {
  SessionApi _api;
  dispatch_queue_t _queue;
  NSPanel* _panel;
  NSTextField* _name;
  NSTextField* _address;
  NSTextField* _port;
  NSSecureTextField* _password;
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

- (void)install {
  [self attachToGameMenu];
  [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer*) {
    [self attachToGameMenu];
    if (self->_panel.isVisible) [self refresh];
  }];
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

- (void)show:(id)sender {
  (void)sender;
  if (!_panel) {
    _panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 470, 365)
         styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
           backing:NSBackingStoreBuffered defer:NO];
    _panel.title = @"Multiplayer";
    _panel.releasedWhenClosed = NO;
    _panel.hidesOnDeactivate = YES;
    _panel.collectionBehavior = NSWindowCollectionBehaviorMoveToActiveSpace |
                                NSWindowCollectionBehaviorFullScreenAuxiliary;

    auto* content = _panel.contentView;
    auto* stack = [[NSStackView alloc] initWithFrame:NSZeroRect];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 8;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
      [stack.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:20],
      [stack.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-20],
      [stack.topAnchor constraintEqualToAnchor:content.topAnchor constant:19],
    ]];

    _name = [NSTextField textFieldWithString:@"MacLink"];
    _address = [NSTextField textFieldWithString:@""];
    _address.placeholderString = @"192.168.1.10, or paste the invite line";
    _port = [NSTextField textFieldWithString:@"27510"];
    _password = [[NSSecureTextField alloc] initWithFrame:NSZeroRect];
    _password.placeholderString = @"Required to join; leave blank to generate one when hosting";
    for (NSArray* row in @[@[@"Nickname", _name], @[@"Host address (to join)", _address], @[@"Port", _port],
                        @[@"Room password", _password]]) {
      [stack addArrangedSubview:[NSTextField labelWithString:row[0]]];
      NSTextField* field = row[1];
      [stack addArrangedSubview:field];
      [field.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    }
    _host = button(@"Host LAN", self, @selector(host:));
    _join = button(@"Join", self, @selector(join:));
    _leave = button(@"Leave", self, @selector(leave:));
    _copy = button(@"Copy invite", self, @selector(copyInvite:));
    auto* actions = [NSStackView stackViewWithViews:@[_host, _join, _leave, _copy]];
    [stack addArrangedSubview:actions];
    _status = [NSTextField wrappingLabelWithString:@""];
    _status.selectable = YES;
    [stack addArrangedSubview:_status];
    [_status.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    [_panel center];
  }
  [self refresh];
  [_panel makeKeyAndOrderFront:nil];
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

void install_multiplayer_menu(void* module) {
  const SessionApi api{
      reinterpret_cast<hos_coop_host_start_fn>(dlsym(module, "hos_coop_host_start")),
      reinterpret_cast<hos_coop_join_fn>(dlsym(module, "hos_coop_join")),
      reinterpret_cast<hos_coop_leave_fn>(dlsym(module, "hos_coop_leave")),
      reinterpret_cast<hos_coop_status_fn>(dlsym(module, "hos_coop_status")),
      reinterpret_cast<void (*)(char*, std::uint32_t)>(dlsym(module, "hos_coop_client_status")),
  };
  if (!api.host || !api.join || !api.leave || !api.status) return;
  dispatch_async(dispatch_get_main_queue(), ^{
    static NxnativeMultiplayerMenu* menu = [[NxnativeMultiplayerMenu alloc] initWithApi:api];
    [menu install];
  });
}
