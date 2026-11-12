/*
 * jb_seed: the package manager's prefix on a /private/var that is kept between boots.
 *
 * The prefix (tools/mk-jb.sh: the Procursus bootstrap with Sileo in it) rides read only in the system
 * image as /jb, or in the boot volume as /private/preboot/jb. What dpkg and apt install has to last, so on a
 * kept volume (HFS Plus, var_hfs=) it is copied once to /private/var/jb, whole and for real, and after that it
 * is the phone's own: a later system image does not touch it. On a /private/var in memory nothing is copied.
 * The installer's image then has no package manager at all, and no icon for one (JB_KEPT_ONLY=1 in
 * userland/build.sh); an image made for a session in RAM has the prefix grafted there by init (src/init.c), back at
 * every boot as the image has it, and what was installed is gone.
 *
 * Nothing of this is in the way of the boot. It is a launchd job (com.hakios.jbseed.plist), not init, starts late,
 * at low priority, and iOS does not need it for anything; launchd does not start it again. Without a prefix in the
 * image it ends at once. The copy is made under another name and renamed when it is whole, so a phone reset in
 * the middle costs that copy and no more: the next boot starts it over. Failing costs a line on the console.
 */
#include <dirent.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define PREFIX "/private/var/jb"
#define TEMP   "/private/var/.jb-seeding"
#define MARK   "/.seeded"                       /* the last thing written into the copy */
#define ROOM   (512ull * 1024 * 1024)           /* what the volume must have free: twice the prefix and more */
#define LATE   30                               /* seconds: SpringBoard first; a first boot has enough to do */

#define STAMP  "/.scion-stamp"                 /* which bootstrap the image carries (tools/mk-jb.sh's jb/.stamp) */

static char buffer[256 * 1024];
static int files;

static int
join(char *out, size_t size, const char *dir, const char *name)
{
	int n = snprintf(out, size, "%s/%s", dir, name);

	if (n < 0 || (size_t)n >= size) {
		printf("jb_seed: %s/%s: the path is too long\n", dir, name);
		return 0;
	}
	return 1;
}

/* PATH, gone, whatever it is */
static void
remove_tree(const char *path)
{
	struct stat st;

	if (lstat(path, &st) != 0) {
		return;
	}
	if ((st.st_mode & S_IFMT) == S_IFDIR) {
		DIR *dir = opendir(path);
		struct dirent *de;
		char sub[1024];

		while (dir != NULL && (de = readdir(dir)) != NULL) {
			if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0 &&
			    join(sub, sizeof(sub), path, de->d_name)) {
				remove_tree(sub);
			}
		}
		if (dir != NULL) {
			closedir(dir);
		}
		rmdir(path);
	} else {
		unlink(path);
	}
}

static int
copy_file(const char *from, const char *to, const struct stat *st)
{
	int in = open(from, O_RDONLY), out, ok = 1;
	ssize_t n = 0;

	if (in < 0) {
		return 0;
	}
	out = open(to, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (out < 0) {
		close(in);
		return 0;
	}
	while (ok && (n = read(in, buffer, sizeof(buffer))) > 0) {
		for (ssize_t done = 0; ok && done < n;) {
			ssize_t wrote = write(out, buffer + done, (size_t)(n - done));

			ok = wrote > 0;
			done += ok ? wrote : 0;
		}
	}
	/* owner first: chown takes set-user-ID off, and giveMeRoot and login live by it */
	ok = ok && n == 0 && fchown(out, st->st_uid, st->st_gid) == 0 && fchmod(out, st->st_mode & 07777) == 0;
	close(out);
	close(in);
	return ok;
}

static int
copy_tree(const char *from, const char *to)
{
	struct stat st, entry;
	DIR *dir;
	struct dirent *de;
	char f[1024], t[1024], link[1024];
	int ok = 1;

	if (lstat(from, &st) != 0 || mkdir(to, 0700) != 0 || (dir = opendir(from)) == NULL) {
		printf("jb_seed: %s: %s\n", from, strerror(errno));
		return 0;
	}
	while (ok && (de = readdir(dir)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
			continue;
		}
		ok = join(f, sizeof(f), from, de->d_name) && join(t, sizeof(t), to, de->d_name) && lstat(f, &entry) == 0;
		if (ok && (entry.st_mode & S_IFMT) == S_IFDIR) {
			ok = copy_tree(f, t);
			continue;
		}
		if (ok && (entry.st_mode & S_IFMT) == S_IFLNK) {
			ssize_t length = readlink(f, link, sizeof(link) - 1);

			link[length > 0 ? length : 0] = 0;
			ok = length > 0 && symlink(link, t) == 0 && lchown(t, entry.st_uid, entry.st_gid) == 0;
		} else if (ok) {
			ok = copy_file(f, t, &entry);
		}
		files++;
		if (!ok) {
			printf("jb_seed: %s: %s\n", f, strerror(errno));
		}
	}
	closedir(dir);
	/* last: the directory was 0700 while it was filled */
	return ok && chown(to, st.st_uid, st.st_gid) == 0 && chmod(to, st.st_mode & 07777) == 0;
}

/* The user's corner of the prefix: applications run as mobile and keep their data there (Sileo's database). */
static int
own_tree(const char *path)
{
	struct stat st;
	DIR *dir;
	struct dirent *de;
	char sub[1024];
	int ok;

	if (lstat(path, &st) != 0) {
		return 0;
	}
	ok = lchown(path, 501, 501) == 0;
	if ((st.st_mode & S_IFMT) == S_IFDIR && (dir = opendir(path)) != NULL) {
		while ((de = readdir(dir)) != NULL) {
			if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0) {
				ok = join(sub, sizeof(sub), path, de->d_name) && own_tree(sub) && ok;
			}
		}
		closedir(dir);
	}
	return ok;
}

static int
for_mobile(const char *root)
{
	/* as src/init.c does on a grafted prefix; sileolists is the index Sileo writes as mobile */
	static const char *const dirs[] = { "var/mobile", "var/mobile/Library", "var/mobile/Library/Caches",
		                            "var/mobile/Library/Preferences", "var/mobile/Documents",
		                            "var/lib/apt/sileolists", "var/lib/apt/sileolists/partial" };
	char path[1024];
	int ok = 1;

	for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		if (!join(path, sizeof(path), root, dirs[i])) {
			return 0;
		}
		mkdir(path, 0755);
		ok = chown(path, 501, 501) == 0 && ok;
	}
	return join(path, sizeof(path), root, "var/mobile") && own_tree(path) && ok;
}

static int
seed(const char *from)
{
	struct statfs fs;
	struct stat st;
	struct timeval t0, t1;
	char mark[256];
	int fd;

	if (statfs("/private/var", &fs) != 0 || (unsigned long long)fs.f_bavail * fs.f_bsize < ROOM) {
		printf("jb_seed: under %llu MB free on /private/var: no prefix\n", ROOM >> 20);
		return 0;
	}
	sleep(LATE);
	gettimeofday(&t0, NULL);
	remove_tree(TEMP);
	if (!copy_tree(from, TEMP) || !for_mobile(TEMP)) {
		remove_tree(TEMP);
		return 0;
	}
	snprintf(mark, sizeof(mark), "%s%s", TEMP, MARK);
	if ((fd = open(mark, O_WRONLY | O_CREAT, 0644)) >= 0) {
		close(fd);
	}
	sync();
	/* whatever had the name and no mark (a prefix grafted here by an earlier image, on the same volume) */
	if (lstat(PREFIX, &st) == 0) {
		remove_tree(PREFIX);
	}
	if (rename(TEMP, PREFIX) != 0) {
		printf("jb_seed: rename: %s\n", strerror(errno));
		remove_tree(TEMP);
		return 0;
	}
	sync();
	gettimeofday(&t1, NULL);
	printf("jb_seed: %s copied to %s: %d files in %ld s\n", from, PREFIX, files, (long)(t1.tv_sec - t0.tv_sec));
	return 1;
}

static void
run(char *const argv[])
{
	static char *const envp[] = { "PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/var/jb/sbin:/usr/bin:/bin:/usr/sbin:/sbin",
		                      "HOME=/var/root", NULL };
	pid_t pid = fork();
	int status = 0;

	if (pid == 0) {
		execve(argv[0], argv, envp);
		_exit(127);
	}
	if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		printf("jb_seed: %s: status %d\n", argv[0], status);
	}
}

/*
 * The applications from ../apps (userland/build.sh lists them in /Applications/.extra-apps, one name a line). They
 * are plain directories in /Applications like the port's own, but LaunchServices keeps its database on the kept
 * /private/var and does not walk /Applications again on a later image, so each is registered with the prefix's
 * uicache -- here and not in rc.jobs, which runs before this job has put uicache there on a fresh phone.
 */
static void
register_apps(void)
{
	FILE *list = fopen("/Applications/.extra-apps", "r");
	char name[256], path[320];
	struct stat st;

	if (list == NULL || stat("/var/jb/usr/bin/uicache", &st) != 0) {
		if (list != NULL) {
			fclose(list);
		}
		return;
	}
	while (fgets(name, sizeof(name), list) != NULL) {
		name[strcspn(name, "\r\n")] = '\0';
		if (name[0] == '\0' || strchr(name, '/') != NULL) {
			continue;
		}
		snprintf(path, sizeof(path), "/Applications/%s", name);
		if (stat(path, &st) == 0) {
			char *const argv[] = { "/var/jb/usr/bin/uicache", "-p", path, NULL };
			run(argv);
		}
	}
	fclose(list);
}

/*
 * A new prefix in the image (its stamp differs from the copy's, or the copy has none): the copy is moved aside,
 * whole, to /private/var/jb.old-SECONDS, and the image's is seeded in its place -- so that what a later bootstrap
 * adds (OpenSSH, ElleKit) reaches a phone seeded before it. Nothing is deleted; a failed seed puts the old one back.
 */
static bool
reseed_if_changed(const char *from)
{
	char want[160] = "", have[160] = "", path[256], aside[64];
	struct stat st;
	int fd;
	ssize_t n;

	snprintf(path, sizeof(path), "%s%s", from, STAMP);
	if ((fd = open(path, O_RDONLY)) < 0) {
		return true;                    /* an image without a stamp: the copy stays as it is */
	}
	n = read(fd, want, sizeof(want) - 1);
	close(fd);
	want[n > 0 ? n : 0] = 0;
	if ((fd = open(PREFIX STAMP, O_RDONLY)) >= 0) {
		n = read(fd, have, sizeof(have) - 1);
		close(fd);
		have[n > 0 ? n : 0] = 0;
	}
	if (stat(PREFIX MARK, &st) != 0 || strcmp(want, have) == 0) {
		return true;
	}
	snprintf(aside, sizeof(aside), "/private/var/jb.old-%ld", (long)time(NULL));
	printf("jb_seed: the image's prefix is another one (%.20s, the copy's %.20s): the copy to %s, the image's seeded\n",
	    want, have[0] ? have : "none", aside);
	if (rename(PREFIX, aside) != 0) {
		printf("jb_seed: %s: %s: the copy stays\n", aside, strerror(errno));
		return true;
	}
	if (!seed(from)) {
		rename(aside, PREFIX);
		printf("jb_seed: the new prefix not seeded: the old one back\n");
		return false;
	}
	return true;
}

int
main(void)
{
	struct statfs fs;
	struct stat st;
	const char *from = stat("/jb", &st) == 0 ? "/jb" : stat("/private/preboot/jb", &st) == 0 ? "/private/preboot/jb" : NULL;
	char *const pwd[] = { "/var/jb/usr/sbin/pwd_mkdb", "-p", "/var/jb/etc/master.passwd", NULL };
	char *const prep[] = { "/var/jb/bin/sh", "/var/jb/prep.sh", NULL };

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (from == NULL || statfs("/private/var", &fs) != 0) {
		return 0;
	}
	setpriority(PRIO_PROCESS, 0, 10);
	if (strcmp(fs.f_fstypename, "hfs") == 0) {
		(void)reseed_if_changed(from);
		if (stat(PREFIX MARK, &st) != 0 && !seed(from)) {
			printf("jb_seed: no prefix this boot; iOS goes on without it, and the next boot tries again\n");
			return 0;
		}
		/* its corner again: an application's own data, whoever wrote there last; $HOME/tmp is where Sileo's downloads go */
		mkdir("/private/var/mobile/tmp", 0755);
		chown("/private/var/mobile/tmp", 501, 501);
		if (stat(PREFIX "/etc/pwd.db", &st) != 0) {
			run(pwd);
		}
	}
	/* on a memory filesystem, if init has grafted it (and made the user database); if not, no */
	if (stat("/var/jb/prep.sh", &st) == 0) {
		run(prep);
	}
	register_apps();
	return 0;
}
