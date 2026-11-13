/*
 * Whether this process may be shown the Metal device, by the level of boot-arg metal (read from sysctl kern.bootargs):
 *
 *   metal=1  only the program metal_test (userland/src/metal_test.m): the safe level, for seeing what Metal does with the device
 *   metal=2  every process but backboardd
 *   metal=3  every process, backboardd too, to find out what the window server asks
 *
 * Why not simply everyone. CoreAnimation's window server tries Metal FIRST and goes to its OpenGL ES path (which works on this port)
 * only when no Metal device exists; with ours it takes Metal and stops on "metallib_url != nil": its own shader library,
 * QuartzCore.framework/default.metallib, is not in this system image (the framework has only a default.pipelinelib that names
 * it), so the window server cannot run on Metal here at all. And mediaserverd, given a device that cannot do anything, ends and
 * is started again. A device that is shown to a process that cannot use it costs that process, so it is shown to as few as the
 * experiment needs.
 *
 * (The Galaxy S9's image has QuartzCore's default.metallib now, sysimage/select-system.py KEEP_METALLIB; the window server stays on
 * OpenGL ES there by the choice of userland/src/inject.c, which takes the device out of backboardd unless boot-arg camtl=1.) The METAL=vk
 * build shows a device that opens the GPU only at its first real use (src/metalvk/HakiosMetalVK.mm), so a process that is shown it
 * and never uses it pays almost nothing.
 *
 * Besides the level, a file HakiosMetal.clients beside the bundle (the way UniXNU's AdrenoMetal chooses its clients) names more
 * processes, one per line, "*" for every one: an image can show the device to the one program being worked on without a boot-arg.
 *
 * (Moved here from src/HakiosMetal.m, unchanged, when the second build of the plug-in, src/metalvk/, came to need it too.)
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "hakios_metal_shown.h"

int
hakios_metal_beside_bundle(const char *name, char *path, unsigned long size)
{
	Dl_info info;
	char *slash;

	if (dladdr((const void *)&hakios_metal_beside_bundle, &info) == 0 || info.dli_fname == NULL) {
		return 0;
	}
	strlcpy(path, info.dli_fname, size);                    /* <Extensions>/HakiosMetal.bundle/HakiosMetal */
	if ((slash = strrchr(path, '/')) == NULL) {
		return 0;
	}
	*slash = '\0';
	if ((slash = strrchr(path, '/')) == NULL) {
		return 0;
	}
	strlcpy(slash + 1, name, size - (unsigned long)(slash + 1 - path));
	return access(path, R_OK) == 0;
}

static int
listedBesideTheBundle(const char *name)
{
	char path[1024], line[256];
	FILE *file;
	int listed = 0;

	if (!hakios_metal_beside_bundle("HakiosMetal.clients", path, sizeof(path)) || (file = fopen(path, "r")) == NULL) {
		return 0;
	}
	while (!listed && fgets(line, sizeof(line), file) != NULL) {
		line[strcspn(line, " \t\r\n#")] = '\0';
		listed = strcmp(line, "*") == 0 || (line[0] != '\0' && strcmp(line, name) == 0);
	}
	fclose(file);
	return listed;
}

int
hakios_metal_shown(void (*say)(const char *line))
{
	char args[1024] = "", line[256];
	size_t size = sizeof(args) - 1;
	const char *name = getprogname();
	int level = 1;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0) {
		/* the word metal=, not the end of another argument's name (ca_metal=, say) */
		for (const char *at = args; (at = strstr(at, "metal=")) != NULL; at += 6) {
			if (at == args || at[-1] == ' ') {
				level = atoi(at + 6);
				break;
			}
		}
	}
	if (name == NULL) {
		return 0;
	}
	if (level >= 3 || (level == 2 && strcmp(name, "backboardd") != 0) || (level == 1 && strcmp(name, "metal_test") == 0)) {
		return 1;
	}
	if (listedBesideTheBundle(name)) {
		snprintf(line, sizeof(line), "%s is shown the device: HakiosMetal.clients beside the bundle names it", name);
		if (say != NULL) {
			say(line);
		}
		return 1;
	}
	snprintf(line, sizeof(line), "%s is not shown the device (metal=%d; 1 is metal_test only, 2 all but backboardd, 3 all)", name, level);
	if (say != NULL) {
		say(line);
	}
	return 0;
}
