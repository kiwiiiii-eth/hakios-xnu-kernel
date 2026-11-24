/*
 * lsplugins: system app extensions registered with LaunchServices one by one (LSApplicationWorkspace registerPlugin:).
 *
 * The data volume keeps LaunchServices' database from the first boot on (Beta 0.5). Five extensions of the frameworks
 * reached the system image only on 16 November (sysimage/select-system.py had left out programs over 4 MB): Files'
 * "On My iPhone" (FileProvider.framework's LocalStorageFileProvider) and the document picker every application opens
 * (DocumentManagerUICore's com.apple.DocumentManager.Service; TrollStore's "Install IPA File" said "The requested app
 * extension could not be found"). The database had them as they were then, bundles with no program, and neither a boot
 * nor uicache -a looks again at a framework's PlugIns (r222: fileproviderctl listproviders the same before and after).
 * So their two frameworks are read again (rebuildDatabaseContentForFrameworkAtURL:) and each is registered again
 * (registerPlugin:) here, at every boot: it is quick, and changes nothing when the records are right already.
 * (LSApplicationWorkspace is CoreServices', loaded here: r223 found no class with Foundation alone.)
 *
 *   usage: lsplugins [BUNDLE.appex ...]      (none: the five, once: /private/var/db/hakios-lsplugins-1 marks it done)
 *          lsplugins --apps                 the system's applications registered again (registerApplication:), once per
 *                                           system image's time: /private/var/db/hakios-lsapps-TIME marks it done. The database has their names
 *                                           in the languages the image had at the first boot; Chinese reached it on 24
 *                                           November (sysimage/select-system.py's KEEP_LANG), and until the applications
 *                                           are read again the home screen names them in English whatever the language.
 * Signed with uicache's entitlements (src/lsplugins.entitlements): lsd asks for them.
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>

static NSString *const defaults[] = {
	@"/System/Library/Frameworks/FileProvider.framework/PlugIns/LocalStorageFileProvider.appex",
	@"/System/Library/PrivateFrameworks/DocumentManagerUICore.framework/PlugIns/com.apple.DocumentManager.Service.appex",
	@"/System/Library/PrivateFrameworks/DocumentManagerUICore.framework/PlugIns/SaveToFiles.appex",
	@"/System/Library/PrivateFrameworks/DocumentManagerUICore.framework/PlugIns/RecentsAvocado.appex",
	@"/System/Library/PrivateFrameworks/DocumentManagerUICore.framework/PlugIns/RecentsAppPopover.appex",
};

int
main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);       /* (r224 said nothing: its lines were in a buffer when it ended) */
	@autoreleasepool {
		Class workspace_class;

		dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices", RTLD_NOW);
		dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
		workspace_class = NSClassFromString(@"LSApplicationWorkspace");
		id workspace = workspace_class ? ((id (*)(id, SEL))objc_msgSend)(workspace_class, sel_registerName("defaultWorkspace")) : nil;
		SEL registerPlugin = sel_registerName("registerPlugin:");
		NSMutableArray<NSString *> *paths = [NSMutableArray array];
		int failed = 0;

		if (workspace == nil || ![workspace respondsToSelector:registerPlugin]) {
			printf("lsplugins: no LSApplicationWorkspace registerPlugin:\n");
			return 1;
		}
		/* lsplugins --names ID...: what LaunchServices says each is called, in the languages of this process (-AppleLanguages) */
		if (argc >= 2 && strcmp(argv[1], "--names") == 0) {
			Class record_class = NSClassFromString(@"LSApplicationRecord");
			unsigned listed = 0;

			printf("lsplugins: languages %s\n", [NSLocale.preferredLanguages componentsJoinedByString:@","].UTF8String);
			for (int i = 2; i < argc && argv[i][0] != '-'; i++) {
				NSError *error = nil;
				id record = ((id (*)(id, SEL, id, BOOL, NSError **))objc_msgSend)([record_class alloc],
				    sel_registerName("initWithBundleIdentifier:allowPlaceholder:error:"), @(argv[i]), NO, &error);
				id name = [record respondsToSelector:sel_registerName("localizedName")] ? [record valueForKey:@"localizedName"] : nil;
				SEL with = sel_registerName("localizedNameWithPreferredLocalizations:");
				for (NSString *language in @[ @"zh-Hant-TW", @"zh_TW", @"zh-Hant", @"en" ]) {
					id named = [record respondsToSelector:with] ? ((id (*)(id, SEL, id))objc_msgSend)(record, with, @[ language ]) : nil;
					printf("lsplugins: %s in %s: \"%s\"\n", argv[i], language.UTF8String, [named description].UTF8String ?: "?");
				}
				id localizations = nil;
				@try { localizations = [record valueForKey:@"localizations"]; } @catch (NSException *e) { localizations = nil; }
				printf("lsplugins: %s: \"%s\" (localizations %s)%s\n", argv[i], [name description].UTF8String ?: "?",
				    [[localizations description] stringByReplacingOccurrencesOfString:@"\n" withString:@" "].UTF8String ?: "?",
				    error != nil ? error.localizedDescription.UTF8String : "");
			}
			if (listed == 0 && record_class != Nil) {
				unsigned count = 0;
				Method *methods = class_copyMethodList(NSClassFromString(@"LSBundleRecord"), &count);
				for (unsigned m = 0; m < count; m++) {
					const char *name = sel_getName(method_getName(methods[m]));
					if (strcasestr(name, "locali") != NULL) {
						printf("lsplugins: LSBundleRecord %s\n", name);
					}
				}
				free(methods);
			}
			return 0;
		}
		if (argc == 2 && strcmp(argv[1], "--apps") == 0) {
			/* once per system image's time. (Neither this nor uicache -a makes LaunchServices read an application's names again,
			 * r282-r291: a database made afresh does, rc.jobs.lsrebuild6t, r292.) */
			NSDate *made = [[NSFileManager defaultManager] attributesOfItemAtPath:@"/Applications/Preferences.app" error:nil].fileModificationDate;
			NSString *mark = [NSString stringWithFormat:@"/private/var/db/hakios-lsapps-%.0f", made.timeIntervalSince1970];
			SEL registerApplication = sel_registerName("registerApplication:");
			int registered = 0, apps = 0;

			if ([[NSFileManager defaultManager] fileExistsAtPath:mark]) {
				return 0;
			}
			if (![workspace respondsToSelector:registerApplication]) {
				printf("lsplugins: no LSApplicationWorkspace registerApplication:\n");
				return 1;
			}
			for (NSString *folder in @[ @"/Applications", @"/System/Cryptexes/App/Applications" ]) {
				for (NSString *name in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:folder error:nil]) {
					if (![name.pathExtension isEqualToString:@"app"]) {
						continue;
					}
					apps++;
					if (((BOOL (*)(id, SEL, NSURL *))objc_msgSend)(workspace, registerApplication,
					    [NSURL fileURLWithPath:[folder stringByAppendingPathComponent:name]])) {
						registered++;
					} else {
						printf("lsplugins: %s: NOT REGISTERED\n", name.UTF8String);
					}
				}
			}
			printf("lsplugins: %d of %d applications registered again\n", registered, apps);
			/* (once, the few that will not register notwithstanding: applications the image has without their program) */
			if (registered > 0) {
				[[NSData data] writeToFile:mark atomically:YES];
			}
			return registered > 0 ? 0 : 1;
		}
		for (int i = 1; i < argc; i++) {
			[paths addObject:@(argv[i])];
		}
		if (paths.count == 0) {
			if ([[NSFileManager defaultManager] fileExistsAtPath:@"/private/var/db/hakios-lsplugins-1"]) {
				return 0;       /* done at an earlier boot: the records are kept with the data volume */
			}
			[paths addObjectsFromArray:[NSArray arrayWithObjects:defaults count:sizeof(defaults) / sizeof(defaults[0])]];
		}
		/* each extension first (registerPlugin:, a plain answer), then their frameworks read again; a line before every call,
		 * so that a crash says where (r224-r226 printed nothing: the framework's completion was taken for an object) */
		for (NSString *path in paths) {
			BOOL there = [[NSFileManager defaultManager] fileExistsAtPath:path], done = NO;

			printf("lsplugins: %s: registering\n", path.lastPathComponent.UTF8String);
			if (there) {
				done = ((BOOL (*)(id, SEL, NSURL *))objc_msgSend)(workspace, registerPlugin, [NSURL fileURLWithPath:path]);
			}
			failed += !done;
			printf("lsplugins: %s: %s\n", path.lastPathComponent.UTF8String, !there ? "NOT THERE" : done ? "registered" : "NOT REGISTERED");
		}
		if (argc < 2) {
			SEL rebuild = sel_registerName("rebuildDatabaseContentForFrameworkAtURL:completionHandler:");
			NSArray<NSString *> *frameworks = @[ @"/System/Library/Frameworks/FileProvider.framework",
			    @"/System/Library/PrivateFrameworks/DocumentManagerUICore.framework" ];

			for (NSString *framework in frameworks) {
				dispatch_semaphore_t done = dispatch_semaphore_create(0);
				__block int answered = 0;

				if (![workspace respondsToSelector:rebuild]) {
					printf("lsplugins: no rebuildDatabaseContentForFrameworkAtURL:completionHandler:\n");
					break;
				}
				printf("lsplugins: %s: reading it again\n", framework.lastPathComponent.UTF8String);
				/* (the completion's argument is not looked at, nor retained: void *) */
				((void (*)(id, SEL, NSURL *, void (^)(void *)))objc_msgSend)(workspace, rebuild, [NSURL fileURLWithPath:framework],
				    ^(void *unused) {
					(void)unused;
					answered = 1;
					dispatch_semaphore_signal(done);
				});
				dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC));
				printf("lsplugins: %s read again: %s\n", framework.lastPathComponent.UTF8String, answered ? "done" : "no answer in 20 s");
			}
		}
		if (argc < 2 && failed == 0) {
			[[NSData data] writeToFile:@"/private/var/db/hakios-lsplugins-1" atomically:YES];
		}
		return failed ? 1 : 0;
	}
}
