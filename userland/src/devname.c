/*
 * devname: give the system its name where Apple's lockdownd would.
 *
 * On an iPhone lockdownd keeps the device's name (Settings > General > About > Name, MobileGestalt's
 * UserAssignedDeviceName) and writes it into SystemConfiguration's preferences as the computer name and the local
 * host name. lockdownd is not run here (mkjobs.py's DENY: its activation chain wants a Secure Enclave), so the
 * preferences never got a "System" section, SCDynamicStoreCopyComputerName answered NULL, and bluetoothd advertised
 * the phone as "(null)"; Bonjour had no host name either.
 *
 * Run at every start (rc.jobs): when the computer name is missing or differs from the device's name, both are written
 * (the local host name made DNS-safe: letters, digits and '-'). A name changed in Settings is taken at the next start.
 * usage: devname [-n]   (-n: say what would be written, write nothing)
 */
#include <CoreFoundation/CoreFoundation.h>
/*
 * SystemConfiguration's own declarations mark SCPreferences and the computer-name calls unavailable on iOS; the
 * framework has them all the same (configd and Apple's daemons use them). Declared here, as it exports them.
 */
typedef const struct __SCPreferences *SCPreferencesRef;
typedef const struct __SCDynamicStore *SCDynamicStoreRef;
extern SCPreferencesRef SCPreferencesCreate(CFAllocatorRef, CFStringRef, CFStringRef);
extern Boolean SCPreferencesLock(SCPreferencesRef, Boolean);
extern Boolean SCPreferencesUnlock(SCPreferencesRef);
extern Boolean SCPreferencesCommitChanges(SCPreferencesRef);
extern Boolean SCPreferencesApplyChanges(SCPreferencesRef);
extern Boolean SCPreferencesSetComputerName(SCPreferencesRef, CFStringRef, CFStringEncoding);
extern Boolean SCPreferencesSetLocalHostName(SCPreferencesRef, CFStringRef);
extern CFStringRef SCDynamicStoreCopyComputerName(SCDynamicStoreRef, CFStringEncoding *);
extern int SCError(void);
extern const char *SCErrorString(int);
#include <stdio.h>
#include <string.h>

extern CFTypeRef MGCopyAnswer(CFStringRef question, CFDictionaryRef options);

static void
say(const char *what, CFStringRef s)
{
	char buf[256] = "(none)";

	if (s != NULL) {
		CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8);
	}
	printf("devname: %s %s\n", what, buf);
}

int
main(int argc, char **argv)
{
	int dry = argc > 1 && strcmp(argv[1], "-n") == 0;
	CFTypeRef answer = MGCopyAnswer(CFSTR("UserAssignedDeviceName"), NULL);
	CFStringRef name = answer != NULL && CFGetTypeID(answer) == CFStringGetTypeID() && CFStringGetLength(answer) > 0 ?
	    (CFStringRef) answer : CFSTR("iPhone");
	SCPreferencesRef prefs = SCPreferencesCreate(NULL, CFSTR("devname"), NULL);
	CFStringRef now;
	CFStringEncoding encoding;
	char local[64];
	size_t n = 0;
	char utf8[256];

	if (prefs == NULL) {
		printf("devname: cannot open SystemConfiguration's preferences\n");
		return 1;
	}
	now = SCDynamicStoreCopyComputerName(NULL, &encoding);
	say("the device's name:", name);
	say("the computer name now:", now);
	if (now != NULL && CFStringCompare(now, name, 0) == kCFCompareEqualTo) {
		return 0;
	}
	/* the local host name: what Bonjour publishes as NAME.local */
	CFStringGetCString(name, utf8, sizeof(utf8), kCFStringEncodingUTF8);
	for (const char *p = utf8; *p != '\0' && n < sizeof(local) - 1; p++) {
		char c = *p;

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
			local[n++] = c;
		} else if (n > 0 && local[n - 1] != '-') {
			local[n++] = '-';
		}
	}
	while (n > 0 && local[n - 1] == '-') {
		n--;
	}
	local[n] = '\0';
	if (n == 0) {
		strcpy(local, "iPhone");
	}
	printf("devname: %s the computer name and the local host name %s\n", dry ? "would write" : "writing", local);
	if (dry) {
		return 0;
	}
	CFStringRef host = CFStringCreateWithCString(NULL, local, kCFStringEncodingUTF8);
	if (!SCPreferencesLock(prefs, true) || !SCPreferencesSetComputerName(prefs, name, kCFStringEncodingUTF8) ||
	    !SCPreferencesSetLocalHostName(prefs, host) || !SCPreferencesCommitChanges(prefs) || !SCPreferencesApplyChanges(prefs)) {
		printf("devname: could not write them: %s\n", SCErrorString(SCError()));
		SCPreferencesUnlock(prefs);
		return 1;
	}
	SCPreferencesUnlock(prefs);
	say("the computer name is now", name);
	return 0;
}
