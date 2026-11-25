/*
 * hotspot: the phone's hotspot on and off (the kernel's sysctl kern.hakios.hotspot*, drivers/wifi/hakios.sta.c).
 *
 *   hotspot on [FILE]   the name and the passphrase from FILE (two lines; /private/var/root/hakios-hotspot by default,
 *                       which build.sh puts there from ssh/hotspot, never in the repository), then on; and then, until
 *                       the hotspot goes off, its network: en0 at 172.20.10.1/28 (an iPhone's hotspot's), IP forwarding,
 *                       pf's NAT out of the cellular interface (pdp_ip0 with the native cellular interfaces, else en1; or HOTSPOT_WAN=), and a DHCP server for the stations
 *                       (172.20.10.2-14; DNS 8.8.8.8 and 1.1.1.1; an MTU of 1400 for the cellular path)
 *   hotspot off         off: the radio a station again, and the network it knows joined
 *   hotspot status      on or off, and the name
 *   hotspot serve       Settings' hotspot (com.hakios.hotspot.plist, root): whenever Settings posts
 *                       com.hakios.hotspot.changed (src/settings_ui.m), CONF's "on=", "name=" and "password=" lines are
 *                       done: on (the name and the passphrase to the kernel, then the network above, in a child) or off.
 *                       With no CONF yet, one is made, off, with ssh/hotspot's name and passphrase if the image has them
 *                       (a random passphrase if not); mobile's, since Settings shows the passphrase as an iPhone does.
 *                       At each start it is off whatever CONF said (on=0 written back): Settings turns it on.
 *
 * The passphrase goes from the file to the kernel and nowhere else: not in argv, not said.
 */
#include <errno.h>
#include <notify.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
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
#include <CommonCrypto/CommonDigest.h>
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

static in_addr_t ipv4_of(const char *name);

/*
 * pf: nat on WAN from 172.20.10.0/28 to any -> WAN's address, and pf on. The address itself, as WAN has it now, not
 * "(WAN)", pf's own following of the interface: over pdp_ip0 that put the stations' packets out with an address that
 * was not pdp_ip0's (25 November: 148 connections, none of them NAT'd to pdp_ip0's address, nothing answered). The
 * caller lays the NAT down again whenever the address changes (dhcp_serve). With no address yet, no NAT.
 */
static int
lan_nat(const char *wan)
{
	struct pfioc_trans trans;
	struct pfioc_trans_e element;
	struct pfioc_pooladdr pool;
	struct pfioc_rule rule;
	in_addr_t address = ipv4_of(wan);
	int dev;

	if (address == 0) {
		printf("hotspot: %s has no IPv4 address yet: the NAT when it has one\n", wan);
		return -1;
	}
	if ((dev = open("/dev/pf", O_RDWR)) < 0) {
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
	pool.addr.addr.type = PF_ADDR_ADDRMASK;
	pool.addr.addr.v.a.addr.v4addr.s_addr = address;
	pool.addr.addr.v.a.mask.v4addr.s_addr = 0xffffffffu;
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
	printf("hotspot: NAT from 172.20.10.0/28 out of %s, to its address as it is now, pf on\n", wan);
	return 0;
}

/*
 * Every 30 s while the hotspot is on: where its traffic goes, so that a station that joins and gets no answer says why.
 * The route (the local address an unbound UDP socket to 8.8.8.8 is given: the unscoped route forwarding uses too, and
 * whether it is WAN's), IP forwarding, pf (on, its states, the NAT rule's packets), and the packets and errors of WAN and
 * en0. No address is printed.
 */
static const char *stats_wan;
static in_addr_t nat_wan_address;           /* WAN's IPv4 address when the NAT was last laid down (0: none then) */

/* NAME's first IPv4 address, or 0 */
static in_addr_t
ipv4_of(const char *name)
{
	struct ifaddrs *all = NULL;
	in_addr_t found = 0;

	if (getifaddrs(&all) != 0) {
		return 0;
	}
	for (struct ifaddrs *p = all; p != NULL && found == 0; p = p->ifa_next) {
		if (p->ifa_addr != NULL && p->ifa_addr->sa_family == AF_INET && strcmp(p->ifa_name, name) == 0) {
			found = ((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr;
		}
	}
	freeifaddrs(all);
	return found;
}

static void
link_counts(const char *name, char *out, size_t size)
{
	struct ifaddrs *all = NULL;

	snprintf(out, size, "%s: none", name);
	if (getifaddrs(&all) != 0) {
		return;
	}
	for (struct ifaddrs *p = all; p != NULL; p = p->ifa_next) {
		if (p->ifa_addr != NULL && p->ifa_addr->sa_family == AF_LINK && p->ifa_data != NULL && strcmp(p->ifa_name, name) == 0) {
			const struct if_data *d = p->ifa_data;

			snprintf(out, size, "%s: in %u packets (%u errors), out %u (%u errors)", name, d->ifi_ipackets, d->ifi_ierrors,
			    d->ifi_opackets, d->ifi_oerrors);
		}
	}
	freeifaddrs(all);
}

static void
stats(void)
{
	struct sockaddr_in to = { .sin_len = sizeof(to), .sin_family = AF_INET, .sin_port = htons(53) }, from;
	socklen_t size = sizeof(from);
	struct ifaddrs *all = NULL;
	const char *route = "none";
	int s = socket(AF_INET, SOCK_DGRAM, 0), forwarding = -1, dev;
	size_t length = sizeof(forwarding);
	char wan[160], lan[160];

	to.sin_addr.s_addr = htonl(0x08080808);
	if (s >= 0 && connect(s, (struct sockaddr *)&to, sizeof(to)) == 0 && getsockname(s, (struct sockaddr *)&from, &size) == 0 &&
	    getifaddrs(&all) == 0) {
		route = "not WAN's";
		for (struct ifaddrs *p = all; p != NULL; p = p->ifa_next) {
			if (p->ifa_addr != NULL && p->ifa_addr->sa_family == AF_INET &&
			    ((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr == from.sin_addr.s_addr) {
				route = strcmp(p->ifa_name, stats_wan) == 0 ? "WAN's" : p->ifa_name;
			}
		}
		freeifaddrs(all);
	}
	if (s >= 0) {
		close(s);
	}
	(void)sysctlbyname("net.inet.ip.forwarding", &forwarding, &length, NULL, 0);
	printf("hotspot: stats: the route to 8.8.8.8 is %s (WAN %s); IP forwarding %d\n", route, stats_wan, forwarding);
	if ((dev = open("/dev/pf", O_RDWR)) >= 0) {
		struct pf_status status;
		struct pfioc_rule rule;

		memset(&status, 0, sizeof(status));
		memset(&rule, 0, sizeof(rule));
		rule.rule.action = PF_NAT;
		if (ioctl(dev, DIOCGETSTATUS, &status) == 0) {
			printf("hotspot: stats: pf %s, %u states; %llu matched, %llu bad offsets, %llu short, %llu bad states, %llu memory\n",
			    status.running ? "on" : "OFF", status.states, (unsigned long long)status.counters[PFRES_MATCH],
			    (unsigned long long)status.counters[PFRES_BADOFF], (unsigned long long)status.counters[PFRES_SHORT],
			    (unsigned long long)status.counters[PFRES_BADSTATE], (unsigned long long)status.counters[PFRES_MEMORY]);
		}
		if (ioctl(dev, DIOCGETRULES, &rule) == 0) {
			unsigned count = rule.nr;

			for (unsigned i = 0; i < count; i++) {
				rule.nr = i;
				if (ioctl(dev, DIOCGETRULE, &rule) == 0) {
					printf("hotspot: stats: NAT rule %u on %s: %llu evaluations, %llu packets out, %llu in, %u states\n", i,
					    rule.rule.ifname, (unsigned long long)rule.rule.evaluations, (unsigned long long)rule.rule.packets[0],
					    (unsigned long long)rule.rule.packets[1], rule.rule.states);
				}
			}
			if (count == 0) {
				printf("hotspot: stats: NO NAT rule\n");
			}
			if (count != 0) {
				struct pfioc_pooladdr pool;

				memset(&pool, 0, sizeof(pool));
				pool.r_action = PF_NAT;
				pool.r_num = 0;
				pool.ticket = rule.ticket;
				if (ioctl(dev, DIOCGETADDRS, &pool) == 0 && pool.nr != 0) {
					pool.nr = 0;
					pool.ticket = rule.ticket;
					if (ioctl(dev, DIOCGETADDR, &pool) == 0) {
						in_addr_t now = ipv4_of(stats_wan);

						printf("hotspot: stats: the NAT translates to %s; %s has %s\n", pool.addr.addr.type != PF_ADDR_ADDRMASK ?
						    "an address pf finds itself" : now != 0 && pool.addr.addr.v.a.addr.v4addr.s_addr == now ?
						    "WAN's address as it is now" : "an address WAN has NOT now", stats_wan, now != 0 ? "an IPv4 address" :
						    "no IPv4 address");
					} else {
						printf("hotspot: stats: the NAT's address not read: %s\n", strerror(errno));
					}
				}
			}
		}
		{
			/*
			 * The stations' connections in pf's state table (their inside address 172.20.10.0/28): how many, how many of them
			 * NAT'd to WAN's address (gwy), and their packets each way -- out (from the station) and back (the answers). Back 0
			 * with WAN's address: nothing answers what went out; back > 0 with en0 still silent: the way to en0 is wrong.
			 */
			static struct pfsync_state table[256];
			struct pfioc_states states;
			in_addr_t now = ipv4_of(stats_wan);
			unsigned stations = 0, translated = 0, out = 0, back = 0;

			memset(&states, 0, sizeof(states));
			states.ps_len = sizeof(table);
			states.ps_states = table;
			if (ioctl(dev, DIOCGETSTATES, &states) == 0) {
				for (unsigned i = 0; i < (unsigned)states.ps_len / sizeof(table[0]); i++) {
					const struct pfsync_state *s = &table[i];

					if (s->af_lan != AF_INET || (ntohl(s->lan.addr.v4addr.s_addr) & 0xfffffff0u) != 0xac140a00u) {
						continue;
					}
					stations++;
					translated += now != 0 && s->gwy.addr.v4addr.s_addr == now;
					out += s->packets[0][1];
					back += s->packets[1][1];
				}
				printf("hotspot: stats: the stations' connections: %u (%u NAT'd to %s's address), %u packets out, %u back\n",
				    stations, translated, stats_wan, out, back);
				for (unsigned i = 0; i < (unsigned)states.ps_len / sizeof(table[0]); i++) {
					const struct pfsync_state *s = &table[i];
					const unsigned char *g = (const unsigned char *)&s->gwy.addr.v4addr.s_addr, *w = (const unsigned char *)&now;

					if (s->af_lan == AF_INET && (ntohl(s->lan.addr.v4addr.s_addr) & 0xfffffff0u) == 0xac140a00u) {
						/* (the first such connection, its addresses cut to their first two parts) */
						printf("hotspot: stats: a station's connection: NAT'd to %u.%u.x.x%s (%s's is %u.%u.x.x), nat rule %d, "
						    "proto %u, direction %u\n", g[0], g[1], s->gwy.addr.v4addr.s_addr == s->lan.addr.v4addr.s_addr ?
						    " = its own, not translated" : "", stats_wan, w[0], w[1], (int)s->nat_rule, s->proto, s->direction);
						break;
					}
				}
			} else {
				printf("hotspot: stats: pf's states not read: %s\n", strerror(errno));
			}
		}
		close(dev);
	}
	link_counts(stats_wan, wan, sizeof(wan));
	link_counts(LAN_IF, lan, sizeof(lan));
	printf("hotspot: stats: %s; %s\n", wan, lan);
	fflush(stdout);
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
	time_t stats_at = 0;

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
		/*
		 * WAN's address, as the NAT was laid down with: at boot Settings turns the hotspot on before CommCenter's data call
		 * has an address, and the NAT so made sent the stations' packets out with none their answers came back to
		 * (25 November: 211 packets out of pdp_ip0, almost nothing back). So when the address comes, or changes, the NAT
		 * is laid down again.
		 */
		if (stats_wan != NULL) {
			in_addr_t now = ipv4_of(stats_wan);

			if (now != nat_wan_address) {
				printf("hotspot: %s's address %s: the NAT laid down again\n", stats_wan, now == 0 ? "gone" :
				    nat_wan_address == 0 ? "is there now" : "changed");
				nat_wan_address = now;
				(void)lan_nat(stats_wan);
			}
		}
		if (stats_wan != NULL && time(NULL) - stats_at >= 30) {
			stats_at = time(NULL);
			stats();
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
	/* (pdp_ip0 when the kernel made the native cellular interfaces, modem_native_pdp=1: CommCenter's own data call; else
	 * en1, the kernel's. pf's "(WAN)" follows the interface's address, so one that has none yet is fine.) */
	const char *wan = getenv("HOTSPOT_WAN") != NULL ? getenv("HOTSPOT_WAN") : if_nametoindex("pdp_ip0") != 0 ? "pdp_ip0" : "en1";
	int on = 0, one = 1;
	size_t size;

	for (int i = 0; i < 90 && !on; i++) {      /* (at boot the radio's firmware may still be on its way) */
		size = sizeof(on);
		if (sysctlbyname("kern.hakios.hotspot", &on, &size, NULL, 0) != 0 || !on) {
			sleep(1);
		}
	}
	if (!on) {
		fprintf(stderr, "hotspot: the hotspot did not come on in 90 s\n");
		return 1;
	}
	if (lan_address() != 0) {
		return 1;
	}
	printf("hotspot: %s is 172.20.10.1/28\n", LAN_IF);
	if (sysctlbyname("net.inet.ip.forwarding", NULL, NULL, &one, sizeof(one)) != 0) {
		say("hotspot: %s: %s\n", "IP forwarding", errno);
	}
	nat_wan_address = ipv4_of(wan);
	(void)lan_nat(wan);
	stats_wan = wan;
	fflush(stdout);
	dhcp_serve();
	return 0;
}

/* ---- Settings' hotspot (hotspot serve) ---- */

#define CONF            "/private/var/mobile/Library/Preferences/com.hakios.hotspot.conf"
#define CONF_CHANGED    "com.hakios.hotspot.changed"
#define MOBILE          501

struct conf {
	int on;
	char name[33];
	char password[64];
};

static int
conf_read(struct conf *c)
{
	char line[160];
	FILE *f = fopen(CONF, "r");

	memset(c, 0, sizeof(*c));
	if (f == NULL) {
		return -1;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		line[strcspn(line, "\r\n")] = 0;
		if (strncmp(line, "on=", 3) == 0) {
			c->on = atoi(line + 3) != 0;
		} else if (strncmp(line, "name=", 5) == 0) {
			strlcpy(c->name, line + 5, sizeof(c->name));
		} else if (strncmp(line, "password=", 9) == 0) {
			strlcpy(c->password, line + 9, sizeof(c->password));
		}
	}
	memset(line, 0, sizeof(line));
	fclose(f);
	return 0;
}

/* The first CONF: off, with the image's name and passphrase (build.sh's ssh/hotspot), or "iPhone" and a random one */
static void
conf_seed(void)
{
	struct stat st;
	char name[64] = "iPhone", password[80] = "";
	FILE *f;
	int fd;

	if (stat(CONF, &st) == 0) {
		return;
	}
	if ((f = fopen("/private/var/root/hakios-hotspot", "r")) != NULL) {
		if (fgets(name, sizeof(name), f) == NULL || fgets(password, sizeof(password), f) == NULL) {
			strlcpy(name, "iPhone", sizeof(name));
			password[0] = 0;
		}
		fclose(f);
		name[strcspn(name, "\r\n")] = 0;
		password[strcspn(password, "\r\n")] = 0;
	}
	if (strlen(password) < 8 || strlen(password) > 63) {
		static const char letters[] = "abcdefghijkmnpqrstuvwxyz23456789";

		for (int i = 0; i < 12; i++) {
			password[i] = letters[arc4random_uniform(sizeof(letters) - 1)];
		}
		password[12] = 0;
	}
	(void)mkdir("/private/var/mobile/Library/Preferences", 0755);
	if ((fd = open(CONF, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0) {
		say("hotspot: %s: %s\n", CONF, errno);
	} else {
		dprintf(fd, "on=0\nname=%s\npassword=%s\n", name, password);
		(void)fchown(fd, MOBILE, MOBILE);
		close(fd);
		printf("hotspot: %s made, off, named \"%s\"\n", CONF, name);
	}
	memset(password, 0, sizeof(password));
}

/*
 * Off at every start: the hotspot is turned on in Settings > Personal Hotspot, not by the phone starting (a CONF left
 * "on=1" by the last session had it up at boot, its network named before anyone asked). Once a boot (kern.boottime,
 * noted in /private/var/run): a serve that launchd starts again later in the same boot leaves Settings' choice alone.
 */
static void
conf_off_at_start(void)
{
	static const char mark[] = "/private/var/run/hakios-hotspot.boot";
	struct timeval boot = { 0, 0 };
	size_t size = sizeof(boot);
	char seen[32] = "", now[32];
	struct conf c;
	FILE *f;
	int fd;

	if (sysctlbyname("kern.boottime", &boot, &size, NULL, 0) != 0) {
		return;
	}
	snprintf(now, sizeof(now), "%ld", (long)boot.tv_sec);
	if ((f = fopen(mark, "r")) != NULL) {
		if (fgets(seen, sizeof(seen), f) == NULL) {
			seen[0] = 0;
		}
		fclose(f);
		seen[strcspn(seen, "\r\n")] = 0;
	}
	if (strcmp(seen, now) == 0) {
		return;
	}
	if ((f = fopen(mark, "w")) != NULL) {
		fprintf(f, "%s\n", now);
		fclose(f);
	}
	if (conf_read(&c) != 0 || !c.on) {
		memset(&c, 0, sizeof(c));
		return;
	}
	if ((fd = open(CONF ".new", O_WRONLY | O_CREAT | O_TRUNC, 0600)) >= 0) {
		dprintf(fd, "on=0\nname=%s\npassword=%s\n", c.name, c.password);
		(void)fchown(fd, MOBILE, MOBILE);
		close(fd);
		if (rename(CONF ".new", CONF) == 0) {
			printf("hotspot: off at the start, as on an iPhone (Settings > Personal Hotspot turns it on)\n");
		}
	}
	memset(&c, 0, sizeof(c));
}

static int
kernel_on(void)
{
	int on = 0;
	size_t size = sizeof(on);

	return sysctlbyname("kern.hakios.hotspot", &on, &size, NULL, 0) == 0 && on;
}

static int
kernel_set(int on)
{
	return sysctlbyname("kern.hakios.hotspot", NULL, NULL, &on, sizeof(on));
}

static pid_t network_child;

/* off, and the network's child gone (its DHCP server stops by itself within 2 s once the hotspot is off) */
static void
serve_off(void)
{
	if (kernel_on()) {
		(void)kernel_set(0);
	}
	if (network_child > 0) {
		int status;

		for (int i = 0; i < 50 && waitpid(network_child, &status, WNOHANG) == 0; i++) {
			usleep(100000);
		}
		if (waitpid(network_child, &status, WNOHANG) == 0) {
			kill(network_child, SIGTERM);
			(void)waitpid(network_child, &status, 0);
		}
		network_child = 0;
	}
	for (int i = 0; i < 50 && kernel_on(); i++) {
		usleep(100000);
	}
}

/* a passphrase's fingerprint: the first 4 hex digits of its SHA-256 -- enough to tell two apart, nothing to guess it by.
 * Settings says the same of the one it shows (src/settings_ui.m), so a phone and its pane can be compared in a log. */
static const char *
fingerprint(const char *passphrase)
{
	static char hex[5];
	unsigned char digest[CC_SHA256_DIGEST_LENGTH];

	CC_SHA256(passphrase, (CC_LONG)strlen(passphrase), digest);
	snprintf(hex, sizeof(hex), "%02x%02x", digest[0], digest[1]);
	memset(digest, 0, sizeof(digest));
	return hex;
}

static void
serve_apply(void)
{
	static struct conf applied;
	static int applied_on;
	struct conf c;

	if (network_child > 0 && waitpid(network_child, NULL, WNOHANG) == network_child) {
		network_child = 0;
	}
	if (conf_read(&c) != 0 || !c.on) {
		if (applied_on || kernel_on()) {
			serve_off();
			printf("hotspot: off (Settings)\n");
		}
		applied_on = 0;
		memset(&applied, 0, sizeof(applied));
		memset(&c, 0, sizeof(c));
		return;
	}
	if (applied_on && network_child > 0 && strcmp(applied.name, c.name) == 0 && strcmp(applied.password, c.password) == 0) {
		memset(&c, 0, sizeof(c));
		return;                 /* on as asked already */
	}
	if (applied_on || kernel_on()) {
		serve_off();            /* another name or passphrase: the stations join again, as on an iPhone */
	}
	if (c.name[0] == 0) {
		strlcpy(c.name, "iPhone", sizeof(c.name));
	}
	if (set_string("kern.hakios.hotspot_name", c.name) != 0 || set_string("kern.hakios.hotspot_password", c.password) != 0 ||
	    kernel_set(1) != 0) {
		say("hotspot: %s: %s\n", "on (Settings)", errno);
		applied_on = 0;
		memset(&c, 0, sizeof(c));
		return;
	}
	printf("hotspot: on (Settings): \"%s\", a passphrase of %zu characters, fingerprint %s\n", c.name, strlen(c.password),
	    fingerprint(c.password));
	fflush(stdout);
	if ((network_child = fork()) == 0) {
		memset(&c, 0, sizeof(c));
		_exit(network());
	}
	applied = c;
	applied_on = 1;
	memset(&c, 0, sizeof(c));
}

static int
serve(void)
{
	int fd = -1, token, on;
	size_t size = sizeof(on);

	if (sysctlbyname("kern.hakios.hotspot", &on, &size, NULL, 0) != 0) {
		printf("hotspot: no hotspot in this kernel (no Wi-Fi): nothing to serve\n");
		return 0;
	}
	signal(SIGPIPE, SIG_IGN);
	conf_seed();
	conf_off_at_start();
	if (notify_register_file_descriptor(CONF_CHANGED, &fd, 0, &token) != NOTIFY_STATUS_OK) {
		fprintf(stderr, "hotspot: the notification %s: not registered\n", CONF_CHANGED);
		return 1;
	}
	printf("hotspot: serving Settings (%s, %s)\n", CONF, CONF_CHANGED);
	fflush(stdout);
	serve_apply();
	for (;;) {
		struct timeval wait = { 5, 0 };
		fd_set ready;

		FD_ZERO(&ready);
		FD_SET(fd, &ready);
		if (select(fd + 1, &ready, NULL, NULL, &wait) > 0) {
			int posted;

			if (read(fd, &posted, sizeof(posted)) != sizeof(posted)) {
				continue;
			}
			serve_apply();
		} else if (network_child > 0 && waitpid(network_child, NULL, WNOHANG) == network_child) {
			network_child = 0;
			printf("hotspot: the network's child ended\n");
		}
		fflush(stdout);
	}
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
	if (argc >= 2 && strcmp(argv[1], "serve") == 0) {
		return serve();
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
	fprintf(stderr, "usage: hotspot on [FILE] | off | status | serve\n");
	return 2;
}
