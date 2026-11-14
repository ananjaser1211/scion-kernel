/*
 * Whether this process may be shown the Metal device, by the level of boot-arg metal (read from sysctl kern.bootargs):
 *
 *   metal=1  only the program metal_test (userland/src/metal_test.m): the safe level, for seeing what Metal does with the device
 *   metal=2  applications and app extensions: a process whose executable is in a .app or .appex bundle (SpringBoard, PosterBoard
 *            and its wallpaper extensions, Maps, the user's programs), and no daemon
 *   metal=3  every process but backboardd (metal=2 until 14 November 2026), which has its rule below
 *   metal=4  every process, backboardd too, to find out what the window server asks (metal=3 until then)
 *
 * The window server, backboardd. At metal=2 or 3 it is shown the device by default when the device is Metal on PanVK (the METAL=vk
 * build whose HakiosMetal.icd names libvulkan_panfrost, hakios_metal_vk_driver()): there CoreAnimation draws with Metal, and the
 * plug-in opens the device as soon as the window server makes it, so that a device that cannot be opened is no device and
 * CoreAnimation takes its OpenGL ES path (src/metalvk/HakiosMetalVK.mm). Boot-arg camtl=0, or a line "-backboardd" in either
 * clients file, keeps the window server on OpenGL ES; the first needs a boot, the second only the window server's next start.
 * With any other driver or build it is shown the device only at metal=4 or when a clients file names it, as before.
 * userland/src/inject.c agrees: it takes the device out of backboardd again with camtl=0, and also with no camtl= when the device
 * is not one the METAL=vk plug-in opened for the window server; camtl=1 leaves any device there.
 *
 * Why not simply everyone. CoreAnimation's window server tries Metal FIRST and goes to its OpenGL ES path (which works on this port)
 * only when no Metal device exists; with ours it takes Metal and stops on "metallib_url != nil": its own shader library,
 * QuartzCore.framework/default.metallib, is not in this system image (the framework has only a default.pipelinelib that names
 * it), so the window server cannot run on Metal here at all. And mediaserverd, given a device that cannot do anything, ends and
 * is started again. A device that is shown to a process that cannot use it costs that process, so it is shown to as few as the
 * experiment needs.
 *
 * (The Galaxy S9's image has QuartzCore's default.metallib now, sysimage/select-system.py KEEP_METALLIB, and the window server
 * draws with Metal there by the rule above.) The METAL=vk
 * build shows a device that opens the GPU only at its first real use (src/metalvk/HakiosMetalVK.mm), so a process that is shown it
 * and never uses it pays almost nothing.
 *
 * Besides the level, a file HakiosMetal.clients beside the bundle (the way UniXNU's AdrenoMetal chooses its clients) names more
 * processes, one per line, "*" for every one: an image can show the device to the one program being worked on without a boot-arg.
 * A line "-NAME" keeps it from that process whatever the level says. The image's file is read-only on a phone, so the same lines
 * are read from /private/var/db/HakiosMetal.clients too (written by hand, kept across boots): a client is added or taken out
 * without a new image, from its next start.
 *
 * (Moved here from src/HakiosMetal.m, unchanged, when the second build of the plug-in, src/metalvk/, came to need it too.)
 */
#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
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

#define DEFAULT_VK_ICD  "/redwood/lib/libhakios.vk.dylib"

void
hakios_metal_vk_driver(char *path, unsigned long size, void (*say)(const char *line))
{
	const char *environment = getenv("HAKIOS_VK_ICD");
	char file[1024], line[1200];
	FILE *f;

	if (environment != NULL && environment[0] != '\0') {
		strlcpy(path, environment, size);
		return;
	}
	if (hakios_metal_beside_bundle("HakiosMetal.icd", file, sizeof(file)) && (f = fopen(file, "r")) != NULL) {
		int named = fgets(path, (int)size, f) != NULL;

		fclose(f);
		path[strcspn(path, " \t\r\n#")] = '\0';
		if (named && path[0] == '/') {
			return;
		}
		if (say != NULL) {
			snprintf(line, sizeof(line), "%s names no driver (an absolute path on its first line): the default", file);
			say(line);
		}
	}
	strlcpy(path, DEFAULT_VK_ICD, size);
}

int
hakios_metal_vk_driver_is_panvk(const char *path)
{
	const char *base = strrchr(path, '/');

	return strncmp(base != NULL ? base + 1 : path, "libvulkan_panfrost", 18) == 0;
}

int
hakios_metal_is_window_server(void)
{
	const char *name = getprogname();

	return name != NULL && strcmp(name, "backboardd") == 0;
}

/* boot-arg camtl: 0 or 1 as given, -1 if it is not on the line */
static int
camtl(const char *args)
{
	const char *at = strstr(args, "camtl=");

	while (at != NULL && at != args && at[-1] != ' ') {
		at = strstr(at + 6, "camtl=");
	}
	return at == NULL ? -1 : atoi(at + 6) != 0;
}

#define CLIENTS_KEPT   "/private/var/db/HakiosMetal.clients"

/* what the clients file at PATH says of NAME: 1 shown ("NAME" or "*"), -1 kept from it ("-NAME"), 0 nothing */
static int
listedIn(const char *path, const char *name)
{
	char line[256];
	FILE *file;
	int listed = 0;

	if ((file = fopen(path, "r")) == NULL) {
		return 0;
	}
	while (listed >= 0 && fgets(line, sizeof(line), file) != NULL) {
		line[strcspn(line, " \t\r\n#")] = '\0';
		if (line[0] == '-' && strcmp(line + 1, name) == 0) {
			listed = -1;
		} else if (strcmp(line, "*") == 0 || (line[0] != '\0' && strcmp(line, name) == 0)) {
			listed = 1;
		}
	}
	fclose(file);
	return listed;
}

/* the same of both files, the one beside the bundle and the kept one; a "-NAME" in either wins */
static int
listed(const char *name, const char **where)
{
	char path[1024];
	int image = hakios_metal_beside_bundle("HakiosMetal.clients", path, sizeof(path)) ? listedIn(path, name) : 0;
	int kept = listedIn(CLIENTS_KEPT, name);

	*where = kept != 0 && (kept < 0 || image >= 0) ? CLIENTS_KEPT : "HakiosMetal.clients beside the bundle";
	return image < 0 || kept < 0 ? -1 : image > 0 || kept > 0 ? 1 : 0;
}

/* whether this process is an application or an app extension: its executable is in a .app or .appex bundle */
static int
isApplication(void)
{
	char path[PATH_MAX];
	uint32_t size = sizeof(path);

	return _NSGetExecutablePath(path, &size) == 0 && (strstr(path, ".app/") != NULL || strstr(path, ".appex/") != NULL);
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
	const char *where;
	int named = listed(name, &where);
	if (named < 0) {
		snprintf(line, sizeof(line), "%s is not shown the device: %s says -%s", name, where, name);
		if (say != NULL) {
			say(line);
		}
		return 0;
	}
	int windowServer = hakios_metal_is_window_server();
	if (windowServer && camtl(args) == 0) {
		if (say != NULL) {
			say("backboardd is not shown the device: camtl=0 keeps the window server on OpenGL ES");
		}
		return 0;
	}
	if (level >= 4 || (level == 3 && !windowServer) || (level == 2 && isApplication()) ||
	    (level == 1 && strcmp(name, "metal_test") == 0)) {
		return 1;
	}
	if (windowServer && level >= 2 && named == 0) {
		char driver[1024];

		hakios_metal_vk_driver(driver, sizeof(driver), say);
		if (hakios_metal_vk_driver_is_panvk(driver)) {
			snprintf(line, sizeof(line), "backboardd is shown the device: the window server draws with Metal on %s by default "
			    "(camtl=0, or -backboardd in %s, keeps it on OpenGL ES)", driver, CLIENTS_KEPT);
			if (say != NULL) {
				say(line);
			}
			return 1;
		}
	}
	if (named > 0) {
		snprintf(line, sizeof(line), "%s is shown the device: %s names it", name, where);
		if (say != NULL) {
			say(line);
		}
		return 1;
	}
	snprintf(line, sizeof(line), "%s is not shown the device (metal=%d; 1 is metal_test only, 2 applications and app extensions, "
	    "3 all but backboardd, 4 all)", name, level);
	if (say != NULL) {
		say(line);
	}
	return 0;
}
