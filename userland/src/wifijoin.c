/*
 * wifijoin SECONDS NAME: what Settings does when a network in its list is tapped, done without the screen -- after
 * SECONDS, MobileWiFi's client asks wifid for a scan, and the network called NAME among what is heard is joined
 * (WiFiDeviceClientScanAsync, WiFiDeviceClientAssociateAsync: four arguments each in Darwin 25's MobileWiFi, the
 * callback and its context last). An open network only: no passphrase is taken, none is printed.
 * Signed with wifijoin.entitlements (wifid serves a client that has com.apple.wifi.manager-access).
 * What happens is said on the console: how many networks the scan heard, whether NAME was among them, the join's answer,
 * and then every 5 seconds for a minute the network wifid says it is on, the address en0 has, and once it has one whether
 * the Internet answers: a name looked up (apple.com) and a TCP connection made (to 1.1.1.1, port 80).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

typedef const void *WiFiManagerRef, *WiFiDeviceRef, *WiFiNetworkRef;
extern WiFiManagerRef WiFiManagerClientCreate(CFAllocatorRef allocator, int type);
extern CFArrayRef WiFiManagerClientCopyDevices(WiFiManagerRef manager);
extern void WiFiManagerClientScheduleWithRunLoop(WiFiManagerRef manager, CFRunLoopRef loop, CFStringRef mode);
extern int WiFiDeviceClientScanAsync(WiFiDeviceRef device, CFDictionaryRef asked,
    void (*done)(WiFiDeviceRef, CFArrayRef, int, void *), void *context);
extern int WiFiDeviceClientAssociateAsync(WiFiDeviceRef device, WiFiNetworkRef network,
    void (*done)(WiFiDeviceRef, WiFiNetworkRef, CFDictionaryRef, int, void *), void *context);
extern WiFiNetworkRef WiFiDeviceClientCopyCurrentNetwork(WiFiDeviceRef device);
extern CFStringRef WiFiNetworkGetSSID(WiFiNetworkRef network);

static CFStringRef wanted;
static int tries;

static void scan(WiFiDeviceRef device);

static void
say_current(CFRunLoopTimerRef timer, void *info)
{
	static int times;
	WiFiNetworkRef now = WiFiDeviceClientCopyCurrentNetwork((WiFiDeviceRef)info);

	printf("wifijoin: wifid is on %s\n", now == NULL ? "no network" : CFEqual(WiFiNetworkGetSSID(now), wanted) ?
	    "the network asked for" : "another network");
	if (now != NULL) {
		CFRelease(now);
	}
	{
		static bool tried;
		struct ifaddrs *all, *a;
		char address[INET_ADDRSTRLEN] = "";

		if (getifaddrs(&all) == 0) {
			for (a = all; a != NULL; a = a->ifa_next) {
				if (a->ifa_addr != NULL && a->ifa_addr->sa_family == AF_INET && strcmp(a->ifa_name, "en0") == 0) {
					inet_ntop(AF_INET, &((struct sockaddr_in *)a->ifa_addr)->sin_addr, address, sizeof(address));
				}
			}
			freeifaddrs(all);
		}
		printf("wifijoin: en0's address: %s\n", address[0] ? address : "none yet");
		if (address[0] && !tried) {
			struct addrinfo *found = NULL, hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
			struct sockaddr_in to = { .sin_len = sizeof(to), .sin_family = AF_INET, .sin_port = htons(80) };
			int fd = socket(AF_INET, SOCK_STREAM, 0), error;

			tried = true;
			error = getaddrinfo("apple.com", "80", &hints, &found);
			printf("wifijoin: apple.com looked up: %s\n", error == 0 ? "an address" : gai_strerror(error));
			if (found != NULL) {
				freeaddrinfo(found);
			}
			inet_pton(AF_INET, "1.1.1.1", &to.sin_addr);
			printf("wifijoin: a TCP connection to 1.1.1.1:80: %s\n", connect(fd, (struct sockaddr *)&to, sizeof(to)) == 0 ?
			    "made" : strerror(errno));
			close(fd);
		}
	}
	if (++times >= 12) {
		exit(0);
	}
}

static void
joined(WiFiDeviceRef device, WiFiNetworkRef network, CFDictionaryRef info, int error, void *context)
{
	static int joins;

	printf("wifijoin: the join is over: %d%s\n", error, error == 0 ? " (joined)" : "");
	if (error != 0 && ++joins < 3) {
		/* (an access point that did not answer, or a weak one: a scan again, and the best one heard of that name then) */
		printf("wifijoin: again in 5 s\n");
		sleep(5);
		tries = 0;
		scan(device);
		return;
	}
	CFRunLoopTimerRef timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 5, 5, 0, 0, say_current,
	    &(CFRunLoopTimerContext){ .info = (void *)device });
	CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);
}

static void
again(CFRunLoopTimerRef timer, void *info)
{
	scan((WiFiDeviceRef)info);
}

static void
scanned(WiFiDeviceRef device, CFArrayRef networks, int error, void *context)
{
	CFIndex count = networks == NULL ? 0 : CFArrayGetCount(networks);

	printf("wifijoin: the scan heard %ld networks (%d)\n", (long)count, error);
	for (CFIndex i = 0; i < count; i++) {
		WiFiNetworkRef network = CFArrayGetValueAtIndex(networks, i);
		CFStringRef name = WiFiNetworkGetSSID(network);

		if (name != NULL && CFEqual(name, wanted)) {
			printf("wifijoin: the network asked for is among them: joining it: %d\n",
			    WiFiDeviceClientAssociateAsync(device, network, joined, NULL));
			return;
		}
	}
	if (++tries < 6) {
		printf("wifijoin: the network asked for is not among them; again in 10 s\n");
		CFRunLoopAddTimer(CFRunLoopGetCurrent(), CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 10, 0, 0, 0, again,
		    &(CFRunLoopTimerContext){ .info = (void *)device }), kCFRunLoopDefaultMode);
	} else {
		exit(1);
	}
}

static void
scan(WiFiDeviceRef device)
{
	CFDictionaryRef asked = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);

	printf("wifijoin: a scan: %d\n", WiFiDeviceClientScanAsync(device, asked, scanned, NULL));
	CFRelease(asked);
}

int
main(int argc, char **argv)
{
	WiFiManagerRef manager;
	CFArrayRef devices;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc != 3) {
		fprintf(stderr, "usage: wifijoin SECONDS NAME\n");
		return 2;
	}
	sleep((unsigned)atoi(argv[1]));
	wanted = CFStringCreateWithCString(NULL, argv[2], kCFStringEncodingUTF8);
	manager = WiFiManagerClientCreate(kCFAllocatorDefault, 0);
	devices = manager == NULL ? NULL : WiFiManagerClientCopyDevices(manager);
	if (devices == NULL || CFArrayGetCount(devices) == 0) {
		printf("wifijoin: %s\n", manager == NULL ? "no MobileWiFi client" : "wifid has no Wi-Fi device");
		return 1;
	}
	WiFiManagerClientScheduleWithRunLoop(manager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
	scan(CFArrayGetValueAtIndex(devices, 0));
	CFRunLoopRun();
	return 0;
}
