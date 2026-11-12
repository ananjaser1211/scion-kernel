/*
 * gnssd: the phone's GNSS receiver (the BCM4775), for iOS. docs/gnssd.md.
 *
 *   (a) the chip:    a thread of its own runs docs/gnss.md's sequence through /dev/hakios-gnss (src/gnss_chip.c): the
 *                    AR table cut, the external LNA's pin mux, the hub's log facilities, GnssPvtDataStart, the ESW
 *                    (/redwood/fw/gnss-esw-N-ADDR.bin) through the baseband's boot ROM, RESET, esw_ctrl, CONFIG_SET
 *                    (no EXT_OSC), EXT_LNA, IDLE, ON, disable_indoor, gnss_start; the engine's statistics now and then
 *                    while nothing is seen (-c); gnss_stop when the receiver is no longer wanted. It reads the on-chip
 *                    engine's NMEA from the hub's log (ONCHIP_NMEA[0][$GPGGA...), its state reports, and its
 *                    statistics (AGC, noise, search table, oscillator) for the diagnostics.
 *   (b) the fixes:   NMEA GGA, RMC, GSA, GSV (from the chip, or a file / FIFO / stdin with -n); MEIF PVT later
 *                    (meif_pvt_decode()). Accuracy is DOP x UERE until the engine gives its own.
 *   (c) locationd:   -L gpsd (the default): gnssd is com.apple.gpsd, in Apple's gpsd protocol (proto.gpsd, recovered from
 *                    locationd), and locationd takes the fixes as the phone's own GPS. That needs locationd to pick its
 *                    gpsd device (src/inject_locationd.c) and its new GNSS architecture (UseNewGnssArchitecture).
 *                    -L sim: CLSimulationManager instead (the fallback: no change to locationd, fixes flagged simulated).
 *   (d) power:       the receiver runs while locationd wants GNSS (its START .. STOP), or from the status app's Start
 *                    until its Pause (Darwin notifications com.hakios.gnssd.start / .pause), or with -a always.
 *   (e) status:      /var/mobile/Library/Caches/com.hakios.gnssd/status.plist about once a second, then the Darwin
 *                    notification com.hakios.gnssd.status: state, satellites, DOPs, accuracy, the fix (on the phone's
 *                    screen only: positions never go to a log), and the diagnostics (step, engine state, ESW, AR cut,
 *                    detections/attempts, AGC, noise, oscillator, masked hub lines).
 *   (f) session log: STATUS_DIR/session-YYYYMMDD-HHMMSS.log for each receiver start (the last 10 kept): steps,
 *                    states, statistics every 10 s, satellites, fix type and accuracy. Never a position.
 *
 *   gnssd [-L gpsd|sim] [-f FWDIR] [-n NMEA] [-t] [-a] [-i MS] [-c S]
 *       -f FWDIR   the ESW's directory (default /redwood/fw)
 *       -n NMEA    NMEA from this file, FIFO or "-" instead of the chip; a regular file is replayed one epoch a second
 *       -t         stamp each fix with the time it is passed on, not the receiver's UTC (for a replayed stream)
 *       -a         the receiver always on
 *       -i MS      at most one fix every MS milliseconds
 *       -c S       the engine's search statistics every S s while no satellite is in view (default 120, 0 never)
 *   gnssd parse [FILE]         NMEA -> one line per epoch: quality, satellites, DOP, accuracy (never the position)
 *   gnssd segments [FWDIR]     the ESW pieces the chip would be given
 */
#import <CoreLocation/CoreLocation.h>
#import <Foundation/Foundation.h>
#include <ctype.h>
#include <dirent.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <math.h>
#include <notify.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include "gnss_chip.h"

/* ---- libxpc: the SDK carries no headers for it, the library is in libSystem ---------------------------- */

typedef void *xpc_object_t;
typedef xpc_object_t xpc_connection_t;
typedef const struct _xpc_type_s *xpc_type_t;
typedef void (^xpc_handler_t)(xpc_object_t);
extern const struct _xpc_type_s _xpc_type_connection, _xpc_type_dictionary, _xpc_type_error;
#define XPC_TYPE_CONNECTION (&_xpc_type_connection)
#define XPC_TYPE_DICTIONARY (&_xpc_type_dictionary)
#define XPC_TYPE_ERROR (&_xpc_type_error)
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

static double
mono_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- a fix --------------------------------------------------------------------------------------------------- */

enum { SYS_GPS, SYS_GLONASS, SYS_GALILEO, SYS_BEIDOU, SYS_QZSS, SYS_SBAS };
static NSString *const sys_names[] = { @"GPS", @"GLONASS", @"Galileo", @"BeiDou", @"QZSS", @"SBAS" };

struct sat { int sys, prn, elev, azim; double cn0; bool used; };
#define MAX_SATS 64

struct gnss_fix {
	int      quality;          /* GGA: 0 none, 1 GPS, 2 DGPS, 6 estimated ...; 0 = no fix */
	int      dim;              /* GSA: 1 none, 2 2D, 3 3D (0: not said) */
	double   lat, lon;         /* degrees, WGS84 */
	double   alt_msl;          /* metres above mean sea level */
	double   undulation;       /* geoid separation: WGS84 height = alt_msl + undulation */
	bool     has_alt;
	double   hdop, vdop, pdop;
	double   hacc, vacc;       /* 1 sigma, metres: the engine's, or DOP x UERE */
	double   speed;            /* m/s, -1 unknown */
	double   course;           /* degrees, -1 unknown */
	int      sats_used;
	int      sats_seen;
	int      nsat;
	struct sat sats[MAX_SATS];
	time_t   utc;              /* RMC date + time; 0 unknown */
	double   utc_frac;
};

#define UERE_M 4.0             /* the receiver's user range error, 1 sigma, metres */

static void
fix_estimate_accuracy(struct gnss_fix *f)
{
	if (f->hacc <= 0 && f->hdop > 0) f->hacc = f->hdop * UERE_M;
	if (f->vacc <= 0 && f->vdop > 0) f->vacc = f->vdop * UERE_M * 1.5;
	if (f->vacc <= 0 && f->hacc > 0) f->vacc = f->hacc * 1.5;
}

static int
fix_type(const struct gnss_fix *f)
{
	if (f->quality <= 0 || isnan(f->lat) || isnan(f->lon)) return 0;
	if (f->dim == 2 || (f->dim == 0 && !f->has_alt)) return 2;
	return 3;
}

/* ---- (b) NMEA ------------------------------------------------------------------------------------------------- */

struct nmea {
	struct gnss_fix cur;
	int    date_d, date_m, date_y;
	struct sat view[8][MAX_SATS];      /* per talker, from its GSV messages */
	int    nview[8], gsv_count[8];
	int    used_prn[8][32];            /* per talker, from its GSA */
	int    nused[8];
	bool   have_epoch;
	double epoch_tod;
};

static const char *const talkers[8] = { "GP", "GL", "GA", "GB", "BD", "GQ", "GN", "QZ" };

static int
talker_index(const char *id)
{
	for (int i = 0; i < 8; i++) if (strncmp(id + 1, talkers[i], 2) == 0) return i;
	return 6;
}

static int
sat_system(int talker, int prn)
{
	switch (talker) {
	case 0: return prn >= 193 && prn <= 200 ? SYS_QZSS : prn >= 33 && prn <= 64 ? SYS_SBAS : SYS_GPS;
	case 1: return SYS_GLONASS;
	case 2: return SYS_GALILEO;
	case 3: case 4: return SYS_BEIDOU;
	case 5: case 7: return SYS_QZSS;
	default:        /* GN: the NMEA 4.x numbering */
		if (prn >= 65 && prn <= 96) return SYS_GLONASS;
		if (prn >= 193 && prn <= 200) return SYS_QZSS;
		if (prn >= 201 && prn <= 263) return SYS_BEIDOU;
		if (prn >= 301 && prn <= 336) return SYS_GALILEO;
		if (prn >= 33 && prn <= 64) return SYS_SBAS;
		return SYS_GPS;
	}
}

static bool
nmea_checksum_ok(const char *s)
{
	const char *star = strchr(s, '*');
	unsigned sum = 0, want;

	if (s[0] != '$') return false;
	if (star == NULL) return true;
	for (const char *p = s + 1; p < star; p++) sum ^= (unsigned char)*p;
	return sscanf(star + 1, "%2x", &want) == 1 && want == sum;
}

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

typedef void (*emit_fn)(const struct gnss_fix *);

static void
nmea_reset(struct nmea *n)
{
	memset(n, 0, sizeof(*n));
	n->cur.speed = n->cur.course = -1;
	n->cur.lat = n->cur.lon = NAN;
}

static void
nmea_close_epoch(struct nmea *n, emit_fn emit)
{
	struct gnss_fix f;

	if (!n->have_epoch) return;
	f = n->cur;
	f.nsat = 0;
	f.sats_seen = 0;
	for (int t = 0; t < 8; t++) {
		f.sats_seen += n->nview[t];
		for (int i = 0; i < n->nview[t] && f.nsat < MAX_SATS; i++) {
			struct sat s = n->view[t][i];

			s.used = false;
			for (int u = 0; u < 8 && !s.used; u++)
				for (int k = 0; k < n->nused[u]; k++)
					if (n->used_prn[u][k] == s.prn && (u == 6 || u == t)) s.used = true;
			f.sats[f.nsat++] = s;
		}
	}
	if (f.sats_used == 0 || f.quality <= 0) {    /* with no fix, GGA's count is not satellites used */
		f.sats_used = 0;
		for (int i = 0; i < f.nsat; i++) f.sats_used += f.sats[i].used;
	}
	if (n->date_y > 0 && n->epoch_tod >= 0) {
		struct tm tm = { 0 };

		tm.tm_year = n->date_y + 100;
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
	n->cur.lat = n->cur.lon = NAN;
	memset(n->nused, 0, sizeof(n->nused));
}

static void
nmea_epoch(struct nmea *n, double tod, emit_fn emit)
{
	if (n->have_epoch && tod >= 0 && fabs(tod - n->epoch_tod) > 0.001) nmea_close_epoch(n, emit);
	if (!n->have_epoch) {
		n->have_epoch = true;
		n->epoch_tod = tod;
	}
}

static void
nmea_sentence(struct nmea *n, const char *line, emit_fn emit)
{
	char buf[256], *f[40];
	int nf, t;
	const char *type;

	if (!nmea_checksum_ok(line) || strlen(line) >= sizeof(buf)) return;
	strcpy(buf, line);
	buf[strcspn(buf, "\r\n")] = 0;
	nf = nmea_split(buf, f, 40);
	if (strlen(f[0]) < 6) return;
	type = f[0] + 3;
	t = talker_index(f[0]);
	if (strcmp(type, "GGA") == 0 && nf >= 12) {
		nmea_epoch(n, nmea_tod(f[1]), emit);
		n->cur.quality = atoi(f[6]);
		if (n->cur.quality > 0) {
			n->cur.lat = nmea_deg(f[2], f[3]);
			n->cur.lon = nmea_deg(f[4], f[5]);
		}
		n->cur.sats_used = atoi(f[7]);
		if (*f[8]) n->cur.hdop = atof(f[8]);
		if (*f[9]) { n->cur.alt_msl = atof(f[9]); n->cur.has_alt = n->cur.quality > 0; }
		if (*f[11]) n->cur.undulation = atof(f[11]);
	} else if (strcmp(type, "RMC") == 0 && nf >= 10) {
		nmea_epoch(n, nmea_tod(f[1]), emit);
		if (strlen(f[9]) == 6) {
			n->date_d = (f[9][0] - '0') * 10 + f[9][1] - '0';
			n->date_m = (f[9][2] - '0') * 10 + f[9][3] - '0';
			n->date_y = (f[9][4] - '0') * 10 + f[9][5] - '0';
		}
		if (*f[2] == 'A') {
			if (n->cur.quality == 0) {
				n->cur.quality = 1;
				n->cur.lat = nmea_deg(f[3], f[4]);
				n->cur.lon = nmea_deg(f[5], f[6]);
			}
			if (*f[7]) n->cur.speed = atof(f[7]) * 0.514444;
			if (*f[8]) n->cur.course = atof(f[8]);
		}
	} else if (strcmp(type, "GSA") == 0 && nf >= 18) {
		n->cur.dim = atoi(f[2]);
		for (int i = 3; i <= 14; i++)
			if (*f[i] && n->nused[t] < 32) n->used_prn[t][n->nused[t]++] = atoi(f[i]);
		if (*f[15]) n->cur.pdop = atof(f[15]);
		if (*f[16]) n->cur.hdop = atof(f[16]);
		if (*f[17]) n->cur.vdop = atof(f[17]);
	} else if (strcmp(type, "GSV") == 0 && nf >= 4) {
		int msg = atoi(f[2]);

		if (msg == 1) n->nview[t] = 0;
		n->gsv_count[t] = atoi(f[3]);
		for (int i = 4; i < nf && n->nview[t] < MAX_SATS; i += 4) {
			struct sat s = { 0 };

			if (!*f[i]) continue;
			s.prn = atoi(f[i]);
			s.sys = sat_system(t, s.prn);
			s.elev = i + 1 < nf && *f[i + 1] ? atoi(f[i + 1]) : -1;      /* -1: not known yet */
			s.azim = i + 2 < nf && *f[i + 2] ? atoi(f[i + 2]) : -1;
			s.cn0 = i + 3 < nf && *f[i + 3] ? atof(f[i + 3]) : 0;
			n->view[t][n->nview[t]++] = s;
		}
	}
}

/* (b) MEIF PVT: when the on-chip engine's position message is known, decode it here (with its own hacc / vacc) */
static bool
meif_pvt_decode(const uint8_t *msg, size_t len, struct gnss_fix *out)
{
	(void)msg; (void)len; (void)out;
	return false;
}

/* ---- the ESW's pieces ------------------------------------------------------------------------------------------- */

static const char *fw_dir = "/redwood/fw";

struct segment { char path[512]; unsigned addr; int order; };

/* gnss-esw-<N>-<ADDR>.bin in FWDIR (devices/star2lte/firmware/gnss), in load order N */
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
	for (int i = 1; i < n; i++)
		for (int j = i; j > 0 && seg[j].order < seg[j - 1].order; j--) {
			struct segment t = seg[j]; seg[j] = seg[j - 1]; seg[j - 1] = t;
		}
	return n;
}

/* ---- the state (on the main queue) --------------------------------------------------------------------------- */

static dispatch_queue_t q;
static const char *nmea_path;             /* -n */
static bool stamp_now, always_on;         /* -t, -a */
static double min_interval_s;             /* -i */
static bool user_on;                      /* the status app's Start .. Pause */
static bool loc_on;                       /* locationd's START .. STOP (-L gpsd) */
static bool running;                      /* the receiver is on */
static double running_since, first_fix_at;
static NSString *state = @"off", *last_error;
static struct gnss_fix last;              /* the last epoch */
static bool have_last;
static unsigned fixes;

enum feed_kind { FEED_GPSD, FEED_SIM };
static enum feed_kind feed = FEED_GPSD;

static void receiver_on(void);
static void receiver_off(void);
static void feed_fix(const struct gnss_fix *f, double utc);
static void feed_lost(void);

/* ---- diagnostics and the session log: what the receiver did, for a field test with no computer ----------------- */

#define STATUS_DIR "/var/mobile/Library/Caches/com.hakios.gnssd"
#define STATUS_FILE STATUS_DIR "/status.plist"
#define SESSIONS_KEPT 10
#define HUB_LINES 30

enum { CON_GPS, CON_GLO, CON_GAL, NCON };
static NSString *const con_names[NCON] = { @"GPS", @"GLONASS", @"Galileo" };

/* written by the chip's thread, read by the main queue: under diag_lock */
static pthread_mutex_t diag_lock = PTHREAD_MUTEX_INITIALIZER;
static struct diag {
	char step[80], step_error[200], engine[16], cntin[96];
	int det[NCON], att[NCON], tables;   /* summed over the engine's statistics tables this session */
	double agc_gps, agc_glo, noise_gps, noise_exp, osc_off_ppb, osc_unc_ppb;     /* NAN: not reported yet */
	bool esw_loaded, ar_cut;
	int esw_blocks;
	double search_since;                /* mono_s() of the first gnss_start; 0: not searching yet */
	char hub[HUB_LINES][160];
	int hub_n, hub_next;
} dg;
static atomic_int sats_in_view;           /* the last epoch's, for the chip's thread (statistics checkpoints) */
static double checkpoint_s = 120;         /* -c: the engine's statistics every so often while no satellite is seen */

static void
diag_reset(void)
{
	pthread_mutex_lock(&diag_lock);
	memset(&dg, 0, sizeof(dg));
	dg.agc_gps = dg.agc_glo = dg.noise_gps = dg.noise_exp = dg.osc_off_ppb = dg.osc_unc_ppb = NAN;
	dg.esw_blocks = -1;
	pthread_mutex_unlock(&diag_lock);
	atomic_store(&sats_in_view, 0);
}

/*
 * The session log: STATUS_DIR/session-YYYYMMDD-HHMMSS.log (0644), one per receiver start, the last SESSIONS_KEPT kept.
 * Steps, states, the engine's statistics, satellites, fix type and accuracy: never a position. Hub lines are taken
 * from a short list of kinds and masked again (mask_strict) before they are written.
 */
static pthread_mutex_t sess_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *sess;
static char sess_path[256];
static double sess_t0;
static atomic_uint sess_gen;

static void
slog(const char *fmt, ...)
{
	va_list ap;
	time_t t = time(NULL);
	char ts[16];
	double e;

	pthread_mutex_lock(&sess_lock);
	if (sess) {
		strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
		e = mono_s() - sess_t0;
		fprintf(sess, "%s +%02d:%02d ", ts, (int)e / 60, (int)e % 60);
		va_start(ap, fmt);
		vfprintf(sess, fmt, ap);
		va_end(ap);
		fputc('\n', sess);
		fflush(sess);
	}
	pthread_mutex_unlock(&sess_lock);
}

static int
cmp_names(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* leave the newest `keep` session logs */
static void
sessions_prune(int keep)
{
	DIR *d = opendir(STATUS_DIR);
	struct dirent *e;
	char *names[256];
	int n = 0;

	if (d == NULL) return;
	while ((e = readdir(d)) != NULL && n < 256)
		if (strncmp(e->d_name, "session-", 8) == 0 && strstr(e->d_name, ".log")) names[n++] = strdup(e->d_name);
	closedir(d);
	qsort(names, n, sizeof(names[0]), cmp_names);           /* the names sort by time */
	for (int i = 0; i < n; i++) {
		if (i < n - keep) {
			char p[512];

			snprintf(p, sizeof(p), STATUS_DIR "/%s", names[i]);
			unlink(p);
		}
		free(names[i]);
	}
}

static void
session_close_locked(const char *why)
{
	if (sess == NULL) return;
	time_t t = time(NULL);
	char ts[16];
	double e = mono_s() - sess_t0;

	strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
	fprintf(sess, "%s +%02d:%02d session end: %s\n", ts, (int)e / 60, (int)e % 60, why);
	fclose(sess);
	sess = NULL;
}

/* the main queue, at each receiver start; returns the session's number */
static unsigned
session_open(const char *who)
{
	time_t t = time(NULL);
	char name[64], up[32];
	struct tm tm;
	int fd;
	unsigned gen;

	localtime_r(&t, &tm);
	strftime(name, sizeof(name), "session-%Y%m%d-%H%M%S.log", &tm);
	pthread_mutex_lock(&sess_lock);
	session_close_locked("a new session started");
	gen = atomic_fetch_add(&sess_gen, 1) + 1;
	sessions_prune(SESSIONS_KEPT - 1);
	snprintf(sess_path, sizeof(sess_path), STATUS_DIR "/%s", name);
	fd = open(sess_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
	if (fd >= 0) {
		fchmod(fd, 0644);
		sess = fdopen(fd, "a");
	}
	if (sess == NULL) {
		say("session log %s: %s\n", sess_path, strerror(errno));
		sess_path[0] = 0;
	} else {
		struct timeval bt;
		size_t len = sizeof(bt);
		char osv[32] = "?";
		size_t ol = sizeof(osv);

		sysctlbyname("kern.osversion", osv, &ol, NULL, 0);
		if (sysctlbyname("kern.boottime", &bt, &len, NULL, 0) == 0) snprintf(up, sizeof(up), "%lds", (long)(t - bt.tv_sec));
		else snprintf(up, sizeof(up), "?");
		sess_t0 = mono_s();
		strftime(name, sizeof(name), "%Y-%m-%d %H:%M:%S %z", &tm);
		fprintf(sess, "# gnssd session log (docs/gnssd.md): %s, started by %s, uptime %s, system %s, gnssd of %s %s\n"
		    "# sequence: AR table cut, pin mux, log facilities, 1c 01, ESW, RESET, esw_ctrl, CONFIG_SET (no EXT_OSC),"
		    " EXT_LNA, IDLE, ON, disable_indoor, gnss_start; statistics every %.0f s while no satellite is seen\n"
		    "# no positions are ever written here: satellites, fix type and accuracy only\n",
		    name, who, up, osv, __DATE__, __TIME__, checkpoint_s);
		fflush(sess);
	}
	pthread_mutex_unlock(&sess_lock);
	return gen;
}

/* closes session `gen` (0: whichever is open) */
static void
session_close(unsigned gen, const char *why)
{
	pthread_mutex_lock(&sess_lock);
	if (gen == 0 || gen == atomic_load(&sess_gen)) session_close_locked(why);
	pthread_mutex_unlock(&sess_lock);
}

static void
set_step(const char *s, const char *error)
{
	pthread_mutex_lock(&diag_lock);
	snprintf(dg.step, sizeof(dg.step), "%s", s);
	if (error) snprintf(dg.step_error, sizeof(dg.step_error), "%s", error);
	pthread_mutex_unlock(&diag_lock);
	if (error) slog("step %s FAILED: %s", s, error);
	else slog("step: %s", s);
}

static void
set_state(NSString *s, NSString *error)
{
	if (![s isEqualToString:state]) {
		say("state: %s%s%s\n", s.UTF8String, error ? ": " : "", error ? error.UTF8String : "");
		slog("state: %s%s%s", s.UTF8String, error ? ": " : "", error ? error.UTF8String : "");
	}
	state = s;
	last_error = error;
}

static void
demand_changed(void)
{
	if (always_on || user_on || loc_on) receiver_on();
	else receiver_off();
}

/* one epoch, from any source */
static double last_sent, last_good;

static void
fix_ready(const struct gnss_fix *f)
{
	double t = mono_s(), utc;
	struct timespec ts;
	int type = fix_type(f);

	if (!running) return;
	last = *f;
	have_last = true;
	atomic_store(&sats_in_view, f->sats_seen);
	if (type < 2 || f->hacc <= 0) {
		if (![state isEqualToString:@"error"]) set_state(@"searching", nil);
		if (last_good > 0 && t - last_good > 10) {
			feed_lost();
			last_good = 0;
			slog("fix lost");
		}
		return;
	}
	set_state(@"fix", nil);
	if (last_good == 0)
		slog("fix: %dD, %d of %d satellites, hdop %.1f, accuracy %.1f m (vertical %.1f)%s", type, f->sats_used,
		    f->sats_seen, f->hdop, f->hacc, f->vacc, first_fix_at == 0 ? ", the first (TTFF below)" : "");
	if (first_fix_at == 0) {
		first_fix_at = t;
		slog("TTFF %.1f s", t - running_since);
	}
	last_good = t;
	if (min_interval_s > 0 && last_sent > 0 && t - last_sent < min_interval_s) return;
	last_sent = t;
	clock_gettime(CLOCK_REALTIME, &ts);
	utc = (stamp_now || f->utc == 0) ? ts.tv_sec + ts.tv_nsec / 1e9 : f->utc + f->utc_frac;
	feed_fix(f, utc);
	if (fixes++ % 60 == 0)
		say("fix %u: %dD, %d of %d satellites, hdop %.1f, accuracy %.0f m (vertical %.0f)\n", fixes, type,
		    f->sats_used, f->sats_seen, f->hdop, f->hacc, f->vacc);
}

/* ---- (a) the chip's thread ------------------------------------------------------------------------------------- */

static atomic_bool chip_want;             /* the main queue says what it wants; the thread does it */
static pthread_mutex_t chip_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t chip_cond = PTHREAD_COND_INITIALIZER;
static bool facilities_on;                /* the hub's log facilities, once per gnssd: never in a loop */

static void
post_state(NSString *s, NSString *error)
{
	dispatch_async(q, ^{ set_state(s, error); });
}

/*
 * Masks a hub line for the app and the session log, harder than gc_mask_position(): EVERY number with four or more
 * decimals (the engine's statistics included; gnssd writes those itself, rounded), then NMEA's latitude and longitude.
 */
static void
mask_strict(char *t)
{
	size_t k = strlen(t);

	for (size_t i = 0; i < k; i++) {
		if (t[i] == '.' && i > 0 && t[i - 1] >= '0' && t[i - 1] <= '9') {
			size_t j = i + 1;

			while (j < k && t[j] >= '0' && t[j] <= '9') j++;
			if (j - i - 1 >= 4) {
				size_t b = i;

				while (b > 0 && ((t[b - 1] >= '0' && t[b - 1] <= '9') || t[b - 1] == '-')) b--;
				for (size_t x = b; x < j; x++) t[x] = '#';
			}
			i = j;
		}
	}
	gc_mask_position(t);
}

/* one line for the app's hub log (the last HUB_LINES) and the session log; masked here. Under diag_lock. */
static void
hub_keep_locked(const char *line)
{
	char m[160];
	int last = (dg.hub_next + HUB_LINES - 1) % HUB_LINES;

	snprintf(m, sizeof(m), "%s", line);
	mask_strict(m);
	if (dg.hub_n > 0 && strcmp(dg.hub[last], m) == 0) return;      /* the same again */
	strcpy(dg.hub[dg.hub_next], m);
	dg.hub_next = (dg.hub_next + 1) % HUB_LINES;
	if (dg.hub_n < HUB_LINES) dg.hub_n++;
	slog("hub: %s", m);
}

/* the kinds of hub line worth a field test's log: the engine's bring-up, its search, its clock, its troubles */
static const char *const hub_kinds[] = { "INIT_STAGE", "LoadMeasEngine", "UpdateOnchipRequest", "Asic Off",
	"AsicToStandby", "IndoorDetectionByHost", "Strong Sats", "meif_state_report_msg", "Detect(#)", "cnt in clk",
	"MeasCNTIN", "SetCntinMeas", "IsExtOscAvailable", "SetRefClockParams", "assert", "Assert", "ASSERT", "WARNING",
	"Error", "ERROR", NULL };

/* one record of the hub's log (its "hhhh Q...D $...X " prefix taken off): numbers for the status, lines for the logs */
static void
hub_record(const char *b)
{
	char name[8], line[200];
	int det, xc, att, vpr, vrr;
	double dp, xp;
	const char *m;

	/* never anything near a position, and nothing of the NMEA (which only the screen shows) */
	if (strstr(b, "Lat") || strstr(b, "Lon") || strstr(b, "LLA") || strstr(b, "NMEA") || strstr(b, "$G") ||
	    strstr(b, "Pos")) return;
	pthread_mutex_lock(&diag_lock);
	if ((m = strstr(b, "AGC report::")) != NULL) {
		const char *g = strstr(m, "GPS_GAL AAGC "), *r = strstr(m, "GLO AAGC ");

		if (g) dg.agc_gps = atof(g + 13);
		if (r) dg.agc_glo = atof(r + 9);
	} else if ((m = strstr(b, "LogNoiseStats(GPS_GAL) Measured Noise(u:")) != NULL) {
		const char *tb = strstr(m, "Noise Table(u:");

		dg.noise_gps = atof(m + 40);
		if (tb) dg.noise_exp = atof(tb + 14);
		snprintf(line, sizeof(line), "noise GPS_GAL %.3f, table %.3f", dg.noise_gps, dg.noise_exp);
		hub_keep_locked(line);
	} else if ((m = strstr(b, "LogNoiseStats(GLNS) Measured Noise(u:")) != NULL) {
		const char *tb = strstr(m, "Noise Table(u:");

		snprintf(line, sizeof(line), "noise GLNS %.3f, table %.3f", atof(m + 37), tb ? atof(tb + 14) : NAN);
		hub_keep_locked(line);
	} else if ((m = strstr(b, "GetOscOffset(")) != NULL) {
		double off = NAN, unc = NAN;

		if (sscanf(m + 13, "%lf, %lf", &off, &unc) == 2) {
			dg.osc_off_ppb = off * 1e9;
			dg.osc_unc_ppb = unc * 1e9;
			snprintf(line, sizeof(line), "oscillator: GetOscOffset %.0f ppb, uncertainty %.0f ppb", dg.osc_off_ppb,
			    dg.osc_unc_ppb);
			hub_keep_locked(line);
		}
	} else if (sscanf(b, " %7[A-Z], %d, %lf, %d, %lf, %d, %d, %d", name, &det, &dp, &xc, &xp, &att, &vpr, &vrr) == 8) {
		int c = strcmp(name, "GPS") == 0 ? CON_GPS : strcmp(name, "GLNS") == 0 ? CON_GLO :
		    strcmp(name, "GAL") == 0 ? CON_GAL : -1;

		if (c >= 0) {
			dg.det[c] += det;
			dg.att[c] += att;
			if (c == CON_GPS) dg.tables++;
		}
		snprintf(line, sizeof(line), "search %s: %d detections in %d attempts, %d xcorr, %d valid PR, %d valid RR",
		    name, det, att, xc, vpr, vrr);
		hub_keep_locked(line);
	} else {
		if ((m = strstr(b, "SetCntinMeas(")) != NULL) {
			const char *e = strchr(m, ')');
			int n = e ? (int)(e - m - 13) : (int)strlen(m + 13);

			snprintf(dg.cntin, sizeof(dg.cntin), "%.*s", n > 60 ? 60 : n, m + 13);
		} else if (strstr(b, "no reference cnt in clk")) {
			snprintf(dg.cntin, sizeof(dg.cntin), "none (no reference clock)");
		}
		for (int i = 0; hub_kinds[i]; i++) {
			if (strstr(b, hub_kinds[i])) {
				hub_keep_locked(b);
				break;
			}
		}
	}
	pthread_mutex_unlock(&diag_lock);
}

/* "hhhh Q1234567D " at t + i, after the start or a record's end ('.', ' '): the start of a hub log record */
static size_t
rec_start(const char *t, size_t i)
{
	size_t j = i, h = 0, d = 0;

	if (i > 0 && t[i - 1] != '.' && t[i - 1] != ' ') return 0;
	while (h < 4 && isxdigit((unsigned char)t[j])) { j++; h++; }
	if (h < 3 || t[j] != ' ' || t[j + 1] != 'Q') return 0;
	j += 2;
	while (isdigit((unsigned char)t[j])) { j++; d++; }
	if (d < 5 || t[j] != 'D' || t[j + 1] != ' ') return 0;
	return j + 2 - i;
}

/* the start of the next record's prefix left at the end of a record: " fffe Q981", " fffe", " f" (lowercase hex) */
static size_t
last_token(const char *r, size_t s, size_t len)
{
	size_t x = len;

	while (x > s && r[x - 1] != ' ') x--;
	return x;
}

static bool
counter_like(const char *r, size_t from, size_t to)
{
	bool letter = false;

	if (to - from < 1 || to - from > 4) return false;
	for (size_t i = from; i < to; i++) {
		if (!isxdigit((unsigned char)r[i]) || isupper((unsigned char)r[i])) return false;
		if (isalpha((unsigned char)r[i])) letter = true;
	}
	return letter;
}

static size_t
trim_next_prefix(char *r, size_t s, size_t len)
{
	size_t x = last_token(r, s, len);

	if (x > s && r[x] == 'Q') {         /* "fffe Q981" */
		for (size_t i = x + 1; i < len; i++) if (!isdigit((unsigned char)r[i])) return len;
		size_t y = last_token(r, s, x - 1);

		if (y > s && counter_like(r, y, x - 1)) len = y - 1;
		else return len;
	} else if (x > s && counter_like(r, x, len)) {
		len = x - 1;
	}
	r[len] = 0;
	return len;
}

/* an RPC 0x19 from the hub: one or more records, the first maybe the tail of one before */
static void
hub_text(const char *t)
{
	size_t n = strlen(t), i = 0, from = 0;
	char rec[1100];

	while (i <= n) {
		size_t p = i < n ? rec_start(t, i) : 0;

		if (i == n || p > 0) {
			if (i > from) {
				size_t len = i - from, s = 0;

				if (len >= sizeof(rec)) len = sizeof(rec) - 1;
				memcpy(rec, t + from, len);
				rec[len] = 0;
				/* the level tag "$274239I " / "#288863D " / "&300274W " */
				if ((rec[0] == '$' || rec[0] == '#' || rec[0] == '&') && isdigit((unsigned char)rec[1])) {
					s = 1;
					while (isdigit((unsigned char)rec[s])) s++;
					if (isalpha((unsigned char)rec[s]) && rec[s + 1] == ' ') s += 2;
					else s = 0;
				}
				/* a prefix cut in two by the RPCs: "0795D $693323W ..." */
				if (s == 0) {
					char *tag = strstr(rec, "D $");

					if (tag && tag - rec < 12 && isdigit((unsigned char)tag[3])) {
						size_t x = (size_t)(tag - rec) + 3;

						while (isdigit((unsigned char)rec[x])) x++;
						if (isalpha((unsigned char)rec[x]) && rec[x + 1] == ' ') s = x + 2;
					}
				}
				while (len > s && (rec[len - 1] == ' ' || rec[len - 1] == '.')) rec[--len] = 0;
				/* ... or at the end: "... 42104 fffe Q981", "... @42 f" (the next record's counter) */
				len = trim_next_prefix(rec, s, len);
				if (len > s) hub_record(rec + s);
			}
			if (i == n) break;
			i += p;
			from = i;
			continue;
		}
		i++;
	}
}

static void
chip_text(const char *line, void *ctx)
{
	const char *p = line, *m;

	(void)ctx;
	if (strstr(line, "ONCHIP_NMEA[") == NULL) {
		hub_text(line);
		return;
	}
	/* "ONCHIP_NMEA[0][$GPGGA,...*hh" (the engine repeats the prefix now and then): the sentence after the last one */
	while ((m = strstr(p, "ONCHIP_NMEA[")) != NULL) p = m + 12;
	if (p == line) return;
	m = strchr(p, '$');
	if (m == NULL) return;
	char s[200];
	size_t k = 0;

	while (m[k] && m[k] != ' ' && k + 1 < sizeof(s)) { s[k] = m[k]; k++; }
	s[k] = 0;
	NSString *sentence = @(s);
	dispatch_async(q, ^{
		static struct nmea parser;
		static bool init;

		if (!init) { nmea_reset(&parser); init = true; }
		nmea_sentence(&parser, sentence.UTF8String, fix_ready);
	});
}

static void
chip_state(unsigned from, unsigned to, void *ctx)
{
	static const char *names[] = { "RESET", "SETUP", "OFF", "SLEEP", "IDLE", "ON", "POWER_SAVE", "TEST", "SHUTDOWN",
		"ERROR", "PASSTHROUGH" };

	(void)ctx;
	say("engine: %s -> %s\n", from < 11 ? names[from] : "?", to < 11 ? names[to] : "?");
	slog("engine: %s -> %s", from < 11 ? names[from] : "?", to < 11 ? names[to] : "?");
	pthread_mutex_lock(&diag_lock);
	snprintf(dg.engine, sizeof(dg.engine), "%s", to < 11 ? names[to] : "?");
	pthread_mutex_unlock(&diag_lock);
	if (to == GC_ERROR) post_state(@"error", @"the measurement engine reports ERROR");
}

static void
chip_assert(const char *file, unsigned line, void *ctx)
{
	(void)ctx;
	say("the chip asserted: %s line %u\n", file, line);
	slog("the chip asserted: %s line %u", file, line);
	post_state(@"error", [NSString stringWithFormat:@"chip assert %s:%u", file, line]);
}

static char chip_last_log[200];           /* what gnss_chip said last: the reason, when a step fails */

static void
chip_log(const char *line, void *ctx)
{
	(void)ctx;
	say("%s\n", line);
	slog("%s", line);
	snprintf(chip_last_log, sizeof(chip_last_log), "%s", strncmp(line, "gnss_chip: ", 11) == 0 ? line + 11 : line);
}

static void
chip_step(const char *name, void *ctx)
{
	(void)ctx;
	set_step(name, NULL);
}

static double
uptime_s(void)
{
	struct timeval bt;
	size_t len = sizeof(bt);

	if (sysctlbyname("kern.boottime", &bt, &len, NULL, 0) != 0) return 1e9;
	return time(NULL) - bt.tv_sec;
}

static bool
chip_wait_want(bool want, double seconds)
{
	double end = mono_s() + seconds;

	while (atomic_load(&chip_want) != want && mono_s() < end)
		if (gc_poll(0.2) != 0) usleep(200000);
	return atomic_load(&chip_want) == want;
}

/* the sequence of docs/gnss.md; true with the receiver searching */
static bool
chip_bringup(void)
{
	struct segment seg[8];
	struct gc_segment gs[8];
	int n = esw_segments(fw_dir, seg, 8);
	static const uint8_t pin_mux[2] = { 17, 1 }, pvt_start[1] = { 1 };
	static const unsigned facilities[] = { 0x0e, 0x16, 0x4f, 0x97 };
	const char *what = "start";
	int cut;

	chip_last_log[0] = 0;
	if (n <= 0) {
		set_step("ESW files", "no gnss-esw-N-ADDR.bin in the firmware directory");
		post_state(@"error", [NSString stringWithFormat:@"no ESW (gnss-esw-N-ADDR.bin) in %s", fw_dir]);
		return false;
	}
	for (int i = 0; i < n; i++) gs[i] = (struct gc_segment){ seg[i].path, seg[i].addr };
	/* the hub's log facilities upset its RPC channel early after boot (docs/gnss.md): not before 3 minutes */
	for (double up; (up = uptime_s()) < 180;) {
		char w[80];

		snprintf(w, sizeof(w), "waiting for the hub (%.0f s more)", 180 - up);
		pthread_mutex_lock(&diag_lock);
		snprintf(dg.step, sizeof(dg.step), "%s", w);
		pthread_mutex_unlock(&diag_lock);
		post_state(@"starting", @"waiting for the hub to settle (3 minutes after boot)");
		if (chip_wait_want(false, 5)) return false;
	}
	slog("the hub has been up 3 minutes: bring-up");
	post_state(@"starting", nil);
	set_step(what = "AR table cut", NULL);
	cut = gc_ar_table_cut();
	if (cut < 0) goto fail;
	pthread_mutex_lock(&diag_lock);
	dg.ar_cut = cut > 0;
	pthread_mutex_unlock(&diag_lock);
	set_step(what = "pin mux", NULL);
	if (gc_rpc(0x34, pin_mux, 2) != 0) goto fail;                         /* P17 = mode 1: the external LNA */
	set_step(what = "log to host", NULL);
	if (gc_pvt("enable_log_to_host_LOG_INFO_Overwrite") != 0) goto fail;
	if (!facilities_on) {
		set_step(what = "log facilities", NULL);
		for (unsigned i = 0; i < sizeof(facilities) / sizeof(facilities[0]); i++) {
			if (gc_log_facility(facilities[i]) != 0) goto fail;
			gc_poll(2);                                                    /* a few, 2 s apart */
		}
		facilities_on = true;
	}
	set_step(what = "GnssPvtDataStart", NULL);
	if (gc_rpc(0x1c, pvt_start, 1) != 0) goto fail;                       /* GnssPvtDataStart(1) */
	post_state(@"loading_esw", nil);
	set_step(what = "ESW load", NULL);
	if (gc_esw_load(gs, n) != 0) {
		set_step(what, chip_last_log[0] ? chip_last_log : "the ESW did not load");
		post_state(@"error", @"the ESW did not load");
		return false;
	}
	pthread_mutex_lock(&diag_lock);
	dg.esw_loaded = true;
	dg.esw_blocks = gc_esw_blocks();
	pthread_mutex_unlock(&diag_lock);
	if (gc_engine_on() != 0) {
		char s[80];

		pthread_mutex_lock(&diag_lock);
		snprintf(s, sizeof(s), "%s", dg.step);
		pthread_mutex_unlock(&diag_lock);
		set_step(s, chip_last_log[0] ? chip_last_log : "the engine did not come ON");
		post_state(@"error", @"the engine did not come ON");
		return false;
	}
	post_state(@"engine_on", nil);
	set_step(what = "disable_indoor", NULL);
	if (gc_pvt("disable_indoor") != 0) goto fail;                         /* or it gives up after 16 s indoors */
	gc_poll(1);
	set_step(what = "gnss_start", NULL);
	if (gc_pvt("gnss_start") != 0) goto fail;                             /* UpdateOnchipRequest: ASIC on */
	pthread_mutex_lock(&diag_lock);
	dg.search_since = mono_s();
	pthread_mutex_unlock(&diag_lock);
	set_step("searching", NULL);
	post_state(@"searching", nil);
	return true;
fail:
	set_step(what, chip_last_log[0] ? chip_last_log : "the hub did not acknowledge a request");
	post_state(@"error", @"the hub did not acknowledge a request");
	return false;
}

/*
 * The engine prints its search statistics (detections and attempts per constellation, the measured noise) only when
 * its measurement engine unloads. So while no satellite is in view, every `checkpoint_s`: gnss_stop, its table, then
 * disable_indoor and gnss_start again (as the open-sky phases of docs/gnss.md did). Nothing is lost: there is nothing
 * being tracked. With a satellite in view it is left alone.
 */
static bool
chip_checkpoint(void)
{
	set_step("statistics (gnss_stop)", NULL);
	if (gc_pvt("gnss_stop") != 0) return false;
	gc_poll(5);
	if (!atomic_load(&chip_want)) return true;
	set_step("disable_indoor", NULL);
	if (gc_pvt("disable_indoor") != 0) return false;
	gc_poll(1);
	set_step("gnss_start", NULL);
	if (gc_pvt("gnss_start") != 0) return false;
	set_step("searching", NULL);
	return true;
}

static void *
chip_thread(void *arg)
{
	struct gc_events ev = { .text = chip_text, .state = chip_state, .chip_assert = chip_assert, .log = chip_log,
		.step = chip_step };

	(void)arg;
	for (;;) {
		unsigned gen;

		pthread_mutex_lock(&chip_lock);
		while (!atomic_load(&chip_want)) pthread_cond_wait(&chip_cond, &chip_lock);
		pthread_mutex_unlock(&chip_lock);
		gen = atomic_load(&sess_gen);
		if (gc_open(&ev) != 0) {
			int e = errno;

			set_step("open /dev/hakios-gnss", strerror(e));
			post_state(@"error", [NSString stringWithFormat:@"/dev/hakios-gnss: %s", strerror(e)]);
			while (atomic_load(&chip_want)) sleep(1);
			session_close(gen, "the receiver was let go");
			continue;
		}
		if (chip_bringup()) {
			double next = mono_s() + checkpoint_s;

			while (atomic_load(&chip_want)) {
				if (gc_poll(0.5) != 0) {
					set_step("searching", "/dev/hakios-gnss failed");
					post_state(@"error", @"/dev/hakios-gnss failed");
					break;
				}
				if (checkpoint_s > 0 && mono_s() >= next) {
					if (atomic_load(&sats_in_view) == 0 && !chip_checkpoint()) {
						set_step("statistics", chip_last_log[0] ? chip_last_log : "the hub did not acknowledge");
						post_state(@"error", @"the hub did not acknowledge a request");
						break;
					}
					next = mono_s() + checkpoint_s;
				}
			}
			bool failed = atomic_load(&chip_want);

			if (!failed) set_step("gnss_stop", NULL);
			gc_pvt("gnss_stop");
			gc_poll(5);                                /* the engine's last statistics, for the session log */
			while (atomic_load(&chip_want))            /* after a failure: in error until the receiver is let go */
				if (gc_poll(0.5) != 0) usleep(500000);
		} else {
			while (atomic_load(&chip_want))            /* stay in error until the receiver is let go */
				if (gc_poll(0.5) != 0) usleep(500000);
		}
		gc_close();
		set_step("off", NULL);
		session_close(gen, "the receiver was let go");
		dispatch_async(q, ^{ if (!running) set_state(@"off", nil); });
	}
	return NULL;
}

/* ---- (b) a file, FIFO or stdin instead -------------------------------------------------------------------------- */

static int src_fd = -1;
static dispatch_source_t src_read, src_tick;
static struct nmea file_parser;
static char linebuf[1024];
static size_t linelen;
static FILE *replay;

static void
replay_tick(void)
{
	char l[256];
	bool gga = false;

	for (;;) {
		long at = ftell(replay);
		if (fgets(l, sizeof(l), replay) == NULL) { rewind(replay); break; }
		if (strlen(l) > 6 && strncmp(l + 3, "GGA,", 4) == 0) {
			if (gga) { fseek(replay, at, SEEK_SET); break; }
			gga = true;
		}
		nmea_sentence(&file_parser, l, fix_ready);
	}
	nmea_close_epoch(&file_parser, fix_ready);
}

static void
file_on(void)
{
	struct stat st;

	nmea_reset(&file_parser);
	linelen = 0;
	if (strcmp(nmea_path, "-") != 0 && stat(nmea_path, &st) == 0 && S_ISREG(st.st_mode)) {
		replay = fopen(nmea_path, "r");
		if (replay == NULL) { set_state(@"error", @(strerror(errno))); return; }
		src_tick = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
		dispatch_source_set_timer(src_tick, dispatch_time(DISPATCH_TIME_NOW, 0), NSEC_PER_SEC, NSEC_PER_SEC / 20);
		dispatch_source_set_event_handler(src_tick, ^{ replay_tick(); });
		dispatch_resume(src_tick);
		return;
	}
	src_fd = strcmp(nmea_path, "-") == 0 ? dup(0) : open(nmea_path, O_RDONLY | O_NONBLOCK);
	if (src_fd < 0) { set_state(@"error", @(strerror(errno))); return; }
	fcntl(src_fd, F_SETFL, fcntl(src_fd, F_GETFL) | O_NONBLOCK);
	src_read = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, src_fd, 0, q);
	dispatch_source_set_event_handler(src_read, ^{
		char b[512];
		ssize_t r = read(src_fd, b, sizeof(b));

		for (ssize_t i = 0; i < r; i++) {
			if (b[i] == '\n' || linelen == sizeof(linebuf) - 1) {
				linebuf[linelen] = 0;
				if (linelen) nmea_sentence(&file_parser, linebuf, fix_ready);
				linelen = 0;
			} else {
				linebuf[linelen++] = b[i];
			}
		}
		if (r == 0) dispatch_source_cancel(src_read);
	});
	dispatch_resume(src_read);
}

static void
file_off(void)
{
	if (src_tick) { dispatch_source_cancel(src_tick); src_tick = NULL; }
	if (replay) { fclose(replay); replay = NULL; }
	if (src_read) { dispatch_source_cancel(src_read); src_read = NULL; }
	if (src_fd >= 0) { close(src_fd); src_fd = -1; }
}

/* ---- (d) the receiver on and off ----------------------------------------------------------------------------- */

static void
receiver_on(void)
{
	if (running) return;
	running = true;
	running_since = mono_s();
	first_fix_at = 0;
	last_good = 0;
	have_last = false;
	diag_reset();
	{
		char who[64];

		snprintf(who, sizeof(who), "%s%s%s", loc_on ? "locationd" : "", loc_on && user_on ? ", " : "",
		    user_on ? "the app" : always_on ? "always" : "");
		session_open(who);
		say("receiver on (%s), session log %s\n", who, sess_path[0] ? sess_path : "none");
	}
	state = @"off";
	set_state(@"starting", nil);
	if (nmea_path) {
		file_on();
		set_state(@"searching", nil);
	} else {
		pthread_mutex_lock(&chip_lock);
		atomic_store(&chip_want, true);
		pthread_cond_signal(&chip_cond);
		pthread_mutex_unlock(&chip_lock);
	}
}

static void
receiver_off(void)
{
	if (!running) return;
	running = false;
	if (nmea_path) file_off();
	else atomic_store(&chip_want, false);
	feed_lost();
	have_last = false;
	set_state(@"off", nil);
	say("receiver off after %.0f s\n", mono_s() - running_since);
	slog("receiver off after %.0f s", mono_s() - running_since);
	if (nmea_path) session_close(0, "the receiver was let go");
}

/* ---- (e) the status, for the app --------------------------------------------------------------------------------- */

static const char sys_abbr[] = "GRECJS";      /* sys_names' letters */

/* every 10 s while the receiver runs: one line of numbers, the satellites if any (no position, ever) */
static void
session_stats(void)
{
	struct gnss_fix *f = have_last ? &last : NULL;
	char l[1200];
	int k;

	pthread_mutex_lock(&diag_lock);
	k = snprintf(l, sizeof(l), "stats: state %s, engine %s, step %s, search %.0f s, %d in view / %d used, fix %dD",
	    state.UTF8String, dg.engine[0] ? dg.engine : "-", dg.step[0] ? dg.step : "-",
	    dg.search_since > 0 ? mono_s() - dg.search_since : 0, f ? f->sats_seen : 0, f ? f->sats_used : 0,
	    f ? fix_type(f) : 0);
	if (f && fix_type(f) >= 2)
		k += snprintf(l + k, sizeof(l) - k, ", accuracy %.1f m (vertical %.1f), hdop %.1f", f->hacc, f->vacc, f->hdop);
	if (!isnan(dg.agc_gps) || !isnan(dg.agc_glo))
		k += snprintf(l + k, sizeof(l) - k, ", AGC GPS %.2f GLO %.2f dB", dg.agc_gps, dg.agc_glo);
	if (!isnan(dg.noise_gps)) k += snprintf(l + k, sizeof(l) - k, ", noise %.3f (table %.3f)", dg.noise_gps, dg.noise_exp);
	if (dg.tables > 0)
		k += snprintf(l + k, sizeof(l) - k, ", detections/attempts GPS %d/%d GLO %d/%d GAL %d/%d (%d tables)",
		    dg.det[CON_GPS], dg.att[CON_GPS], dg.det[CON_GLO], dg.att[CON_GLO], dg.det[CON_GAL], dg.att[CON_GAL],
		    dg.tables);
	pthread_mutex_unlock(&diag_lock);
	slog("%s", l);
	if (f && f->nsat > 0) {
		k = snprintf(l, sizeof(l), "sats:");
		for (int i = 0; i < f->nsat && k < (int)sizeof(l) - 40; i++) {
			const struct sat *s = &f->sats[i];

			k += snprintf(l + k, sizeof(l) - k, " %c%d %.0fdB", sys_abbr[s->sys], s->prn, s->cn0);
			if (s->elev >= 0 && s->azim >= 0) k += snprintf(l + k, sizeof(l) - k, " el%d az%d", s->elev, s->azim);
			if (s->used) k += snprintf(l + k, sizeof(l) - k, " used");
			k += snprintf(l + k, sizeof(l) - k, ";");
		}
		slog("%s", l);
	}
}

static void
diag_status(NSMutableDictionary *d)
{
	pthread_mutex_lock(&diag_lock);
	if (dg.engine[0]) d[@"engineState"] = @(dg.engine);
	if (dg.step[0]) d[@"step"] = @(dg.step);
	if (dg.step_error[0]) d[@"stepError"] = @(dg.step_error);
	if (dg.tables > 0) {
		NSMutableDictionary *det = [NSMutableDictionary dictionary], *att = [NSMutableDictionary dictionary];

		for (int c = 0; c < NCON; c++) {
			det[con_names[c]] = @(dg.det[c]);
			att[con_names[c]] = @(dg.att[c]);
		}
		d[@"detections"] = det;
		d[@"attempts"] = att;
		d[@"statsTables"] = @(dg.tables);
	}
	if (!isnan(dg.agc_gps)) d[@"agcGps"] = @((float)dg.agc_gps);
	if (!isnan(dg.agc_glo)) d[@"agcGlonass"] = @((float)dg.agc_glo);
	if (!isnan(dg.noise_gps)) d[@"noiseGps"] = @((float)dg.noise_gps);
	if (!isnan(dg.noise_exp)) d[@"noiseExpected"] = @((float)dg.noise_exp);
	if (!isnan(dg.osc_off_ppb)) d[@"oscOffsetPpb"] = @((float)dg.osc_off_ppb);
	if (!isnan(dg.osc_unc_ppb)) d[@"oscUncertaintyPpb"] = @((float)dg.osc_unc_ppb);
	if (dg.cntin[0]) d[@"cntin"] = @(dg.cntin);
	d[@"eswLoaded"] = @(dg.esw_loaded);
	if (dg.esw_blocks >= 0) d[@"eswBlocks"] = @(dg.esw_blocks);
	d[@"arTableCut"] = @(dg.ar_cut);
	if (dg.search_since > 0) d[@"searchSeconds"] = @((float)(mono_s() - dg.search_since));
	NSMutableArray *hub = [NSMutableArray array];
	for (int i = 0; i < dg.hub_n; i++) {
		NSString *s = @(dg.hub[(dg.hub_next + HUB_LINES - dg.hub_n + i) % HUB_LINES]);

		if (s) [hub addObject:s];
	}
	d[@"hubLog"] = hub;
	pthread_mutex_unlock(&diag_lock);
	pthread_mutex_lock(&sess_lock);
	if (sess_path[0]) d[@"sessionLog"] = @(sess_path);
	pthread_mutex_unlock(&sess_lock);
}

static void
write_status(void)
{
	@autoreleasepool {
		NSMutableDictionary *d = [NSMutableDictionary dictionary];
		struct gnss_fix *f = have_last ? &last : NULL;
		int type = f ? fix_type(f) : 0;

		d[@"state"] = state;
		if (last_error) d[@"error"] = last_error;
		d[@"fixType"] = @(type);
		d[@"satsInView"] = @(f ? f->sats_seen : 0);
		d[@"satsUsed"] = @(f ? f->sats_used : 0);
		NSMutableArray *sats = [NSMutableArray array];
		for (int i = 0; f && i < f->nsat; i++) {
			const struct sat *s = &f->sats[i];

			NSMutableDictionary *e = [@{ @"system": sys_names[s->sys], @"prn": @(s->prn), @"cn0": @((float)s->cn0),
			                            @"used": @(s->used) } mutableCopy];

			if (s->elev >= 0) e[@"elevation"] = @(s->elev);     /* left out until the engine knows it */
			if (s->azim >= 0) e[@"azimuth"] = @(s->azim);
			[sats addObject:e];
		}
		d[@"sats"] = sats;
		if (f) {
			d[@"hdop"] = @((float)f->hdop);
			d[@"vdop"] = @((float)f->vdop);
			d[@"pdop"] = @((float)f->pdop);
			d[@"hAccuracy"] = @((float)f->hacc);
			d[@"vAccuracy"] = @((float)f->vacc);
		}
		if (type >= 2) {
			d[@"latitude"] = @(f->lat);
			d[@"longitude"] = @(f->lon);
			d[@"altitude"] = @((float)f->alt_msl);
			d[@"speed"] = @((float)f->speed);
			d[@"course"] = @((float)f->course);
		}
		if (f && f->utc) {
			char u[32];
			struct tm tm;

			gmtime_r(&f->utc, &tm);
			strftime(u, sizeof(u), "%Y-%m-%dT%H:%M:%SZ", &tm);
			d[@"utc"] = @(u);
		}
		if (first_fix_at > 0) d[@"ttffSeconds"] = @((float)(first_fix_at - running_since));
		d[@"receiverOnSeconds"] = @((float)(running ? mono_s() - running_since : 0));
		diag_status(d);
		d[@"updated"] = [NSDate date];
		NSData *data = [NSPropertyListSerialization dataWithPropertyList:d format:NSPropertyListBinaryFormat_v1_0
		                                                        options:0 error:nil];
		/* atomically: a file of our own beside it, then renamed over it (no temporary names left behind if killed) */
		int fd = data ? open(STATUS_DIR "/.status.plist.new", O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
		if (fd >= 0) {
			bool ok = write(fd, data.bytes, data.length) == (ssize_t)data.length;

			fchmod(fd, 0644);
			close(fd);
			if (ok && rename(STATUS_DIR "/.status.plist.new", STATUS_FILE) == 0) notify_post("com.hakios.gnssd.status");
		}
	}
}

static void
status_start(void)
{
	static dispatch_source_t tick;
	int token;

	mkdir(STATUS_DIR, 0755);
	chown(STATUS_DIR, 501, 501);
	chmod(STATUS_DIR, 0755);
	tick = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
	dispatch_source_set_timer(tick, dispatch_time(DISPATCH_TIME_NOW, 0), NSEC_PER_SEC, NSEC_PER_SEC / 10);
	dispatch_source_set_event_handler(tick, ^{
		static unsigned n;

		write_status();
		if (running && ++n % 10 == 0) session_stats();
	});
	dispatch_resume(tick);
	notify_register_dispatch("com.hakios.gnssd.start", &token, q, ^(int t) {
		(void)t;
		say("the app: Start\n");
		user_on = true;
		demand_changed();
		write_status();         /* the app sees its Start taken at once */
	});
	notify_register_dispatch("com.hakios.gnssd.pause", &token, q, ^(int t) {
		(void)t;
		say("the app: Pause\n");
		user_on = false;
		demand_changed();
		write_status();
	});
}

/* ---- (c) locationd, the fallback: CLSimulationManager (-L sim) ---------------------------------------------------- */

@interface CLSimulationManager : NSObject
@property (assign) uint8_t locationDeliveryBehavior;
@property (assign) uint8_t locationRepeatBehavior;
- (void)clearSimulatedLocations;
- (void)appendSimulatedLocation:(CLLocation *)l;
- (void)flush;
- (void)startLocationSimulation;
- (void)stopLocationSimulation;
@end

static CLSimulationManager *sim;
static bool sim_active;

static void
sim_fix(const struct gnss_fix *f, double utc)
{
	@autoreleasepool {
		CLLocation *l = [[CLLocation alloc] initWithCoordinate:CLLocationCoordinate2DMake(f->lat, f->lon)
		                                              altitude:f->has_alt ? f->alt_msl : 0
		                                    horizontalAccuracy:f->hacc
		                                      verticalAccuracy:f->has_alt ? f->vacc : -1
		                                                course:f->course >= 0 ? f->course : -1
		                                        courseAccuracy:f->course >= 0 && f->speed > 1 ? 10 : -1
		                                                 speed:f->speed >= 0 ? f->speed : -1
		                                         speedAccuracy:f->speed >= 0 ? 0.5 + f->hdop * 0.2 : -1
		                                             timestamp:[NSDate dateWithTimeIntervalSince1970:utc]];
		if (sim == nil) sim = [NSClassFromString(@"CLSimulationManager") new];
		if (!sim_active) {
			[sim stopLocationSimulation];
			[sim clearSimulatedLocations];
			sim.locationDeliveryBehavior = 0;
			sim.locationRepeatBehavior = 1;
			[sim appendSimulatedLocation:l];
			[sim flush];
			[sim startLocationSimulation];
			sim_active = true;
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
}

/* ---- (c) locationd: the com.apple.gpsd service (-L gpsd) ------------------------------------------------------- */

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

/* the top-level varint fields of a Request: field -> value; the rest skipped */
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

/* proto.gpsd.Request.Type / Indication.Type, proto.gnss.Result and friends (GpsdProtocol.proto, GnssTypes.proto) */
enum { REQ_START = 1, REQ_STOP = 2, REQ_PING = 32, REQ_BUILD_DEVICE = 34, REQ_EXIT = 36 };
enum { IND_PVTM = 1, IND_STATUS = 2, IND_STOP_COMPLETED = 20, IND_START_COMPLETED = 22 };
enum { RESULT_SUCCESS = 1 };
enum { GNSS_CONTENT_2D = 4, GNSS_CONTENT_3D = 5, ESTIMATION_FILTERED = 3, RELIABILITY_HIGH = 75, RELIABILITY_MEDIUM = 50 };

static xpc_connection_t client;           /* locationd */
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
	if (client != NULL) {
		xpc_object_t d = xpc_dictionary_create(NULL, NULL, 0);

		xpc_dictionary_set_data(d, key, m->b, m->n);
		xpc_connection_send_message(client, d);
		xpc_release(d);
	}
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
	pb_u64(&s, 5, running);                                     /* is_device_active */
	pb_u64(&r, 1, IND_STATUS);
	pb_u64(&r, 3, continuous_ns());
	pb_msg(&r, 9, &s);
	send_to_client("gpsdIndication", &r);
}

/* one fix as Indication(PVTM), proto.gnss.Fix */
static void
gpsd_fix(const struct gnss_fix *f, double utc)
{
	struct pb ind = { 0 }, fix = { 0 }, t = { 0 }, pos = { 0 }, vel = { 0 };
	uint64_t mono = continuous_ns();

	if (!loc_on || client == NULL) return;
	/* Time: GPS time (from 6 Jan 1980; 18 leap seconds ahead of UTC since 2017), and when it was received */
	pb_u64(&t, 1, (uint64_t)((utc - 315964800.0 + 18) * 1e9));
	pb_float(&t, 2, 100.0f);
	pb_u64(&t, 3, 1);
	pb_u64(&t, 4, 18);
	pb_u64(&t, 5, mono);
	pb_double(&t, 6, mono / 1e9);
	pb_double(&t, 7, 0.001);
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
	pb_u64(&pos, 12, fix_type(f) == 2 ? GNSS_CONTENT_2D : GNSS_CONTENT_3D);
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
feed_fix(const struct gnss_fix *f, double utc)
{
	if (feed == FEED_SIM) sim_fix(f, utc);
	else gpsd_fix(f, utc);
}

static void
feed_lost(void)
{
	if (feed == FEED_SIM) sim_lost();
	/* gpsd: nothing to withdraw, locationd ages the last PVTM out itself */
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
	uint64_t v[4] = { 0 }, type;
	const char *name;

	if (!pb_read_varints(b, n, v, 3)) { say("locationd: a request that does not parse (%zu bytes)\n", n); return; }
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
		loc_on = true;
		demand_changed();
		send_indication_result(IND_START_COMPLETED, type, 22);
		break;
	case REQ_STOP:
		loc_on = false;
		demand_changed();
		send_indication_result(IND_STOP_COMPLETED, type, 20);
		break;
	case REQ_EXIT:
		say("locationd asks gnssd to exit\n");
		receiver_off();
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
		if (client) {
			loc_on = false;
			xpc_connection_cancel(client);
			xpc_release(client);
		}
		client = xpc_retain(peer);
		demand_changed();
		send_status();          /* gpsd says it is alive as soon as locationd connects (its "alive" timer) */
		xpc_connection_set_event_handler(peer, ^(xpc_object_t m) {
			if (xpc_get_type(m) == XPC_TYPE_DICTIONARY) {
				size_t len = 0;
				const void *req = xpc_dictionary_get_data(m, "gpsdRequest", &len);

				if (req) handle_request(req, len);
				/* the connection message (connection_ns) needs no answer */
			} else if (xpc_get_type(m) == XPC_TYPE_ERROR && peer == client) {
				say("locationd went away\n");
				loc_on = false;
				xpc_release(client);
				client = NULL;
				demand_changed();
			}
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(l);
	/* ... and then now and then: without a word from gpsd locationd takes it for dead and asks it to EXIT */
	static dispatch_source_t alive;
	alive = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
	dispatch_source_set_timer(alive, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), 5 * NSEC_PER_SEC, NSEC_PER_SEC);
	dispatch_source_set_event_handler(alive, ^{ if (client) send_status(); });
	dispatch_resume(alive);
}

/* ---- the command line -------------------------------------------------------------------------------------------- */

static void
print_epoch(const struct gnss_fix *f)
{
	printf("epoch: type %d, %d used / %d seen (%d listed), hdop %.1f, accuracy %.1f m (vertical %.1f), speed %.1f m/s, "
	       "course %.0f, utc %s\n", fix_type(f), f->sats_used, f->sats_seen, f->nsat, f->hdop, f->hacc, f->vacc,
	       f->speed, f->course, f->utc ? "yes" : "no");
}

int
main(int argc, char **argv)
{
	int c;
	pthread_t thread;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc > 1 && strcmp(argv[1], "parse") == 0) {
		FILE *in = argc > 2 ? fopen(argv[2], "r") : stdin;
		char l[256];
		static struct nmea n;

		if (in == NULL) { perror(argv[2]); return 1; }
		nmea_reset(&n);
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
	while ((c = getopt(argc, argv, "L:f:n:tai:c:")) != -1) {
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
		case 'c': checkpoint_s = atof(optarg); break;
		default:
		usage:
			fprintf(stderr, "usage: gnssd [-L gpsd|sim] [-f FWDIR] [-n NMEA] [-t] [-a] [-i MS] [-c S] | gnssd parse [FILE] |"
			    " gnssd segments [FWDIR]\n");
			return 2;
		}
	}
	(void)meif_pvt_decode;
	q = dispatch_get_main_queue();
	if (nmea_path == NULL) pthread_create(&thread, NULL, chip_thread, NULL);
	signal(SIGTERM, SIG_IGN);
	dispatch_source_t term = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, q);
	dispatch_source_set_event_handler(term, ^{
		receiver_off();
		write_status();
		/* give the chip's thread its gnss_stop and the engine's last statistics (5 s) */
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(6.5 * NSEC_PER_SEC)), q, ^{
			session_close(0, "gnssd stopped (SIGTERM)");
			exit(0);
		});
	});
	dispatch_resume(term);
	/* locationd's GNSS stack (the seed in var/mobile sets it on a fresh data volume; a kept one may predate it) */
	if (feed == FEED_GPSD) {
		CFPropertyListRef v = CFPreferencesCopyValue(CFSTR("UseNewGnssArchitecture"), CFSTR("com.apple.locationd"),
		    CFSTR("mobile"), kCFPreferencesAnyHost);
		if (v != kCFBooleanTrue) {
			CFPreferencesSetValue(CFSTR("UseNewGnssArchitecture"), kCFBooleanTrue, CFSTR("com.apple.locationd"),
			    CFSTR("mobile"), kCFPreferencesAnyHost);
			CFPreferencesSynchronize(CFSTR("com.apple.locationd"), CFSTR("mobile"), kCFPreferencesAnyHost);
			say("locationd's UseNewGnssArchitecture set: it takes gnssd's fixes from its next start\n");
		}
		if (v) CFRelease(v);
	}
	status_start();
	if (feed == FEED_GPSD) serve_gpsd();
	say("gnssd: %s, fixes to locationd %s\n", nmea_path ? nmea_path : "the BCM4775",
	    feed == FEED_GPSD ? "as its GPS (com.apple.gpsd)" : "through CLSimulationManager");
	demand_changed();
	dispatch_main();
	return 0;
}
