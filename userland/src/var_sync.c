/*
 * The kept data volume (HFS Plus on USERDATA, var_hfs=) to the flash every few seconds. The phone is never shut
 * down: it is reset (the keys, the console's return), and what was only in the buffer cache or the journal's
 * memory at that moment was gone -- Settings, the Home Screen, an app's data: "every reboot is a fresh system"
 * (and sbca.c's boot52-53 lost files the same way). sync() starts the writes of every dirty buffer; F_FULLFSYNC on
 * the volume then waits for HFS's journal and asks the UFS device to flush its own cache.
 *
 *   usage: var_sync [SECONDS]      every SECONDS (default 5, 1 to 600), until the run ends
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	long every = argc > 1 ? strtol(argv[1], NULL, 10) : 5;
	unsigned long rounds = 0, failed = 0;

	if (every < 1 || every > 600) {
		every = 5;
	}
	printf("var_sync: /private/var to the flash every %ld s\n", every);
	fflush(stdout);
	for (;;) {
		int fd;

		sleep((unsigned)every);
		sync();
		fd = open("/private/var", O_RDONLY);
		if (fd < 0 || fcntl(fd, F_FULLFSYNC) != 0) {
			failed++;
		}
		if (fd >= 0) {
			close(fd);
		}
		/* one line an hour, so the log says it is still at it */
		if (++rounds % (unsigned long)(3600 / every) == 0) {
			printf("var_sync: %lu flushes, %lu without F_FULLFSYNC\n", rounds, failed);
			fflush(stdout);
		}
	}
}
