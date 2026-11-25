/*
 * Darwin-on-hakios: what stands between the two sides of the station boundary
 * (sys/hakios.sta.h).
 *
 * Whoever wants a network asks from wherever it happens to be: an ioctl of wifid's, the thread
 * that brings the radio up. The station side wants to be called by one thread, one call at a
 * time, and a scan or a join takes seconds. So what is asked for is noted here and done by a
 * thread of this file's; the asker returns at once and hears the outcome as an event. One
 * request of each kind can wait: a second scan asked for while the first has not begun
 * replaces it, as does a second join.
 *
 * What the last scan heard is kept here, so that it can be read without entering the station
 * side, and so is the list of channels.
 *
 * The network the kernel was given in a file is a standing request: joined at the start, and
 * again whenever the radio is turned back on (Settings' switch), until somebody asks for
 * another network -- as Darwin itself rejoins the networks it knows. If that other network cannot
 * be joined (a wrong passphrase), the standing one is joined again.
 */
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/errno.h>
#include <sys/hakios.input.h>
#include <sys/hakios.sta.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/random.h>
#include <pexpert/pexpert.h>

/* (memory that held a passphrase, cleared so that the compiler cannot leave it out: the kernel has no explicit_bzero) */
static void
wipe(void *memory, size_t length)
{
	volatile uint8_t *p = memory;

	while (length-- != 0) {
		*p++ = 0;
	}
}

static void hotspot_test_start(void);

extern void thread_set_thread_name(thread_t thread, const char *name);

#define HEARD_MOST      128
#define CHANNELS_MOST   96

static LCK_GRP_DECLARE(sta_lock_group, "hakios-sta");
static LCK_MTX_DECLARE(sta_lock, &sta_lock_group);

static const struct hakios_sta_ops *sta_ops;   /* the station side, while there is one */
static bool sta_thread_made;
static bool sta_calls;                          /* the thread is in a call of the station side's */
static unsigned sta_reads;                      /* ... and so many others are in its state() */
static bool sta_on;                             /* the radio's power, as last asked for */

/* what has been asked for and not begun */
static bool asked_power, asked_power_on, asked_scan, asked_join, asked_leave;
static bool asked_ap, asked_ap_on;              /* the hotspot on (with asked_ap_what) or off */
static struct hakios_sta_ap asked_ap_what;
static bool ap_running;                         /* the radio is an access point */
static struct hakios_sta_scan asked_scan_what;
static struct hakios_sta_join asked_join_what;
static struct hakios_sta_join standing;        /* the network joined whenever the radio comes on (ssid_length 0: none) */
static struct hakios_sta_join fallback;        /* ... as it was before a join that has not come through yet */

static struct hakios_sta_bss *heard;           /* by the last scan that came to an end */
static unsigned heard_count;
static struct hakios_sta_channel channels[CHANNELS_MOST];
static unsigned channel_count;

static void
heard_one(const struct hakios_sta_bss *bss, __unused void *context)
{
	/* (only the thread writes the list, and it reads the count without the lock) */
	if (heard_count < HEARD_MOST) {
		lck_mtx_lock(&sta_lock);
		heard[heard_count++] = *bss;
		lck_mtx_unlock(&sta_lock);
	}
}

static void
sta_runs(__unused void *parameter, __unused wait_result_t result)
{
	static struct hakios_sta_scan scan;
	static struct hakios_sta_join join;

	for (;;) {
		const struct hakios_sta_ops *ops;
		bool power = false, power_on = false, leave = false, do_join = false, do_scan = false, do_ap = false, ap_on = false;
		static struct hakios_sta_ap ap;
		int error = 0;

		lck_mtx_lock(&sta_lock);
		while (sta_ops == NULL || !(asked_power || asked_leave || asked_join || asked_scan || asked_ap)) {
			lck_mtx_sleep(&sta_lock, LCK_SLEEP_DEFAULT, (event_t)&sta_ops, THREAD_UNINT);
		}
		ops = sta_ops;
		/* one thing at a time, in the order that makes sense when several wait */
		if (asked_ap) {
			do_ap = true;
			ap_on = asked_ap_on;
			ap = asked_ap_what;
			wipe(&asked_ap_what, sizeof(asked_ap_what));
			asked_ap = false;
		} else if (asked_power) {
			power = true;
			power_on = asked_power_on;
			asked_power = false;
		} else if (asked_leave) {
			leave = true;
			asked_leave = false;
		} else if (asked_join) {
			do_join = true;
			join = asked_join_what;
			bzero(&asked_join_what, sizeof(asked_join_what));
			asked_join = false;
		} else {
			do_scan = true;
			scan = asked_scan_what;
			asked_scan = false;
		}
		sta_calls = true;
		lck_mtx_unlock(&sta_lock);

		if (do_ap) {
			if (ops->ap == NULL) {
				error = ENOTSUP;
			} else {
				error = ops->ap(ap_on ? &ap : NULL);
			}
			wipe(&ap, sizeof(ap));
			lck_mtx_lock(&sta_lock);
			ap_running = ap_on && error == 0;
			/* the hotspot off: the radio a station again, and the standing network joined */
			if (!ap_on && standing.ssid_length != 0 && !asked_join) {
				asked_power = true;
				asked_power_on = true;
			}
			lck_mtx_unlock(&sta_lock);
			hakios_wifi_tell(ap_on ? (error == 0 ? "wifi: the hotspot is on\n" : "wifi: the hotspot did not start\n") :
			    "wifi: the hotspot is off\n");
		} else if (power) {
			struct hakios_sta_state *state = kalloc_data(sizeof(*state), Z_WAITOK | Z_ZERO);
			bool was_joined = false;

			if (state != NULL) {
				ops->state(state);
				was_joined = state->phase == HAKIOS_STA_JOINED;
				kfree_data(state, sizeof(*state));
			}
			error = ops->power(power_on);
			if (!power_on && was_joined) {
				hakios_apple80211_event(HAKIOS_STA_LINK, 0);
			}
			hakios_apple80211_event(HAKIOS_STA_POWER, error == 0 ? power_on : !power_on);
			lck_mtx_lock(&sta_lock);
			if (error == 0 && power_on && standing.ssid_length != 0 && !asked_join && !ap_running) {
				asked_join_what = standing;
				asked_join = true;
			}
			lck_mtx_unlock(&sta_lock);
		} else if (leave) {
			ops->leave();
			hakios_apple80211_event(HAKIOS_STA_LINK, 0);
		} else if (do_join) {
			error = ops->join(&join);
			bzero(&join, sizeof(join));
			hakios_apple80211_event(HAKIOS_STA_JOIN_DONE, error);
			if (error == 0) {
				hakios_apple80211_event(HAKIOS_STA_LINK, 1);
			}
			/* a join that did not come through (a wrong passphrase) leaves things as they were: the standing network again */
			lck_mtx_lock(&sta_lock);
			if (error != 0 && fallback.ssid_length != 0 && !asked_join && !asked_leave && !asked_power) {
				standing = asked_join_what = fallback;
				asked_join = true;
			}
			bzero(&fallback, sizeof(fallback));
			lck_mtx_unlock(&sta_lock);
		} else if (do_scan) {
			error = ops->scan(&scan);
			lck_mtx_lock(&sta_lock);
			heard_count = 0;
			lck_mtx_unlock(&sta_lock);
			ops->heard(heard_one, NULL);
			hakios_apple80211_event(HAKIOS_STA_SCAN_DONE, error);
		}

		lck_mtx_lock(&sta_lock);
		sta_calls = false;
		lck_mtx_unlock(&sta_lock);
		thread_wakeup((event_t)&sta_calls);
	}
}

/* ---- the station side ---------------------------------------------------------------------------------- */

void
hakios_sta_attach(const struct hakios_sta_ops *ops)
{
	thread_t thread;

	if (ops == NULL) {
		lck_mtx_lock(&sta_lock);
		sta_ops = NULL;
		asked_power = asked_scan = asked_join = asked_leave = false;
		while (sta_calls || sta_reads != 0) {
			lck_mtx_sleep(&sta_lock, LCK_SLEEP_DEFAULT, (event_t)&sta_calls, THREAD_UNINT);
		}
		heard_count = channel_count = 0;
		lck_mtx_unlock(&sta_lock);
		return;
	}
	if (heard == NULL) {
		heard = kalloc_data(HEARD_MOST * sizeof(*heard), Z_WAITOK | Z_ZERO);
	}
	if (heard == NULL) {
		hakios_wifi_tell("wifi: no memory for what a scan hears: nobody can ask the radio for a network\n");
		return;
	}
	if (!sta_thread_made) {
		if (kernel_thread_start(sta_runs, NULL, &thread) != KERN_SUCCESS) {
			hakios_wifi_tell("wifi: no thread for the station: nobody can ask the radio for a network\n");
			return;
		}
		thread_set_thread_name(thread, "wifi-station");
		thread_deallocate(thread);
		sta_thread_made = true;
	}
	channel_count = ops->channels(channels, CHANNELS_MOST);
	lck_mtx_lock(&sta_lock);
	sta_ops = ops;
	sta_on = true;
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	hakios_apple80211_event(HAKIOS_STA_POWER, 1);
	hotspot_test_start();
}

void
hakios_sta_lost(int error)
{
	hakios_apple80211_event(HAKIOS_STA_LINK, error != 0 ? error : ECONNRESET);
}

/* ---- who wants a network ------------------------------------------------------------------------------- */

bool
hakios_sta_present(void)
{
	return sta_ops != NULL;
}

bool
hakios_sta_powered(void)
{
	return sta_ops != NULL && sta_on;
}

bool
hakios_sta_busy(void)
{
	bool busy;

	lck_mtx_lock(&sta_lock);
	busy = sta_ops != NULL && (sta_calls || asked_power || asked_leave || asked_join || asked_scan);
	lck_mtx_unlock(&sta_lock);
	return busy;
}

int
hakios_sta_ask_power(bool on)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL) {
		/*
		 * Before the radio is there, wifid asks for it to be powered at once (r158: "Unable to set power state (OFF) to ON
		 * (err = -3903)", ENXIO here) and gives up its auto-join for good. Taken as said instead, as an iPhone's driver takes
		 * it: the radio comes up powered (hakios_sta_attach), and the power event it posts then is the one wifid waits for.
		 */
		sta_on = on;
		lck_mtx_unlock(&sta_lock);
		return 0;
	}
	sta_on = on;
	asked_power = true;
	asked_power_on = on;
	if (!on) {
		asked_scan = asked_join = asked_leave = false;
	}
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

int
hakios_sta_ask_scan(const struct hakios_sta_scan *what)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL || !sta_on) {
		lck_mtx_unlock(&sta_lock);
		return sta_ops == NULL ? ENXIO : ENETDOWN;
	}
	asked_scan_what = *what;
	asked_scan = true;
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

int
hakios_sta_ask_join_standing(const struct hakios_sta_join *what)
{
	int error = hakios_sta_ask_join(what);

	lck_mtx_lock(&sta_lock);
	if (error == 0) {
		standing = *what;
	}
	lck_mtx_unlock(&sta_lock);
	return error;
}

int
hakios_sta_ask_join(const struct hakios_sta_join *what)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL || !sta_on) {
		lck_mtx_unlock(&sta_lock);
		return sta_ops == NULL ? ENXIO : ENETDOWN;
	}
	if (standing.ssid_length != 0) {
		fallback = standing;
	}
	bzero(&standing, sizeof(standing));     /* (whoever asks for a network now has the say) */
	asked_join_what = *what;
	asked_join = true;
	asked_leave = false;
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

/*
 * The hotspot: the radio an access point (what: its name, passphrase, channel), or a station again (NULL). While it is
 * one, scans and joins are refused (EBUSY) and the standing network is not joined; turned off, the radio comes up as a
 * station and joins it again.
 */
int
hakios_sta_ask_ap(const struct hakios_sta_ap *what)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL) {
		lck_mtx_unlock(&sta_lock);
		return ENXIO;
	}
	asked_ap = true;
	asked_ap_on = what != NULL;
	if (what != NULL) {
		asked_ap_what = *what;
	} else {
		wipe(&asked_ap_what, sizeof(asked_ap_what));
	}
	asked_join = asked_scan = false;
	bzero(&asked_join_what, sizeof(asked_join_what));
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

/*
 * sysctl kern.hakios.hotspot (1: on, 0: off; reads whether it is on), kern.hakios.hotspot_name (the network's name) and
 * kern.hakios.hotspot_password (8 to 63 characters; written only: a read gives nothing). Root's to write.
 */
static struct hakios_sta_ap hotspot_config;

static int
sysctl_hotspot SYSCTL_HANDLER_ARGS
{
	int value = ap_running ? 1 : 0, error = sysctl_handle_int(oidp, &value, 0, req);

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (value != 0) {
		struct hakios_sta_ap what = hotspot_config;

		error = hakios_sta_ask_ap(&what);
		wipe(&what, sizeof(what));
		return error;
	}
	return hakios_sta_ask_ap(NULL);
}

static int
sysctl_hotspot_name SYSCTL_HANDLER_ARGS
{
	char name[33];
	int error;

	bzero(name, sizeof(name));
	memcpy(name, hotspot_config.ssid, hotspot_config.ssid_length);
	error = sysctl_handle_string(oidp, name, sizeof(name), req);
	if (error == 0 && req->newptr != USER_ADDR_NULL) {
		hotspot_config.ssid_length = (uint8_t)strnlen(name, 32);
		bzero(hotspot_config.ssid, sizeof(hotspot_config.ssid));
		memcpy(hotspot_config.ssid, name, hotspot_config.ssid_length);
	}
	return error;
}

static int
sysctl_hotspot_password SYSCTL_HANDLER_ARGS
{
	char password[65];
	int error;

	bzero(password, sizeof(password));      /* (a read gives an empty string) */
	error = sysctl_handle_string(oidp, password, sizeof(password), req);
	if (error == 0 && req->newptr != USER_ADDR_NULL) {
		size_t length = strnlen(password, 64);

		if (length < 8 || length > 63) {
			wipe(password, sizeof(password));
			return EINVAL;
		}
		wipe(hotspot_config.passphrase, sizeof(hotspot_config.passphrase));
		memcpy(hotspot_config.passphrase, password, length);
		hotspot_config.passphrase_length = (uint8_t)length;
	}
	wipe(password, sizeof(password));
	return error;
}

/*
 * kern.hakios.hotspot_mhz: the channel the hotspot starts on next, in MHz (2412..2472, or 5180..5240, 5745..5825; 0: 2437).
 * Settings' Maximize Compatibility: on, 2.4 GHz; off, 5 GHz where the radio may send first there (the lx layer says
 * when it may not, and src/hotspot.c then goes back to 2.4). kern.hakios.hotspot_stations: the stations joined, now.
 */
static unsigned ap_stations;

void
hakios_sta_ap_stations(unsigned count)
{
	ap_stations = count;
}

static int
sysctl_hotspot_mhz SYSCTL_HANDLER_ARGS
{
	int value = hotspot_config.frequency != 0 ? hotspot_config.frequency : 2437, error = sysctl_handle_int(oidp, &value, 0, req);

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (!(value >= 2412 && value <= 2472 && (value - 2407) % 5 == 0) && !(value >= 5180 && value <= 5240 && value % 20 == 0) &&
	    !(value >= 5745 && value <= 5825 && (value - 5745) % 20 == 0)) {
		return EINVAL;
	}
	hotspot_config.frequency = (uint16_t)value;
	return 0;
}

static int
sysctl_hotspot_stations SYSCTL_HANDLER_ARGS
{
	int value = ap_running ? (int)ap_stations : 0;

	return sysctl_handle_int(oidp, &value, 0, req);
}

SYSCTL_DECL(_kern_hakios);
SYSCTL_PROC(_kern_hakios, OID_AUTO, hotspot_mhz, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_hotspot_mhz, "I",
    "the hotspot's channel next time, MHz");
SYSCTL_PROC(_kern_hakios, OID_AUTO, hotspot_stations, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_hotspot_stations,
    "I", "the hotspot's stations joined, now");
SYSCTL_PROC(_kern_hakios, OID_AUTO, hotspot, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_hotspot, "I",
    "the hotspot: 1 on, 0 off");
SYSCTL_PROC(_kern_hakios, OID_AUTO, hotspot_name, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_hotspot_name,
    "A", "the hotspot's network name");
SYSCTL_PROC(_kern_hakios, OID_AUTO, hotspot_password, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0,
    sysctl_hotspot_password, "A", "the hotspot's WPA2 passphrase (written only)");

/*
 * Boot-arg wifi_ap=1 (a test): the hotspot on as soon as the radio is there, named "HakiOS-6T" (or what
 * kern.hakios.hotspot_name holds by then), with the passphrase that kern.hakios.hotspot_password holds, else a random
 * one nobody knows (so its beacons can be seen, and nothing can join); wifi_ap_mhz=N its channel (2437).
 */
static void
hotspot_test_start(void)
{
	static const char hex[] = "0123456789abcdef";
	struct hakios_sta_ap what = hotspot_config;
	uint32_t on = 0, mhz = 0;

	if (!PE_parse_boot_argn("wifi_ap", &on, sizeof(on)) || on == 0) {
		return;
	}
	if (what.ssid_length == 0) {
		memcpy(what.ssid, "HakiOS-6T", 9);
		what.ssid_length = 9;
	}
	if (what.passphrase_length == 0) {
		uint8_t random[12];

		read_random(random, sizeof(random));
		for (unsigned i = 0; i < sizeof(random); i++) {
			what.passphrase[2 * i] = (uint8_t)hex[random[i] >> 4];
			what.passphrase[2 * i + 1] = (uint8_t)hex[random[i] & 15];
		}
		what.passphrase_length = 2 * sizeof(random);
		wipe(random, sizeof(random));
	}
	if (PE_parse_boot_argn("wifi_ap_mhz", &mhz, sizeof(mhz))) {
		what.frequency = (uint16_t)mhz;
	}
	hakios_wifi_tell("wifi: wifi_ap=1: the hotspot asked for as soon as the radio is there\n");
	(void)hakios_sta_ask_ap(&what);
	wipe(&what, sizeof(what));
}

int
hakios_sta_ask_leave(void)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL) {
		lck_mtx_unlock(&sta_lock);
		return ENXIO;
	}
	asked_leave = true;
	asked_join = false;
	bzero(&asked_join_what, sizeof(asked_join_what));
	bzero(&fallback, sizeof(fallback));
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

bool
hakios_sta_heard(unsigned index, struct hakios_sta_bss *bss)
{
	bool there;

	lck_mtx_lock(&sta_lock);
	if ((there = index < heard_count)) {
		*bss = heard[index];
	}
	lck_mtx_unlock(&sta_lock);
	return there;
}

void
hakios_sta_state(struct hakios_sta_state *state)
{
	const struct hakios_sta_ops *ops;

	bzero(state, sizeof(*state));
	lck_mtx_lock(&sta_lock);
	if ((ops = sta_ops) != NULL) {
		sta_reads++;
	}
	lck_mtx_unlock(&sta_lock);
	if (ops != NULL) {
		ops->state(state);
		lck_mtx_lock(&sta_lock);
		sta_reads--;
		lck_mtx_unlock(&sta_lock);
		thread_wakeup((event_t)&sta_calls);
	}
}

unsigned
hakios_sta_channels(struct hakios_sta_channel *out, unsigned most)
{
	unsigned count;

	lck_mtx_lock(&sta_lock);
	/*
	 * Asked of the radio again every time, not only when it attached: the 6T's (ath10k through lx) had every channel
	 * disabled at that moment, its regulatory domain not yet applied, and the count stayed 0 for good -- Apple80211's
	 * SUPPORTED_CHANNELS answered ENXIO, and wifid's auto-join gave up at once ("WiFiDeviceManagerStartAutoJoinAndReply:
	 * FAILED to get supported channels (err=-3903)", r156): a network joined before was never joined again by itself.
	 */
	if (sta_ops != NULL && sta_ops->channels != NULL) {
		channel_count = sta_ops->channels(channels, CHANNELS_MOST);
	}
	count = channel_count < most ? channel_count : most;
	memcpy(out, channels, count * sizeof(*out));
	lck_mtx_unlock(&sta_lock);
	return count;
}

bool
hakios_sta_address(uint8_t *address)
{
	const struct hakios_sta_ops *ops = sta_ops;

	if (ops != NULL) {
		memcpy(address, ops->address, 6);
	}
	return ops != NULL;
}
