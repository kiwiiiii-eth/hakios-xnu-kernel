/*
 * cellnet_test: does the native cellular interface carry traffic (abb=1 modem_native_pdp=1: CommCenter's own data call
 * on pdp_ip0)? From cc-native's probe (25 November).
 *
 *   usage: cellnet_test [SECONDS]    wait until the system has been up SECONDS (default: now), then say what pdp_ip0
 *                                    has (up, its IPv4 and IPv6 addresses), and make one HTTP request to example.com
 *                                    in each family it has an address of, bound to pdp_ip0 (IP_BOUND_IF), so that
 *                                    Wi-Fi cannot answer for it. The addresses come from the phone's own resolver;
 *                                    without an answer, example.com's of 24 November. No address is printed.
 *
 * Each line starts "cellnet:"; the exit status is 0 when every request made answered 200.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static unsigned at;

static double
uptime(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void
timeout(int number)
{
	static const char message[] = "cellnet: timed out\n";

	(void)number;
	write(1, message, sizeof(message) - 1);
	_exit(124);
}

/* one GET / to example.com at ADDRESS, bound to the interface INDEX: the status it answered, or 0 */
static int
request(unsigned index, const struct sockaddr *address, socklen_t length)
{
	const char *family = address->sa_family == AF_INET6 ? "IPv6" : "IPv4";
	int fd = socket(address->sa_family, SOCK_STREAM, 0), code = 0, flags, error = 0;
	static const char get[] = "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n";
	char header[128] = { 0 };
	struct timeval bound = { 4, 0 };
	socklen_t size = sizeof(error);
	ssize_t n;

	if (fd < 0 || (address->sa_family == AF_INET6 ? setsockopt(fd, IPPROTO_IPV6, IPV6_BOUND_IF, &index, sizeof(index)) :
	    setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &index, sizeof(index))) != 0) {
		printf("cellnet: %s: no socket bound to pdp_ip0 (errno %d)\n", family, errno);
		if (fd >= 0) {
			close(fd);
		}
		return 0;
	}
	flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	if (connect(fd, address, length) != 0) {
		struct pollfd p = { fd, POLLOUT, 0 };

		if (errno != EINPROGRESS || poll(&p, 1, 5000) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error) {
			printf("cellnet: %s: no connection over pdp_ip0 (errno %d) at %u s\n", family, error ? error : errno, at);
			close(fd);
			return 0;
		}
	}
	fcntl(fd, F_SETFL, flags);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &bound, sizeof(bound));
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &bound, sizeof(bound));
	if (send(fd, get, sizeof(get) - 1, 0) == (ssize_t)sizeof(get) - 1 && (n = recv(fd, header, sizeof(header) - 1, 0)) > 0) {
		sscanf(header, "HTTP/%*u.%*u %d", &code);
	}
	close(fd);
	printf("cellnet: %s over pdp_ip0: HTTP %d at %u s\n", family, code, at);
	return code;
}

int
main(int argc, char **argv)
{
	struct ifaddrs *interfaces = NULL;
	struct addrinfo hints = { 0 }, *resolved = NULL;
	struct sockaddr_in in4 = { sizeof(in4), AF_INET, htons(80), { 0 }, { 0 } };
	struct sockaddr_in6 in6 = { sizeof(in6), AF_INET6, htons(80), 0, IN6ADDR_ANY_INIT, 0 };
	unsigned index, up = 0, v4 = 0, v6 = 0, requests = 0, answered = 0;
	int dns;
	bool have4 = false, have6 = false;

	setvbuf(stdout, NULL, _IOLBF, 0);
	at = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 0;
	while (uptime() < at) {
		usleep(200000);
	}
	at = (unsigned)uptime();
	signal(SIGALRM, timeout);
	signal(SIGPIPE, SIG_IGN);
	alarm(30);
	if ((index = if_nametoindex("pdp_ip0")) == 0) {
		printf("cellnet: no pdp_ip0 (not modem_native_pdp=1) at %u s\n", at);
		return 2;
	}
	if (getifaddrs(&interfaces) == 0) {
		for (struct ifaddrs *p = interfaces; p != NULL; p = p->ifa_next) {
			if (p->ifa_addr == NULL || strcmp(p->ifa_name, "pdp_ip0") != 0) {
				continue;
			}
			up = (p->ifa_flags & IFF_UP) != 0;
			if (p->ifa_addr->sa_family == AF_INET) {
				v4++;
			} else if (p->ifa_addr->sa_family == AF_INET6 &&
			    !IN6_IS_ADDR_LINKLOCAL(&((struct sockaddr_in6 *)p->ifa_addr)->sin6_addr)) {
				v6++;
			}
		}
		freeifaddrs(interfaces);
	}
	printf("cellnet: pdp_ip0 %s, %u IPv4 and %u global IPv6 addresses, at %u s\n", up ? "up" : "down", v4, v6, at);
	hints.ai_socktype = SOCK_STREAM;
	dns = getaddrinfo("example.com", "80", &hints, &resolved);
	for (struct addrinfo *p = dns == 0 ? resolved : NULL; p != NULL; p = p->ai_next) {
		if (p->ai_family == AF_INET && !have4) {
			memcpy(&in4, p->ai_addr, sizeof(in4));
			have4 = true;
		} else if (p->ai_family == AF_INET6 && !have6) {
			memcpy(&in6, p->ai_addr, sizeof(in6));
			have6 = true;
		}
	}
	printf("cellnet: the phone's resolver: %s (IPv4 %s, IPv6 %s)\n", dns == 0 ? "answered" : gai_strerror(dns), have4 ? "yes" : "no",
	    have6 ? "yes" : "no");
	if (resolved != NULL) {
		freeaddrinfo(resolved);
	}
	if (!have4) {
		inet_pton(AF_INET, "104.20.23.154", &in4.sin_addr);
	}
	if (!have6) {
		inet_pton(AF_INET6, "2606:4700:10::ac42:93f3", &in6.sin6_addr);
	}
	if (v4 != 0) {
		requests++;
		answered += request(index, (struct sockaddr *)&in4, sizeof(in4)) == 200;
	}
	if (v6 != 0) {
		requests++;
		answered += request(index, (struct sockaddr *)&in6, sizeof(in6)) == 200;
	}
	alarm(0);
	return requests != 0 && answered == requests ? 0 : 1;
}
