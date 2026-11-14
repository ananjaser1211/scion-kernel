/*
 * The libdrm of xf86drm.h (see there): Linux's DRM requests to the kernel's GPU driver as calls
 * of its IOKit user client. A request's number (the low byte of the ioctl code) is the call's
 * selector, its structure goes in and comes back as it is; a buffer's memory is the client
 * memory of the type that is the buffer's handle.
 *
 * Two of the kernel's services speak so, one for each GPU; a phone has one of them:
 *   HakiosGPU    the POCO X5 Pro's Adreno, as Linux's msm driver, for Mesa's freedreno
 *                (drivers/gpu/hakios.gpu.cpp)
 *   HakiosMali   the Galaxy S9's Mali-G72, as Linux's panfrost driver, for Mesa's panfrost
 *                (drivers/gpu/mali/hakios.mali.cpp)
 * drmOpenWithType() opens the one asked for by the name of its Linux driver, and drmGetVersion()
 * answers with that name: it is what Mesa's panfrost picks its backend by (pan_kmod.c). Every
 * request of freedreno's goes to the kernel as it is. Of panfrost's, two are seen to here first,
 * because HakiosMali does not do them as Linux's driver does (submit(), wait_buffer()), and its
 * fences are made here, of the kernel's job numbers (the sync objects at the end).
 *
 * The driver knows its device by a file descriptor. It gets one that means nothing (/dev/null):
 * there is one GPU and one connection to it in a process.
 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <IOKit/IOKitLib.h>

#define HAKIOS_DRM_DEVICES 1           /* (xf86drm.h: the device list is this file's to give) */
#include "xf86drm.h"
#include "drm-uapi/msm_drm.h"
#include "drm-uapi/panfrost_drm.h"

#define HAKIOS_GPU_USER_MEMORY 0x7d            /* the Adreno service's: pages of this process as a buffer */
#define HAKIOS_MALI_USER_MEMORY 0x7b           /* HakiosMali's, the same request (its 0x7d is a wait) */
#define HAKIOS_GPU_SURFACE     0x7e            /* the request that is not Linux's (both services) */
#define HAKIOS_GPU_DESCRIBE    0xffffffffu     /* ... asked only what the buffer would be like */
#define HAKIOS_MALI_WAIT       0x7d            /* HakiosMali's own: wait for a job by its number */
#define HAKIOS_MALI_SYNC       0x7a            /* HakiosMali's own: a range of a buffer's lines cleaned and invalidated */

/* The kernel's services, by the names of the Linux drivers they stand in for. */
static const struct service {
	const char *name;               /* Linux's: what the driver opens, and what drmGetVersion() says */
	const char *service;            /* the kernel's */
	int major, minor;               /* the version of Linux's interface that it has */
	const char *desc;
} services[] = {
	/* 1.7: buffers at addresses the kernel chooses and tells ("softpin"), nothing newer */
	{ "msm", "HakiosGPU", 1, 7, "Adreno on Darwin" },
	/*
	 * 1.2: buffers that grow on a fault and buffers that are not run (1.1; the kernel takes the
	 * flags and makes every buffer whole), and the AFBC features (1.2; the kernel answers 0, which
	 * Mesa takes as AFBC supported: see hakios_mali_parameter()).
	 * Not 1.3: Mesa would then ask for the frequency of the GPU's timestamps, as a parameter it
	 * must have, and the kernel does not answer that one.
	 */
	{ "panfrost", "HakiosMali", 1, 2, "Mali on Darwin" },
};
#define PANFROST        (&services[1])

static const struct service *opened;
static io_connect_t connection;
static int connection_fd = -1;

static int noop_call(unsigned number, void *data, size_t size);
static void *noop_map(uint32_t handle);

/* HAKIOS_DRM_NOOP=1: no kernel, a made-up Mali-G72 in this process (xf86drm.h; the end of this file) */
int
hakios_drm_noop(void)
{
	static int noop = -1;

	if (noop < 0) {
		const char *v = getenv("HAKIOS_DRM_NOOP");
		noop = v != NULL && v[0] != '\0' && strcmp(v, "0") != 0;
	}
	return noop;
}

/* The connection to the service of the driver asked for, made once in a process: 0, or -1. */
static int
open_service(const struct service *wanted)
{
	io_service_t service;

	if (wanted == NULL || (opened != NULL && opened != wanted)) {
		return -1;
	}
	if (connection_fd >= 0) {
		return 0;
	}
	if (hakios_drm_noop()) {
		if (wanted != PANFROST) {
			return -1;
		}
	} else {
		service = IOServiceGetMatchingService(0, IOServiceMatching(wanted->service));
		if (service == 0) {
			return -1;
		}
		if (IOServiceOpen(service, mach_task_self(), 0, &connection) != 0) {
			IOObjectRelease(service);
			return -1;
		}
		IOObjectRelease(service);
	}
	opened = wanted;
	connection_fd = open("/dev/null", O_RDWR);
	return 0;
}

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	const struct service *wanted = NULL;

	(void)busid;
	(void)type;
	for (size_t i = 0; i < sizeof(services) / sizeof(services[0]); i++) {
		if (strcmp(name, services[i].name) == 0) {
			wanted = &services[i];
		}
	}
	if (open_service(wanted) != 0) {
		return -1;
	}
	return dup(connection_fd);
}

static int
call(unsigned number, void *data, size_t size)
{
	size_t returned = size;
	kern_return_t result;

	if (hakios_drm_noop()) {
		return noop_call(number, data, size);
	}
	result = IOConnectCallStructMethod(connection, number, data, size, data, &returned);

	switch (result) {
	case 0:
		return 0;
	case kIOReturnNoMemory:
		return ENOMEM;
	case kIOReturnBadArgument:
		return EINVAL;
	case kIOReturnTimeout:
		return ETIMEDOUT;
	case kIOReturnBusy:
		return EBUSY;
	case kIOReturnNotFound:
		return ENOENT;
	case kIOReturnUnsupported:
		return ENOSYS;
	default:
		return EIO;
	}
}

static int submit(struct drm_panfrost_submit *request);
static void forget_mapping(uint32_t handle);
static int wait_buffer(struct drm_panfrost_wait_bo *request);
static bool pan_syncobj_ioctl(unsigned number, void *arg, int *error);

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	unsigned number = (unsigned)(request & 0xff);
	int error;

	(void)fd;
	if (opened == PANFROST && number == DRM_COMMAND_BASE + DRM_PANFROST_SUBMIT) {
		error = submit(arg);
	} else if (opened == PANFROST && number == DRM_COMMAND_BASE + DRM_PANFROST_WAIT_BO) {
		error = wait_buffer(arg);
	} else if (opened == PANFROST && pan_syncobj_ioctl(number, arg, &error)) {
		/* (done there) */
	} else {
		if (number == (DRM_IOCTL_GEM_CLOSE & 0xff)) {
			forget_mapping(((struct drm_gem_close *)arg)->handle);
		}
		error = call(number, arg, IOCPARM_LEN(request));
	}
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), (void *)(uintptr_t)data, size);
}

static int submit_with_syncobjs(struct drm_msm_gem_submit *submit, unsigned long size);

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	if (opened != PANFROST && index == DRM_MSM_GEM_SUBMIT && size >= sizeof(struct drm_msm_gem_submit)) {
		return submit_with_syncobjs(data, size);
	}
	return -call((unsigned)(DRM_COMMAND_BASE + index), data, size);
}

/* The driver calls itself what Linux's is called, and is of the version whose interface it has. */
drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr version;

	(void)fd;
	if (opened == NULL) {
		errno = ENODEV;
		return NULL;
	}
	version = calloc(1, sizeof(*version));
	if (version != NULL) {
		version->version_major = opened->major;
		version->version_minor = opened->minor;
		version->name = strdup(opened->name);
		version->name_len = (int)strlen(opened->name);
		version->date = strdup("");
		version->desc = strdup(opened->desc);
		version->desc_len = (int)strlen(opened->desc);
	}
	return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (version != NULL) {
		free(version->name);
		free(version->date);
		free(version->desc);
		free(version);
	}
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	*value = 0;
	return 0;
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct drm_gem_close request = { .handle = handle };

	return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &request);
}

/*
 * A buffer's mapping is one per process here: IOKit gives a task that maps the same memory again the
 * mapping it already has, where Linux's mmap() makes a new one each time. So a buffer mapped twice
 * (PanVK maps a VkBuffer that covers its whole VkDeviceMemory as well as the memory itself) is one
 * mapping with two users, counted here, and it goes when the last of them unmaps it. Unmapping it at
 * the first left the second user's unmap over a hole: a VM guard exception (kGUARD_EXC_DEALLOC_GAP),
 * fatal in applications and app extensions, on every device PanVK opened (docs/metal.md, build 45).
 */
static struct mapped {
	uint32_t handle, users;
	mach_vm_address_t address;
} *maps;
static uint32_t map_count;
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;

/* (with map_lock held) the entry of HANDLE, or NULL */
static struct mapped *
mapped_of(uint32_t handle)
{
	for (uint32_t i = 0; i < map_count; i++) {
		if (maps[i].users != 0 && maps[i].handle == handle) {
			return &maps[i];
		}
	}
	return NULL;
}

/* a buffer closed (GEM_CLOSE): its mapping goes with its memory, and its handle may come back for another */
static void
forget_mapping(uint32_t handle)
{
	struct mapped *m;

	pthread_mutex_lock(&map_lock);
	if ((m = mapped_of(handle)) != NULL) {
		m->users = 0;
	}
	pthread_mutex_unlock(&map_lock);
}

void *
hakios_drm_map(int fd, uint32_t handle, uint64_t size)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;
	struct mapped *m;

	(void)fd;
	(void)size;
	if (hakios_drm_noop()) {
		void *memory = noop_map(handle);
		return memory != NULL ? memory : (void *)-1;
	}
	pthread_mutex_lock(&map_lock);
	if ((m = mapped_of(handle)) != NULL) {
		m->users++;
		pthread_mutex_unlock(&map_lock);
		return (void *)(uintptr_t)m->address;
	}
	/*
	 * Linux gives freedreno and panfrost their buffers write-combined, with no cache of this
	 * processor's between the program and the memory the GPU sees. So do the kernel's drivers
	 * here: the memory is made that way, and this mapping is as the memory is.
	 */
	if (IOConnectMapMemory64(connection, handle, mach_task_self(), &address, &mapped, kIOMapAnywhere) != 0) {
		pthread_mutex_unlock(&map_lock);
		return (void *)-1;              /* mmap()'s MAP_FAILED */
	}
	for (m = maps; m != NULL && m < maps + map_count && m->users != 0; m++) {
	}
	if (m == NULL || m == maps + map_count) {
		struct mapped *grown = realloc(maps, (map_count + 64) * sizeof(*grown));

		if (grown == NULL) {
			IOConnectUnmapMemory64(connection, handle, mach_task_self(), address);
			pthread_mutex_unlock(&map_lock);
			return (void *)-1;
		}
		memset(grown + map_count, 0, 64 * sizeof(*grown));
		m = grown + map_count;
		maps = grown;
		map_count += 64;
	}
	*m = (struct mapped){ handle, 1, address };
	pthread_mutex_unlock(&map_lock);
	return (void *)(uintptr_t)address;
}

void
hakios_drm_unmap(int fd, uint32_t handle, void *address)
{
	struct mapped *m;

	(void)fd;
	if (hakios_drm_noop()) {
		return;                 /* (the memory goes with the buffer: GEM_CLOSE) */
	}
	pthread_mutex_lock(&map_lock);
	if ((m = mapped_of(handle)) != NULL) {
		if (--m->users != 0) {
			pthread_mutex_unlock(&map_lock);
			return;         /* (another user still has it) */
		}
		address = (void *)(uintptr_t)m->address;   /* (the user may hold a pointer into it, not its start) */
	}
	pthread_mutex_unlock(&map_lock);
	IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)address);
}

int
hakios_drm_surface(int fd, struct hakios_drm_surface *surface)
{
	int error;

	(void)fd;
	surface->handle = HAKIOS_GPU_DESCRIBE;
	error = call(HAKIOS_GPU_SURFACE, surface, sizeof(*surface));
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
	struct hakios_drm_surface request = { .surface = (uint32_t)surface };
	int error = call(HAKIOS_GPU_SURFACE, &request, sizeof(request));

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	*handle = request.handle;
	return 0;
}

static int
none(void)
{
	errno = ENOSYS;
	return -1;
}

/* ---- the Mali's jobs ------------------------------------------------------------------------- */

/*
 * HakiosMali puts every job chain it is given into a queue, which a thread of its own runs one
 * chain at a time, in the order given; a submission comes back at once with the job's number, and
 * its request 0x7d waits for a number (struct mali_wait: the number, then how long at most). Since
 * the jobs end in that order, a job that is done is done with every job before it.
 */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t submitted;              /* the last job number the kernel gave (to this process) */
static pthread_cond_t pan_given = PTHREAD_COND_INITIALIZER;     /* a sync object was given a job or signalled */
static uint64_t finished;               /* a job known to be done, and with it every one before it */

/*
 * Mesa's deadlines are moments of its clock (os_time_get_nano(): CLOCK_MONOTONIC on this system),
 * as Linux's are; the kernel's waits take a length of time. What is left until one, then.
 */
static int64_t
left_ns(int64_t deadline)
{
	struct timespec t;
	int64_t now;

	clock_gettime(CLOCK_MONOTONIC, &t);
	now = (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
	return deadline > now ? deadline - now : 0;
}

/*
 * Job `seq` done before `deadline` (INT64_MAX: whenever that is): 0, or an errno. The kernel waits
 * ten seconds at most at a time and is asked again until the deadline; a job does not run for that
 * long, since the kernel resets the GPU under one that has not ended within a second.
 */
static int
wait_job(uint64_t seq, int64_t deadline)
{
	struct {
		uint64_t seq;
		int64_t timeout_ns;
	} request;                              /* struct mali_wait */
	bool done;
	int error;

	pthread_mutex_lock(&lock);
	done = seq <= finished;
	pthread_mutex_unlock(&lock);
	if (done) {
		return 0;
	}
	do {
		request.seq = seq;
		request.timeout_ns = left_ns(deadline);
		error = call(HAKIOS_MALI_WAIT, &request, sizeof(request));
	} while (error == ETIMEDOUT && left_ns(deadline) > 0);
	if (error == 0) {
		pthread_mutex_lock(&lock);
		finished = seq > finished ? seq : finished;
		pthread_mutex_unlock(&lock);
	}
	return error;
}

static uint64_t *pan_syncobj(uint32_t handle);

/*
 * A job chain to the GPU. Linux's driver gives back nothing and puts the job's fence into the sync
 * object out_sync; HakiosMali gives back the job's number where the chain's address was (jc). The
 * address is put back, as Linux leaves it, and the number goes into the sync object. The fences to
 * wait for before the job (in_syncs) are looked at neither here nor by the kernel: they can only
 * stand for jobs given before this one, which the queue has run by the time this one begins.
 */
static int
submit(struct drm_panfrost_submit *request)
{
	uint64_t chain = request->jc, seq, *fence;
	int error = call(DRM_COMMAND_BASE + DRM_PANFROST_SUBMIT, request, sizeof(*request));

	if (error != 0) {
		return error;
	}
	seq = request->jc;
	request->jc = chain;
	pthread_mutex_lock(&lock);
	submitted = seq > submitted ? seq : submitted;
	if (request->out_sync != 0 && (fence = pan_syncobj(request->out_sync)) != NULL) {
		*fence = seq;
		pthread_cond_broadcast(&pan_given);
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

/*
 * Whether the GPU is done with a buffer. HakiosMali waits for the last job that named it, for a
 * length of time; Linux's timeout_ns is a moment (panfrost_drm.h: "absolute"), which panfrost gives
 * as 0 (only a look) or INT64_MAX (until it is done).
 */
static int
wait_buffer(struct drm_panfrost_wait_bo *request)
{
	struct drm_panfrost_wait_bo asked = *request;
	int error;

	do {
		asked.timeout_ns = left_ns(request->timeout_ns);
		error = call(DRM_COMMAND_BASE + DRM_PANFROST_WAIT_BO, &asked, sizeof(asked));
	} while (error == ETIMEDOUT && left_ns(request->timeout_ns) > 0);
	return error;
}

/* ---- sync objects ---------------------------------------------------------------------------- */

/*
 * Linux's fences that a program holds by a handle. Here, for panfrost, each is the number of the
 * job it waits for, kept in this process: the kernel has no fences, it has numbers. 0 is no job,
 * nothing to wait for: a sync object made signalled, or signalled by the CPU. SYNCOBJ_UNSUBMITTED is
 * one without a fence yet (made unsignalled, or reset), which no job has been given since.
 * Waiting for several is waiting for the highest of their numbers (all of them) or the lowest (any),
 * since the jobs end in order. freedreno waits by requests of its own and has none of these, as
 * before (ENOSYS).
 *
 * A sync object without a fence is waited for as Linux does with DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT:
 * until a submission gives it a job (or the CPU signals it), then for that job. Vulkan's common code
 * waits that way for every fence and semaphore (vk_drm_syncobj.c), and it has to: with timelines
 * emulated (vk_sync_timeline, there being no DRM_CAP_SYNCOBJ_TIMELINE here) the queue submits from a
 * thread of its own, so vkQueueSubmit can return before the fence's job is given, and a wait that
 * took the fence for done returned before the GPU had run anything (docs/metal.md, 14 November: a
 * Metal texture read back with -getBytes: had lines of zeroes, read before its copy had run).
 * Without that flag, which panfrost's OpenGL ES never gives, such a sync object counts as signalled,
 * as before; Linux would refuse the wait (EINVAL).
 */
#define SYNCOBJ_FREE            UINT64_MAX
#define SYNCOBJ_UNSUBMITTED     (UINT64_MAX - 1)
static uint64_t *pan_syncobjs;              /* handle n is pan_syncobjs[n - 1]: its job, or SYNCOBJ_FREE */
static uint32_t pan_syncobj_count;

/* (with the lock held) NULL: no such sync object */
static uint64_t *
pan_syncobj(uint32_t handle)
{
	if (handle == 0 || handle > pan_syncobj_count || pan_syncobjs[handle - 1] == SYNCOBJ_FREE) {
		return NULL;
	}
	return &pan_syncobjs[handle - 1];
}

int
pan_drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	uint32_t i;

	(void)fd;
	if (opened != PANFROST) {
		return none();
	}
	pthread_mutex_lock(&lock);
	for (i = 0; i < pan_syncobj_count && pan_syncobjs[i] != SYNCOBJ_FREE; i++) {
	}
	if (i == pan_syncobj_count) {
		uint32_t count = pan_syncobj_count != 0 ? 2 * pan_syncobj_count : 64;
		uint64_t *grown = realloc(pan_syncobjs, count * sizeof(*grown));

		if (grown == NULL) {
			pthread_mutex_unlock(&lock);
			errno = ENOMEM;
			return -1;
		}
		for (uint32_t k = pan_syncobj_count; k < count; k++) {
			grown[k] = SYNCOBJ_FREE;
		}
		pan_syncobjs = grown;
		pan_syncobj_count = count;
	}
	pan_syncobjs[i] = (flags & DRM_SYNCOBJ_CREATE_SIGNALED) ? 0 : SYNCOBJ_UNSUBMITTED;
	*handle = i + 1;
	pthread_mutex_unlock(&lock);
	return 0;
}

int
pan_drmSyncobjDestroy(int fd, uint32_t handle)
{
	uint64_t *fence;

	(void)fd;
	if (opened != PANFROST) {
		return none();
	}
	pthread_mutex_lock(&lock);
	if ((fence = pan_syncobj(handle)) != NULL) {
		*fence = SYNCOBJ_FREE;
	}
	pthread_mutex_unlock(&lock);
	if (fence == NULL) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

int
pan_drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	uint64_t *fence;

	(void)fd;
	if (opened != PANFROST) {
		return none();
	}
	pthread_mutex_lock(&lock);
	for (uint32_t i = 0; i < count; i++) {
		if ((fence = pan_syncobj(handles[i])) != NULL) {
			*fence = SYNCOBJ_UNSUBMITTED;
		}
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

/* As libdrm's: 0, or minus an errno, which errno is too (ETIME: not done by the deadline, a moment of Mesa's clock). */
int
pan_drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled)
{
	bool all = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0;
	bool for_submit = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) != 0;
	uint64_t seq = 0, job, *fence;
	unsigned first = 0, given;
	int64_t left;
	int error;

	(void)fd;
	if (opened != PANFROST) {
		return -ENOSYS;
	}
	pthread_mutex_lock(&lock);
	for (;;) {
		given = 0;
		for (unsigned i = 0; i < count; i++) {
			if ((fence = pan_syncobj(handles[i])) == NULL) {
				pthread_mutex_unlock(&lock);
				errno = EINVAL;
				return -EINVAL;
			}
			given += *fence != SYNCOBJ_UNSUBMITTED;
		}
		/* every one has a job (all), or one has (any); without the flag, none has to */
		if (!for_submit || given == count || (!all && given != 0)) {
			break;
		}
		if ((left = left_ns(timeout_nsec)) == 0) {
			pthread_mutex_unlock(&lock);
			errno = ETIME;
			return -ETIME;
		}
		struct timespec slice = { 0, left < 10000000 ? left : 10000000 };
		pthread_cond_timedwait_relative_np(&pan_given, &lock, &slice);
	}
	for (unsigned i = 0, chosen = 0; i < count; i++) {
		job = *pan_syncobj(handles[i]);
		if (job == SYNCOBJ_UNSUBMITTED) {
			if (for_submit) {
				continue;       /* (any: another one has a job) */
			}
			job = 0;
		}
		if (!chosen++ || (all ? job > seq : job < seq)) {
			seq = job;
			first = i;
		}
	}
	pthread_mutex_unlock(&lock);
	error = wait_job(seq, timeout_nsec);
	if (error != 0) {
		errno = error == ETIMEDOUT ? ETIME : error;
		return -errno;
	}
	if (first_signaled != NULL) {
		*first_signaled = first;
	}
	return 0;
}

/*
 * PanVK's JM backend makes some of its sync object calls as ioctls of their own, not through libdrm's
 * functions (panvk_vX_queue.c: a submission's fence handed to the semaphores it signals, through a
 * sync file; panvk_vX_event.c: VkEvents). Those are the same sync objects as above, kept in this
 * process; the kernel has none. True when NUMBER is one of them, with what it gave in ERROR (0 or
 * an errno). Panfrost's OpenGL ES uses libdrm's functions only.
 */
int pan_drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count);

static bool
pan_syncobj_ioctl(unsigned number, void *arg, int *error)
{
	int result;

	errno = 0;
	if (number == (DRM_IOCTL_SYNCOBJ_CREATE & 0xff)) {
		struct drm_syncobj_create *create = arg;

		result = pan_drmSyncobjCreate(-1, create->flags, &create->handle);
	} else if (number == (DRM_IOCTL_SYNCOBJ_DESTROY & 0xff)) {
		result = pan_drmSyncobjDestroy(-1, ((struct drm_syncobj_destroy *)arg)->handle);
	} else if (number == (DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD & 0xff)) {
		struct drm_syncobj_handle *handle = arg;

		if (!(handle->flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE)) {
			*error = ENOSYS;        /* (a sync object as a descriptor of its own: none here) */
			return true;
		}
		result = pan_drmSyncobjExportSyncFile(-1, handle->handle, &handle->fd);
	} else if (number == (DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE & 0xff)) {
		struct drm_syncobj_handle *handle = arg;

		if (!(handle->flags & DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE)) {
			*error = ENOSYS;
			return true;
		}
		result = pan_drmSyncobjImportSyncFile(-1, handle->handle, handle->fd);
	} else if (number == (DRM_IOCTL_SYNCOBJ_WAIT & 0xff)) {
		struct drm_syncobj_wait *wait = arg;

		result = pan_drmSyncobjWait(-1, (uint32_t *)(uintptr_t)wait->handles, wait->count_handles, wait->timeout_nsec,
		    wait->flags, &wait->first_signaled);
	} else if (number == (DRM_IOCTL_SYNCOBJ_RESET & 0xff)) {
		struct drm_syncobj_array *array = arg;

		result = pan_drmSyncobjReset(-1, (const uint32_t *)(uintptr_t)array->handles, array->count_handles);
	} else if (number == (DRM_IOCTL_SYNCOBJ_SIGNAL & 0xff)) {
		struct drm_syncobj_array *array = arg;

		result = pan_drmSyncobjSignal(-1, (const uint32_t *)(uintptr_t)array->handles, array->count_handles);
	} else {
		return false;
	}
	*error = result == 0 ? 0 : errno != 0 ? errno : EINVAL;
	return true;
}

/*
 * A sync object's fence as a file descriptor (a "sync file"): panfrost makes one of its context's
 * sync object for every fence it hands out, and takes it into a new sync object at once
 * (pan_fence.c). Here it is another descriptor of /dev/null, with the job number kept beside its
 * number. The program closes it when it likes, without a word, so a number that comes back may be
 * another descriptor by then: one made here again (the entry is made anew whenever a number is
 * made here, so it is the newest), or one that was not made here at all, which is taken for the
 * last job given to the GPU so far -- later than the one meant, if anything, never earlier.
 */
#define SYNC_FILES      64
static struct {
	bool used;
	int fd;
	uint64_t seq;
} sync_files[SYNC_FILES];
static unsigned sync_file_next;

int
pan_drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	uint64_t *fence;
	int file;

	(void)fd;
	if (opened != PANFROST) {
		return none();
	}
	if ((file = fcntl(connection_fd, F_DUPFD_CLOEXEC, 0)) < 0) {
		return -1;
	}
	pthread_mutex_lock(&lock);
	if ((fence = pan_syncobj(handle)) == NULL) {
		pthread_mutex_unlock(&lock);
		close(file);
		errno = EINVAL;
		return -1;
	}
	for (unsigned i = 0; i < SYNC_FILES; i++) {
		if (sync_files[i].used && sync_files[i].fd == file) {
			sync_files[i].used = false;     /* (a number made here before, closed since) */
		}
	}
	sync_files[sync_file_next % SYNC_FILES].used = true;
	sync_files[sync_file_next % SYNC_FILES].fd = file;
	sync_files[sync_file_next % SYNC_FILES].seq = *fence != SYNCOBJ_UNSUBMITTED ? *fence : 0;
	sync_file_next++;
	pthread_mutex_unlock(&lock);
	*sync_file_fd = file;
	return 0;
}

int
pan_drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	uint64_t *fence, seq;

	(void)fd;
	if (opened != PANFROST) {
		return none();
	}
	pthread_mutex_lock(&lock);
	if ((fence = pan_syncobj(handle)) == NULL) {
		pthread_mutex_unlock(&lock);
		errno = EINVAL;
		return -1;
	}
	seq = submitted;
	for (unsigned i = 0; i < SYNC_FILES; i++) {
		if (sync_files[i].used && sync_files[i].fd == sync_file_fd) {
			seq = sync_files[i].seq;
		}
	}
	*fence = seq;
	pthread_cond_broadcast(&pan_given);
	pthread_mutex_unlock(&lock);
	return 0;
}

/*
 * Buffers and sync objects as file descriptors of their own, for handing them to other programs and
 * drivers, and timelines (panthor's): none of that here.
 */
#define NONE(name, ...) int name(__VA_ARGS__) { return none(); }
NONE(drmPrimeHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) uint32_t flags, __attribute__((unused)) int *prime_fd)
NONE(drmSyncobjFDToHandle, __attribute__((unused)) int fd, __attribute__((unused)) int obj_fd,
    __attribute__((unused)) uint32_t *handle)
NONE(drmSyncobjHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int *obj_fd)

/*
 * DRM's synchronisation objects (syncobj), which Turnip's msm backend makes its VkFences and VkSemaphores of, kept in
 * this process: there is one GPU and one connection to it here, so what a syncobj waits for is always one of this
 * process's submissions, a fence number of the kernel's (MSM_WAIT_FENCE tells when it is done). A syncobj is a timeline:
 * the last point known reached, and the points still on their way, each with the submission that reaches it. A binary
 * one is the timeline of point 1 (signalled: 1, reset: 0). A submission's MSM_SUBMIT_SYNCOBJ_IN syncobjs are waited for
 * here, before it goes to the kernel, and its MSM_SUBMIT_SYNCOBJ_OUT ones are given its fence (submit_with_syncobjs).
 * Not here: syncobjs and sync files as file descriptors (nothing outside this process waits for them).
 */
#define SYNCOBJS        4096
#define PENDING         16

struct syncobj {
	bool used;
	uint64_t reached;                       /* the last point known reached */
	unsigned pending;
	struct { uint64_t point; uint32_t fence; } on_the_way[PENDING];
};
static struct syncobj syncobjs[SYNCOBJS];
static pthread_mutex_t syncobj_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t syncobj_submitted = PTHREAD_COND_INITIALIZER;

static struct syncobj *
syncobj_of(uint32_t handle)
{
	return handle != 0 && handle < SYNCOBJS && syncobjs[handle].used ? &syncobjs[handle] : NULL;
}

/* whether the kernel is done with submission FENCE: waited for until DEADLINE (absolute CLOCK_MONOTONIC ns; 0: not at all) */
static bool
fence_done(uint32_t fence, int64_t deadline)
{
	struct drm_msm_wait_fence wait = { .fence = fence, .queueid = 0 };

	wait.timeout.tv_sec = deadline / 1000000000;
	wait.timeout.tv_nsec = deadline % 1000000000;
	return call(DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE, &wait, sizeof(wait)) == 0;
}

static int64_t
now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

/* the points on their way that their submissions have reached since: into REACHED (lock held) */
static void
syncobj_poll(struct syncobj *s)
{
	unsigned kept = 0;

	for (unsigned i = 0; i < s->pending; i++) {
		if (fence_done(s->on_the_way[i].fence, 0)) {
			if (s->on_the_way[i].point > s->reached) s->reached = s->on_the_way[i].point;
		} else {
			s->on_the_way[kept++] = s->on_the_way[i];
		}
	}
	s->pending = kept;
}

static void
syncobj_add(struct syncobj *s, uint64_t point, uint32_t fence)
{
	if (s->pending == PENDING) {
		syncobj_poll(s);
	}
	if (s->pending == PENDING) {
		/* (full: the oldest is waited for, its point taken as reached) */
		fence_done(s->on_the_way[0].fence, now_ns() + 10000000000LL);
		if (s->on_the_way[0].point > s->reached) s->reached = s->on_the_way[0].point;
		memmove(&s->on_the_way[0], &s->on_the_way[1], (PENDING - 1) * sizeof(s->on_the_way[0]));
		s->pending--;
	}
	s->on_the_way[s->pending].point = point;
	s->on_the_way[s->pending].fence = fence;
	s->pending++;
}

int
msm_drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	for (uint32_t h = 1; h < SYNCOBJS; h++) {
		if (!syncobjs[h].used) {
			memset(&syncobjs[h], 0, sizeof(syncobjs[h]));
			syncobjs[h].used = true;
			syncobjs[h].reached = (flags & DRM_SYNCOBJ_CREATE_SIGNALED) ? 1 : 0;
			*handle = h;
			pthread_mutex_unlock(&syncobj_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&syncobj_lock);
	errno = ENOMEM;
	return -ENOMEM;
}

int
msm_drmSyncobjDestroy(int fd, uint32_t handle)
{
	struct syncobj *s;

	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	s = syncobj_of(handle);
	if (s != NULL) s->used = false;
	pthread_mutex_unlock(&syncobj_lock);
	return s != NULL ? 0 : -EINVAL;
}

int
msm_drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	for (uint32_t i = 0; i < count; i++) {
		struct syncobj *s = syncobj_of(handles[i]);
		if (s != NULL) s->reached = 0, s->pending = 0;
	}
	pthread_mutex_unlock(&syncobj_lock);
	return 0;
}

int
msm_drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t count)
{
	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	for (uint32_t i = 0; i < count; i++) {
		struct syncobj *s = syncobj_of(handles[i]);
		uint64_t point = points != NULL && points[i] != 0 ? points[i] : 1;
		if (s != NULL && point > s->reached) s->reached = point;
	}
	pthread_cond_broadcast(&syncobj_submitted);
	pthread_mutex_unlock(&syncobj_lock);
	return 0;
}

int
msm_drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
	return msm_drmSyncobjTimelineSignal(fd, handles, NULL, count);
}

int
msm_drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t count, uint32_t flags)
{
	(void)fd;
	(void)flags;
	pthread_mutex_lock(&syncobj_lock);
	for (uint32_t i = 0; i < count; i++) {
		struct syncobj *s = syncobj_of(handles[i]);
		if (s != NULL) syncobj_poll(s);
		points[i] = s != NULL ? s->reached : 0;
	}
	pthread_mutex_unlock(&syncobj_lock);
	return 0;
}

int
msm_drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t count)
{
	return msm_drmSyncobjQuery2(fd, handles, points, count, 0);
}

/*
 * Wait for each syncobj's point (0 or NULL: 1, a binary one's) -- all of them, or the first (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL
 * clear) -- until DEADLINE (absolute CLOCK_MONOTONIC ns). A point no submission is on its way to yet is waited for to be
 * submitted with DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, an error without (as Linux answers); WAIT_AVAILABLE: submitted
 * is enough.
 */
int
msm_drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned count, int64_t deadline, unsigned flags,
    uint32_t *first)
{
	const bool all = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0;
	int result = -ETIME;

	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	for (;;) {
		unsigned reached = 0, unknown = 0;
		uint32_t wait_fence = 0;
		bool have_fence = false;

		for (unsigned i = 0; i < count; i++) {
			struct syncobj *s = syncobj_of(handles[i]);
			uint64_t point = points != NULL && points[i] != 0 ? points[i] : 1;
			bool on_the_way = false;

			if (s == NULL) {
				pthread_mutex_unlock(&syncobj_lock);
				return -EINVAL;
			}
			syncobj_poll(s);
			for (unsigned k = 0; k < s->pending; k++) {
				if (s->on_the_way[k].point >= point) {
					on_the_way = true;
					if (!have_fence || (int32_t)(s->on_the_way[k].fence - wait_fence) < 0) wait_fence = s->on_the_way[k].fence;
					have_fence = true;
					break;
				}
			}
			if (s->reached >= point || (on_the_way && (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE))) {
				reached++;
				if (!all) {
					if (first != NULL) *first = i;
					pthread_mutex_unlock(&syncobj_lock);
					return 0;
				}
			} else if (!on_the_way) {
				unknown++;
			}
		}
		if (reached == count) {
			if (first != NULL) *first = 0;
			pthread_mutex_unlock(&syncobj_lock);
			return 0;
		}
		if (unknown != 0 && !(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)) {
			pthread_mutex_unlock(&syncobj_lock);
			return -EINVAL;
		}
		if (now_ns() >= deadline) {
			break;
		}
		if (have_fence && unknown == 0) {
			/* every point is on its way: the earliest submission among them waited for, without the lock */
			pthread_mutex_unlock(&syncobj_lock);
			fence_done(wait_fence, deadline);
			pthread_mutex_lock(&syncobj_lock);
		} else {
			/* some point not submitted yet: until a submission or a signal, in steps of a millisecond at most */
			struct timespec until;
			int64_t step = now_ns() + 1000000;
			struct timeval tv;
			gettimeofday(&tv, NULL);
			int64_t real = (int64_t)tv.tv_sec * 1000000000 + (int64_t)tv.tv_usec * 1000 + (step < deadline ? 1000000 : deadline - now_ns());
			until.tv_sec = real / 1000000000;
			until.tv_nsec = real % 1000000000;
			pthread_cond_timedwait(&syncobj_submitted, &syncobj_lock, &until);
		}
	}
	pthread_mutex_unlock(&syncobj_lock);
	return result;
}

int
msm_drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t deadline, unsigned flags, uint32_t *first)
{
	return msm_drmSyncobjTimelineWait(fd, handles, NULL, count, deadline, flags, first);
}

/* DST at DST_POINT (0: binary) gets what SRC at SRC_POINT (0: binary) is or will be */
int
msm_drmSyncobjTransfer(int fd, uint32_t dst, uint64_t dst_point, uint32_t src, uint64_t src_point, uint32_t flags)
{
	struct syncobj *d, *s;
	uint64_t want = src_point != 0 ? src_point : 1, to = dst_point != 0 ? dst_point : 1;

	(void)fd;
	(void)flags;
	pthread_mutex_lock(&syncobj_lock);
	d = syncobj_of(dst);
	s = syncobj_of(src);
	if (d == NULL || s == NULL) {
		pthread_mutex_unlock(&syncobj_lock);
		return -EINVAL;
	}
	syncobj_poll(s);
	if (dst_point == 0) d->reached = 0, d->pending = 0;
	if (s->reached >= want) {
		if (to > d->reached) d->reached = to;
	} else {
		for (unsigned k = 0; k < s->pending; k++) {
			if (s->on_the_way[k].point >= want) {
				syncobj_add(d, to, s->on_the_way[k].fence);
				break;
			}
		}
	}
	pthread_cond_broadcast(&syncobj_submitted);
	pthread_mutex_unlock(&syncobj_lock);
	return 0;
}

static uint32_t last_fence;     /* the fence of the last submission the kernel took */

static int
submit_with_syncobjs(struct drm_msm_gem_submit *submit, unsigned long size)
{
	const struct drm_msm_gem_submit_syncobj *in = (const void *)(uintptr_t)submit->in_syncobjs;
	const struct drm_msm_gem_submit_syncobj *out = (const void *)(uintptr_t)submit->out_syncobjs;
	const uint32_t stride = submit->syncobj_stride ? submit->syncobj_stride : sizeof(*in);
	const uint32_t nin = (submit->flags & MSM_SUBMIT_SYNCOBJ_IN) ? submit->nr_in_syncobjs : 0;
	const uint32_t nout = (submit->flags & MSM_SUBMIT_SYNCOBJ_OUT) ? submit->nr_out_syncobjs : 0;
	int error;

	/* what it waits for first (this queue's own submissions are in order on the one ring anyway) */
	for (uint32_t i = 0; i < nin; i++) {
		const struct drm_msm_gem_submit_syncobj *x = (const void *)((const uint8_t *)in + i * stride);
		uint32_t h = x->handle;
		uint64_t p = x->point;
		int r = msm_drmSyncobjTimelineWait(0, &h, &p, 1, now_ns() + 10000000000LL,
		    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL);
		if (r != 0) return r;
		if (x->flags & MSM_SUBMIT_SYNCOBJ_RESET) msm_drmSyncobjReset(0, &h, 1);
	}
	submit->flags &= ~(uint32_t)(MSM_SUBMIT_SYNCOBJ_IN | MSM_SUBMIT_SYNCOBJ_OUT);
	submit->in_syncobjs = submit->out_syncobjs = 0;
	submit->nr_in_syncobjs = submit->nr_out_syncobjs = 0;
	if (submit->nr_cmds == 0) {
		/*
		 * Nothing for the GPU (Turnip's submissions that only signal): the kernel takes none, and what it stands for
		 * is done when everything submitted before it is, the last fence -- there is one ring.
		 */
		submit->fence = __atomic_load_n(&last_fence, __ATOMIC_ACQUIRE);
	} else {
		error = call((unsigned)(DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT), submit, size);
		if (error != 0) return -error;
		__atomic_store_n(&last_fence, submit->fence, __ATOMIC_RELEASE);
	}
	pthread_mutex_lock(&syncobj_lock);
	for (uint32_t i = 0; i < nout; i++) {
		const struct drm_msm_gem_submit_syncobj *x = (const void *)((const uint8_t *)out + i * stride);
		struct syncobj *s = syncobj_of(x->handle);
		if (s == NULL) continue;
		if (x->point == 0) s->reached = 0, s->pending = 0;   /* a binary one: what it holds now is this submission */
		syncobj_add(s, x->point != 0 ? x->point : 1, submit->fence);
	}
	pthread_cond_broadcast(&syncobj_submitted);
	pthread_mutex_unlock(&syncobj_lock);
	return 0;
}

/* Linux's device nodes, which Turnip's lists of devices ask about: one GPU, a render node 128 */
int drmGetNodeTypeFromFd(int fd) { (void)fd; return DRM_NODE_RENDER; }

/* (the device list itself, drmGetDevices2() and the rest, is below: the Mali when HakiosMali is there, none for the Adreno, which Turnip finds itself) */

/*
 * The IOSurfaces mapped in this process, by address (hakios_drm_host_surface): a host pointer Turnip is asked to import
 * (VK_EXT_external_memory_host) is the memory of one of them, and the kernel makes a buffer of a surface by its ID
 * (drmPrimeFDToHandle). A surface's mapping lasts as long as the surface; a later surface at the same address replaces it.
 */
#define HOST_SURFACES 1024
static struct { uintptr_t base; uint64_t size; uint32_t surface; } host_surfaces[HOST_SURFACES];
static unsigned host_surfaces_next;
static pthread_mutex_t host_surfaces_lock = PTHREAD_MUTEX_INITIALIZER;

__attribute__((visibility("default")))     /* (called by AdrenoMetal's frontend, outside Turnip) */
void
hakios_drm_host_surface(const void *base, uint64_t size, uint32_t surface)
{
	unsigned i, free_slot = HOST_SURFACES;

	pthread_mutex_lock(&host_surfaces_lock);
	for (i = 0; i < HOST_SURFACES; i++) {
		if (host_surfaces[i].base == (uintptr_t)base) {
			break;
		}
		if (host_surfaces[i].base == 0 && free_slot == HOST_SURFACES) {
			free_slot = i;
		}
	}
	if (i == HOST_SURFACES) {
		i = free_slot != HOST_SURFACES ? free_slot : host_surfaces_next++ % HOST_SURFACES;
	}
	host_surfaces[i].base = (uintptr_t)base;
	host_surfaces[i].size = size;
	host_surfaces[i].surface = surface;
	pthread_mutex_unlock(&host_surfaces_lock);
}

uint32_t
hakios_drm_host_surface_at(const void *base, uint64_t size)
{
	uint32_t surface = 0;

	pthread_mutex_lock(&host_surfaces_lock);
	for (unsigned i = 0; i < HOST_SURFACES; i++) {
		if (host_surfaces[i].base == (uintptr_t)base && host_surfaces[i].size >= size) {
			surface = host_surfaces[i].surface;
			break;
		}
	}
	pthread_mutex_unlock(&host_surfaces_lock);
	return surface;
}

int
hakios_drm_user_memory(int fd, const void *address, uint64_t size, uint32_t *handle)
{
	struct { uint64_t address, size; uint32_t handle, flags; } request = { (uintptr_t)address, size, 0, 0 };
	int error = call(opened == PANFROST ? HAKIOS_MALI_USER_MEMORY : HAKIOS_GPU_USER_MEMORY, &request, sizeof(request));

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	*handle = request.handle;
	return 0;
}

/*
 * Bytes `offset` to `offset + size` of buffer `handle` cleaned and invalidated in the processors' caches, to the point of
 * coherency (HakiosMali's request 0x7a): a buffer of this process's own pages (hakios_drm_user_memory) is cached memory,
 * which the Mali does not see through those caches. The kernel does it at the instruction's own cost; this process
 * cannot (its DC CIVAC traps with the Exynos M3s up, xnu patch 1511, and sys_dcache_flush() is a DSB alone). ENOSYS
 * from a kernel without the request; the Adreno's service has none.
 */
int
hakios_drm_sync(int fd, uint32_t handle, uint64_t offset, uint64_t size)
{
	struct { uint32_t handle, flags; uint64_t offset, size; } request = { handle, 0, offset, size };
	int error = opened == PANFROST ? call(HAKIOS_MALI_SYNC, &request, sizeof(request)) : ENOSYS;

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

/*
 * The libdrm entry points Mesa calls, for whichever service was opened: HakiosMali's panfrost (pan_*, sync objects of the
 * kernel's job numbers) or HakiosGPU's msm (msm_*, timeline sync objects over MSM_WAIT_FENCE).
 */
#define PAN (opened == PANFROST)
int pan_drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{ return PAN ? pan_drmSyncobjCreate(fd, flags, handle) : msm_drmSyncobjCreate(fd, flags, handle); }
int drmSyncobjDestroy(int fd, uint32_t handle)
{ return PAN ? pan_drmSyncobjDestroy(fd, handle) : msm_drmSyncobjDestroy(fd, handle); }
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{ return PAN ? pan_drmSyncobjReset(fd, handles, count) : msm_drmSyncobjReset(fd, handles, count); }
int drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{ return PAN ? pan_drmSyncobjWait(fd, handles, count, timeout_nsec, flags, first_signaled) :
    msm_drmSyncobjWait(fd, handles, count, timeout_nsec, flags, first_signaled); }
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{ return PAN ? pan_drmSyncobjExportSyncFile(fd, handle, sync_file_fd) : none(); }
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{ return PAN ? pan_drmSyncobjImportSyncFile(fd, handle, sync_file_fd) : none(); }
int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t count)
{ return PAN ? none() : msm_drmSyncobjTimelineSignal(fd, handles, points, count); }
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{ return PAN ? pan_drmSyncobjSignal(fd, handles, count) : msm_drmSyncobjSignal(fd, handles, count); }
int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t count, uint32_t flags)
{ return PAN ? none() : msm_drmSyncobjQuery2(fd, handles, points, count, flags); }
int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t count)
{ return PAN ? none() : msm_drmSyncobjQuery(fd, handles, points, count); }
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned count, int64_t deadline, unsigned flags,
    uint32_t *first)
{ return PAN ? none() : msm_drmSyncobjTimelineWait(fd, handles, points, count, deadline, flags, first); }
int drmSyncobjTransfer(int fd, uint32_t dst, uint64_t dst_point, uint32_t src, uint64_t src_point, uint32_t flags)
{ return PAN ? none() : msm_drmSyncobjTransfer(fd, dst, dst_point, src, src_point, flags); }

/*
 * Signalled by the CPU: no job to wait for, as a sync object made signalled (0). Vulkan's common
 * code calls this for vkSignalSemaphore and for fences and semaphores it signals itself.
 */
int
pan_drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
	uint64_t *fence;

	(void)fd;
	if (opened != PANFROST) {
		return none();
	}
	pthread_mutex_lock(&lock);
	for (uint32_t i = 0; i < count; i++) {
		if ((fence = pan_syncobj(handles[i])) == NULL) {
			pthread_mutex_unlock(&lock);
			errno = EINVAL;
			return -1;
		}
	}
	for (uint32_t i = 0; i < count; i++) {
		*pan_syncobj(handles[i]) = 0;
	}
	pthread_cond_broadcast(&pan_given);
	pthread_mutex_unlock(&lock);
	return 0;
}

/* ---- the devices there are (xf86drm.h) ------------------------------------------------------- */

static drmDevicePtr
mali_device(void)
{
	drmDevicePtr device;

	/* one allocation: the device, its node table, its bus and device information, the strings */
	size_t size = sizeof(*device) + DRM_NODE_MAX * sizeof(char *) + sizeof(drmPlatformBusInfo) +
	    sizeof(drmPlatformDeviceInfo) + 2 * sizeof(char *) + sizeof("/dev/null") + sizeof("arm,mali-bifrost");
	char *p = calloc(1, size);

	if (p == NULL) {
		return NULL;
	}
	device = (drmDevicePtr)(void *)p;
	p += sizeof(*device);
	device->nodes = (char **)(void *)p;
	p += DRM_NODE_MAX * sizeof(char *);
	device->businfo.platform = (drmPlatformBusInfoPtr)(void *)p;
	p += sizeof(drmPlatformBusInfo);
	device->deviceinfo.platform = (drmPlatformDeviceInfoPtr)(void *)p;
	p += sizeof(drmPlatformDeviceInfo);
	device->deviceinfo.platform->compatible = (char **)(void *)p;
	p += 2 * sizeof(char *);
	device->nodes[DRM_NODE_RENDER] = strcpy(p, "/dev/null");
	p += sizeof("/dev/null");
	device->deviceinfo.platform->compatible[0] = strcpy(p, "arm,mali-bifrost");
	strcpy(device->businfo.platform->fullname, "/mali@17500000");
	device->available_nodes = 1 << DRM_NODE_RENDER;
	device->bustype = DRM_BUS_PLATFORM;
	return device;
}

/*
 * The Mali, when HakiosMali is there (or HAKIOS_DRM_NOOP is set), and nothing else: the Adreno's
 * service speaks msm, which no Vulkan driver here opens by enumeration. With devices NULL, how many
 * there are, as libdrm.
 */
int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	(void)flags;
	if (open_service(PANFROST) != 0) {
		return 0;
	}
	if (devices == NULL) {
		return 1;
	}
	if (max_devices < 1) {
		return 0;
	}
	if ((devices[0] = mali_device()) == NULL) {
		return -ENOMEM;
	}
	return 1;
}

int
drmGetDevices(drmDevicePtr devices[], int max_devices)
{
	return drmGetDevices2(0, devices, max_devices);
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void)fd;
	(void)flags;
	if (opened != PANFROST) {
		return -ENODEV;
	}
	return (*device = mali_device()) != NULL ? 0 : -ENOMEM;
}

void
drmFreeDevice(drmDevicePtr *device)
{
	if (device != NULL) {
		free(*device);
		*device = NULL;
	}
}

/* there is one device: two descriptions of a device are the same one when both are there */
int
drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
	return a != NULL && b != NULL && a->bustype == b->bustype;
}

void
drmFreeDevices(drmDevicePtr devices[], int count)
{
	for (int i = 0; devices != NULL && i < count; i++) {
		drmFreeDevice(&devices[i]);
	}
}

/* ---- HAKIOS_DRM_NOOP: a Mali-G72 that runs nothing ------------------------------------------- */

/*
 * What HakiosMali answers, made up in this process (see hakios_drm_noop() in xf86drm.h). The
 * parameters are those of tools/mali-drmshim/hakios_mali_shim.c, the Linux drm-shim check of the
 * same GPU, where each value and the reason for it is written down: a Mali-G72 MP18 as the S9's
 * (GPU_ID 0x6221). Buffers are this process's memory at made-up GPU addresses between 32 MB and
 * 4 GB, as the kernel's are (drivers/gpu/mali/hakios.mali_va.h); a job is done as soon as it is
 * given, and none is run.
 */
static struct noop_buffer {
	void *memory;
	uint64_t size, address;
} *noop_buffers;
static uint32_t noop_count;
static uint64_t noop_next = 32ull << 20;
static uint64_t noop_jobs;

static void *
noop_map(uint32_t handle)
{
	void *memory = NULL;

	pthread_mutex_lock(&lock);
	if (handle != 0 && handle <= noop_count) {
		memory = noop_buffers[handle - 1].memory;
	}
	pthread_mutex_unlock(&lock);
	return memory;
}

static int
noop_param(struct drm_panfrost_get_param *param)
{
	static const struct {
		uint32_t param;
		uint64_t value;
	} g72[] = {
		{ DRM_PANFROST_PARAM_GPU_PROD_ID, 0x6221 },
		{ DRM_PANFROST_PARAM_GPU_REVISION, 0x0030 },
		{ DRM_PANFROST_PARAM_SHADER_PRESENT, 0x3ffff },
		{ DRM_PANFROST_PARAM_TILER_PRESENT, 0x1 },
		{ DRM_PANFROST_PARAM_L2_PRESENT, 0x1 },
		{ DRM_PANFROST_PARAM_AS_PRESENT, 0xff },
		{ DRM_PANFROST_PARAM_JS_PRESENT, 0x7 },
		{ DRM_PANFROST_PARAM_L2_FEATURES, 0x07120206 },
		{ DRM_PANFROST_PARAM_TILER_FEATURES, 0x809 },
		{ DRM_PANFROST_PARAM_MEM_FEATURES, 0x1 },
		{ DRM_PANFROST_PARAM_MMU_FEATURES, 0x2830 },
		{ DRM_PANFROST_PARAM_MAX_THREADS, 384 },
		{ DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ, 384 },
		{ DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ, 384 },
		{ DRM_PANFROST_PARAM_COHERENCY_FEATURES, 0x1 },
		{ DRM_PANFROST_PARAM_TEXTURE_FEATURES0, 0xffffffff },
		{ DRM_PANFROST_PARAM_TEXTURE_FEATURES1, 0xffffffff },
		{ DRM_PANFROST_PARAM_TEXTURE_FEATURES2, 0xffffffff },
		{ DRM_PANFROST_PARAM_TEXTURE_FEATURES3, 0xffffffff },
		{ DRM_PANFROST_PARAM_NR_CORE_GROUPS, 1 },
		{ DRM_PANFROST_PARAM_THREAD_TLS_ALLOC, 384 },
	};

	param->value = 0;               /* the rest (stack, core features, thread features, AFBC): 0 */
	if (param->param >= DRM_PANFROST_PARAM_JS_FEATURES0 && param->param <= DRM_PANFROST_PARAM_JS_FEATURES15) {
		param->value = param->param - DRM_PANFROST_PARAM_JS_FEATURES0 < 3 ? 0x20e : 0;
	}
	for (size_t i = 0; i < sizeof(g72) / sizeof(g72[0]); i++) {
		if (g72[i].param == param->param) {
			param->value = g72[i].value;
		}
	}
	return 0;
}

static int
noop_call(unsigned number, void *data, size_t size)
{
	int error = 0;

	(void)size;
	pthread_mutex_lock(&lock);
	switch (number) {
	case DRM_COMMAND_BASE + DRM_PANFROST_GET_PARAM:
		error = noop_param(data);
		break;
	case DRM_COMMAND_BASE + DRM_PANFROST_CREATE_BO: {
		struct drm_panfrost_create_bo *create = data;
		uint64_t length = (create->size + 4095) & ~4095ull;
		struct noop_buffer *grown;
		void *memory;

		if (length == 0 || noop_next + length > (4ull << 30) ||
		    (memory = calloc(1, (size_t)length)) == NULL) {
			error = ENOMEM;
			break;
		}
		grown = realloc(noop_buffers, (noop_count + 1) * sizeof(*grown));
		if (grown == NULL) {
			free(memory);
			error = ENOMEM;
			break;
		}
		noop_buffers = grown;
		/* code is kept off 16 MB lines (hakios.mali_va.h): every buffer starts on a 2 MB line here */
		noop_buffers[noop_count] = (struct noop_buffer){ memory, length, noop_next };
		create->handle = ++noop_count;
		create->offset = noop_next;
		noop_next = (noop_next + length + (2ull << 20) - 1) & ~((2ull << 20) - 1);
		break;
	}
	case DRM_COMMAND_BASE + DRM_PANFROST_MMAP_BO: {
		struct drm_panfrost_mmap_bo *map = data;

		map->offset = (uint64_t)map->handle << 12;
		break;
	}
	case DRM_COMMAND_BASE + DRM_PANFROST_GET_BO_OFFSET: {
		struct drm_panfrost_get_bo_offset *offset = data;

		if (offset->handle == 0 || offset->handle > noop_count) {
			error = EINVAL;
		} else {
			offset->offset = noop_buffers[offset->handle - 1].address;
		}
		break;
	}
	case DRM_COMMAND_BASE + DRM_PANFROST_SUBMIT: {
		struct drm_panfrost_submit *submit = data;

		submit->jc = ++noop_jobs;       /* (submit() above puts the chain back) */
		break;
	}
	case DRM_COMMAND_BASE + DRM_PANFROST_MADVISE: {
		struct drm_panfrost_madvise *madvise = data;

		madvise->retained = 1;
		break;
	}
	case 0x09: {                         /* DRM_IOCTL_GEM_CLOSE */
		struct drm_gem_close *close = data;

		if (close->handle != 0 && close->handle <= noop_count) {
			free(noop_buffers[close->handle - 1].memory);
			noop_buffers[close->handle - 1].memory = NULL;
		}
		break;
	}
	case DRM_COMMAND_BASE + DRM_PANFROST_WAIT_BO:
	case HAKIOS_MALI_WAIT:
		break;                          /* every job is done */
	default:
		error = ENOSYS;                 /* IOSurfaces (0x7e), performance counters */
		break;
	}
	pthread_mutex_unlock(&lock);
	return error;
}
