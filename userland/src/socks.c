/*
 * socks: the network sockets of the processes whose names contain a word, the way lsof -i would show them (the phone has
 * no lsof): for each, its protocol, its state for TCP, and the two ends. To see whether a program that seems to wait for
 * the network (Safari's WebKit.Networking, r157: pages that never load while net_test's own fetch does) has asked the
 * network for anything at all.
 *
 *   usage: socks [WORD...]      (default: Networking WebContent MobileSafari mDNSResponder trustd)
 *
 * libproc's calls and structures come from XNU's bsd/sys/proc_info.h (the iPhoneOS SDK has no copy): build.sh passes the
 * macOS SDK's with -idirafter.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/proc_info.h>
#include <unistd.h>

int proc_listpids(uint32_t type, uint32_t typeinfo, void *buffer, int buffersize);
int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
int proc_pidfdinfo(int pid, int fd, int flavor, void *buffer, int buffersize);
int proc_name(int pid, void *buffer, uint32_t buffersize);

static const char *tcp_states[] = {
	"CLOSED", "LISTEN", "SYN_SENT", "SYN_RECEIVED", "ESTABLISHED", "CLOSE_WAIT", "FIN_WAIT_1", "CLOSING", "LAST_ACK",
	"FIN_WAIT_2", "TIME_WAIT",
};

static void
address(const struct in_sockinfo *in, int foreign, char *out, size_t size)
{
	char text[INET6_ADDRSTRLEN] = "?";
	int port = ntohs((uint16_t)(foreign ? in->insi_fport : in->insi_lport));

	if (in->insi_vflag & INI_IPV4) {
		inet_ntop(AF_INET, foreign ? &in->insi_faddr.ina_46.i46a_addr4 : &in->insi_laddr.ina_46.i46a_addr4, text,
		    sizeof(text));
	} else {
		inet_ntop(AF_INET6, foreign ? &in->insi_faddr.ina_6 : &in->insi_laddr.ina_6, text, sizeof(text));
	}
	snprintf(out, size, "%s:%d", text, port);
}

int
main(int argc, char **argv)
{
	static int pids[4096];
	static struct proc_fdinfo fds[4096];
	const char *fallback[] = { "Networking", "WebContent", "MobileSafari", "mDNSResponder", "trustd" };
	const char **words = argc > 1 ? (const char **)argv + 1 : fallback;
	int nwords = argc > 1 ? argc - 1 : (int)(sizeof(fallback) / sizeof(fallback[0]));
	int bytes = proc_listpids(1 /* PROC_ALL_PIDS */, 0, pids, sizeof(pids)), shown = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	for (int i = 0; i < bytes / (int)sizeof(int); i++) {
		char name[64] = "";
		int match = 0;

		if (pids[i] <= 0 || proc_name(pids[i], name, sizeof(name)) <= 0) {
			continue;
		}
		for (int w = 0; w < nwords; w++) {
			match |= strstr(name, words[w]) != NULL;
		}
		if (!match) {
			continue;
		}
		int fdbytes = proc_pidinfo(pids[i], PROC_PIDLISTFDS, 0, fds, sizeof(fds)), sockets = 0;

		for (int f = 0; f < fdbytes / (int)sizeof(fds[0]); f++) {
			struct socket_fdinfo info;
			char local[80], remote[80];

			if (fds[f].proc_fdtype != PROX_FDTYPE_SOCKET ||
			    proc_pidfdinfo(pids[i], fds[f].proc_fd, PROC_PIDFDSOCKETINFO, &info, sizeof(info)) != sizeof(info)) {
				continue;
			}
			sockets++;
			if (info.psi.soi_kind == SOCKINFO_TCP) {
				int state = info.psi.soi_proto.pri_tcp.tcpsi_state;

				address(&info.psi.soi_proto.pri_tcp.tcpsi_ini, 0, local, sizeof(local));
				address(&info.psi.soi_proto.pri_tcp.tcpsi_ini, 1, remote, sizeof(remote));
				printf("socks: %s[%d] fd %d: tcp %s -> %s %s\n", name, pids[i], fds[f].proc_fd, local, remote,
				    state >= 0 && state <= 10 ? tcp_states[state] : "?");
			} else if (info.psi.soi_kind == SOCKINFO_IN) {
				address(&info.psi.soi_proto.pri_in, 0, local, sizeof(local));
				address(&info.psi.soi_proto.pri_in, 1, remote, sizeof(remote));
				printf("socks: %s[%d] fd %d: %s %s -> %s\n", name, pids[i], fds[f].proc_fd,
				    info.psi.soi_protocol == IPPROTO_UDP ? "udp" : "ip", local, remote);
			} else if (info.psi.soi_kind == SOCKINFO_UN) {
				printf("socks: %s[%d] fd %d: unix %s\n", name, pids[i], fds[f].proc_fd,
				    info.psi.soi_proto.pri_un.unsi_addr.ua_sun.sun_path[0] ?
				    info.psi.soi_proto.pri_un.unsi_addr.ua_sun.sun_path : "(no path)");
			} else {
				printf("socks: %s[%d] fd %d: family %d kind %d\n", name, pids[i], fds[f].proc_fd, info.psi.soi_family,
				    info.psi.soi_kind);
			}
		}
		printf("socks: %s[%d]: %d sockets\n", name, pids[i], sockets);
		shown++;
	}
	if (shown == 0) {
		printf("socks: no process by those names\n");
	}
	return 0;
}
