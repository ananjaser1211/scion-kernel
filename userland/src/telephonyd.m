/*
 * telephonyd: the stand-in for CommCenter's two client services, so that CoreTelephony's clients find a
 * telephony server that says "no SIM, no service" instead of no server at all.
 *
 * Apple's CommCenter stays out (userland/mkjobs.py's DENY: it needs an Apple baseband on PCIe and dies on
 * AppleBasebandPCI / AppleIPAppender). Without anything behind its services every CoreTelephony client fails
 * its lookups, 190-200 of them a session, and some do worse than fail: SpringBoard's SBTelephonyManager treats
 * a failed CTServerConnection call as a broken connection ("CTServerConnection returned error: (2, 5).
 * Destroying connection"), builds a new one and asks again, about once a second for as long as the display is
 * on, and the idle timer is reset each time -- the screen never turns itself off. This program serves the two
 * services that CoreTelephony.framework (Darwin userland 16.5, 20F66) connects to:
 *
 *   com.apple.commcenter.xpc                  _CTServerConnection*: plain XPC. A request is a dictionary with
 *                                             "kRequest" = the request's name (kRegistrationGetProperty,
 *                                             kPacketCreateDormancySuspendAssertion, kWhois, kPing, ... about
 *                                             270 names) and its own keys; the answer is a dictionary, sent
 *                                             as the reply when one is expected. CoreTelephony reads an
 *                                             answer as an error only if it is not a dictionary (that is
 *                                             the (2, 5) above: kCTErrorDomainMach, KERN_FAILURE) or it
 *                                             carries "kPosixError" / "kMachError"; otherwise it takes the
 *                                             keys it wants from it, and a key that is not there is "no
 *                                             value" (no country code, no operator, not in emergency
 *                                             callback mode). So every request is answered, with an empty
 *                                             dictionary unless serve_ctserver() knows better. An
 *                                             assertion (dormancy suspend, keep-alive, ...) is a
 *                                             connection of its own that lives as long as it is held.
 *
 *   com.apple.commcenter.coretelephony.xpc    CoreTelephonyClient (CoreTelephonyClientMux): NSXPC, the protocol
 *                                             CTXPCServiceInterface (31 protocols, 384 methods in 16.5). The
 *                                             interface is made the way the client makes it (-[CoreTelephonyClientMux
 *                                             _connect_sync]: the protocol, plus the property-list classes for
 *                                             the carrier bundle and preference values), and each method is
 *                                             answered from its own signature, read from the protocol at
 *                                             run time (the extended method types): every method but one
 *                                             has a reply block, and the reply block is always called.
 *                                             The answers that set the "no SIM" state are given in full --
 *                                             getSubscriptionInfo: and getActiveContexts: with no
 *                                             subscriptions, getDescriptorsForDomain:completion: with no
 *                                             descriptors, getDualSimCapability: "not supported", ping: and
 *                                             registerForNotifications:completion: as done. With no
 *                                             subscription a client has no context to ask about, and the
 *                                             rest (each one about a SIM, a carrier, a network, a call) is
 *                                             answered as a device without cellular hardware would be:
 *                                             no value (nil, NO, 0) and an NSError, NSPOSIXErrorDomain
 *                                             ENODEV. Nothing is ever sent to the clients unasked.
 *
 * The other services in CommCenter's plist (cupolicy, carrierspace, atcs, the CellularPlan daemon, ...) are
 * asked for a handful of times a session and are left alone.
 *
 * The modem: this is meant to become the place where the phone's own modem state reaches CoreTelephony. The
 * kernel's modem drivers publish it as sysctl kern.hakios.cellular_* (drivers/modem/hakios.mpss.c on redwood;
 * the star2lte Shannon driver will, docs/cellular-star2lte.md), and modem_read() below reads them. Today
 * nothing there says that a SIM is in, so have_subscription() is false and every answer is "no SIM"; that
 * function and the two subscription answers are where a real subscription goes once the kernel has one.
 *
 *   telephonyd            serve (launchd: com.hakios.telephonyd.plist)
 *   kill -USR1 <pid>      say the counts: every request and method asked so far, how often, and by whom
 */
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

/* ---- libxpc: the SDK carries no headers for it, the library is in libSystem (as in gnssd.m) ---------------- */

typedef void *xpc_object_t;
typedef xpc_object_t xpc_connection_t;
typedef const struct _xpc_type_s *xpc_type_t;
typedef void (^xpc_handler_t)(xpc_object_t);
typedef bool (^xpc_dictionary_applier_t)(const char *, xpc_object_t);
extern const struct _xpc_type_s _xpc_type_connection, _xpc_type_dictionary, _xpc_type_error;
#define XPC_TYPE_CONNECTION (&_xpc_type_connection)
#define XPC_TYPE_DICTIONARY (&_xpc_type_dictionary)
#define XPC_TYPE_ERROR (&_xpc_type_error)
#define XPC_CONNECTION_MACH_SERVICE_LISTENER (1 << 0)
xpc_connection_t xpc_connection_create_mach_service(const char *, dispatch_queue_t, uint64_t);
void xpc_connection_set_event_handler(xpc_connection_t, xpc_handler_t);
void xpc_connection_resume(xpc_connection_t);
void xpc_connection_send_message(xpc_connection_t, xpc_object_t);
pid_t xpc_connection_get_pid(xpc_connection_t);
xpc_type_t xpc_get_type(xpc_object_t);
void xpc_release(xpc_object_t);
xpc_object_t xpc_dictionary_create_reply(xpc_object_t);
const char *xpc_dictionary_get_string(xpc_object_t, const char *);
bool xpc_dictionary_apply(xpc_object_t, xpc_dictionary_applier_t);

/* libobjc's, as the runtime has it: a protocol method's types with the class names and the blocks' own signatures */
const char *_protocol_getMethodTypeEncoding(Protocol *, SEL, BOOL, BOOL);
/* libproc's */
int proc_name(int pid, void *buffer, uint32_t size);

static void
say(const char *fmt, ...)
{
	va_list ap;
	time_t t = time(NULL);
	char ts[16];

	strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
	fprintf(stderr, "telephonyd %s: ", ts);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static NSString *
process_name(pid_t pid)
{
	char name[64] = "";

	if (pid <= 0 || proc_name(pid, name, sizeof(name)) <= 0) {
		return [NSString stringWithFormat:@"pid %d", pid];
	}
	return [NSString stringWithFormat:@"%s[%d]", name, pid];
}

/* ---- what is asked: counted, the first of each said, all of them said on SIGUSR1 ----------------------- */

static NSMutableDictionary<NSString *, NSNumber *> *asked;            /* "service what" -> count */
static NSMutableDictionary<NSString *, NSMutableSet<NSString *> *> *askers;  /* "service what" -> process names */

static void
count(const char *service, NSString *what, pid_t pid)
{
	NSString *key = [NSString stringWithFormat:@"%s %@", service, what];
	NSString *who = process_name(pid);
	bool first;

	@synchronized(asked) {
		first = asked[key] == nil;
		asked[key] = @(asked[key].unsignedLongValue + 1);
		if (askers[key] == nil) {
			askers[key] = [NSMutableSet set];
		}
		if (askers[key].count < 12) {
			[askers[key] addObject:[who componentsSeparatedByString:@"["][0]];
		}
	}
	if (first) {
		say("%s: %s (first from %s)", service, what.UTF8String, who.UTF8String);
	}
}

static void
say_counts(void)
{
	@synchronized(asked) {
		NSArray *keys = [asked keysSortedByValueUsingComparator:^NSComparisonResult(NSNumber *a, NSNumber *b) {
			return [b compare:a];
		}];

		say("%lu kinds of request so far:", (unsigned long)keys.count);
		for (NSString *key in keys) {
			say("  %6lu  %s  (%s)", asked[key].unsignedLongValue, key.UTF8String,
			    [askers[key].allObjects componentsJoinedByString:@" "].UTF8String);
		}
	}
}

/* ---- the modem, as the kernel publishes it ---------------------------------------------------------------- */

struct modem {
	bool published;                 /* the kernel has kern.hakios.cellular_* at all */
	int up, registered, bars, dbm, radio;
	char operator_[48];             /* "MCC MNC name" of the serving network, "" when none */
};

static int
sysctl_int(const char *name, bool *found)
{
	int value = 0;
	size_t size = sizeof(value);

	if (sysctlbyname(name, &value, &size, NULL, 0) != 0) {
		return 0;
	}
	*found = true;
	return value;
}

static void
modem_read(struct modem *m)
{
	size_t size = sizeof(m->operator_) - 1;

	memset(m, 0, sizeof(*m));
	m->up = sysctl_int("kern.hakios.cellular_up", &m->published);
	m->registered = sysctl_int("kern.hakios.cellular_registered", &m->published);
	m->bars = sysctl_int("kern.hakios.cellular_bars", &m->published);
	m->dbm = sysctl_int("kern.hakios.cellular_dbm", &m->published);
	m->radio = sysctl_int("kern.hakios.cellular_radio", &m->published);
	if (sysctlbyname("kern.hakios.cellular_operator", m->operator_, &size, NULL, 0) != 0) {
		m->operator_[0] = '\0';
	}
}

/*
 * Whether there is a subscription (a SIM) to tell the clients about. The kernel publishes the modem's
 * registration and signal, not a SIM's identity or state, so the answer is no for now: every client sees a
 * device with no SIM. When the kernel publishes the SIM (and telephonyd can say its slot, its ICCID-less
 * context and its home network), this is the switch, and getSubscriptionInfo: / getActiveContexts: below
 * are where the subscription is described.
 */
static bool
have_subscription(const struct modem *m)
{
	(void)m;
	return false;
}

/* ---- com.apple.commcenter.xpc: _CTServerConnection ---------------------------------------------------- */

/*
 * The answer to one request. Empty is "no value" for every property a request can ask for (see the top of
 * this file); requests that need more than that go here.
 */
static void
serve_ctserver(const char *request, xpc_object_t message, xpc_object_t reply)
{
	(void)request;
	(void)message;
	(void)reply;
}

static void
listen_ctserver(dispatch_queue_t queue)
{
	static const char service[] = "com.apple.commcenter.xpc";
	xpc_connection_t listener = xpc_connection_create_mach_service(service, queue, XPC_CONNECTION_MACH_SERVICE_LISTENER);

	xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) {
			return;
		}
		xpc_connection_set_event_handler(peer, ^(xpc_object_t message) {
			const char *request;
			xpc_object_t reply;

			if (xpc_get_type(message) != XPC_TYPE_DICTIONARY) {
				return;                 /* XPC_ERROR_CONNECTION_INVALID: the client (or its assertion) went away */
			}
			request = xpc_dictionary_get_string(message, "kRequest");
			@autoreleasepool {
				NSMutableArray *keys = [NSMutableArray array];

				xpc_dictionary_apply(message, ^bool(const char *key, xpc_object_t value) {
					(void)value;
					if (strcmp(key, "kRequest") != 0) {
						[keys addObject:@(key)];
					}
					return true;
				});
				[keys sortUsingSelector:@selector(compare:)];
				count(service, [NSString stringWithFormat:@"%s%s%@", request ? request : "(no kRequest)",
				                keys.count ? " " : "", [keys componentsJoinedByString:@","]],
				      xpc_connection_get_pid(peer));
			}
			reply = xpc_dictionary_create_reply(message);
			if (reply == NULL) {
				return;                 /* a message that wants no answer (kWhois, kRegisterNotification, ...) */
			}
			serve_ctserver(request ? request : "", message, reply);
			xpc_connection_send_message(peer, reply);
			xpc_release(reply);
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(listener);
}

/* ---- com.apple.commcenter.coretelephony.xpc: CoreTelephonyClient ------------------------------------- */

#define MAX_REPLY_ARGS 6

/* One method of CTXPCServiceInterface, from the protocol's extended types. */
@interface TelephonyMethod : NSObject {
@public
	SEL selector;
	const char *types;              /* the plain types, for the invocation */
	int reply;                      /* the reply block's index in the invocation (2 = the first argument); -1: none */
	int replyCount;                 /* the reply block's arguments */
	char replyType[MAX_REPLY_ARGS]; /* each one's type: '@' an object, 'B' a BOOL, one of "cCsSiIlLqQ" a number */
	NSString *replyClass[MAX_REPLY_ARGS];  /* an object's class, where the protocol names it */
}
@end
@implementation TelephonyMethod
@end

static NSMutableDictionary<NSString *, TelephonyMethod *> *methods;   /* selector name -> method */

/*
 * The reply block's arguments, from an extended method type such as
 *   v32@0:8@"CTXPCServiceSubscriptionContext"16@?<v@?@"NSString"@"NSError">24
 * The block's own signature is inside "@?<...>": "v@?" (it returns nothing, and is itself the first
 * argument), then its arguments: @"Class", @, or one character for a number or a BOOL.
 */
static bool
parse_reply(TelephonyMethod *m, const char *extended)
{
	const char *p = strstr(extended, "@?<"), *end;

	if (p == NULL || strstr(p + 3, "@?<") != NULL) {
		return false;                   /* no reply block, or more than one: none in 16.5 */
	}
	p += 3;
	if (strncmp(p, "v@?", 3) != 0) {
		return false;
	}
	p += 3;
	end = strchr(p, '>');
	if (end == NULL) {
		return false;
	}
	m->replyCount = 0;
	while (p < end) {
		if (m->replyCount == MAX_REPLY_ARGS) {
			return false;
		}
		if (*p == '@') {
			m->replyType[m->replyCount] = '@';
			p++;
			if (*p == '"') {
				const char *close = strchr(p + 1, '"');

				if (close == NULL || close > end) {
					return false;
				}
				m->replyClass[m->replyCount] = [[NSString alloc] initWithBytes:p + 1 length:close - p - 1
				                                                     encoding:NSUTF8StringEncoding];
				p = close + 1;
			}
		} else if (strchr("BcCsSiIlLqQ", *p) != NULL) {
			m->replyType[m->replyCount] = *p++;
		} else {
			return false;                   /* a float, a structure, a pointer: none in 16.5 */
		}
		m->replyCount++;
	}
	return true;
}

static void
learn_protocol(Protocol *protocol)
{
	unsigned count = 0;
	Protocol *__unsafe_unretained *incorporated = protocol_copyProtocolList(protocol, &count);

	for (unsigned i = 0; i < count; i++) {
		if (strcmp(protocol_getName(incorporated[i]), "NSObject") != 0) {
			learn_protocol(incorporated[i]);
		}
	}
	free(incorporated);
	for (int required = 1; required >= 0; required--) {
		struct objc_method_description *list = protocol_copyMethodDescriptionList(protocol, required, YES, &count);

		for (unsigned i = 0; i < count; i++) {
			TelephonyMethod *m = [TelephonyMethod new];
			const char *extended = _protocol_getMethodTypeEncoding(protocol, list[i].name, required, YES);
			NSMethodSignature *signature = [NSMethodSignature signatureWithObjCTypes:list[i].types];

			m->selector = list[i].name;
			m->types = list[i].types;
			m->reply = -1;
			for (NSUInteger a = 2; a < signature.numberOfArguments; a++) {
				if (strcmp([signature getArgumentTypeAtIndex:a], "@?") == 0) {
					m->reply = (int)a;
				}
			}
			if (m->reply >= 0 && (extended == NULL || !parse_reply(m, extended))) {
				say("%s: a reply this program cannot make (%s); it will not be answered", sel_getName(list[i].name),
				    extended ? extended : list[i].types);
				m->reply = -1;
			}
			methods[@(sel_getName(list[i].name))] = m;
		}
		free(list);
	}
}

/* What a method is answered with: the reply's first object that is not the error, its first number or BOOL, and whether it is an error. */
struct answer {
	__unsafe_unretained id object;
	long number;
	bool failed;
};

static int dual_sim_not_supported;

/*
 * The answers that say "no SIM" in full. A method not named here is about a subscription, a carrier, a
 * network or a call, which this device has none of: no value and an error.
 */
static struct answer
answer_for(NSString *name, const struct modem *modem)
{
	static id subscription_info, contexts, descriptors;
	static dispatch_once_t once;
	struct answer answer = { nil, 0, true };

	if (have_subscription(modem)) {
		/* never yet: a subscription's answers (its context, its SIM, its network) are to be written here */
	}
	dispatch_once(&once, ^{
		id info = [objc_getClass("CTXPCServiceSubscriptionInfo") new];
		id all = [objc_getClass("CTXPCContexts") new];

		((void (*)(id, SEL, id))objc_msgSend)(info, @selector(setSubscriptions:), @[]);
		((void (*)(id, SEL, id))objc_msgSend)(info, sel_registerName("setSubscriptionsInUse:"), @[]);
		((void (*)(id, SEL, id))objc_msgSend)(info, sel_registerName("setSubscriptionsValid:"), @[]);
		((void (*)(id, SEL, id))objc_msgSend)(all, @selector(setSubscriptions:), @[]);
		subscription_info = info;
		contexts = all;
		descriptors = ((id (*)(id, SEL, id))objc_msgSend)([objc_getClass("CTServiceDescriptorContainer") alloc],
		    sel_registerName("initWithDescriptors:"), @[]);
	});
	if ([name isEqualToString:@"ping:"] || [name isEqualToString:@"registerForNotifications:completion:"]) {
		answer.failed = false;
	} else if ([name isEqualToString:@"getSubscriptionInfo:"]) {
		answer = (struct answer){ subscription_info, 0, false };
	} else if ([name isEqualToString:@"getActiveContexts:"]) {
		answer = (struct answer){ contexts, 0, false };
	} else if ([name isEqualToString:@"getDescriptorsForDomain:completion:"]) {
		answer = (struct answer){ descriptors, 0, false };
	} else if ([name isEqualToString:@"getDualSimCapability:"]) {
		answer = (struct answer){ nil, dual_sim_not_supported, false };
	}
	return answer;
}

/* A block's own function: called with the block first, then its arguments (as inject_sb.c's BLOCK_CALL). */
struct block_layout {
	void *isa;
	int flags, reserved;
	void (*invoke)(void *, ...);
};

/*
 * CTXPCServiceInterface's exported object. It implements none of the methods itself: NSXPC decodes a message
 * against the protocol, finds no method and forwards the invocation, and forwardInvocation: answers it.
 */
@interface TelephonyService : NSObject <NSXPCListenerDelegate>
@end

@implementation TelephonyService

- (BOOL)respondsToSelector:(SEL)selector
{
	return methods[@(sel_getName(selector))] != nil || [super respondsToSelector:selector];
}

- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
	TelephonyMethod *m = methods[@(sel_getName(selector))];

	return m != nil ? [NSMethodSignature signatureWithObjCTypes:m->types] : [super methodSignatureForSelector:selector];
}

- (void)forwardInvocation:(NSInvocation *)invocation
{
	NSString *name = @(sel_getName(invocation.selector));
	TelephonyMethod *m = methods[name];
	__unsafe_unretained id block = nil;
	struct modem modem;
	struct answer answer;
	NSError *error = nil;
	long arguments[MAX_REPLY_ARGS] = { 0 };
	bool object_given = false, number_given = false;

	count("com.apple.commcenter.coretelephony.xpc", name, NSXPCConnection.currentConnection.processIdentifier);
	if (m == nil || m->reply < 0) {
		return;                         /* SIMUnlockProcedureDidComplete: told, not asked */
	}
	[invocation getArgument:&block atIndex:m->reply];
	if (block == nil) {
		return;
	}
	modem_read(&modem);
	answer = answer_for(name, &modem);
	if (answer.failed) {
		error = [NSError errorWithDomain:NSPOSIXErrorDomain code:ENODEV
		                        userInfo:@{ NSLocalizedDescriptionKey: @"No cellular service on this device (telephonyd)" }];
	}
	for (int i = 0; i < m->replyCount; i++) {
		if (m->replyType[i] == '@') {
			if ([m->replyClass[i] isEqualToString:@"NSError"]) {
				arguments[i] = (long)(__bridge void *)error;
			} else if (!object_given) {
				object_given = true;
				arguments[i] = (long)(__bridge void *)answer.object;
			}
		} else if (!number_given) {
			number_given = true;
			arguments[i] = answer.number;   /* a BOOL is 0 or 1 already, and goes in a register like any other */
		}
	}
	/* every argument of every reply in 16.5 is an object, a number or a BOOL: each goes in a register of its own (x1..x6) */
	((void (*)(void *, long, long, long, long, long, long))((struct block_layout *)(__bridge void *)block)->invoke)(
	    (__bridge void *)block, arguments[0], arguments[1], arguments[2], arguments[3], arguments[4], arguments[5]);
}

- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)connection
{
	static NSXPCInterface *exported, *remote;
	static dispatch_once_t once;

	(void)listener;
	dispatch_once(&once, ^{
		/* as -[CoreTelephonyClientMux _connect_sync] makes it: the protocol, and the property-list values */
		NSSet *values = [NSSet setWithObjects:NSArray.class, NSDictionary.class, NSString.class, NSNumber.class,
		                 NSData.class, nil];
		static const struct { const char *selector; NSUInteger index; BOOL reply; } classes[] = {
			{ "copyCarrierBundleValue:key:bundleType:completion:", 0, YES },
			{ "copyCarrierBundleValue:keyHierarchy:bundleType:completion:", 1, NO },
			{ "copyCarrierBundleValue:keyHierarchy:bundleType:completion:", 0, YES },
			{ "copyCarrierBundleValueWithDefault:key:bundleType:completion:", 0, YES },
			{ "copyCarrierBundleValueWithDefault:keyHierarchy:bundleType:completion:", 1, NO },
			{ "copyCarrierBundleValueWithDefault:keyHierarchy:bundleType:completion:", 0, YES },
			{ "copyCarrierBundleValueWithCountryBundleLookup:keyHierarchy:matchingInfo:completion:", 1, NO },
			{ "getWiFiCallingSettingPreferences:key:completion:", 0, YES },
			{ "setWiFiCallingSettingPreferences:key:value:completion:", 2, NO },
			{ "evaluateMobileSubscriberIdentity:identity:completion:", 1, NO },
			{ "setPrefForKey:heirarchy:value:storage:completion:", 2, NO },
			{ "getPrefForKey:heirarchy:storage:completion:", 0, YES },
			{ "getRadioPersonality:completion:", 0, YES },
		};

		exported = [NSXPCInterface interfaceWithProtocol:objc_getProtocol("CTXPCServiceInterface")];
		for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); i++) {
			SEL selector = sel_registerName(classes[i].selector);

			if (methods[@(classes[i].selector)] != nil) {
				[exported setClasses:values forSelector:selector argumentIndex:classes[i].index ofReply:classes[i].reply];
			}
		}
		remote = [NSXPCInterface interfaceWithProtocol:objc_getProtocol("CoreTelephonyClientDelegateProxyInterface")];
	});
	connection.exportedInterface = exported;
	connection.exportedObject = self;
	connection.remoteObjectInterface = remote;    /* (never called: nothing is sent unasked) */
	[connection resume];
	return YES;
}

@end

static void
listen_coretelephony(TelephonyService *service)
{
	static NSXPCListener *listener;

	/* the SDK marks it unavailable on iOS, where Apple's daemons use it all the same (as in passcoded.m) */
	listener = ((NSXPCListener * (*)(id, SEL, NSString *))objc_msgSend)([NSXPCListener alloc],
	    @selector(initWithMachServiceName:), @"com.apple.commcenter.coretelephony.xpc");
	listener.delegate = service;
	[listener resume];
}

int
main(void)
{
	@autoreleasepool {
		static TelephonyService *service;
		static dispatch_source_t usr1;
		Protocol *protocol = objc_getProtocol("CTXPCServiceInterface");
		struct modem modem;

		setvbuf(stderr, NULL, _IOLBF, 0);
		asked = [NSMutableDictionary dictionary];
		askers = [NSMutableDictionary dictionary];
		methods = [NSMutableDictionary dictionary];
		if (protocol == NULL || objc_getClass("CTXPCServiceSubscriptionInfo") == Nil) {
			say("CoreTelephony has no CTXPCServiceInterface: nothing to serve");
			return 1;
		}
		learn_protocol(protocol);
		/* kCTServerConnectionDualSimCapability* by number (CoreTelephony's, not in the SDK's stubs) */
		const char *(*capability_name)(int) = dlsym(RTLD_DEFAULT, "CTServerConnectionDualSimCapabilityAsString");
		for (int i = 0; capability_name != NULL && i < 8; i++) {
			const char *s = capability_name(i);

			if (s != NULL && strcmp(s, "kCTServerConnectionDualSimCapabilityNotSupported") == 0) {
				dual_sim_not_supported = i;
			}
		}

		service = [TelephonyService new];
		listen_ctserver(dispatch_queue_create("com.hakios.telephonyd.ctserver", DISPATCH_QUEUE_SERIAL));
		listen_coretelephony(service);

		signal(SIGUSR1, SIG_IGN);
		usr1 = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGUSR1, 0, dispatch_get_main_queue());
		dispatch_source_set_event_handler(usr1, ^{ say_counts(); });
		dispatch_resume(usr1);

		modem_read(&modem);
		say("serving com.apple.commcenter.xpc and com.apple.commcenter.coretelephony.xpc (%lu methods): no SIM; "
		    "the kernel's modem: %s", (unsigned long)methods.count,
		    !modem.published ? "not published" :
		    [NSString stringWithFormat:@"up %d, registered %d, %d bars", modem.up, modem.registered, modem.bars].UTF8String);
	}
	dispatch_main();
}
