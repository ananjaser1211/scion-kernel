/*
 * gnssd: the phone's GNSS receiver (the BCM4775), as iOS's location stack hears about it. docs/gnssd.md.
 *
 * Its parts:
 *   (a) the chip:        chip_up() / chip_down() / chip_fd(): load the ESW and run the engine (gnsstool esw's
 *                        sequence, docs/gnss.md; until the bring-up is final gnsstool is called, not copied here);
 *   (b) the fix source:  NMEA (GGA, RMC, GSA, GSV) from the engine, or from a file / FIFO / stdin (-n);
 *                        MEIF PVT later (meif_pvt_decode());
 *   (c) the feed:        every fix to locationd, by one of two routes (-L):
 *                          sim   CLSimulationManager, locationd's own interface for positions from outside it (Xcode's,
 *                                `loctool sim`'s). Every app gets the fixes through CoreLocation; their
 *                                sourceInformation says isSimulatedBySoftware. The default: on this phone's identity
 *                                (iPhone10,3) it is the only route that needs no change to locationd (docs/gnssd.md).
 *                          gpsd  the service com.apple.gpsd, in the protocol of Apple's gpsd (proto.gpsd, recovered
 *                                from locationd): fixes as the phone's own GPS. locationd uses it only on hardware it
 *                                knows has Apple's gpsd, which this identity is not; kept for when that changes;
 *   (d) power:           the receiver runs only while a position is wanted: with -L gpsd between locationd's START
 *                        and STOP; with -L sim (where locationd says nothing about its clients) while the screen is
 *                        on and Location Services are on, and for a minute after the screen goes dark.
 *
 *   gnssd [-L sim|gpsd] [-f FWDIR] [-n NMEA] [-t] [-a] [-i MS]   the service (the job com.hakios.gnssd)
 *       -f FWDIR   where the ESW segments are (default /redwood/fw: build.sh DEVICE=star2lte's firmware)
 *       -n NMEA    the NMEA from this file, FIFO or "-" (stdin) instead of the chip; a regular file is replayed in a
 *                  loop, one epoch a second (a recorded or generated test stream: tools/nmea-synth.py)
 *       -t         stamp every fix with the time it is passed on, not the receiver's UTC (for a replayed stream)
 *       -a         (sim) the receiver always on, whatever the screen
 *       -i MS      at most one fix every MS milliseconds (default: every epoch the source gives)
 *   gnssd parse [FILE]         parse NMEA and print each epoch's fix quality (never the position)
 *   gnssd segments [FWDIR]     the ESW segments chip_up() would load
 *
 * It never logs a position: only fix quality, accuracy, satellites and speed.
 */
#import <CoreLocation/CoreLocation.h>
#import <Foundation/Foundation.h>
#include <notify.h>
#include <dirent.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <math.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* ---- libxpc: the SDK carries no headers for it, the library is in libSystem ---------------------------- */

typedef void *xpc_object_t;
typedef xpc_object_t xpc_connection_t;
typedef const struct _xpc_type_s *xpc_type_t;
typedef void (^xpc_handler_t)(xpc_object_t);
extern const struct _xpc_type_s _xpc_type_connection, _xpc_type_dictionary, _xpc_type_error;
extern const struct _xpc_dictionary_s _xpc_error_connection_invalid, _xpc_error_connection_interrupted;
#define XPC_TYPE_CONNECTION (&_xpc_type_connection)
#define XPC_TYPE_DICTIONARY (&_xpc_type_dictionary)
#define XPC_TYPE_ERROR (&_xpc_type_error)
#define XPC_ERROR_CONNECTION_INVALID ((xpc_object_t)&_xpc_error_connection_invalid)
#define XPC_CONNECTION_MACH_SERVICE_LISTENER (1 << 0)
xpc_connection_t xpc_connection_create_mach_service(const char *, dispatch_queue_t, uint64_t);
void xpc_connection_set_event_handler(xpc_connection_t, xpc_handler_t);
void xpc_connection_resume(xpc_connection_t);
void xpc_connection_cancel(xpc_connection_t);
void xpc_connection_send_message(xpc_connection_t, xpc_object_t);
pid_t xpc_connection_get_pid(xpc_connection_t);
xpc_type_t xpc_get_type(xpc_object_t);
xpc_object_t xpc_retain(xpc_object_t);
void xpc_release(xpc_object_t);
xpc_object_t xpc_dictionary_create(const char *const *, const xpc_object_t *, size_t);
const void *xpc_dictionary_get_data(xpc_object_t, const char *, size_t *);
void xpc_dictionary_set_data(xpc_object_t, const char *, const void *, size_t);
uint64_t xpc_dictionary_get_uint64(xpc_object_t, const char *);

/* ---- logging -------------------------------------------------------------------------------------------- */

static void
say(const char *fmt, ...)
{
	va_list ap;
	time_t t = time(NULL);
	char ts[16];

	strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
	printf("gnssd %s: ", ts);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

/* ---- a fix ------------------------------------------------------------------------------------------------ */

struct gnss_fix {
	int      quality;          /* GGA: 0 none, 1 GPS, 2 DGPS, 6 estimated ...; 0 = no fix */
	int      dim;              /* GSA: 1 none, 2 2D, 3 3D */
	double   lat, lon;         /* degrees, WGS84 */
	double   alt_msl;          /* metres above mean sea level (GGA) */
	double   undulation;       /* geoid separation, metres (GGA): WGS84 height = alt_msl + undulation */
	bool     has_alt;
	double   hdop, vdop, pdop;
	double   hacc, vacc;       /* 1-sigma metres, from the engine (MEIF PVT) or estimated from the DOPs */
	double   speed;            /* m/s, -1 unknown */
	double   course;           /* degrees true, -1 unknown */
	int      sats_used;        /* GGA */
	int      sats_seen;        /* GSV, every constellation */
	time_t   utc;              /* the fix's UTC (RMC date + GGA/RMC time); 0 unknown */
	double   utc_frac;
};

/* the receiver's user range error, metres (1 sigma): accuracy = DOP * UERE when the engine gives no estimate */
#define UERE_M 4.0

static void
fix_estimate_accuracy(struct gnss_fix *f)
{
	if (f->hacc <= 0 && f->hdop > 0) f->hacc = f->hdop * UERE_M;
	if (f->vacc <= 0 && f->vdop > 0) f->vacc = f->vdop * UERE_M * 1.5;
	if (f->vacc <= 0 && f->hacc > 0) f->vacc = f->hacc * 1.5;
}

/* ---- (b) the fix source: NMEA ---------------------------------------------------------------------------- */

struct nmea {
	struct gnss_fix cur;       /* the epoch being gathered */
	int    date_d, date_m, date_y;
	int    gsv_sats[8];        /* per talker: satellites in view */
	bool   have_epoch;
	double epoch_tod;          /* the epoch's UTC time of day (seconds), -1 none */
};

static bool
nmea_checksum_ok(const char *s)
{
	const char *star = strchr(s, '*');
	unsigned sum = 0, want;

	if (s[0] != '$') return false;
	if (star == NULL) return true;         /* the checksum is optional in NMEA 0183 */
	for (const char *p = s + 1; p < star; p++) sum ^= (unsigned char)*p;
	return sscanf(star + 1, "%2x", &want) == 1 && want == sum;
}

/* split a sentence into its fields (in place); returns the count */
static int
nmea_split(char *s, char **f, int max)
{
	int n = 0;
	char *star = strchr(s, '*');

	if (star) *star = 0;
	while (n < max) {
		f[n++] = s;
		s = strchr(s, ',');
		if (s == NULL) break;
		*s++ = 0;
	}
	return n;
}

static double
nmea_deg(const char *v, const char *hemi)
{
	double x, d;

	if (!*v) return NAN;
	x = atof(v);
	d = floor(x / 100);
	d += (x - d * 100) / 60;
	return (*hemi == 'S' || *hemi == 'W') ? -d : d;
}

static double
nmea_tod(const char *v)
{
	if (strlen(v) < 6) return -1;
	return ((v[0] - '0') * 10 + v[1] - '0') * 3600 + ((v[2] - '0') * 10 + v[3] - '0') * 60 + atof(v + 4);
}

static int
talker_index(const char *id)
{
	static const char *t[] = { "GP", "GL", "GA", "GB", "BD", "GQ", "GN", "QZ" };
	for (int i = 0; i < 8; i++) if (strncmp(id + 1, t[i], 2) == 0) return i;
	return 7;
}

static void
nmea_close_epoch(struct nmea *n, void (*emit)(const struct gnss_fix *))
{
	struct gnss_fix f;
	int seen = 0;

	if (!n->have_epoch) return;
	f = n->cur;
	for (int i = 0; i < 8; i++) seen += n->gsv_sats[i];
	f.sats_seen = seen;
	if (n->date_y > 0 && n->epoch_tod >= 0) {
		struct tm tm = { 0 };
		tm.tm_year = n->date_y + 100;          /* yy: 20yy */
		tm.tm_mon = n->date_m - 1;
		tm.tm_mday = n->date_d;
		f.utc = timegm(&tm) + (time_t)floor(n->epoch_tod);
		f.utc_frac = n->epoch_tod - floor(n->epoch_tod);
	}
	fix_estimate_accuracy(&f);
	emit(&f);
	n->have_epoch = false;
	memset(&n->cur, 0, sizeof(n->cur));
	n->cur.speed = n->cur.course = -1;
}

/* begin the epoch at time of day tod, closing the one before if this is a new time */
static void
nmea_epoch(struct nmea *n, double tod, void (*emit)(const struct gnss_fix *))
{
	if (n->have_epoch && tod >= 0 && fabs(tod - n->epoch_tod) > 0.001) nmea_close_epoch(n, emit);
	if (!n->have_epoch) {
		n->have_epoch = true;
		n->epoch_tod = tod;
	}
}

/* one sentence; emit() is called with every complete epoch */
static void
nmea_sentence(struct nmea *n, const char *line, void (*emit)(const struct gnss_fix *))
{
	char buf[256], *f[40];
	int nf;
	const char *type;

	if (!nmea_checksum_ok(line) || strlen(line) >= sizeof(buf)) return;
	strcpy(buf, line);
	buf[strcspn(buf, "\r\n")] = 0;
	nf = nmea_split(buf, f, 40);
	if (strlen(f[0]) < 6) return;
	type = f[0] + 3;
	if (strcmp(type, "GGA") == 0 && nf >= 12) {
		nmea_epoch(n, nmea_tod(f[1]), emit);
		n->cur.quality = atoi(f[6]);
		if (n->cur.quality > 0) {
			n->cur.lat = nmea_deg(f[2], f[3]);
			n->cur.lon = nmea_deg(f[4], f[5]);
		}
		n->cur.sats_used = atoi(f[7]);
		if (*f[8]) n->cur.hdop = atof(f[8]);
		if (*f[9]) { n->cur.alt_msl = atof(f[9]); n->cur.has_alt = true; }
		if (*f[11]) n->cur.undulation = atof(f[11]);
	} else if (strcmp(type, "RMC") == 0 && nf >= 10) {
		nmea_epoch(n, nmea_tod(f[1]), emit);
		if (strlen(f[9]) == 6) {
			n->date_d = (f[9][0] - '0') * 10 + f[9][1] - '0';
			n->date_m = (f[9][2] - '0') * 10 + f[9][3] - '0';
			n->date_y = (f[9][4] - '0') * 10 + f[9][5] - '0';
		}
		if (*f[2] == 'A') {
			if (n->cur.quality == 0) {      /* RMC before GGA, or no GGA at all */
				n->cur.quality = 1;
				n->cur.lat = nmea_deg(f[3], f[4]);
				n->cur.lon = nmea_deg(f[5], f[6]);
			}
			if (*f[7]) n->cur.speed = atof(f[7]) * 0.514444;
			if (*f[8]) n->cur.course = atof(f[8]);
		}
	} else if (strcmp(type, "GSA") == 0 && nf >= 18) {
		n->cur.dim = atoi(f[2]);
		if (*f[15]) n->cur.pdop = atof(f[15]);
		if (*f[16] && n->cur.hdop <= 0) n->cur.hdop = atof(f[16]);
		if (*f[17]) n->cur.vdop = atof(f[17]);
	} else if (strcmp(type, "GSV") == 0 && nf >= 4) {
		n->gsv_sats[talker_index(f[0])] = atoi(f[3]);
	}
	/* $PGLOR,..,FIX,%.1f (the on-chip engine's own accuracy estimate) goes here once its format is confirmed */
}

/* ---- (b) the fix source: MEIF PVT (stub) ----------------------------------------------------------------- */

/*
 * The on-chip engine may also report its position as a MEIF PVT message (through RPC 0x1F, docs/gnss.md) rather
 * than as NMEA. When its layout is known, decode it here into a struct gnss_fix (with the engine's own hacc / vacc,
 * which are better than DOP * UERE) and pass it to fix_ready(). Returns false for anything it does not know.
 */
static bool
meif_pvt_decode(const uint8_t *msg, size_t len, struct gnss_fix *out)
{
	(void)msg; (void)len; (void)out;
	return false;
}

/* ---- (a) the chip ----------------------------------------------------------------------------------------- */

static const char *fw_dir = "/redwood/fw";

struct segment { char path[512]; unsigned addr; int order; };

/*
 * The ESW segments in FWDIR, in load order. tools/fetch-firmware.sh (devices/star2lte/firmware/gnss/, installed
 * flat in root/fw by build.sh) names them gnss-esw-<N>-<ADDR>.bin: N the load order, ADDR the baseband address in hex
 * (gpsd's a4775_B0_ImageSegments: 0 at 5fdf0, 1 at 78000, then the 12-byte word at 5fff0).
 */
static int
esw_segments(const char *dir, struct segment *seg, int max)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int n = 0;

	if (d == NULL) return -1;
	while ((e = readdir(d)) != NULL && n < max) {
		int order;
		unsigned addr;
		char tail[8];

		if (sscanf(e->d_name, "gnss-esw-%d-%x.%7s", &order, &addr, tail) == 3 && strcmp(tail, "bin") == 0) {
			snprintf(seg[n].path, sizeof(seg[n].path), "%s/%s", dir, e->d_name);
			seg[n].addr = addr;
			seg[n].order = order;
			n++;
		}
	}
	closedir(d);
	for (int i = 1; i < n; i++)            /* by load order */
		for (int j = i; j > 0 && seg[j].order < seg[j - 1].order; j--) {
			struct segment t = seg[j]; seg[j] = seg[j - 1]; seg[j - 1] = t;
		}
	return n;
}

static int
run(char *const argv[])
{
	pid_t pid;
	int st;

	if (posix_spawn(&pid, argv[0], NULL, NULL, argv, environ) != 0) return -1;
	if (waitpid(pid, &st, 0) < 0) return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/*
 * Bring the receiver up: the ESW through PASSTHROUGH, then RESET, esw_ctrl, IDLE and ON (gnsstool esw; docs/gnss.md).
 * Until that sequence is final (the GNSS bring-up), this calls gnsstool rather than repeating its code. What is still
 * missing for fixes to flow is the step that makes the engine report positions; it belongs here, after ON, and the
 * positions come back through chip_fd() as NMEA (or MEIF PVT for meif_pvt_decode()).
 */
static bool
chip_up(void)
{
	struct segment seg[8];
	int n = esw_segments(fw_dir, seg, 8);
	char *argv[16], arg[8][540];
	int a = 0;

	if (n <= 0) {
		say("chip: no ESW segments (gnss-esw-N-ADDR.bin) in %s; the receiver stays off\n", fw_dir);
		return false;
	}
	argv[a++] = "/redwood/bin/gnsstool";
	argv[a++] = "esw";
	argv[a++] = "-r";
	argv[a++] = "-x";
	for (int i = 0; i < n; i++) {
		snprintf(arg[i], sizeof(arg[i]), "%s@%x", seg[i].path, seg[i].addr);
		argv[a++] = arg[i];
	}
	argv[a] = NULL;
	say("chip: loading %d ESW segments from %s\n", n, fw_dir);
	if (run(argv) != 0) {
		say("chip: gnsstool esw failed; the receiver stays off\n");
		return false;
	}
	/* TODO (GNSS bring-up): start the engine's position reports here */
	return true;
}

static void
chip_down(void)
{
	/* TODO (GNSS bring-up): gnss_stop / the engine OFF, then the ASIC's low power state */
	say("chip: off\n");
}

/* the engine's NMEA, as a descriptor of lines; -1 until the bring-up provides one */
static int
chip_fd(void)
{
	return -1;
}


/* ---- the receiver: on and off ---------------------------------------------------------------------------- */

static dispatch_queue_t q;
static const char *nmea_path;             /* -n */
static bool stamp_now;                    /* -t */
static double min_interval_s;             /* -i */
static int src_fd = -1;
static dispatch_source_t src_read, src_tick;
static struct nmea parser;
static char linebuf[1024];
static size_t linelen;
static bool chip_on;
static bool running;                      /* the receiver is on */
static FILE *replay;

static void fix_ready(const struct gnss_fix *f);

static void
feed_bytes(const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (b[i] == '\n' || linelen == sizeof(linebuf) - 1) {
			linebuf[linelen] = 0;
			if (linelen) nmea_sentence(&parser, linebuf, fix_ready);
			linelen = 0;
		} else {
			linebuf[linelen++] = b[i];
		}
	}
}

/* a regular file: one epoch (up to the next GGA) per tick, the file over again at its end */
static void
replay_tick(void)
{
	char l[256];
	bool gga = false;

	for (;;) {
		long at = ftell(replay);
		if (fgets(l, sizeof(l), replay) == NULL) {
			rewind(replay);
			break;
		}
		if (strlen(l) > 6 && strncmp(l + 3, "GGA,", 4) == 0) {
			if (gga) { fseek(replay, at, SEEK_SET); break; }
			gga = true;
		}
		nmea_sentence(&parser, l, fix_ready);
	}
	nmea_close_epoch(&parser, fix_ready);
}

static void
receiver_on(void)
{
	struct stat st;

	if (running) return;
	running = true;
	memset(&parser, 0, sizeof(parser));
	parser.cur.speed = parser.cur.course = -1;
	linelen = 0;
	if (nmea_path && strcmp(nmea_path, "-") != 0 && stat(nmea_path, &st) == 0 && S_ISREG(st.st_mode)) {
		replay = fopen(nmea_path, "r");
		if (replay == NULL) { say("%s: %s\n", nmea_path, strerror(errno)); return; }
		src_tick = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
		dispatch_source_set_timer(src_tick, dispatch_time(DISPATCH_TIME_NOW, 0), NSEC_PER_SEC, NSEC_PER_SEC / 20);
		dispatch_source_set_event_handler(src_tick, ^{ replay_tick(); });
		dispatch_resume(src_tick);
		say("receiver on: replaying %s, one epoch a second\n", nmea_path);
		return;
	}
	if (nmea_path) {
		src_fd = strcmp(nmea_path, "-") == 0 ? dup(0) : open(nmea_path, O_RDONLY | O_NONBLOCK);
	} else {
		chip_on = chip_up();
		src_fd = chip_on ? chip_fd() : -1;
	}
	if (src_fd < 0) {
		if (nmea_path) say("%s: %s\n", nmea_path, strerror(errno));
		return;
	}
	fcntl(src_fd, F_SETFL, fcntl(src_fd, F_GETFL) | O_NONBLOCK);
	src_read = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, src_fd, 0, q);
	dispatch_source_set_event_handler(src_read, ^{
		char b[512];
		ssize_t r = read(src_fd, b, sizeof(b));
		if (r > 0) feed_bytes(b, r);
		else if (r == 0) dispatch_source_cancel(src_read);
	});
	dispatch_resume(src_read);
	say("receiver on: %s\n", nmea_path ? nmea_path : "the chip's NMEA");
}

static void feed_lost(void);

static void
receiver_off(void)
{
	if (!running) return;
	running = false;
	if (src_tick) { dispatch_source_cancel(src_tick); src_tick = NULL; }
	if (replay) { fclose(replay); replay = NULL; }
	if (src_read) { dispatch_source_cancel(src_read); src_read = NULL; }
	if (src_fd >= 0) { close(src_fd); src_fd = -1; }
	if (chip_on) { chip_down(); chip_on = false; }
	feed_lost();
	say("receiver off\n");
}

/* ---- (c) the feed: CLSimulationManager ------------------------------------------------------------------ */

@interface CLSimulationManager : NSObject
@property (assign) uint8_t locationDeliveryBehavior;
@property (assign) uint8_t locationRepeatBehavior;
- (void)clearSimulatedLocations;
- (void)appendSimulatedLocation:(CLLocation *)l;
- (void)flush;
- (void)startLocationSimulation;
- (void)stopLocationSimulation;
@end

enum feed_kind { FEED_SIM, FEED_GPSD };
static enum feed_kind feed = FEED_SIM;
static CLSimulationManager *sim;
static bool sim_active;
static unsigned fixes_sent;
static double last_sent, last_fix;

static double
now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void
sim_fix(const struct gnss_fix *f, NSDate *when)
{
	CLLocation *l;

	@autoreleasepool {
		l = [[CLLocation alloc] initWithCoordinate:CLLocationCoordinate2DMake(f->lat, f->lon)
		                                  altitude:f->has_alt ? f->alt_msl : 0
		                        horizontalAccuracy:f->hacc
		                          verticalAccuracy:f->has_alt ? f->vacc : -1
		                                    course:f->course >= 0 ? f->course : -1
		                            courseAccuracy:f->course >= 0 && f->speed > 1 ? 10 : -1
		                                     speed:f->speed >= 0 ? f->speed : -1
		                             speedAccuracy:f->speed >= 0 ? 0.5 + f->hdop * 0.2 : -1
		                                 timestamp:when];
		if (sim == nil) sim = [NSClassFromString(@"CLSimulationManager") new];
		if (!sim_active) {
			[sim stopLocationSimulation];
			[sim clearSimulatedLocations];
			sim.locationDeliveryBehavior = 0;      /* deliver as given */
			sim.locationRepeatBehavior = 1;        /* hold the last one until the next */
			[sim appendSimulatedLocation:l];
			[sim flush];
			[sim startLocationSimulation];
			sim_active = true;
			say("feed: positions to locationd (CLSimulationManager)\n");
		} else {
			[sim clearSimulatedLocations];
			[sim appendSimulatedLocation:l];
			[sim flush];
		}
	}
}

static void
sim_lost(void)
{
	if (!sim_active) return;
	@autoreleasepool {
		[sim stopLocationSimulation];
		[sim clearSimulatedLocations];
		[sim flush];
	}
	sim_active = false;
	say("feed: no fix, locationd back on its own sources\n");
}

static void gpsd_fix(const struct gnss_fix *f, double utc);

/* one epoch, from any source */
static void
fix_ready(const struct gnss_fix *f)
{
	double t = now_s(), utc;
	struct timespec ts;

	if (!running) return;
	if (f->quality <= 0 || isnan(f->lat) || isnan(f->lon) || f->hacc <= 0) {
		if (last_fix > 0 && t - last_fix > 10) { feed_lost(); last_fix = 0; }
		return;
	}
	last_fix = t;
	if (min_interval_s > 0 && last_sent > 0 && t - last_sent < min_interval_s) return;
	last_sent = t;
	clock_gettime(CLOCK_REALTIME, &ts);
	utc = (stamp_now || f->utc == 0) ? ts.tv_sec + ts.tv_nsec / 1e9 : f->utc + f->utc_frac;
	if (feed == FEED_SIM) sim_fix(f, [NSDate dateWithTimeIntervalSince1970:utc]);
	else gpsd_fix(f, utc);
	if (fixes_sent++ % 30 == 0)
		say("fix %u: %dD, %d of %d satellites, hdop %.1f, accuracy %.0f m (vertical %.0f), speed %.1f m/s\n",
		    fixes_sent, f->dim ? f->dim : 3, f->sats_used, f->sats_seen, f->hdop, f->hacc, f->vacc,
		    f->speed < 0 ? 0 : f->speed);
}

static void gpsd_lost(void);

static void
feed_lost(void)
{
	if (feed == FEED_SIM) sim_lost();
	else gpsd_lost();
}

/* ---- (d) power, for the sim feed: the screen and the switch ------------------------------------------------ */

static bool always_on;                    /* -a */
static int blank_token = -1;
static dispatch_source_t grace;

static bool
screen_on(void)
{
	uint64_t blanked = 0;

	if (blank_token < 0 || notify_get_state(blank_token, &blanked) != NOTIFY_STATUS_OK) return true;
	return blanked == 0;
}

static void
demand_check(void)
{
	bool want = always_on || (screen_on() && [CLLocationManager locationServicesEnabled]);

	if (want) {
		if (grace) { dispatch_source_cancel(grace); grace = NULL; }
		receiver_on();
	} else if (running && grace == NULL) {
		/* a minute's grace: a short look at the lock screen does not cost a cold start */
		grace = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
		dispatch_source_set_timer(grace, dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC), DISPATCH_TIME_FOREVER, NSEC_PER_SEC);
		dispatch_source_set_event_handler(grace, ^{
			dispatch_source_cancel(grace);
			grace = NULL;
			receiver_off();
		});
		dispatch_resume(grace);
	}
}

static void
serve_sim(void)
{
	int t;

	notify_register_check("com.apple.springboard.hasBlankedScreen", &blank_token);
	notify_register_dispatch("com.apple.springboard.hasBlankedScreen", &t, q, ^(int token) { (void)token; demand_check(); });
	notify_register_dispatch("com.apple.locationd/Prefs", &t, q, ^(int token) { (void)token; demand_check(); });
	/* the switch has no notification of its own that is sure to come: look again now and then */
	dispatch_source_t poll = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
	dispatch_source_set_timer(poll, dispatch_time(DISPATCH_TIME_NOW, 0), 15 * NSEC_PER_SEC, NSEC_PER_SEC);
	dispatch_source_set_event_handler(poll, ^{ demand_check(); });
	dispatch_resume(poll);
	signal(SIGTERM, SIG_IGN);
	dispatch_source_t term = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, q);
	dispatch_source_set_event_handler(term, ^{
		receiver_off();                      /* and with it the simulation: locationd back on Wi-Fi */
		say("stopped\n");
		exit(0);
	});
	dispatch_resume(term);
	say("feeding locationd through CLSimulationManager (%s)\n", nmea_path ? nmea_path : "the BCM4775");
	dispatch_main();
}

/* ---- (c) the feed: the com.apple.gpsd service (-L gpsd) ----------------------------------------------------- */

/* protocol buffers, the little we need: varint, 64-bit and length-delimited fields */
struct pb { uint8_t *b; size_t n, cap; };

static void
pb_raw(struct pb *p, const void *d, size_t n)
{
	if (p->n + n > p->cap) {
		p->cap = (p->n + n) * 2 + 64;
		p->b = realloc(p->b, p->cap);
	}
	memcpy(p->b + p->n, d, n);
	p->n += n;
}

static void
pb_varint(struct pb *p, uint64_t v)
{
	uint8_t c[10];
	int i = 0;

	do {
		c[i] = v & 0x7f;
		v >>= 7;
		if (v) c[i] |= 0x80;
		i++;
	} while (v);
	pb_raw(p, c, i);
}

static void pb_key(struct pb *p, int field, int wire) { pb_varint(p, (uint64_t)field << 3 | wire); }
static void pb_u64(struct pb *p, int field, uint64_t v) { pb_key(p, field, 0); pb_varint(p, v); }
static void pb_double(struct pb *p, int field, double v) { pb_key(p, field, 1); pb_raw(p, &v, 8); }
static void pb_float(struct pb *p, int field, float v) { pb_key(p, field, 5); pb_raw(p, &v, 4); }

static void
pb_msg(struct pb *p, int field, struct pb *m)
{
	pb_key(p, field, 2);
	pb_varint(p, m->n);
	pb_raw(p, m->b, m->n);
	free(m->b);
	memset(m, 0, sizeof(*m));
}

/* the top-level varint fields of a message we receive (Request): field -> value, the rest skipped */
static bool
pb_read_varints(const uint8_t *b, size_t n, uint64_t *v, int maxfield)
{
	size_t i = 0;

	while (i < n) {
		uint64_t k = 0, x = 0;
		int s = 0;
		do { if (i >= n) return false; k |= (uint64_t)(b[i] & 0x7f) << s; s += 7; } while (b[i++] & 0x80);
		switch (k & 7) {
		case 0:
			s = 0;
			do { if (i >= n) return false; x |= (uint64_t)(b[i] & 0x7f) << s; s += 7; } while (b[i++] & 0x80);
			if ((int)(k >> 3) <= maxfield) v[k >> 3] = x;
			break;
		case 1: i += 8; break;
		case 5: i += 4; break;
		case 2:
			s = 0;
			do { if (i >= n) return false; x |= (uint64_t)(b[i] & 0x7f) << s; s += 7; } while (b[i++] & 0x80);
			i += x;
			break;
		default: return false;
		}
	}
	return i == n;
}

/* proto.gpsd.Request.Type / Indication.Type / proto.gnss.Result (GpsdProtocol.proto, GnssTypes.proto) */
enum { REQ_START = 1, REQ_STOP = 2, REQ_PING = 32, REQ_BUILD_DEVICE = 34, REQ_TERMINATION_IMMINENT = 35, REQ_EXIT = 36 };
enum { IND_PVTM = 1, IND_STATUS = 2, IND_STOP_COMPLETED = 20, IND_START_COMPLETED = 22 };
enum { RESULT_SUCCESS = 1 };
enum { GNSS_CONTENT_2D = 4, GNSS_CONTENT_3D = 5, ESTIMATION_FILTERED = 3, RELIABILITY_HIGH = 75, RELIABILITY_MEDIUM = 50 };

static xpc_connection_t client;           /* locationd */
static bool started;                      /* between locationd's START and STOP */
static time_t boot_time;

static uint64_t
continuous_ns(void)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) mach_timebase_info(&tb);
	return mach_continuous_time() * tb.numer / tb.denom;
}

static void
send_to_client(const char *key, struct pb *m)
{
	xpc_object_t d;

	if (client == NULL) { free(m->b); return; }
	d = xpc_dictionary_create(NULL, NULL, 0);
	xpc_dictionary_set_data(d, key, m->b, m->n);
	xpc_connection_send_message(client, d);
	xpc_release(d);
	free(m->b);
}

static void
send_response(uint64_t type)
{
	struct pb r = { 0 };

	pb_u64(&r, 1, type);                    /* request_type */
	pb_u64(&r, 2, continuous_ns());         /* mach_continuous_time_ns */
	pb_u64(&r, 3, RESULT_SUCCESS);          /* result_code */
	send_to_client("gpsdResponse", &r);
}

static void
send_indication_result(int type, uint64_t request, int field)
{
	struct pb r = { 0 };

	pb_u64(&r, 1, type);
	pb_u64(&r, 2, request);
	pb_u64(&r, 3, continuous_ns());
	pb_u64(&r, field, RESULT_SUCCESS);      /* start_status (22) / stop_status (20) */
	send_to_client("gpsdIndication", &r);
}

static void
send_status(void)
{
	struct pb r = { 0 }, s = { 0 };

	pb_u64(&s, 1, (uint64_t)(time(NULL) - boot_time) * 1000);   /* gpsd_uptime */
	pb_u64(&s, 2, (uint64_t)(time(NULL) - boot_time) * 1000);   /* device_uptime */
	pb_u64(&s, 3, 1);                                           /* is_device_initialized */
	pb_u64(&s, 4, 0);                                           /* is_device_interface_busy */
	pb_u64(&s, 5, started);                                     /* is_device_active */
	pb_u64(&r, 1, IND_STATUS);
	pb_u64(&r, 3, continuous_ns());
	pb_msg(&r, 9, &s);
	send_to_client("gpsdIndication", &r);
}

/* one fix as Indication(PVTM): proto.gnss.Fix (GnssTypes.proto) */
static void
gpsd_fix(const struct gnss_fix *f, double utc)
{
	struct pb ind = { 0 }, fix = { 0 }, t = { 0 }, pos = { 0 }, vel = { 0 };
	uint64_t mono = continuous_ns();

	if (!started || client == NULL) return;
	/* proto.gnss.Time: GPS time (from 6 Jan 1980, 18 leap seconds ahead of UTC since 2017) and the receive time */
	pb_u64(&t, 1, (uint64_t)((utc - 315964800.0 + 18) * 1e9));
	pb_float(&t, 2, 100.0f);
	pb_u64(&t, 3, 1);
	pb_u64(&t, 4, 18);
	pb_u64(&t, 5, mono);
	pb_double(&t, 6, mono / 1e9);
	pb_double(&t, 7, 0.001);
	/* proto.gnss.Position */
	pb_double(&pos, 1, f->lat);
	pb_double(&pos, 2, f->lon);
	if (f->has_alt) {
		pb_double(&pos, 3, f->alt_msl + f->undulation);
		pb_double(&pos, 4, f->undulation);
		pb_double(&pos, 5, f->vacc);
	}
	pb_double(&pos, 6, f->hacc);
	pb_double(&pos, 7, f->hacc);
	pb_double(&pos, 8, 0);
	pb_u64(&pos, 10, f->hdop > 0 && f->hdop < 2 ? RELIABILITY_HIGH : RELIABILITY_MEDIUM);
	pb_u64(&pos, 12, f->dim == 2 ? GNSS_CONTENT_2D : GNSS_CONTENT_3D);
	/* proto.gnss.Velocity */
	if (f->speed >= 0) {
		pb_double(&vel, 1, f->speed);
		pb_double(&vel, 2, 0.5 + f->hdop * 0.2);
		pb_double(&vel, 3, 0);
		pb_double(&vel, 4, 1.0);
		if (f->course >= 0) {
			pb_double(&vel, 5, f->course);
			pb_double(&vel, 6, f->speed > 1 ? 10.0 : 180.0);
		}
	}
	pb_msg(&fix, 1, &t);
	pb_msg(&fix, 2, &pos);
	if (vel.n) pb_msg(&fix, 3, &vel);
	pb_u64(&fix, 5, ESTIMATION_FILTERED);
	pb_u64(&ind, 1, IND_PVTM);
	pb_u64(&ind, 3, mono);
	pb_msg(&ind, 4, &fix);
	send_to_client("gpsdIndication", &ind);
}

static void
gpsd_lost(void)
{
	/* nothing to withdraw: locationd ages the last PVTM out by itself */
}

static const char *
request_name(uint64_t t)
{
	switch (t) {
	case 1: return "START"; case 2: return "STOP"; case 3: return "RESET"; case 4: return "CLEAR";
	case 5: return "DELETE_GNSS_DATA"; case 6: return "SET_PVTM_REPORT"; case 7: return "SET_NMEA_HANDLER";
	case 8: return "INJECT_ASSISTANCE_POSITION"; case 9: return "INJECT_ASSISTANCE_TIME";
	case 10: return "INJECT_ASSISTANCE_FILE"; case 23: return "SET_POWER_REPORT";
	case 24: return "SET_CONFIG_SIMULATOR_MODE"; case 25: return "SET_CONFIG_ENABLE_GNSS_CONSTELLATIONS";
	case 26: return "SET_CONFIG_DUTY_CYCLING"; case 32: return "PING"; case 34: return "BUILD_DEVICE";
	case 35: return "TERMINATION_IMMINENT"; case 36: return "EXIT"; case 53: return "SET_EMERGENCY_CONFIG";
	default: return NULL;
	}
}

static void
handle_request(const uint8_t *b, size_t n)
{
	uint64_t v[4] = { 0 };
	uint64_t type;
	const char *name;

	if (!pb_read_varints(b, n, v, 3)) { say("a request that does not parse (%zu bytes)\n", n); return; }
	type = v[3];
	name = request_name(type);
	if (type != REQ_PING) {
		if (name) say("locationd: %s\n", name);
		else say("locationd: request %llu\n", (unsigned long long)type);
	}
	/* every request is answered; assistance and configuration are taken and not used (yet) */
	send_response(type);
	switch (type) {
	case REQ_BUILD_DEVICE:
		send_status();
		break;
	case REQ_START:
		started = true;
		receiver_on();
		send_indication_result(IND_START_COMPLETED, type, 22);
		break;
	case REQ_STOP:
		started = false;
		receiver_off();
		send_indication_result(IND_STOP_COMPLETED, type, 20);
		break;
	case REQ_EXIT:
		say("locationd asks gnssd to exit\n");
		exit(0);
	}
}

static void
serve_gpsd(void)
{
	xpc_connection_t l;

	boot_time = time(NULL);
	l = xpc_connection_create_mach_service("com.apple.gpsd", q, XPC_CONNECTION_MACH_SERVICE_LISTENER);
	xpc_connection_set_event_handler(l, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) return;
		say("locationd connected (pid %d)\n", xpc_connection_get_pid(peer));
		if (client) {                    /* a new locationd: the old connection is gone */
			started = false;
			receiver_off();
			xpc_connection_cancel(client);
			xpc_release(client);
		}
		client = xpc_retain(peer);
		xpc_connection_set_event_handler(peer, ^(xpc_object_t m) {
			if (xpc_get_type(m) == XPC_TYPE_DICTIONARY) {
				size_t len = 0;
				const void *req = xpc_dictionary_get_data(m, "gpsdRequest", &len);
				if (req) handle_request(req, len);
				/* the connection message (connection_ns) needs no answer */
			} else if (xpc_get_type(m) == XPC_TYPE_ERROR && peer == client) {
				say("locationd went away\n");
				started = false;
				receiver_off();
				xpc_release(client);
				client = NULL;
			}
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(l);
	say("serving com.apple.gpsd (%s)\n", nmea_path ? nmea_path : "the BCM4775");
	dispatch_main();
}

/* ---- the command line -------------------------------------------------------------------------------------- */

static void
print_epoch(const struct gnss_fix *f)
{
	printf("epoch: quality %d, %dD, %d used / %d seen, hdop %.1f, accuracy %.1f m (vertical %.1f), speed %.1f m/s, "
	       "course %.0f, altitude %s, utc %s\n", f->quality, f->dim, f->sats_used, f->sats_seen, f->hdop, f->hacc,
	       f->vacc, f->speed, f->course, f->has_alt ? "yes" : "no", f->utc ? "yes" : "no");
}

int
main(int argc, char **argv)
{
	int c;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc > 1 && strcmp(argv[1], "parse") == 0) {
		FILE *in = argc > 2 ? fopen(argv[2], "r") : stdin;
		char l[256];
		struct nmea n = { 0 };
		if (in == NULL) { perror(argv[2]); return 1; }
		n.cur.speed = n.cur.course = -1;
		while (fgets(l, sizeof(l), in)) nmea_sentence(&n, l, print_epoch);
		nmea_close_epoch(&n, print_epoch);
		return 0;
	}
	if (argc > 1 && strcmp(argv[1], "segments") == 0) {
		struct segment s[8];
		int n = esw_segments(argc > 2 ? argv[2] : fw_dir, s, 8);
		if (n < 0) { perror(argc > 2 ? argv[2] : fw_dir); return 1; }
		for (int i = 0; i < n; i++) printf("%s@%x\n", s[i].path, s[i].addr);
		return n > 0 ? 0 : 1;
	}
	while ((c = getopt(argc, argv, "L:f:n:tai:")) != -1) {
		switch (c) {
		case 'L':
			if (strcmp(optarg, "sim") == 0) feed = FEED_SIM;
			else if (strcmp(optarg, "gpsd") == 0) feed = FEED_GPSD;
			else goto usage;
			break;
		case 'f': fw_dir = optarg; break;
		case 'n': nmea_path = optarg; break;
		case 't': stamp_now = true; break;
		case 'a': always_on = true; break;
		case 'i': min_interval_s = atof(optarg) / 1000; break;
		default:
		usage:
			fprintf(stderr, "usage: gnssd [-L sim|gpsd] [-f FWDIR] [-n NMEA] [-t] [-a] [-i MS] | gnssd parse [FILE] |"
			    " gnssd segments [FWDIR]\n");
			return 2;
		}
	}
	(void)meif_pvt_decode;
	q = dispatch_get_main_queue();
	if (feed == FEED_GPSD) serve_gpsd();
	else serve_sim();
	return 0;
}
