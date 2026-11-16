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
 * Signed with uicache's entitlements (src/lsplugins.entitlements): lsd asks for them.
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <objc/message.h>

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
