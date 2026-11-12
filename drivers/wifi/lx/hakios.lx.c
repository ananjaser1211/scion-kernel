/*
 * Darwin-on-hakios: the kernel's side of the Wi-Fi driver's object.
 *
 * The driver of the phone's radio is Linux's ath11k, compiled unmodified outside this tree on a
 * layer that gives it the kernel interfaces it expects, and tied into one object that the
 * kernel links when it is there (tools/mk-ath11k.sh and wifi/lx in the repository; the kernel
 * is built with that object when xnu-build.sh finds it). The object shows one name, lx_start(),
 * and asks for the calls of sys/hakios.lx.h, which are here: memory, time, threads, one lock
 * to sleep under, the kernel's options. What it asks of the radio itself -- its firmware's
 * files, its view of memory, the processor its firmware runs on -- is hakios.wifi.c's to
 * answer, which knows the phone.
 *
 * A kernel built without the object has the lx_start() at the end of this file, which says so.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/random.h>
#include <sys/hakios.input.h>
#include <sys/hakios.lx.h>
#include <sys/hakios.net.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>
#include <sys/hakios.xnu.h>


#define say hakios_wifi_say

static LCK_GRP_DECLARE(lx_lock_group, "hakios-lx");
static LCK_MTX_DECLARE(lx_lock, &lx_lock_group);

/* ---- memory ------------------------------------------------------------------------------------------ */

void *
lx_os_alloc(unsigned long size)
{
	return kalloc_data(size, Z_WAITOK | Z_ZERO);
}

void
lx_os_free(void *memory, unsigned long size)
{
	kfree_data(memory, size);
}

/* The radio's reads and writes do not look in the processors' caches unless the machine says so (hakios.exynos_pcie.c) */
__attribute__((weak)) bool
hakios_wifi_radio_coherent(void)
{
	return false;
}

/*
 * In one physical piece, so that it can be mapped a second time without this processor's cache
 * in between (as hakios.adreno.c does with what it shares with the GPU); the first, cached
 * view is written out and never used again. A radio whose reads and writes look in the caches
 * (hakios_wifi_radio_coherent()) gets the cached view itself: an uncached second view would be
 * wrong there, its writes going around lines the caches may still hold of the same memory.
 */
void *
lx_os_radio_memory(unsigned long size, unsigned long long *address)
{
	vm_offset_t cached = 0, uncached;
	uint64_t physical;

	size = round_page(size);
	physical = 0;
	PE_parse_boot_argn("wifi_dma_at", &physical, sizeof(physical));
	if (physical != 0) {
		/*
		 * wifi_dma_at=ADDRESS: a piece of DRAM the kernel does not have (a gap of the device tree's, e.g. 0xDC000000 on
		 * the S9), below 4 GB, for a radio whose bus may not reach the kernel's own memory above it; mapped only once.
		 */
		physical = trunc_page(physical);
		uncached = ml_io_map_unmappable(physical, size, VM_WIMG_WCOMB);
		if (uncached == 0 || !hakios_wifi_radio_map(physical, size, address)) {
			return NULL;
		}
		bzero((void *)uncached, size);
		say("lx: the radio's %lu MB at %#llx, memory the kernel does not have (wifi_dma_at)\n", size >> 20, physical);
		return (void *)uncached;
	}
	if (kmem_alloc_contig(kernel_map, &cached, size, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		say("lx: no %lu bytes of memory in one piece\n", size);
		return NULL;
	}
	physical = kvtophys(cached);
	bzero((void *)cached, size);
	for (vm_offset_t line = cached; line < cached + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	if (hakios_wifi_radio_coherent()) {
		if (!hakios_wifi_radio_map(physical, size, address)) {
			return NULL;
		}
		say("lx: the radio's memory is cached: its reads and writes look in the caches (wifi_pcie_coherent=1)\n");
		return (void *)cached;
	}
	uncached = ml_io_map_unmappable(physical, size, VM_WIMG_WCOMB);
	if (uncached == 0 || !hakios_wifi_radio_map(physical, size, address)) {
		return NULL;
	}
	return (void *)uncached;
}

void *
lx_os_map_registers(unsigned long long physical, unsigned long size)
{
	unsigned long long page = trunc_page(physical);
	vm_offset_t mapped = ml_io_map_unmappable(page, round_page(physical + size) - page, VM_WIMG_IO);

	return mapped == 0 ? NULL : (void *)(mapped + (physical - page));
}

/* ---- saying things ----------------------------------------------------------------------------------- */

/*
 * The radio's log, kept: what the Linux driver and its bus say (lx_os_print(), lx_os_tell(), hakios_wifi_report_add()),
 * the last REPORT_SIZE bytes of it, whole lines. The kernel's own log is a ring that the boot's chatter wraps before
 * anyone is connected; this is said again on the USB console when it is first up and on its "wifi"
 * (hakios_wifi_report()), with what the driver says of itself at that moment (lx_os_wifi_state()).
 */
#define REPORT_SIZE     (32u * 1024u)
static LCK_GRP_DECLARE(report_group, "hakios-wifi-report");
static LCK_SPIN_DECLARE(report_lock, &report_group);
static char report[REPORT_SIZE];
static uint32_t report_len;
static void (*report_state)(void);
/* what was said when the driver's setup failed (lx_os_failure()): apart, so that the rest of the log does not push it out */
#define FAILURE_SIZE    (16u * 1024u)
static char failure[FAILURE_SIZE];
static uint32_t failure_len;
static bool failure_on;
static uint64_t failure_at;            /* when, in seconds since boot */

void
hakios_wifi_report_add(const char *line)
{
	size_t n = strlen(line);

	if (n == 0) {
		return;
	}
	if (n > REPORT_SIZE / 4) {
		n = REPORT_SIZE / 4;
	}
	lck_spin_lock(&report_lock);
	if (report_len + n > REPORT_SIZE) {
		uint32_t drop = report_len + (uint32_t)n - REPORT_SIZE + REPORT_SIZE / 8;
		char *nl;

		drop = drop < report_len ? drop : report_len;
		nl = memchr(report + drop, '\n', report_len - drop);
		drop = nl != NULL ? (uint32_t)(nl + 1 - report) : report_len;
		memmove(report, report + drop, report_len - drop);
		report_len -= drop;
	}
	memcpy(report + report_len, line, n);
	report_len += (uint32_t)n;
	if (failure_on && failure_len < FAILURE_SIZE) {
		uint32_t room = FAILURE_SIZE - failure_len;

		memcpy(failure + failure_len, line, n < room ? n : room);
		failure_len += (uint32_t)(n < room ? n : room);
	}
	lck_spin_unlock(&report_lock);
}

void
lx_os_failure(bool on)
{
	uint64_t ns;

	lck_spin_lock(&report_lock);
	if (on) {
		failure_len = 0;
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		failure_at = ns / NSEC_PER_SEC;
	}
	failure_on = on;
	lck_spin_unlock(&report_lock);
}

/* lines of a copy of the kept log, one at a time to the loader's log (and so to the USB console) */
static void
report_lines(const char *copy, uint32_t len)
{
	char line[300];
	uint32_t at = 0;

	while (at < len) {
		uint32_t n = 0;

		while (at + n < len && n < sizeof(line) - 2 && copy[at + n] != '\n') {
			n++;
		}
		memcpy(line, copy + at, n);
		line[n] = '\n';
		line[n + 1] = 0;
		loader_log_line(line);
		at += n + (at + n < len && copy[at + n] == '\n' ? 1 : 0);
	}
}

void
lx_os_wifi_state(void (*state)(void))
{
	report_state = state;
}

void
hakios_wifi_report(bool first)
{
	char *copy, line[300];
	uint32_t len, flen;
	uint64_t fat;
	void (*state)(void) = report_state;

	if (first && report_len == 0) {
		return;
	}
	copy = kalloc_data(REPORT_SIZE + FAILURE_SIZE, Z_WAITOK);
	if (copy == NULL) {
		return;
	}
	lck_spin_lock(&report_lock);
	len = report_len;
	memcpy(copy, report, len);
	flen = failure_len;
	memcpy(copy + REPORT_SIZE, failure, flen);
	fat = failure_at;
	lck_spin_unlock(&report_lock);
	snprintf(line, sizeof(line), "wifi: the radio's log so far, %u bytes kept%s:\n", len,
	    first ? " (said again now that the console is up; again with \"wifi\")" : "");
	loader_log_line(line);
	report_lines(copy, len);
	loader_log_line("wifi: (end of what was kept)\n");
	if (flen != 0) {
		snprintf(line, sizeof(line), "wifi: what the driver said of itself when its setup failed, %llu s after boot (%u bytes):\n",
		    fat, flen);
		loader_log_line(line);
		report_lines(copy + REPORT_SIZE, flen);
		loader_log_line("wifi: (end of the failure's account)\n");
	}
	kfree_data(copy, REPORT_SIZE + FAILURE_SIZE);
	if (state != NULL) {
		loader_log_line("wifi: the driver now:\n");
		state();
	} else {
		loader_log_line("wifi: no driver to ask now\n");
	}
}

void
lx_os_print(const char *text)
{
	hakios_wifi_report_add(text);
	say("%s", text);
}

void
lx_os_tell(const char *text)
{
	hakios_wifi_report_add(text);
	hakios_wifi_tell("%s", text);
}

void
lx_os_panic(const char *text)
{
	panic("%s", text);
}

/* ---- time, threads, sleeping --------------------------------------------------------------------------- */

unsigned long long
lx_os_nanoseconds(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds;
}

void *
lx_os_thread_self(void)
{
	return current_thread();
}

struct lx_thread {
	void (*function)(void *);
	void *argument;
};

static void
lx_thread_runs(void *parameter, __unused wait_result_t result)
{
	struct lx_thread start = *(struct lx_thread *)parameter;

	kfree_type(struct lx_thread, parameter);
	start.function(start.argument);
	thread_terminate(current_thread());
}

int
lx_os_thread(const char *name, void (*function)(void *), void *argument)
{
	struct lx_thread *start = kalloc_type(struct lx_thread, Z_WAITOK | Z_ZERO);
	thread_t thread;

	if (start == NULL) {
		return ENOMEM;
	}
	start->function = function;
	start->argument = argument;
	if (kernel_thread_start(lx_thread_runs, start, &thread) != KERN_SUCCESS) {
		kfree_type(struct lx_thread, start);
		return EAGAIN;
	}
	thread_set_thread_name(thread, name);
	thread_deallocate(thread);
	return 0;
}

void
lx_os_lock(void)
{
	lck_mtx_lock(&lx_lock);
}

void
lx_os_unlock(void)
{
	lck_mtx_unlock(&lx_lock);
}

int
lx_os_sleep(const void *address, unsigned long long nanoseconds)
{
	uint64_t deadline;

	if (nanoseconds == 0) {
		return lck_mtx_sleep(&lx_lock, LCK_SLEEP_DEFAULT, (event_t)address, THREAD_UNINT) == THREAD_TIMED_OUT;
	}
	nanoseconds_to_absolutetime(nanoseconds, &deadline);
	deadline += mach_absolute_time();
	return lck_mtx_sleep_deadline(&lx_lock, LCK_SLEEP_DEFAULT, (event_t)address, THREAD_UNINT, deadline) == THREAD_TIMED_OUT;
}

void
lx_os_wakeup(const void *address)
{
	thread_wakeup((event_t)address);
}

/* ---- the rest of the machine ------------------------------------------------------------------------- */

void
lx_os_random(void *buffer, unsigned long size)
{
	read_random(buffer, (u_int)size);
}

unsigned
lx_os_option(const char *name)
{
	return hakios_wifi_option(name);
}

int
lx_os_firmware(const char *name, const void **data, unsigned long *size)
{
	uint64_t length = 0;
	const uint8_t *file = hakios_wifi_driver_file(name, &length);

	if (file == NULL) {
		return ENOENT;
	}
	*data = file;
	*size = (unsigned long)length;
	return 0;
}

int
lx_os_rproc_boot(void)
{
	return hakios_wifi_dsp_is_up() ? 0 : ENXIO;
}

int
lx_os_radio_address(unsigned char *address)
{
	return hakios_wifi_radio_address(address);
}

int
lx_os_network(unsigned char *name, unsigned *name_length, unsigned char *passphrase, unsigned *passphrase_length)
{
	return hakios_wifi_network(name, name_length, passphrase, passphrase_length);
}

/* ---- frames: the radio as the link behind en0 (sys/hakios.net.h) ------------------------------------ */

/*
 * A frame from the network stack comes on whatever thread sends it, and the driver's side keeps
 * a record of every thread that enters it: so frames are put in a queue here, and one thread of
 * ours takes them to the driver. A queue that is full drops what comes (TCP sends it again).
 */
#define LX_QUEUE        256

struct lx_frame {
	uint16_t length;
	uint8_t bytes[HAKIOS_NET_FRAME_MAX];
};

static bool lx_link_attached;
static LCK_MTX_DECLARE(lx_queue_lock, &lx_lock_group);
static struct lx_frame *lx_queue;
static uint32_t lx_queue_in, lx_queue_out;      /* frames put in and taken out, counted from the start */
static uint64_t lx_queue_dropped;

static void
lx_queue_runs(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		struct lx_frame *frame;

		lck_mtx_lock(&lx_queue_lock);
		while (lx_queue_in == lx_queue_out) {
			lck_mtx_sleep(&lx_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&lx_queue_in, THREAD_UNINT);
		}
		frame = &lx_queue[lx_queue_out % LX_QUEUE];     /* (stays ours until lx_queue_out moves) */
		lck_mtx_unlock(&lx_queue_lock);
		lx_transmit(frame->bytes, frame->length);
		lck_mtx_lock(&lx_queue_lock);
		lx_queue_out++;
		lck_mtx_unlock(&lx_queue_lock);
	}
}

static bool
lx_link_transmit(mbuf_t packet, size_t length)
{
	struct lx_frame *frame;

	lck_mtx_lock(&lx_queue_lock);
	if (length > HAKIOS_NET_FRAME_MAX || lx_queue_in - lx_queue_out == LX_QUEUE) {
		lx_queue_dropped++;
		lck_mtx_unlock(&lx_queue_lock);
		return false;
	}
	frame = &lx_queue[lx_queue_in % LX_QUEUE];
	if (mbuf_copydata(packet, 0, length, frame->bytes) != 0) {
		lck_mtx_unlock(&lx_queue_lock);
		return false;
	}
	frame->length = (uint16_t)length;
	lx_queue_in++;
	lck_mtx_unlock(&lx_queue_lock);
	thread_wakeup((event_t)&lx_queue_in);
	return true;
}

/* "wifi stat": the queue to the driver's thread */
static void
lx_link_counts(char *line, size_t room)
{
	snprintf(line, room, "to the radio's driver: %u frames queued, %u taken, %llu dropped (the queue full)", lx_queue_in,
	    lx_queue_out, lx_queue_dropped);
}

static struct hakios_net_link lx_link = {
	.name = "the phone's radio (ath11k)",
	.wireless = true,
	.transmit = lx_link_transmit,
	.counts = lx_link_counts,
};

void
lx_os_net_attach(const unsigned char *address)
{
	thread_t thread;

	if (lx_link_attached) {
		return;
	}
	lx_queue = kalloc_data(LX_QUEUE * sizeof(*lx_queue), Z_WAITOK | Z_ZERO);
	if (lx_queue == NULL || kernel_thread_start(lx_queue_runs, NULL, &thread) != KERN_SUCCESS) {
		say("lx: no memory or no thread for the frames to send: the radio's frames go nowhere\n");
		return;
	}
	thread_set_thread_name(thread, "wifi-send");
	thread_deallocate(thread);
	memcpy(lx_link.address, address, sizeof(lx_link.address));
	lx_link_attached = hakios_net_attach(&lx_link);
	say("lx: %s\n", lx_link_attached ? "the radio is the link behind en0" : "EN0 HAS A LINK ALREADY: the radio's frames go "
	    "nowhere");
}

/*
 * With wifi=1 the interface is made when the kernel starts, before the radio is up: Darwin's Wi-Fi
 * side looks for it and for its registry entry as soon as it runs, once, and a radio that
 * turns up twelve seconds later is one it has already failed to open.
 */
void
hakios_lx_net_early(void)
{
	uint8_t address[6];

	if (hakios_wifi_radio_address(address)) {
		lx_os_net_attach(address);
	}
}

void
lx_os_net_link(int up)
{
	if (lx_link_attached) {
		hakios_net_link_state(up != 0);
	}
}

void
lx_os_net_input(const unsigned char *frame, unsigned long length)
{
	if (lx_link_attached) {
		hakios_net_input(frame, length);        /* (one that is too big is counted, and said, there) */
	}
}

/* ---- the radio's interrupts ------------------------------------------------------------------------------- */

extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
extern void gic_v3_spi_mask(uint32_t intid);
extern void gic_v3_spi_unmask(uint32_t intid);
extern void gic_v3_spi_detach(uint32_t intid);
extern uint64_t gic_v3_spi_count(uint32_t intid);

#define LX_VECTORS      32

static struct {
	void (*function)(void *);
	void *argument;
	bool attached;
} lx_vector[LX_VECTORS];
static uint32_t lx_irq_first;                   /* the GIC's number of vector 0; 0: no interrupts */
static uint32_t lx_irq_pending;                 /* the vectors that have fired and are not dealt with yet */
static uint32_t lx_irq_running;                 /* the vectors whose function the thread is in */
static thread_t lx_irq_thread;
static LCK_MTX_DECLARE(lx_irq_lock, &lx_lock_group);

/* In interrupt context: the vector masked, and the thread told. */
static void
lx_irq_arrived(__unused void *context, uint32_t intid)
{
	gic_v3_spi_mask(intid);
	os_atomic_or(&lx_irq_pending, 1u << (intid - lx_irq_first), relaxed);
	thread_wakeup((event_t)&lx_irq_pending);
}

static void
lx_irq_runs(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		uint32_t pending;

		for (;;) {
			assert_wait((event_t)&lx_irq_pending, THREAD_UNINT);
			if ((pending = os_atomic_xchg(&lx_irq_pending, 0, acquire)) != 0) {
				clear_wait(current_thread(), THREAD_AWAKENED);
				break;
			}
			thread_block(THREAD_CONTINUE_NULL);
		}
		for (unsigned v = 0; v < LX_VECTORS; v++) {
			if (!(pending & 1u << v)) {
				continue;
			}
			lck_mtx_lock(&lx_irq_lock);
			if (!lx_vector[v].attached) {
				lck_mtx_unlock(&lx_irq_lock);
				continue;
			}
			lx_irq_running |= 1u << v;
			lck_mtx_unlock(&lx_irq_lock);
			lx_vector[v].function(lx_vector[v].argument);
			lck_mtx_lock(&lx_irq_lock);
			lx_irq_running &= ~(1u << v);
			lck_mtx_unlock(&lx_irq_lock);
			thread_wakeup((event_t)&lx_irq_running);
		}
	}
}

int
lx_os_irq_msi(unsigned long long *address, unsigned *data)
{
	uint32_t first = 0;

	if (!hakios_wifi_radio_msi(address, data, &first)) {
		return 0;
	}
	lx_irq_first = first;
	return 1;
}

int
lx_os_irq_attach(unsigned vector, void (*function)(void *), void *argument)
{
	kern_return_t kr;

	if (vector >= LX_VECTORS || lx_irq_first == 0 || function == NULL) {
		return EINVAL;
	}
	if (lx_irq_thread == THREAD_NULL) {
		hakios_wifi_tell("lx: wifi_irq: THE GIC'S DISTRIBUTOR IS WRITTEN for the radio's interrupts: each vector's SPI (%u + v) edge-"
		    "triggered, group 1, routed to this processor, then enabled\n", lx_irq_first);
		if (kernel_thread_start(lx_irq_runs, NULL, &lx_irq_thread) != KERN_SUCCESS) {
			lx_irq_thread = THREAD_NULL;
			return ENOMEM;
		}
		thread_set_thread_name(lx_irq_thread, "wifi-irq");
	}
	lck_mtx_lock(&lx_irq_lock);
	lx_vector[vector].function = function;
	lx_vector[vector].argument = argument;
	lx_vector[vector].attached = true;
	lck_mtx_unlock(&lx_irq_lock);
	if ((kr = gic_v3_spi_attach(lx_irq_first + vector, true, lx_irq_arrived, NULL)) != KERN_SUCCESS) {
		lck_mtx_lock(&lx_irq_lock);
		lx_vector[vector].attached = false;
		lck_mtx_unlock(&lx_irq_lock);
		return kr == KERN_NAME_EXISTS ? EBUSY : EINVAL;
	}
	gic_v3_spi_mask(lx_irq_first + vector);
	return 0;
}

void
lx_os_irq_enable(unsigned vector)
{
	if (vector < LX_VECTORS && lx_vector[vector].attached) {
		gic_v3_spi_unmask(lx_irq_first + vector);
	}
}

void
lx_os_irq_disable(unsigned vector)
{
	if (vector >= LX_VECTORS || !lx_vector[vector].attached) {
		return;
	}
	gic_v3_spi_mask(lx_irq_first + vector);
	if (current_thread() == lx_irq_thread) {
		return;                         /* (from the vector's own function, or another's: nothing to wait for) */
	}
	lck_mtx_lock(&lx_irq_lock);
	while (lx_irq_running & 1u << vector) {
		lck_mtx_sleep(&lx_irq_lock, LCK_SLEEP_DEFAULT, (event_t)&lx_irq_running, THREAD_UNINT);
	}
	lck_mtx_unlock(&lx_irq_lock);
}

void
lx_os_irq_detach_all(void)
{
	for (unsigned v = 0; v < LX_VECTORS; v++) {
		if (lx_vector[v].attached) {
			gic_v3_spi_detach(lx_irq_first + v);
		}
	}
	lck_mtx_lock(&lx_irq_lock);
	for (unsigned v = 0; v < LX_VECTORS; v++) {
		lx_vector[v].attached = false;
	}
	while (lx_irq_running != 0 && current_thread() != lx_irq_thread) {
		lck_mtx_sleep(&lx_irq_lock, LCK_SLEEP_DEFAULT, (event_t)&lx_irq_running, THREAD_UNINT);
	}
	lck_mtx_unlock(&lx_irq_lock);
	os_atomic_store(&lx_irq_pending, 0, relaxed);
}

unsigned long long
lx_os_irq_count(unsigned vector)
{
	return vector < LX_VECTORS && lx_irq_first != 0 ? gic_v3_spi_count(lx_irq_first + vector) : 0;
}

/* ---- the station (sys/hakios.sta.h) --------------------------------------------------------------------- */

void
lx_os_station(const struct hakios_sta_ops *ops)
{
	hakios_sta_attach(ops);
}

void
lx_os_station_lost(int error)
{
	hakios_sta_lost(error);
}

/* ---- the driver ---------------------------------------------------------------------------------------- */

/* The object has its own; this is the one of a kernel built without it. */
__attribute__((weak)) int
lx_start(void)
{
	say("lx: THE DRIVER IS NOT BUILT IN: the selected external Wi-Fi object was missing when this kernel was built\n");
	return ENXIO;
}

__attribute__((weak)) void
lx_stop(void)
{
}

__attribute__((weak)) int
lx_prepare(void)
{
	return 0;
}

__attribute__((weak)) int
lx_transmit(__unused const unsigned char *frame, __unused unsigned long length)
{
	return ENXIO;
}

int
hakios_lx_start(void)
{
	return lx_start();
}

void
hakios_lx_stop(void)
{
	lx_stop();
}

int
hakios_lx_prepare(void)
{
	return lx_prepare();
}
