/*
 * libhakios.settings.dylib: what Settings shows of this phone that iOS's own panes cannot see. In Preferences only,
 * opened by the tweak loader there (src/inject_tweaks.c), since one inserted library per program is what is reliable.
 *
 *   Personal Hotspot   the main list's row and its pane, as on an iPhone and in iOS's own words (WirelessModemSettings.bundle):
 *                      Allow Others to Join and the Wi-Fi Password; the network is named after the phone. iOS's own row
 *                      needs the carrier's provisioning (and MobileGestalt's personal-hotspot), so it is never there. The
 *                      pane writes CONF and posts com.hakios.hotspot.changed; src/hotspot.c "serve" (root) does the rest.
 *   Cellular           the modem's state, read only (the kernel's kern.hakios.cellular_*): there is no CoreTelephony here.
 *   Location Services  its row in Privacy, back where it goes missing.
 *
 * From the diagnostic adapter of 2026-11-23 (cellular, location; phone-tested in RAM runs), with the hotspot made to work.
 * Nothing here touches MobileGestalt: hooks of it in the UI processes cost SIGBUSes in those runs.
 */
#import <UIKit/UIKit.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <net/if.h>

#define CONF            @"/private/var/mobile/Library/Preferences/com.hakios.hotspot.conf"
#define CONF_CHANGED    "com.hakios.hotspot.changed"

/* PSCellType */
enum { GROUP = 0, LINK = 1, LINK_LIST = 2, TITLE_VALUE = 4, SWITCH = 6, EDIT_TEXT = 8 };

static int console = -1;
static char rows_key, built_key;
static BOOL building_root, building_privacy;
static __weak id root_controller;
static id (*original_root)(id, SEL);
static id (*original_privacy)(id, SEL);
static id (*original_cell_status)(id, SEL, id);

static void
say(NSString *text)
{
	if (console < 0) {
		console = open("/dev/console", O_WRONLY | O_NOCTTY);
	}
	const char *line = [@"settings: " stringByAppendingFormat:@"%@\n", text].UTF8String;
	if (line != NULL) {
		(void)write(console >= 0 ? console : 2, line, strlen(line));
	}
}

static long
number(const char *name, long otherwise)
{
	int value = 0;
	size_t size = sizeof(value);

	return sysctlbyname(name, &value, &size, NULL, 0) == 0 ? value : otherwise;
}

static NSString *
string(const char *name)
{
	char value[128] = { 0 };
	size_t size = sizeof(value) - 1;

	if (sysctlbyname(name, value, &size, NULL, 0) != 0 || value[0] == 0) {
		return nil;
	}
	return [NSString stringWithUTF8String:value];
}

/* in the language Settings itself is in (its bundle's choice), not merely the first one asked for */
static BOOL
chinese(void)
{
	return [NSBundle.mainBundle.preferredLocalizations.firstObject hasPrefix:@"zh"];
}

static NSString *
T(NSString *english, NSString *zh)
{
	return chinese() ? zh : english;
}

/* iOS's own words: KEY of TABLE in the bundle at PATH, or OTHERWISE */
static NSString *
words(NSString *path, NSString *table, NSString *key, NSString *otherwise)
{
	static NSMutableDictionary *bundles;
	NSBundle *bundle;
	NSString *text;

	if (bundles == nil) {
		bundles = [NSMutableDictionary dictionary];
	}
	if ((bundle = bundles[path]) == nil && (bundle = [NSBundle bundleWithPath:path]) != nil) {
		bundles[path] = bundle;
	}
	text = [bundle localizedStringForKey:key value:@"\x01" table:table];
	return text == nil || [text isEqualToString:@"\x01"] || [text isEqualToString:key] ? otherwise : text;
}

static NSString *
M(NSString *key, NSString *otherwise)
{
	return words(@"/System/Library/PreferenceBundles/WirelessModemSettings.bundle", @"WirelessModemSettings", key, otherwise);
}

static NSString *
S(NSString *key, NSString *otherwise)
{
	return words(@"/System/Library/PrivateFrameworks/PreferencesUI.framework", @"Settings", key, otherwise);
}

static id
property(id spec, NSString *key)
{
	SEL get = NSSelectorFromString(@"propertyForKey:");
	return [spec respondsToSelector:get] ? ((id (*)(id, SEL, id))objc_msgSend)(spec, get, key) : nil;
}

static void
set_property(id spec, id value, NSString *key)
{
	SEL set = NSSelectorFromString(@"setProperty:forKey:");
	if (spec != nil && value != nil && [spec respondsToSelector:set]) {
		((void (*)(id, SEL, id, id))objc_msgSend)(spec, set, value, key);
	}
}

static void
set_identifier(id spec, NSString *identifier)
{
	SEL set = NSSelectorFromString(@"setIdentifier:");
	if ([spec respondsToSelector:set]) {
		((void (*)(id, SEL, id))objc_msgSend)(spec, set, identifier);
	}
}

static NSString *
identifier_of(id spec)
{
	SEL get = NSSelectorFromString(@"identifier");
	return [spec respondsToSelector:get] ? ((id (*)(id, SEL))objc_msgSend)(spec, get) : nil;
}

static id
specifier(NSString *name, id target, SEL set, SEL get, Class detail, long cell)
{
	Class spec = NSClassFromString(@"PSSpecifier");
	SEL make = NSSelectorFromString(@"preferenceSpecifierNamed:target:set:get:detail:cell:edit:");

	if (![spec respondsToSelector:make]) {
		return nil;
	}
	return ((id (*)(id, SEL, id, id, SEL, SEL, Class, long, Class))objc_msgSend)(spec, make, name, target, set, get, detail, cell, Nil);
}

/* A settings icon: iOS's own from its cache, or SYMBOL in white on COLOR, as the list draws them */
static UIImage *
icon(NSString *key, NSString *symbol, UIColor *color)
{
	Class cache_class = NSClassFromString(@"PKIconImageCache");
	SEL shared = NSSelectorFromString(@"settingsIconCache"), image_for = NSSelectorFromString(@"imageForKey:");
	id cache = [cache_class respondsToSelector:shared] ? ((id (*)(id, SEL))objc_msgSend)(cache_class, shared) : nil;
	id image = [cache respondsToSelector:image_for] ? ((id (*)(id, SEL, id))objc_msgSend)(cache, image_for, key) : nil;

	if ([image isKindOfClass:UIImage.class]) {
		return image;
	}
	UIImage *glyph = [[UIImage systemImageNamed:symbol withConfiguration:
	    [UIImageSymbolConfiguration configurationWithPointSize:15 weight:UIImageSymbolWeightSemibold]]
	    imageWithTintColor:UIColor.whiteColor renderingMode:UIImageRenderingModeAlwaysOriginal];
	UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(29, 29)];
	return [renderer imageWithActions:^(__unused UIGraphicsImageRendererContext *context) {
		[color setFill];
		[[UIBezierPath bezierPathWithRoundedRect:CGRectMake(0, 0, 29, 29) cornerRadius:6.5] fill];
		if (glyph != nil) {
			[glyph drawInRect:CGRectMake((29 - glyph.size.width) / 2, (29 - glyph.size.height) / 2, glyph.size.width, glyph.size.height)];
		}
	}];
}

static void
keep_rows(id controller, NSArray *rows)
{
	objc_setAssociatedObject(controller, &rows_key, rows, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	SEL set = NSSelectorFromString(@"setSpecifiers:");
	if ([controller respondsToSelector:set]) {
		((void (*)(id, SEL, id))objc_msgSend)(controller, set, rows);
	}
}

static void
reload_values(id controller)
{
	SEL reload = NSSelectorFromString(@"reloadSpecifier:");
	for (id spec in objc_getAssociatedObject(controller, &rows_key)) {
		if ([controller respondsToSelector:reload]) {
			((void (*)(id, SEL, id))objc_msgSend)(controller, reload, spec);
		}
	}
}

static void
reload_root_row(NSString *identifier)
{
	id root = root_controller;
	SEL reload = NSSelectorFromString(@"reloadSpecifierID:");
	if ([root respondsToSelector:reload]) {
		((void (*)(id, SEL, id))objc_msgSend)(root, reload, identifier);
	}
}

static void
alert(UIViewController *controller, NSString *title, NSString *message)
{
	UIAlertController *box = [UIAlertController alertControllerWithTitle:title message:message
	    preferredStyle:UIAlertControllerStyleAlert];
	[box addAction:[UIAlertAction actionWithTitle:T(@"OK", @"好") style:UIAlertActionStyleDefault handler:nil]];
	[controller presentViewController:box animated:YES completion:nil];
}


/* ---- Personal Hotspot ---- */

static BOOL
hotspot_here(void)
{
	return number("kern.hakios.hotspot", -1) >= 0;
}

/*
 * The phone's name as General > About > Name has it: MobileGestalt's UserAssignedDeviceName (what lockdownd keeps). Not
 * UIDevice's name, which on iOS 16 is "iPhone" to a program without the user-assigned-device-name entitlement.
 */
static NSString *
phone_name(void)
{
	static CFTypeRef (*answer)(CFStringRef);
	static dispatch_once_t once;
	NSString *name = nil;

	dispatch_once(&once, ^{
		void *gestalt = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_LAZY);
		answer = gestalt != NULL ? (CFTypeRef (*)(CFStringRef))dlsym(gestalt, "MGCopyAnswer") : NULL;
	});
	CFTypeRef value = answer != NULL ? answer(CFSTR("UserAssignedDeviceName")) : NULL;
	if (value != NULL && CFGetTypeID(value) == CFStringGetTypeID()) {
		name = (__bridge_transfer NSString *)value;
	} else if (value != NULL) {
		CFRelease(value);
	}
	return name.length > 0 ? name : UIDevice.currentDevice.name;
}

/* The network's name: the phone's, as an iPhone's hotspot is named, cut to the 32 bytes of an SSID */
static NSString *
network_name(void)
{
	NSString *name = phone_name();

	name = [[name componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet] componentsJoinedByString:@" "];
	while (name.length > 0 && [name lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 32) {
		name = [name substringToIndex:[name rangeOfComposedCharacterSequenceAtIndex:name.length - 1].location];
	}
	return name.length > 0 ? name : @"iPhone";
}

static NSMutableDictionary *
conf_read(void)
{
	NSMutableDictionary *conf = [NSMutableDictionary dictionary];
	NSString *text = [NSString stringWithContentsOfFile:CONF encoding:NSUTF8StringEncoding error:nil];

	for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
		NSRange equals = [line rangeOfString:@"="];
		if (equals.location != NSNotFound) {
			conf[[line substringToIndex:equals.location]] = [line substringFromIndex:equals.location + 1];
		}
	}
	return conf;
}

static NSString *
random_password(void)
{
	static const char letters[] = "abcdefghijkmnpqrstuvwxyz23456789";
	char password[13];

	for (int i = 0; i < 12; i++) {
		password[i] = letters[arc4random_uniform(sizeof(letters) - 1)];
	}
	password[12] = 0;
	return [NSString stringWithUTF8String:password];
}

/* CONF as CONF says, named after the phone; then the server is told */
static BOOL
conf_write(NSMutableDictionary *conf)
{
	NSError *error = nil;

	conf[@"name"] = network_name();
	if ([conf[@"password"] length] < 8) {
		conf[@"password"] = random_password();
	}
	NSString *text = [NSString stringWithFormat:@"on=%d\nname=%@\npassword=%@\n", [conf[@"on"] intValue] != 0, conf[@"name"],
	    conf[@"password"]];
	if (![text writeToFile:CONF atomically:YES encoding:NSUTF8StringEncoding error:&error]) {
		say([NSString stringWithFormat:@"hotspot: %@ not written: %@", CONF, error.localizedDescription]);
		return NO;
	}
	(void)chmod(CONF.fileSystemRepresentation, 0600);
	notify_post(CONF_CHANGED);
	return YES;
}

static BOOL
hotspot_wanted(void)
{
	return [conf_read()[@"on"] intValue] != 0;
}

static id
hotspot_switch_value(__unused id self, __unused SEL command, __unused id spec)
{
	return @(hotspot_wanted());
}

static void
hotspot_switch_set(__unused id self, __unused SEL command, id value, __unused id spec)
{
	NSMutableDictionary *conf = conf_read();

	conf[@"on"] = [value boolValue] ? @"1" : @"0";
	if (conf_write(conf)) {
		say([NSString stringWithFormat:@"hotspot: asked %@", [value boolValue] ? @"on" : @"off"]);
	}
	reload_root_row(@"INTERNET_TETHERING");
}

static id
hotspot_password_value(__unused id self, __unused SEL command, __unused id spec)
{
	return conf_read()[@"password"] ?: @"";
}

static void
hotspot_password_set(id self, __unused SEL command, id value, __unused id spec)
{
	NSString *password = [value isKindOfClass:NSString.class] ? value : @"";
	NSMutableDictionary *conf = conf_read();
	BOOL printable = YES;

	if ([password isEqualToString:conf[@"password"] ?: @""]) {
		return;
	}
	for (NSUInteger i = 0; i < password.length; i++) {
		unichar c = [password characterAtIndex:i];
		printable = printable && c >= 0x20 && c < 0x7f;
	}
	if (password.length < 8 || password.length > 63 || !printable) {
		alert(self, [NSString stringWithFormat:M(@"WIFI_PASSWORD_TITLE", @"%@ Password"), M(@"WIFI", @"Wi-Fi")],
		    T(@"The password must be 8 to 63 characters: letters, numbers and punctuation.",
		      @"密碼必須是 8 到 63 個字元（英文字母、數字或符號）。"));
		reload_values(self);
		return;
	}
	conf[@"password"] = password;
	if (conf_write(conf)) {
		say(@"hotspot: a new password");
	}
}

static id
password_rows(id self, __unused SEL command)
{
	id cached = objc_getAssociatedObject(self, &rows_key);
	if (cached != nil) {
		return cached;
	}
	NSString *wifi = M(@"WIFI", @"Wi-Fi");
	id group = specifier(@"", self, NULL, NULL, Nil, GROUP);
	id field = specifier(M(@"PASSWORD", @"Password"), self, NSSelectorFromString(@"hk_setPassword:specifier:"),
	    NSSelectorFromString(@"hk_password:"), Nil, EDIT_TEXT);
	SEL keyboard = NSSelectorFromString(@"setKeyboardType:autoCaps:autoCorrection:");

	if (group == nil || field == nil) {
		return @[];
	}
	set_property(group, [NSString stringWithFormat:M(@"WIFI_PASSWORD_FOOTER", @"Other users will join your shared %@ network "
	    "using this password.\n\nThe password must contain at least 8 characters. Changing the password will disconnect any "
	    "currently connected users."), wifi], @"footerText");
	if ([field respondsToSelector:keyboard]) {
		((void (*)(id, SEL, long long, long long, long long))objc_msgSend)(field, keyboard, UIKeyboardTypeASCIICapable,
		    UITextAutocapitalizationTypeNone, UITextAutocorrectionTypeNo);
	}
	set_identifier(field, @"HAKIOS_HOTSPOT_PASSWORD");
	NSArray *rows = @[group, field];
	keep_rows(self, rows);
	return rows;
}

static id
hotspot_rows(id self, __unused SEL command)
{
	id cached = objc_getAssociatedObject(self, &rows_key);
	if (cached != nil) {
		return cached;
	}
	NSString *name = network_name(), *wifi = M(@"WIFI", @"Wi-Fi");
	NSMutableDictionary *conf = conf_read();
	NSMutableArray *rows = [NSMutableArray array];
	id group = specifier(@"", self, NULL, NULL, Nil, GROUP);
	id allow = specifier(M(@"ALLOW_OTHERS", @"Allow Others to Join"), self, NSSelectorFromString(@"hk_setOn:specifier:"),
	    NSSelectorFromString(@"hk_on:"), Nil, SWITCH);
	id password = specifier([NSString stringWithFormat:M(@"WIFI_PASSWORD", @"%@ Password"), wifi], self, NULL,
	    NSSelectorFromString(@"hk_password:"), NSClassFromString(@"HKHotspotPasswordController"), LINK_LIST);
	id connect = specifier([NSString stringWithFormat:M(@"CONNECT_OVER_WIFI_LABEL", @"TO CONNECT USING %@"),
	    M(@"WIFI_UPPERCASE", @"WI-FI")], self, NULL, NULL, Nil, GROUP);

	if (group == nil || allow == nil || password == nil || connect == nil) {
		return @[];
	}
	/* the phone renamed since: the network follows, as on an iPhone */
	if (conf.count > 0 && ![conf[@"name"] isEqual:name]) {
		(void)conf_write(conf);
	}
	set_property(group, [NSString stringWithFormat:M(@"TETHERING_TEXT_DEFAULT", @"Allow other users or devices not signed into "
	    "iCloud to look for your shared network “%@” when you are in Personal Hotspot settings or when you turn it on in "
	    "Control Center."), name], @"footerText");
	set_identifier(allow, @"HAKIOS_HOTSPOT_SWITCH");
	if (!hotspot_here()) {
		set_property(allow, @NO, @"enabled");
	}
	set_identifier(password, @"HAKIOS_HOTSPOT_PASSWORD_LINK");
	set_property(connect, [NSString stringWithFormat:@"%@  %@\n%@  %@", M(@"STEP_1", @"1"),
	    [NSString stringWithFormat:M(@"CONNECT_OVER_WIFI_STEP_1", @"Choose “%@” from the %@ settings on your computer or other "
	    "device."), name, wifi], M(@"STEP_2", @"2"), M(@"CONNECT_OVER_WIFI_STEP_2", @"Enter the password when prompted.")],
	    @"footerText");
	[rows addObjectsFromArray:@[group, allow, password, connect]];
	keep_rows(self, rows);
	say(@"hotspot: the pane is up");
	return rows;
}

/* back from the password's pane: the row says the new one */
static void
hotspot_will_appear(id self, SEL command, BOOL animated)
{
	struct objc_super parent = { self, class_getSuperclass(object_getClass(self)) };

	((void (*)(struct objc_super *, SEL, BOOL))objc_msgSendSuper)(&parent, command, animated);
	reload_values(self);
}

static id
hotspot_status(__unused id self, __unused SEL command, __unused id spec)
{
	return hotspot_wanted() ? M(@"ON", @"On") : M(@"OFF", @"Off");
}


/* Prefer the original iOS cellular pane only with a real native interface,
 * a loaded original controller and a successful native capability query.
 * Legacy en1-only profiles retain their existing status-only fallback. */
static BOOL native_cellular_ready(void)
{
    if(!NSClassFromString(@"PSUICellularController") || !if_nametoindex("pdp_ip0"))return NO;
    static void *library,*connection;
    if(!library)library=dlopen("/System/Library/Frameworks/CoreTelephony.framework/CoreTelephony",RTLD_NOW|RTLD_GLOBAL);
    if(!library)return NO;
    void *(*create)(CFAllocatorRef,void *,void *)=dlsym(library,"_CTServerConnectionCreate");
    uint64_t (*settings)(void *,bool *,bool *,bool *)=dlsym(library,"_CTServerConnectionGetCellularDataSettings");
    if(!create || !settings)return NO;
    if(!connection)connection=create(kCFAllocatorDefault,NULL,NULL);
    bool capable=false,enabled=false,airplane=false;
    return connection && settings(connection,&capable,&enabled,&airplane)==0 && capable;
}

/* ---- Cellular, read only ---- */

static id
cellular_value(__unused id self, __unused SEL command, id spec)
{
	NSString *key = property(spec, @"HKKey");

	if ([key isEqualToString:@"data"]) {
		return number("kern.hakios.cellular_data", 0) == 1 ? T(@"Connected", @"已連線") : T(@"Not Connected", @"未連線");
	}
	if ([key isEqualToString:@"registered"]) {
		return number("kern.hakios.cellular_registered", 0) == 1 ? T(@"Registered", @"已註冊") : T(@"Searching", @"搜尋中");
	}
	if ([key isEqualToString:@"operator"]) {
		return string("kern.hakios.cellular_operator") ?: T(@"Unknown", @"未知");
	}
	if ([key isEqualToString:@"signal"]) {
		long dbm = number("kern.hakios.cellular_dbm", 0);
		return dbm < 0 ? [NSString stringWithFormat:@"%ld dBm", dbm] : T(@"Unknown", @"未知");
	}
	if ([key isEqualToString:@"radio"]) {
		switch (number("kern.hakios.cellular_radio", 0)) {
		case 4: return @"GSM";
		case 5: return @"UMTS";
		case 8: return @"LTE";
		case 12: return @"5G";
		default: return T(@"Unknown", @"未知");
		}
	}
	return @"";
}

static id
cellular_rows(id self, __unused SEL command)
{
	id cached = objc_getAssociatedObject(self, &rows_key);
	if (cached != nil) {
		return cached;
	}
	NSMutableArray *rows = [NSMutableArray array];
	id group = specifier(@"", self, NULL, NULL, Nil, GROUP);

	if (group == nil) {
		return @[];
	}
	set_property(group, T(@"Cellular data connects by itself. Turning it off, choosing a carrier and editing the APN are not "
	    "supported yet.", @"行動數據會自動連線。目前還不能在這裡關閉行動數據、選擇電信業者或編輯 APN。"), @"footerText");
	[rows addObject:group];
	for (NSArray *row in @[@[T(@"Cellular Data", @"行動數據"), @"data"], @[T(@"Network", @"行動網路"), @"registered"],
	    @[T(@"Carrier", @"電信業者"), @"operator"], @[T(@"Network Type", @"網路類型"), @"radio"],
	    @[T(@"Signal", @"訊號強度"), @"signal"]]) {
		id spec = specifier(row[0], self, NULL, NSSelectorFromString(@"hk_value:"), Nil, TITLE_VALUE);
		if (spec != nil) {
			set_property(spec, row[1], @"HKKey");
			[rows addObject:spec];
		}
	}
	keep_rows(self, rows);
	return rows;
}

static id
cellular_status(id self, SEL command, id spec)
{
	if(native_cellular_ready() && original_cell_status!=NULL)return original_cell_status(self,command,spec);
	if (number("kern.hakios.cellular_up", 0) == 1) {
		return number("kern.hakios.cellular_data", 0) == 1 ? @"" : T(@"Not Connected", @"未連線");
	}
	return original_cell_status != NULL ? original_cell_status(self, command, spec) : @"";
}


/* ---- the main list ---- */

static NSUInteger
index_of(NSArray *rows, NSString *identifier)
{
	for (NSUInteger i = 0; i < rows.count; i++) {
		if ([identifier_of(rows[i]) isEqual:identifier]) {
			return i;
		}
	}
	return NSNotFound;
}

static id
root_rows(id self, SEL command)
{
	if (building_root) {
		return objc_getAssociatedObject(self, &rows_key);
	}
	id native = original_root(self, command);
	BOOL cellular = number("kern.hakios.cellular_up", 0) == 1, hotspot = hotspot_here();

	if (![native isKindOfClass:NSArray.class] || (!cellular && !hotspot)) {
		return native;
	}
	NSMutableArray *rows = [native mutableCopy];
	NSUInteger at_cellular = index_of(rows, @"MOBILE_DATA_SETTINGS_ID"), at_bluetooth = index_of(rows, @"Bluetooth");
	BOOL changed = NO;

	root_controller = self;
	if (cellular) {
		id native_row = at_cellular != NSNotFound ? rows[at_cellular] : nil;
		if(native_cellular_ready()) {
            BOOL keep=native_row && ![property(native_row,@"HKPortCellular") boolValue];
            id row=keep?native_row:specifier(S(@"CELLULAR",@"Cellular"),self,NULL,NULL,NSClassFromString(@"PSUICellularController"),LINK);
            if(row) {
                set_identifier(row,@"MOBILE_DATA_SETTINGS_ID");
                set_property(row,@YES,@"HKNativeCellular");
                if(!keep)set_property(row,property(native_row,@"iconImage")?:icon(@"CellularData",@"antenna.radiowaves.left.and.right",UIColor.systemGreenColor),@"iconImage");
                if(at_cellular!=NSNotFound)rows[at_cellular]=row;
                else if(at_bluetooth!=NSNotFound){[rows insertObject:row atIndex:at_bluetooth+1];at_cellular=at_bluetooth+1;}
                changed=YES;
                static unsigned announced;
                if(announced++<4)say([NSString stringWithFormat:@"native-cellular-v1 row=1 original-row-preserved=%d controller=PSUICellularController",keep]);
            }
        } else if (![property(native_row, @"HKPortCellular") boolValue]) {
			id row = specifier(S(@"CELLULAR", @"Cellular"), self, NULL, NULL, NSClassFromString(@"HKCellularStatusController"), LINK);
			if (row != nil) {
				set_identifier(row, @"MOBILE_DATA_SETTINGS_ID");
				set_property(row, @YES, @"HKPortCellular");
				set_property(row, property(native_row, @"iconImage") ?: icon(@"CellularData", @"antenna.radiowaves.left.and.right",
				    UIColor.systemGreenColor), @"iconImage");
				if (at_cellular != NSNotFound) {
					rows[at_cellular] = row;
				} else if (at_bluetooth != NSNotFound) {
					[rows insertObject:row atIndex:at_bluetooth + 1];
					at_cellular = at_bluetooth + 1;
				}
				changed = YES;
			}
		}
	}
	if (hotspot && index_of(rows, @"INTERNET_TETHERING") == NSNotFound) {
		NSUInteger after = at_cellular != NSNotFound ? at_cellular : index_of(rows, @"Bluetooth");
		id row = specifier(S(@"PERSONAL_HOTSPOT", @"Personal Hotspot"), self, NULL, NSSelectorFromString(@"hk_hotspotStatus:"),
		    NSClassFromString(@"HKHotspotController"), LINK_LIST);
		if (row != nil && after != NSNotFound) {
			set_identifier(row, @"INTERNET_TETHERING");
			set_property(row, icon(@"PersonalHotspot", @"personalhotspot", UIColor.systemGreenColor), @"iconImage");
			[rows insertObject:row atIndex:after + 1];
			changed = YES;
		}
	}
	if (!changed) {
		return native;
	}
	building_root = YES;
	keep_rows(self, rows);
	building_root = NO;
	if (objc_getAssociatedObject(self, &built_key) == nil) {
		objc_setAssociatedObject(self, &built_key, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
		say([NSString stringWithFormat:@"main list: cellular %d, hotspot %d", cellular, hotspot]);
	}
	return rows;
}

static void
refresh_root(UIViewController *controller)
{
	if (controller == nil) {
		return;
	}
	if ([controller isKindOfClass:NSClassFromString(@"PSUIPrefsListController")]) {
		SEL reload = NSSelectorFromString(@"reloadSpecifiers");
		if ([controller respondsToSelector:reload]) {
			((void (*)(id, SEL))objc_msgSend)(controller, reload);
		}
	}
	for (UIViewController *child in controller.childViewControllers) {
		refresh_root(child);
	}
}

static id
privacy_rows(id self, SEL command)
{
	if (building_privacy) {
		return objc_getAssociatedObject(self, &rows_key);
	}
	id native = original_privacy(self, command);
	Class location = NSClassFromString(@"PUILocationServicesListController");

	if (![native isKindOfClass:NSArray.class] || location == Nil) {
		return native;
	}
	for (id spec in native) {
		SEL get = NSSelectorFromString(@"detailControllerClass");
		Class detail = [spec respondsToSelector:get] ? ((Class (*)(id, SEL))objc_msgSend)(spec, get) : Nil;
		if (detail == location) {
			set_identifier(spec, @"LOCATION");
			return native;
		}
	}
	id row = specifier(T(@"Location Services", @"定位服務"), self, NULL, NULL, location, LINK);
	if (row == nil) {
		return native;
	}
	set_identifier(row, @"LOCATION");
	NSMutableArray *rows = [native mutableCopy];
	[rows insertObject:row atIndex:MIN((NSUInteger)1, rows.count)];
	building_privacy = YES;
	keep_rows(self, rows);
	building_privacy = NO;
	say(@"privacy: Location Services' row put back");
	return rows;
}

/* CLASS's NAME replaced by IMPLEMENTATION, if it returns an object and takes what NAME says; the old one returned */
static IMP
replace(Class cls, NSString *name, IMP implementation)
{
	SEL selector = NSSelectorFromString(name);
	Method method = class_getInstanceMethod(cls, selector);

	if (method == NULL || method_getNumberOfArguments(method) != (unsigned)([name componentsSeparatedByString:@":"].count + 1)) {
		return NULL;
	}
	char *type = method_copyReturnType(method);
	BOOL object = type != NULL && strcmp(type, "@") == 0;
	free(type);
	if (!object) {
		return NULL;
	}
	IMP previous = method_getImplementation(method);
	if (!class_addMethod(cls, selector, implementation, method_getTypeEncoding(method))) {
		method_setImplementation(class_getInstanceMethod(cls, selector), implementation);
	}
	return previous;
}

static Class
controller(Class base, const char *name, IMP rows)
{
	Class cls = objc_allocateClassPair(base, name, 0);

	if (cls != Nil) {
		class_addMethod(cls, NSSelectorFromString(@"specifiers"), rows, "@16@0:8");
	}
	return cls;
}

__attribute__((constructor))
static void
settings_init(void)
{
	if (strcmp(getprogname(), "Preferences") != 0) {
		return;
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		dlopen("/System/Library/PrivateFrameworks/Settings/PrivacySettingsUI.framework/PrivacySettingsUI", RTLD_LAZY);
		[[NSBundle bundleWithPath:@"/System/Library/PreferenceBundles/SettingsCellular.bundle"] load];
		Class base = NSClassFromString(@"PSListController"), root = NSClassFromString(@"PSUIPrefsListController");
		Class privacy = NSClassFromString(@"PUIPrivacyController"), cls;

		if (base == Nil || root == Nil) {
			say(@"no PSListController or PSUIPrefsListController: Settings as it is");
			return;
		}
		if ((cls = controller(base, "HKHotspotController", (IMP)hotspot_rows)) != Nil) {
			class_addMethod(cls, NSSelectorFromString(@"hk_on:"), (IMP)hotspot_switch_value, "@24@0:8@16");
			class_addMethod(cls, NSSelectorFromString(@"hk_setOn:specifier:"), (IMP)hotspot_switch_set, "v32@0:8@16@24");
			class_addMethod(cls, NSSelectorFromString(@"hk_password:"), (IMP)hotspot_password_value, "@24@0:8@16");
			class_addMethod(cls, NSSelectorFromString(@"viewWillAppear:"), (IMP)hotspot_will_appear, "v20@0:8B16");
			objc_registerClassPair(cls);
		}
		if ((cls = controller(base, "HKHotspotPasswordController", (IMP)password_rows)) != Nil) {
			class_addMethod(cls, NSSelectorFromString(@"hk_password:"), (IMP)hotspot_password_value, "@24@0:8@16");
			class_addMethod(cls, NSSelectorFromString(@"hk_setPassword:specifier:"), (IMP)hotspot_password_set, "v32@0:8@16@24");
			objc_registerClassPair(cls);
		}
		if ((cls = controller(base, "HKCellularStatusController", (IMP)cellular_rows)) != Nil) {
			class_addMethod(cls, NSSelectorFromString(@"hk_value:"), (IMP)cellular_value, "@24@0:8@16");
			objc_registerClassPair(cls);
		}
		class_addMethod(root, NSSelectorFromString(@"hk_hotspotStatus:"), (IMP)hotspot_status, "@24@0:8@16");
		original_root = (void *)replace(root, @"specifiers", (IMP)root_rows);
		original_cell_status = (void *)replace(root, @"cellularDataStatusForSpecifier:", (IMP)cellular_status);
		if (privacy != Nil) {
			original_privacy = (void *)replace(privacy, @"specifiers", (IMP)privacy_rows);
		}
		say([NSString stringWithFormat:@"in (main list %d, hotspot %d, cellular %ld)", original_root != NULL, hotspot_here(),
		    number("kern.hakios.cellular_up", 0)]);
		for (UIWindow *window in UIApplication.sharedApplication.windows) {
			refresh_root(window.rootViewController);
		}
	});
}
