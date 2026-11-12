/*
 * gps_status_demo -- a FAKE gnssd status, for looking at HakiosGPS.app (userland/src/gps_app.m)
 * without a receiver that sees anything. Runs on the phone.
 *
 * It writes /var/mobile/Library/Caches/com.hakios.gnssd/status.plist once a second, the way
 * gnssd does (a temporary file, then rename), and posts com.hakios.gnssd.status after each
 * write, with demo = true in it, which makes the application show a big "DEMO DATA, not the
 * receiver" banner. The satellites are invented (nine, over all five constellations, drifting slowly so
 * that the plot moves) and the fix is at 0.0, 0.0 -- Null Island, in the Gulf of Guinea, so
 * nothing on the screen can be taken for a real position. It touches nothing else: not
 * locationd, not /dev/hakios-gnss, not gnssd.
 *
 * It refuses to start while the file is fresh and not its own (gnssd is writing it): the
 * two would overwrite each other and the screen would mix real and fake.
 *
 * It answers the two commands the application sends: com.hakios.gnssd.pause turns it "off",
 * com.hakios.gnssd.start walks starting -> loading_esw -> engine_on -> searching -> 2D -> 3D.
 *
 *   gps_status_demo [-s off|searching|fix2|fix|error] [-t SECONDS] [-k]
 *
 *     -s   the state to begin in (default fix: a 3D fix at once)
 *     -t   stop after SECONDS (default 120)
 *     -k   keep the file at exit (default: it is removed, and so is the directory if this
 *          program made it), e.g. to look at what the application does with a stale file
 *
 * Build (as userland/build.sh builds its programs):
 *   xnu-clang -arch arm64 -isysroot $THEOS/sdks/iPhoneOS16.5.sdk -O1 \
 *       -framework CoreFoundation -o gps_status_demo tools/gps_status_demo.c
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <notify.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DIR_PATH  "/var/mobile/Library/Caches/com.hakios.gnssd"
#define FILE_PATH DIR_PATH "/status.plist"
#define TMP_PATH  DIR_PATH "/.status.plist.demo"
#define MOBILE    501

static const struct { const char *sys; int prn, el, az; double cn0; } fake[] = {
	{ "GPS", 5, 62, 40, 44 },   { "GPS", 13, 35, 130, 38 },  { "GPS", 21, 18, 250, 27 },
	{ "GPS", 29, 8, 310, 17 },  { "GLONASS", 7, 48, 200, 36 }, { "GLONASS", 16, 22, 75, 24 },
	{ "Galileo", 11, 55, 290, 41 }, { "BeiDou", 23, 30, 165, 33 }, { "QZSS", 194, 12, 15, 21 },
};
#define NFAKE (sizeof(fake) / sizeof(fake[0]))

enum { S_OFF, S_STARTING, S_ESW, S_ENGINE, S_SEARCH, S_FIX2, S_FIX3, S_ERROR };
static int state = S_FIX3;
static double on_since, state_since, ttff = -1;
static int made_dir, keep;

static void
put(CFMutableDictionaryRef d, const char *k, CFTypeRef v)
{
	CFStringRef key = CFStringCreateWithCString(NULL, k, kCFStringEncodingUTF8);
	CFDictionarySetValue(d, key, v);
	CFRelease(key);
	CFRelease(v);
}
static CFTypeRef num_i(int v) { return CFNumberCreate(NULL, kCFNumberIntType, &v); }
static CFTypeRef num_d(double v) { return CFNumberCreate(NULL, kCFNumberDoubleType, &v); }
static CFTypeRef str(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }

static void
set_state(int s)
{
	double now = CFAbsoluteTimeGetCurrent();
	if (s == S_OFF) { on_since = 0; ttff = -1; }
	else if (state == S_OFF || on_since == 0) on_since = now;
	if (s >= S_FIX2 && s <= S_FIX3 && ttff < 0) ttff = on_since ? now - on_since : 0;
	state = s;
	state_since = now;
}

static void
write_status(void)
{
	double now = CFAbsoluteTimeGetCurrent();
	double in_state = now - state_since;
	/* the walk after Start */
	if (state == S_STARTING && in_state > 2) set_state(S_ESW);
	else if (state == S_ESW && in_state > 2) set_state(S_ENGINE);
	else if (state == S_ENGINE && in_state > 1) set_state(S_SEARCH);
	else if (state == S_SEARCH && in_state > 6) set_state(S_FIX2);
	else if (state == S_FIX2 && in_state > 4) set_state(S_FIX3);

	static const char *names[] = { "off", "starting", "loading_esw", "engine_on", "searching",
	    "fix", "fix", "error" };
	int fix = state == S_FIX3 ? 3 : state == S_FIX2 ? 2 : 0;
	int visible = state == S_SEARCH || state == S_FIX2 || state == S_FIX3;
	CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	put(d, "demo", CFRetain(kCFBooleanTrue));     /* the application's DEMO DATA banner */
	put(d, "state", str(names[state]));
	if (state == S_ERROR)
		put(d, "error", str("DEMO: esw load timed out"));
	put(d, "fixType", num_i(fix));

	CFMutableArrayRef sats = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	int used = 0, nview = 0;
	double t = now / 30.0;
	for (size_t i = 0; visible && i < NFAKE; i++) {
		/* searching: only the strongest few, weaker, none used */
		if (state == S_SEARCH && fake[i].cn0 < 30) continue;
		CFMutableDictionaryRef s = CFDictionaryCreateMutable(NULL, 0,
		    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		double cn0 = fake[i].cn0 + 2.5 * sin(t * 7 + i) - (state == S_SEARCH ? 6 : 0);
		int use = fix && cn0 >= 25 && !(fix == 2 && i % 2);
		put(s, "system", str(fake[i].sys));
		put(s, "prn", num_i(fake[i].prn));
		put(s, "cn0", num_d(round(cn0 * 10) / 10));
		put(s, "elevation", num_i(fake[i].el));
		put(s, "azimuth", num_i(((int)(fake[i].az + fmod(now / 20.0, 360))) % 360));
		put(s, "used", CFRetain(use ? kCFBooleanTrue : kCFBooleanFalse));
		CFArrayAppendValue(sats, s);
		CFRelease(s);
		used += use;
		nview++;
	}
	put(d, "sats", sats);
	put(d, "satsInView", num_i(nview));
	put(d, "satsUsed", num_i(used));
	if (fix) {
		put(d, "hdop", num_d(fix == 3 ? 0.9 : 2.1));
		put(d, "vdop", num_d(fix == 3 ? 1.3 : 9.9));
		put(d, "pdop", num_d(fix == 3 ? 1.6 : 10.1));
		put(d, "hAccuracy", num_d(fix == 3 ? 3.5 : 12.0));
		put(d, "vAccuracy", num_d(fix == 3 ? 5.0 : 50.0));
		put(d, "latitude", num_d(0.0));     /* SYNTHETIC: Null Island */
		put(d, "longitude", num_d(0.0));
		put(d, "altitude", num_d(0.0));
		put(d, "speed", num_d(0.0));
		put(d, "course", num_d(0.0));
	}
	if (state != S_OFF) {
		time_t tt = time(NULL);
		char buf[32];
		strftime(buf, sizeof buf, "%H:%M:%S", gmtime(&tt));
		put(d, "utc", str(buf));
		put(d, "receiverOnSeconds", num_d(on_since ? now - on_since : 0));
	} else {
		put(d, "receiverOnSeconds", num_d(0));
	}
	if (ttff >= 0)
		put(d, "ttffSeconds", num_d(round(ttff * 10) / 10));
	put(d, "updated", CFDateCreate(NULL, now));

	CFDataRef xml = CFPropertyListCreateData(NULL, d, kCFPropertyListXMLFormat_v1_0, 0, NULL);
	CFRelease(d);
	FILE *f = fopen(TMP_PATH, "wb");
	if (!f || fwrite(CFDataGetBytePtr(xml), 1, CFDataGetLength(xml), f) != (size_t)CFDataGetLength(xml)) {
		fprintf(stderr, "gps_status_demo: %s: %s\n", TMP_PATH, strerror(errno));
		exit(1);
	}
	fclose(f);
	CFRelease(xml);
	chmod(TMP_PATH, 0644);
	chown(TMP_PATH, MOBILE, MOBILE);
	if (rename(TMP_PATH, FILE_PATH) != 0) {
		fprintf(stderr, "gps_status_demo: rename: %s\n", strerror(errno));
		exit(1);
	}
	notify_post("com.hakios.gnssd.status");
	printf("gps_status_demo: %s, fix %d, %d/%d sats\n", names[state], fix, used, nview);
	fflush(stdout);
}

static void
finish(void)
{
	if (!keep) {
		unlink(FILE_PATH);
		if (made_dir)
			rmdir(DIR_PATH);
	}
	unlink(TMP_PATH);
	printf("gps_status_demo: done%s\n", keep ? " (file kept)" : ", file removed");
	exit(0);
}

int
main(int argc, char **argv)
{
	int c, secs = 120;
	while ((c = getopt(argc, argv, "s:t:k")) != -1) {
		if (c == 't') secs = atoi(optarg);
		else if (c == 'k') keep = 1;
		else if (c == 's') {
			if (!strcmp(optarg, "off")) state = S_OFF;
			else if (!strcmp(optarg, "searching")) state = S_SEARCH;
			else if (!strcmp(optarg, "fix2")) state = S_FIX2;
			else if (!strcmp(optarg, "fix")) state = S_FIX3;
			else if (!strcmp(optarg, "error")) state = S_ERROR;
			else goto usage;
		} else {
usage:
			fprintf(stderr, "usage: gps_status_demo [-s off|searching|fix2|fix|error] [-t secs] [-k]\n");
			return 2;
		}
	}
	{
		struct stat st;
		if (stat(FILE_PATH, &st) == 0 && time(NULL) - st.st_mtime < 10) {
			CFURLRef u = CFURLCreateFromFileSystemRepresentation(NULL,
			    (const UInt8 *)FILE_PATH, strlen(FILE_PATH), false);
			CFReadStreamRef rs = CFReadStreamCreateWithFile(NULL, u);
			CFPropertyListRef pl = NULL;
			if (rs && CFReadStreamOpen(rs))
				pl = CFPropertyListCreateWithStream(NULL, rs, 0, 0, NULL, NULL);
			CFTypeRef dv = pl && CFGetTypeID(pl) == CFDictionaryGetTypeID() ?
			    CFDictionaryGetValue(pl, CFSTR("demo")) : NULL;
			if (dv != kCFBooleanTrue) {
				fprintf(stderr, "gps_status_demo: %s was written %lds ago and is not a demo "
				    "file: gnssd is running, so this will not overwrite it\n", FILE_PATH,
				    (long)(time(NULL) - st.st_mtime));
				return 1;
			}
		}
	}
	if (mkdir(DIR_PATH, 0755) == 0) {
		made_dir = 1;
		chown(DIR_PATH, MOBILE, MOBILE);
	} else if (errno != EEXIST) {
		fprintf(stderr, "gps_status_demo: %s: %s\n", DIR_PATH, strerror(errno));
		return 1;
	}
	double now = CFAbsoluteTimeGetCurrent();
	on_since = state == S_OFF ? 0 : now - 47;    /* "on for 0:47" */
	state_since = now;
	if (state == S_FIX2 || state == S_FIX3) ttff = 31.4;

	int tok;
	dispatch_queue_t q = dispatch_get_main_queue();
	notify_register_dispatch("com.hakios.gnssd.start", &tok, q, ^(int t) {
		(void)t;
		printf("gps_status_demo: start received\n");
		if (state == S_OFF || state == S_ERROR) set_state(S_STARTING);
		write_status();
	});
	notify_register_dispatch("com.hakios.gnssd.pause", &tok, q, ^(int t) {
		(void)t;
		printf("gps_status_demo: pause received\n");
		set_state(S_OFF);
		write_status();
	});
	signal(SIGINT, SIG_IGN);
	signal(SIGTERM, SIG_IGN);
	signal(SIGHUP, SIG_IGN);
	int sigs[] = { SIGINT, SIGTERM, SIGHUP };
	for (int i = 0; i < 3; i++) {
		dispatch_source_t s = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, sigs[i], 0, q);
		dispatch_source_set_event_handler(s, ^{ finish(); });
		dispatch_resume(s);
	}
	dispatch_source_t tick = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
	dispatch_source_set_timer(tick, dispatch_time(DISPATCH_TIME_NOW, 0), NSEC_PER_SEC, NSEC_PER_SEC / 10);
	dispatch_source_set_event_handler(tick, ^{ write_status(); });
	dispatch_resume(tick);
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)secs * NSEC_PER_SEC), q, ^{ finish(); });
	dispatch_main();
}
