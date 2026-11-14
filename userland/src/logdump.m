/*
 * logdump: what logd has kept (/private/var/db/diagnostics), read on the phone itself through OSLog's OSLogStore, the
 * way "log show" reads it on a Mac -- the phone has no "log" command, macOS's cannot read the phone's store
 * ("partial or missing metadata", r157), and only the processes named in OSLOG print their os_log on the console.
 * Safari and its WebKit processes are none of those: their pages never loaded (r156, r157) while the network did.
 *
 *   usage: logdump SECONDS [PREDICATE]
 *          the entries of the last SECONDS, those PREDICATE matches (NSPredicate over OSLogEntryLog: process,
 *          subsystem, category, composedMessage, level...; default: Safari's and WebKit's processes)
 *
 * The system's store is not for iOS applications (OSLogStore's scope is macOS's only in the SDK): the class is
 * reached at run time, and the program is signed with com.apple.logging.local-store (src/logdump.entitlements).
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * What "log show" itself uses on a Mac (LoggingSupport.framework, private): the local store, prepared into a source, read by
 * an event stream from a date on, with the predicate as its filter.
 */
static int
through_stream(double seconds, NSString *wanted)
{
	Class stores, streams;
	id store, stream;
	__block id source = nil;
	__block unsigned long count = 0;
	dispatch_semaphore_t ready = dispatch_semaphore_create(0), done = dispatch_semaphore_create(0);
	NSDateFormatter *clock = [[NSDateFormatter alloc] init];

	dlopen("/System/Library/PrivateFrameworks/LoggingSupport.framework/LoggingSupport", RTLD_NOW);
	stores = NSClassFromString(@"OSLogEventStore");
	streams = NSClassFromString(@"OSLogEventStream");
	if (stores == Nil || streams == Nil) {
		printf("logdump: no LoggingSupport here either\n");
		return 1;
	}
	store = ((id (*)(id, SEL))objc_msgSend)(stores, @selector(localStore));
	((void (*)(id, SEL, id))objc_msgSend)(store, @selector(prepareWithCompletionHandler:), ^(id prepared, NSError *error) {
		source = prepared;
		if (prepared == nil) {
			printf("logdump: the local store cannot be prepared: %s\n", error.description.UTF8String);
		}
		dispatch_semaphore_signal(ready);
	});
	dispatch_semaphore_wait(ready, DISPATCH_TIME_FOREVER);
	if (source == nil) {
		return 1;
	}
	clock.dateFormat = @"HH:mm:ss.SSS";
	stream = ((id (*)(id, SEL, id))objc_msgSend)([streams alloc], @selector(initWithSource:), source);
	((void (*)(id, SEL, NSPredicate *))objc_msgSend)(stream, @selector(setFilterPredicate:), [NSPredicate predicateWithFormat:wanted]);
	((void (*)(id, SEL, id))objc_msgSend)(stream, @selector(setEventHandler:), ^(id event) {
		NSString *message = [event valueForKey:@"composedMessage"];

		printf("%s %s[%d] %s:%s %s\n", [clock stringFromDate:[event valueForKey:@"date"]].UTF8String,
		    [[event valueForKey:@"process"] description].UTF8String, [[event valueForKey:@"processIdentifier"] intValue],
		    [[event valueForKey:@"subsystem"] description].UTF8String, [[event valueForKey:@"category"] description].UTF8String,
		    [[message stringByReplacingOccurrencesOfString:@"\n" withString:@" "] UTF8String]);
		count++;
	});
	((void (*)(id, SEL, id))objc_msgSend)(stream, @selector(setInvalidationHandler:), ^(NSUInteger reason, id position) {
		printf("logdump: %lu entries (the stream ended: %lu)\n", count, (unsigned long)reason);
		dispatch_semaphore_signal(done);
	});
	((void (*)(id, SEL, NSDate *))objc_msgSend)(stream, @selector(activateStreamFromDate:), [NSDate dateWithTimeIntervalSinceNow:-seconds]);
	dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(120 * NSEC_PER_SEC)));
	return 0;
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		double seconds = argc > 1 ? atof(argv[1]) : 300;
		NSString *wanted = argc > 2 ? @(argv[2]) :
		    @"process CONTAINS 'Safari' OR process CONTAINS 'WebKit' OR process CONTAINS 'WebContent' OR "
		    @"process CONTAINS 'Networking' OR subsystem BEGINSWITH 'com.apple.WebKit' OR subsystem BEGINSWITH 'com.apple.Safari'";
		void *framework = dlopen("/System/Library/Frameworks/OSLog.framework/OSLog", RTLD_NOW);
		Class stores = framework != NULL ? NSClassFromString(@"OSLogStore") : Nil;
		NSError *error = nil;
		id store, position, entries;
		NSPredicate *predicate;
		unsigned long count = 0;

		setvbuf(stdout, NULL, _IOLBF, 0);
		if (stores == Nil) {
			printf("logdump: no OSLogStore here\n");
			return 1;
		}
		if ([stores respondsToSelector:@selector(localStoreAndReturnError:)]) {
			store = ((id (*)(id, SEL, NSError **))objc_msgSend)(stores, @selector(localStoreAndReturnError:), &error);
		} else {
			store = ((id (*)(id, SEL, NSInteger, NSError **))objc_msgSend)(stores, @selector(storeWithScope:error:), 0, &error);
		}
		if (store == nil) {
			store = ((id (*)(id, SEL, NSInteger, NSError **))objc_msgSend)(stores, @selector(storeWithScope:error:), 0, &error);
		}
		if (store == nil) {
			printf("logdump: OSLogStore gives no store (%s): through LoggingSupport's event stream\n",
			    error.description.UTF8String);
			return through_stream(seconds, wanted);
		}
		@try {
			predicate = [NSPredicate predicateWithFormat:wanted];
		} @catch (NSException *exception) {
			printf("logdump: bad predicate: %s\n", exception.reason.UTF8String);
			return 1;
		}
		position = ((id (*)(id, SEL, NSDate *))objc_msgSend)(store, @selector(positionWithDate:),
		    [NSDate dateWithTimeIntervalSinceNow:-seconds]);
		entries = ((id (*)(id, SEL, NSUInteger, id, NSPredicate *, NSError **))objc_msgSend)(store,
		    @selector(entriesEnumeratorWithOptions:position:predicate:error:), 0, position, predicate, &error);
		if (entries == nil) {
			printf("logdump: no entries: %s\n", error.description.UTF8String);
			return 1;
		}
		NSDateFormatter *clock = [[NSDateFormatter alloc] init];

		clock.dateFormat = @"HH:mm:ss.SSS";
		for (id entry in entries) {
			NSString *process = [entry respondsToSelector:@selector(process)] ? [entry valueForKey:@"process"] : @"?";
			NSString *subsystem = [entry respondsToSelector:@selector(subsystem)] ? [entry valueForKey:@"subsystem"] : @"";
			NSString *category = [entry respondsToSelector:@selector(category)] ? [entry valueForKey:@"category"] : @"";
			NSString *message = [entry valueForKey:@"composedMessage"];
			NSNumber *pid = [entry respondsToSelector:@selector(processIdentifier)] ? [entry valueForKey:@"processIdentifier"] : @0;

			printf("%s %s[%d] %s:%s %s\n", [clock stringFromDate:[entry valueForKey:@"date"]].UTF8String, process.UTF8String,
			    pid.intValue, subsystem.UTF8String, category.UTF8String,
			    [[message stringByReplacingOccurrencesOfString:@"\n" withString:@" "] UTF8String]);
			count++;
		}
		printf("logdump: %lu entries\n", count);
	}
	return 0;
}
