/*
 * hotspot: the phone's hotspot on and off (the kernel's sysctl kern.hakios.hotspot*, drivers/wifi/hakios.sta.c).
 *
 *   hotspot on [FILE]   the name and the passphrase from FILE (two lines; /private/var/root/hakios-hotspot by default,
 *                       which build.sh puts there from ssh/hotspot, never in the repository), then on; and then, until
 *                       the hotspot goes off, its network: en0 at 172.20.10.1/28 (an iPhone's hotspot's), IP forwarding,
 *                       pf's NAT out of the cellular interface (en1, or HOTSPOT_WAN=), and a DHCP server for the stations
 *                       (172.20.10.2-14; DNS 8.8.8.8 and 1.1.1.1; an MTU of 1400 for the cellular path)
 *   hotspot off         off: the radio a station again, and the network it knows joined
 *   hotspot status      on or off, and the name
 *
 * The passphrase goes from the file to the kernel and nowhere else: not in argv, not said.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <net/bpf.h>
#include <net/if_dl.h>
#include <ifaddrs.h>
#define PRIVATE 1
#include <net/pfvar.h>

#define LAN_IF          "en0"
#define LAN_ADDRESS     0xac140a01u     /* 172.20.10.1 */
#define LAN_MASK        0xfffffff0u     /* /28 */

static int
set_string(const char *name, const char *value)
{
	return sysctlbyname(name, NULL, NULL, (void *)value, strlen(value) + 1);
}


/* ---- the hotspot's network ---- */

static void
say(const char *format, const char *what, int error)
{
	fprintf(stderr, format, what, strerror(error));
}

/* en0 at 172.20.10.1/28, up */
static int
lan_address(void)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct ifaliasreq ar;
	struct ifreq fr;

	if (s < 0) {
		return -1;
	}
	memset(&ar, 0, sizeof(ar));
	strlcpy(ar.ifra_name, LAN_IF, sizeof(ar.ifra_name));
	((struct sockaddr_in *)&ar.ifra_addr)->sin_len = sizeof(struct sockaddr_in);
	((struct sockaddr_in *)&ar.ifra_addr)->sin_family = AF_INET;
	((struct sockaddr_in *)&ar.ifra_addr)->sin_addr.s_addr = htonl(LAN_ADDRESS);
	((struct sockaddr_in *)&ar.ifra_mask)->sin_len = sizeof(struct sockaddr_in);
	((struct sockaddr_in *)&ar.ifra_mask)->sin_family = AF_INET;
	((struct sockaddr_in *)&ar.ifra_mask)->sin_addr.s_addr = htonl(LAN_MASK);
	((struct sockaddr_in *)&ar.ifra_broadaddr)->sin_len = sizeof(struct sockaddr_in);
	((struct sockaddr_in *)&ar.ifra_broadaddr)->sin_family = AF_INET;
	((struct sockaddr_in *)&ar.ifra_broadaddr)->sin_addr.s_addr = htonl((LAN_ADDRESS & LAN_MASK) | ~LAN_MASK);
	if (ioctl(s, SIOCAIFADDR, &ar) != 0 && errno != EEXIST) {
		say("hotspot: %s: the address: %s\n", LAN_IF, errno);
		close(s);
		return -1;
	}
	memset(&fr, 0, sizeof(fr));
	strlcpy(fr.ifr_name, LAN_IF, sizeof(fr.ifr_name));
	if (ioctl(s, SIOCGIFFLAGS, &fr) == 0 && !(fr.ifr_flags & IFF_UP)) {
		fr.ifr_flags |= IFF_UP;
		(void)ioctl(s, SIOCSIFFLAGS, &fr);
	}
	close(s);
	return 0;
}

/* pf: nat on WAN from 172.20.10.0/28 to any -> (WAN), and pf on */
static int
lan_nat(const char *wan)
{
	struct pfioc_trans trans;
	struct pfioc_trans_e element;
	struct pfioc_pooladdr pool;
	struct pfioc_rule rule;
	int dev = open("/dev/pf", O_RDWR);

	if (dev < 0) {
		say("hotspot: %s: %s\n", "/dev/pf", errno);
		return -1;
	}
	memset(&element, 0, sizeof(element));
	element.rs_num = PF_RULESET_NAT;
	memset(&trans, 0, sizeof(trans));
	trans.size = 1;
	trans.esize = sizeof(element);
	trans.array = &element;
	if (ioctl(dev, DIOCXBEGIN, &trans) != 0) {
		say("hotspot: pf: %s: %s\n", "a transaction", errno);
		close(dev);
		return -1;
	}
	memset(&pool, 0, sizeof(pool));
	if (ioctl(dev, DIOCBEGINADDRS, &pool) != 0) {
		say("hotspot: pf: %s: %s\n", "the address pool", errno);
		close(dev);
		return -1;
	}
	pool.af = AF_INET;
	pool.addr.addr.type = PF_ADDR_DYNIFTL;
	strlcpy(pool.addr.addr.v.ifname, wan, sizeof(pool.addr.addr.v.ifname));
	if (ioctl(dev, DIOCADDADDR, &pool) != 0) {
		say("hotspot: pf: %s: %s\n", "the interface's address", errno);
		close(dev);
		return -1;
	}
	memset(&rule, 0, sizeof(rule));
	rule.ticket = element.ticket;
	rule.pool_ticket = pool.ticket;
	rule.rule.action = PF_NAT;
	rule.rule.direction = PF_OUT;
	rule.rule.af = AF_INET;
	strlcpy(rule.rule.ifname, wan, sizeof(rule.rule.ifname));
	rule.rule.src.addr.type = PF_ADDR_ADDRMASK;
	rule.rule.src.addr.v.a.addr.v4addr.s_addr = htonl(LAN_ADDRESS & LAN_MASK);
	rule.rule.src.addr.v.a.mask.v4addr.s_addr = htonl(LAN_MASK);
	rule.rule.dst.addr.type = PF_ADDR_ADDRMASK;          /* (0/0: anywhere) */
	rule.rule.rpool.proxy_port[0] = 50001;              /* (pfctl's PF_NAT_PROXY_PORT_LOW..HIGH) */
	rule.rule.rpool.proxy_port[1] = 65535;
	if (ioctl(dev, DIOCADDRULE, &rule) != 0) {
		say("hotspot: pf: %s: %s\n", "the rule", errno);
		close(dev);
		return -1;
	}
	if (ioctl(dev, DIOCXCOMMIT, &trans) != 0) {
		say("hotspot: pf: %s: %s\n", "the commit", errno);
		close(dev);
		return -1;
	}
	if (ioctl(dev, DIOCSTART) != 0 && errno != EEXIST) {
		say("hotspot: pf: %s: %s\n", "on", errno);
		close(dev);
		return -1;
	}
	close(dev);
	printf("hotspot: NAT from 172.20.10.0/28 out of %s, pf on\n", wan);
	return 0;
}

/* ---- DHCP for the stations (RFC 2131, as much as a hotspot needs) ---- */

/*
 * An answer out of en0 as a whole Ethernet frame through BPF, to everybody (as Apple's bootpd sends to a client that has
 * no address yet): a UDP datagram sent to 255.255.255.255 through the stack may not leave by en0 at all. -1 when BPF is not
 * to be had (the caller then sends it through the socket).
 */
static int bpf = -1;
static unsigned char lan_hw[6];

static int
bpf_open(void)
{
	struct ifaddrs *all, *a;
	struct ifreq ifr;
	char path[32];

	if (getifaddrs(&all) == 0) {
		for (a = all; a != NULL; a = a->ifa_next) {
			if (a->ifa_addr != NULL && a->ifa_addr->sa_family == AF_LINK && strcmp(a->ifa_name, LAN_IF) == 0) {
				memcpy(lan_hw, LLADDR((struct sockaddr_dl *)a->ifa_addr), 6);
			}
		}
		freeifaddrs(all);
	}
	for (int i = 0; i < 16 && bpf < 0; i++) {
		snprintf(path, sizeof(path), "/dev/bpf%d", i);
		bpf = open(path, O_RDWR);
	}
	if (bpf < 0) {
		say("hotspot: %s: %s\n", "BPF", errno);
		return -1;
	}
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, LAN_IF, sizeof(ifr.ifr_name));
	if (ioctl(bpf, BIOCSETIF, &ifr) != 0) {
		say("hotspot: %s: %s\n", "BPF on en0", errno);
		close(bpf);
		bpf = -1;
		return -1;
	}
	return 0;
}

static unsigned
checksum(const unsigned char *p, unsigned length)
{
	uint32_t sum = 0;

	for (unsigned i = 0; i + 1 < length; i += 2) {
		sum += (uint32_t)p[i] << 8 | p[i + 1];
	}
	if (length & 1) {
		sum += (uint32_t)p[length - 1] << 8;
	}
	while (sum >> 16) {
		sum = (sum & 0xffff) + (sum >> 16);
	}
	return ~sum & 0xffff;
}

static int
bpf_send(const unsigned char *payload, unsigned length)
{
	unsigned char f[14 + 20 + 8 + 600];
	unsigned ip = 20 + 8 + length, sum;

	if (bpf < 0 || length > 600) {
		return -1;
	}
	memset(f, 0, sizeof(f));
	memset(f, 0xff, 6);                     /* to everybody */
	memcpy(f + 6, lan_hw, 6);
	f[12] = 0x08;
	f[13] = 0x00;
	f[14] = 0x45;                           /* IPv4, 20 bytes */
	f[16] = (unsigned char)(ip >> 8);
	f[17] = (unsigned char)ip;
	f[22] = 64;                             /* TTL */
	f[23] = 17;                             /* UDP */
	f[26] = 172; f[27] = 20; f[28] = 10; f[29] = 1;
	f[30] = 255; f[31] = 255; f[32] = 255; f[33] = 255;
	sum = checksum(f + 14, 20);
	f[24] = (unsigned char)(sum >> 8);
	f[25] = (unsigned char)sum;
	f[34] = 0; f[35] = 67;                  /* from the server's port */
	f[36] = 0; f[37] = 68;                  /* to the client's */
	f[38] = (unsigned char)((8 + length) >> 8);
	f[39] = (unsigned char)(8 + length);    /* (a UDP checksum of 0: none, as IPv4 allows) */
	memcpy(f + 42, payload, length);
	return write(bpf, f, 14 + ip) == (ssize_t)(14 + ip) ? 0 : -1;
}

#define LEASES  13                      /* 172.20.10.2 .. .14 */
static struct { unsigned char hw[6]; int used; } leases[LEASES];

static int
lease_of(const unsigned char hw[6])
{
	for (int i = 0; i < LEASES; i++) {
		if (leases[i].used && memcmp(leases[i].hw, hw, 6) == 0) {
			return i;
		}
	}
	for (int i = 0; i < LEASES; i++) {
		if (!leases[i].used) {
			leases[i].used = 1;
			memcpy(leases[i].hw, hw, 6);
			return i;
		}
	}
	return -1;
}

static void
dhcp_serve(void)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0), yes = 1;
	unsigned index = if_nametoindex(LAN_IF);
	struct sockaddr_in here = { .sin_len = sizeof(here), .sin_family = AF_INET, .sin_port = htons(67) };
	unsigned char m[1024], r[548];
	int on;
	size_t size;

	if (s < 0 || setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0 ||
	    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) != 0 ||
	    setsockopt(s, IPPROTO_IP, IP_BOUND_IF, &index, sizeof(index)) != 0 || bind(s, (struct sockaddr *)&here, sizeof(here)) != 0) {
		say("hotspot: %s: %s\n", "the DHCP server's socket", errno);
		return;
	}
	(void)bpf_open();
	printf("hotspot: DHCP on %s (172.20.10.2-14), answers %s\n", LAN_IF, bpf >= 0 ? "through BPF" : "through the socket");
	fflush(stdout);
	for (;;) {
		struct timeval wait = { 2, 0 };
		fd_set ready;
		ssize_t n;
		int type = 0, lease, at;
		unsigned char *o;

		size = sizeof(on);
		if (sysctlbyname("kern.hakios.hotspot", &on, &size, NULL, 0) == 0 && !on) {
			printf("hotspot: the hotspot is off: the DHCP server stops\n");
			return;
		}
		FD_ZERO(&ready);
		FD_SET(s, &ready);
		if (select(s + 1, &ready, NULL, NULL, &wait) <= 0) {
			continue;
		}
		n = recv(s, m, sizeof(m), 0);
		if (n < 240 || m[0] != 1 || m[1] != 1 || m[2] != 6 || memcmp(m + 236, "\x63\x82\x53\x63", 4) != 0) {
			continue;
		}
		for (at = 240; at + 2 <= n && m[at] != 255; at += m[at] == 0 ? 1 : 2 + m[at + 1]) {
			if (m[at] == 53 && m[at + 1] == 1) {
				type = m[at + 2];
			}
		}
		printf("hotspot: DHCP %s from ..:%02x:%02x (xid ..%02x%02x)\n", type == 1 ? "DISCOVER" : type == 3 ? "REQUEST" :
		    type == 7 ? "RELEASE" : type == 4 ? "DECLINE" : type == 8 ? "INFORM" : "message", m[28 + 4], m[28 + 5], m[6], m[7]);
		fflush(stdout);
		if (type != 1 && type != 3) {
			continue;                       /* (a DISCOVER or a REQUEST; RELEASE and the rest ignored) */
		}
		if ((lease = lease_of(m + 28)) < 0) {
			continue;
		}
		memset(r, 0, sizeof(r));
		r[0] = 2;
		r[1] = 1;
		r[2] = 6;
		memcpy(r + 4, m + 4, 4);                /* xid */
		memcpy(r + 10, m + 10, 2);              /* flags */
		{
			uint32_t yiaddr = htonl((LAN_ADDRESS & LAN_MASK) + 2 + (uint32_t)lease), server = htonl(LAN_ADDRESS);

			memcpy(r + 16, &yiaddr, 4);
			memcpy(r + 20, &server, 4);
		}
		memcpy(r + 28, m + 28, 16);             /* chaddr */
		memcpy(r + 236, "\x63\x82\x53\x63", 4);
		o = r + 240;
		*o++ = 53; *o++ = 1; *o++ = type == 1 ? 2 : 5;  /* OFFER, ACK */
		*o++ = 54; *o++ = 4; *o++ = 172; *o++ = 20; *o++ = 10; *o++ = 1;
		*o++ = 51; *o++ = 4; *o++ = 0; *o++ = 0; *o++ = 0x0e; *o++ = 0x10;      /* an hour */
		*o++ = 1; *o++ = 4; *o++ = 255; *o++ = 255; *o++ = 255; *o++ = 240;
		*o++ = 3; *o++ = 4; *o++ = 172; *o++ = 20; *o++ = 10; *o++ = 1;
		*o++ = 6; *o++ = 8; *o++ = 8; *o++ = 8; *o++ = 8; *o++ = 8; *o++ = 1; *o++ = 1; *o++ = 1; *o++ = 1;
		*o++ = 26; *o++ = 2; *o++ = 0x05; *o++ = 0x78;  /* MTU 1400 */
		*o++ = 255;
		{
			struct sockaddr_in to = { .sin_len = sizeof(to), .sin_family = AF_INET, .sin_port = htons(68),
				                  .sin_addr.s_addr = htonl(0xffffffffu) };

			int sent = bpf_send(r, (unsigned)(o - r) > 300 ? (unsigned)(o - r) : 300) == 0 ? 1 :
			    sendto(s, r, sizeof(r), 0, (struct sockaddr *)&to, sizeof(to)) > 0 ? 2 : 0;

			printf("hotspot: DHCP %s to ..:%02x:%02x: 172.20.10.%d (%s%s%s)\n", type == 1 ? "OFFER" : "ACK", m[28 + 4], m[28 + 5],
			    2 + lease, sent == 1 ? "BPF" : sent == 2 ? "socket" : "NOT SENT: ", sent == 0 ? strerror(errno) : "", "");
		}
		fflush(stdout);
	}
}

/* Once the hotspot is on: its network, then the DHCP server until it goes off. */
static int
network(void)
{
	const char *wan = getenv("HOTSPOT_WAN") != NULL ? getenv("HOTSPOT_WAN") : "en1";
	int on = 0, one = 1;
	size_t size;

	for (int i = 0; i < 30 && !on; i++) {
		size = sizeof(on);
		if (sysctlbyname("kern.hakios.hotspot", &on, &size, NULL, 0) != 0 || !on) {
			sleep(1);
		}
	}
	if (!on) {
		fprintf(stderr, "hotspot: the hotspot did not come on in 30 s\n");
		return 1;
	}
	if (lan_address() != 0) {
		return 1;
	}
	printf("hotspot: %s is 172.20.10.1/28\n", LAN_IF);
	if (sysctlbyname("net.inet.ip.forwarding", NULL, NULL, &one, sizeof(one)) != 0) {
		say("hotspot: %s: %s\n", "IP forwarding", errno);
	}
	(void)lan_nat(wan);
	fflush(stdout);
	dhcp_serve();
	return 0;
}

int
main(int argc, char **argv)
{
	int on;
	size_t size = sizeof(on);

	if (argc >= 2 && strcmp(argv[1], "on") == 0) {
		const char *path = argc >= 3 ? argv[2] : "/private/var/root/hakios-hotspot";
		char name[64] = "", passphrase[80] = "";
		FILE *f = fopen(path, "r");

		if (f == NULL || fgets(name, sizeof(name), f) == NULL || fgets(passphrase, sizeof(passphrase), f) == NULL) {
			fprintf(stderr, "hotspot: %s: no name and passphrase (two lines)\n", path);
			if (f != NULL) {
				fclose(f);
			}
			return 1;
		}
		fclose(f);
		name[strcspn(name, "\r\n")] = 0;
		passphrase[strcspn(passphrase, "\r\n")] = 0;
		if (set_string("kern.hakios.hotspot_name", name) != 0 || set_string("kern.hakios.hotspot_password", passphrase) != 0) {
			memset(passphrase, 0, sizeof(passphrase));
			perror("hotspot: the name or the passphrase (8 to 63 characters)");
			return 1;
		}
		memset(passphrase, 0, sizeof(passphrase));
		on = 1;
		if (sysctlbyname("kern.hakios.hotspot", NULL, NULL, &on, sizeof(on)) != 0) {
			perror("hotspot: on");
			return 1;
		}
		printf("hotspot: asked on: \"%s\"\n", name);
		fflush(stdout);
		return network();
	}
	if (argc >= 2 && strcmp(argv[1], "off") == 0) {
		on = 0;
		if (sysctlbyname("kern.hakios.hotspot", NULL, NULL, &on, sizeof(on)) != 0) {
			perror("hotspot: off");
			return 1;
		}
		printf("hotspot: asked off\n");
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "status") == 0) {
		char name[64] = "";
		size_t length = sizeof(name);

		if (sysctlbyname("kern.hakios.hotspot", &on, &size, NULL, 0) != 0) {
			perror("hotspot: status");
			return 1;
		}
		(void)sysctlbyname("kern.hakios.hotspot_name", name, &length, NULL, 0);
		printf("hotspot: %s (\"%s\")\n", on ? "ON" : "off", name);
		return 0;
	}
	fprintf(stderr, "usage: hotspot on [FILE] | off | status\n");
	return 2;
}
