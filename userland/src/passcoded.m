/*
 * passcoded: the stand-in for keybagd's XPC service (com.apple.mobile.keybagd.xpc), so that a passcode
 * can be set, changed and removed.
 *
 * Apple's keybagd keeps the system keybag (/private/var/keybags/systembag.kb) and changes its secret in
 * the Secure Enclave. Neither exists here (userland/mkjobs.py keeps keybagd out), and the kernel's
 * key store stand-in (scion/xnu-8796:iokit/Kernel/MiniAppleKeyStore.cpp) holds the passcode's verifier
 * itself. What reaches keybagd from the rest of the system goes through MobileKeyBag.framework's
 * MKBKeyStoreDevice, an NSXPCConnection to com.apple.mobile.keybagd.xpc with the protocol
 * KBXPCProtocol. This program serves that protocol; the methods that matter for a passcode are passed
 * on to the kernel, the rest answer as a device without those features would:
 *
 *   changeSystemSecretfromOldSecret:...      MKBKeyBagChangeSystemSecret (ManagedConfiguration's
 *   changeSystemSecretWithEscrow:...         profiled, for Settings' Turn Passcode On / Change / Off):
 *                                            the kernel's kHKChangeSecret, with the opaque data
 *   getPasscodeBlobWithReply:                MKBKeyBagCopySytemSecretBlob: the opaque data (MC reads the
 *                                            passcode's kind from it, and the lock screen its keypad)
 *   getDeviceLockState:needsExtended:        MKBGetDeviceLockStateIndirect
 *   passcodeUnlockStart/Success/Failed       told by MKBUnlockDevice; nothing to do
 *   everything else                          an error, or "nothing", as the method's reply allows
 *
 * The protocol's methods and their argument types are MobileKeyBag's (its protocol's extended method
 * types, Darwin userland 16.5); the secrets arrive as file handles with their sizes.
 *
 * It also turns the key store's IOGeneralInterest messages into the notifications keybagd posts:
 * com.apple.mobile.keybagd.lock_status on every change of the lock state, and
 * com.apple.mobile.keybagd.first_unlock on the first unlock of a boot.
 *
 * Nothing secret is logged. Run as root by launchd (com.hakios.passcoded.plist), on demand and at load.
 */
#import <Foundation/Foundation.h>
#include <IOKit/IOKitLib.h>
#include <notify.h>
#include <objc/message.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

#define kHKChangeSecret 0x100
#define kHKCopyBlob     0x101
#define kHKStatus       0x102
#define kHKChangeMagic  0x484b6373
#define kHKMaxSecret    256
#define kHKMaxBlob      2048

#define kAKSMessageStatusChanged 0xe3ffd00d
#define kAKSMessageFirstUnlock   0xe3ffd00f

struct HKChangeSecret {
	uint32_t magic, oldLength, newLength, blobLength;
};

@protocol KBXPCProtocol <NSObject>
- (void)Event:(int)event;
- (void)SeshatDebugWithDebugMask:(int)mask withReply:(void (^)(NSError *))reply;
- (void)passcodeUnlockSuccessWithReply:(void (^)(NSError *))reply;
- (void)SeshatEnrollWithSecret:(NSFileHandle *)secret secretSize:(unsigned long long)size withReply:(void (^)(NSError *))reply;
- (void)SeshatRecoverWithSecret:(NSFileHandle *)secret secretSize:(unsigned long long)size withReply:(void (^)(int, NSError *))reply;
- (void)SeshatUnlockWithSecret:(NSFileHandle *)secret secretSize:(unsigned long long)size withReply:(void (^)(int, NSError *))reply;
- (void)backupUUIDForVolume:(NSData *)volume reply:(void (^)(NSData *, NSError *))reply;
- (void)changeClassKeysGenerationWithSecret:(NSFileHandle *)secret secretSize:(unsigned long long)size generationOption:(int)option reply:(void (^)(NSError *))reply;
- (void)changeSystemSecretWithEscrow:(NSData *)escrow fromOldSecret:(NSFileHandle *)oldSecret oldSize:(unsigned long long)oldSize toNewSecret:(NSFileHandle *)newSecret newSize:(unsigned long long)newSize opaqueData:(NSData *)opaque keepstate:(BOOL)keep reply:(void (^)(NSError *))reply;
- (void)changeSystemSecretfromOldSecret:(NSFileHandle *)oldSecret oldSize:(unsigned long long)oldSize toNewSecret:(NSFileHandle *)newSecret newSize:(unsigned long long)newSize opaqueData:(NSData *)opaque reply:(void (^)(NSError *))reply;
- (void)createKeybagForUserSession:(NSDictionary *)session withSessionUID:(int)uid WithSecret:(NSFileHandle *)secret secretSize:(unsigned long long)size withGracePeriod:(int)grace withOpaqeStuff:(NSData *)opaque withReply:(void (^)(NSError *))reply;
- (void)createPersonaKeyForUserSession:(NSString *)session forPath:(NSString *)path withUID:(unsigned int)uid WithSecret:(NSFileHandle *)secret secretSize:(unsigned long long)size withReply:(void (^)(NSError *))reply;
- (void)createSyncBagForUserSession:(NSDictionary *)session withSessionUID:(int)uid withReply:(void (^)(NSError *))reply;
- (void)deleteKeybagForUserSession:(int)uid withReply:(void (^)(NSError *))reply;
- (void)disableBackupForVolume:(NSData *)volume reply:(void (^)(NSError *))reply;
- (void)enableBackupForVolume:(NSData *)volume withSecret:(NSFileHandle *)secret secretSize:(unsigned long long)size reply:(void (^)(NSData *, NSError *))reply;
- (void)getBackupBlobreply:(void (^)(NSData *, NSError *))reply;
- (void)getBackupkeyForVolume:(NSData *)volume andCryptoID:(unsigned long long)cryptoID withReply:(void (^)(NSData *, NSError *))reply;
- (void)getDeviceLockState:(int)uid needsExtended:(BOOL)extended withReply:(void (^)(int, NSError *))reply;
- (void)getLockSateInfoforUser:(int)uid WithReply:(void (^)(NSDictionary *, NSError *))reply;
- (void)getPasscodeBlobWithReply:(void (^)(NSData *, NSError *))reply;
- (void)isKeyRollingInProgresswithreply:(void (^)(int, NSError *))reply;
- (void)loadKeybagForUserSession:(NSDictionary *)session withSessionID:(int)sid withSecret:(NSFileHandle *)secret secretSize:(unsigned long long)size shouldSetGracePeriod:(BOOL)setGrace withGracePeriod:(int)grace isInEarlyStar:(BOOL)early withReply:(void (^)(NSError *))reply;
- (void)loadSyncBagForUserSession:(NSDictionary *)session withSessionUID:(int)uid withReply:(void (^)(NSError *))reply;
- (void)migrationWithReply:(void (^)(NSError *))reply;
- (void)passcodeUnlockFailedWithReply:(void (^)(NSError *))reply;
- (void)passcodeUnlockStartWithReply:(void (^)(NSError *))reply;
- (void)registerBackupBag:(NSData *)bag withSecret:(NSFileHandle *)secret secretSize:(unsigned long long)size reply:(void (^)(NSError *))reply;
- (void)removePersonaKeyForUserSession:(NSString *)session withUID:(unsigned int)uid withVolumeUUIDString:(NSString *)volume withReply:(void (^)(NSError *))reply;
- (void)removeSyncBagForUserSession:(NSDictionary *)session withSessionUID:(int)uid withReply:(void (^)(NSError *))reply;
- (void)setVolumeToPersona:(NSString *)volume withPersonaString:(NSString *)persona withReply:(void (^)(NSError *))reply;
- (void)startBackupSessionForVolume:(NSData *)volume withReply:(void (^)(NSError *))reply;
- (void)stashCommitwithUID:(unsigned int)uid WithFlags:(unsigned int)flags WithReply:(void (^)(NSError *))reply;
- (void)stashCreatewithSecret:(NSFileHandle *)secret secrestSize:(unsigned long long)size withMode:(int)mode withUID:(unsigned int)uid WithFlags:(unsigned int)flags reply:(void (^)(NSError *))reply;
- (void)stashDestroywithReply:(void (^)(NSError *))reply;
- (void)stashVerifywithUID:(unsigned int)uid WithFlags:(unsigned int)flags WithReply:(void (^)(int, NSError *))reply;
- (void)stopBackupSessionForVolume:(NSData *)volume withReply:(void (^)(NSError *))reply;
- (void)unloadKeybagForUserSession:(int)uid withReply:(void (^)(NSError *))reply;
- (void)unloadSyncBagForUserSession:(NSDictionary *)session withSessionUID:(int)uid withReply:(void (^)(NSError *))reply;
- (void)verifySyncBagForUserSession:(NSDictionary *)session withSessionUID:(int)uid withReply:(void (^)(NSError *))reply;
@end

static void
say(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	fputs("passcoded: ", stderr);
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
	va_end(args);
}

static io_connect_t
keystore(void)
{
	static io_connect_t conn = IO_OBJECT_NULL;
	io_service_t service;
	kern_return_t kr;

	if (conn != IO_OBJECT_NULL) {
		return conn;
	}
	service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleKeyStore"));
	if (service == IO_OBJECT_NULL) {
		say("no AppleKeyStore service");
		return IO_OBJECT_NULL;
	}
	kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
	IOObjectRelease(service);
	if (kr != KERN_SUCCESS) {
		say("AppleKeyStore: IOServiceOpen 0x%x", kr);
		conn = IO_OBJECT_NULL;
	}
	return conn;
}

/* MobileKeyBag's codes for the key store's answers (MobileKeyBag's own mapping, for the ones used here) */
static int
mkbCode(kern_return_t kr)
{
	switch (kr) {
	case KERN_SUCCESS:            return 0;
	case kIOReturnNotReadable:    return -3;     /* the old passcode is wrong */
	case kIOReturnNotPermitted:   return -2;     /* failed attempts: a delay runs */
	case kIOReturnBadArgument:    return -17;
	default:                      return -1;
	}
}

static NSError *
failure(int code)
{
	return code == 0 ? nil : [NSError errorWithDomain:@"com.hakios.passcoded" code:code userInfo:nil];
}

static NSData *
readSecret(NSFileHandle *handle, unsigned long long size)
{
	NSError *error = nil;
	NSData *data;

	if (handle == nil || size == 0) {
		return [NSData data];
	}
	if (size > kHKMaxSecret) {
		return nil;
	}
	data = [handle readDataUpToLength:(NSUInteger) size error:&error];
	if (data == nil || data.length != size) {
		say("a secret of %llu bytes could not be read (%s)", size, error ? error.localizedDescription.UTF8String : "short");
		return nil;
	}
	return data;
}

static int
changeSecret(NSFileHandle *oldHandle, unsigned long long oldSize, NSFileHandle *newHandle, unsigned long long newSize, NSData *opaque)
{
	NSData *oldSecret = readSecret(oldHandle, oldSize), *newSecret = readSecret(newHandle, newSize);
	NSMutableData *request;
	struct HKChangeSecret head;
	io_connect_t conn = keystore();
	kern_return_t kr;

	if (oldSecret == nil || newSecret == nil || opaque.length > kHKMaxBlob) {
		return -17;
	}
	if (conn == IO_OBJECT_NULL) {
		return -1;
	}
	head.magic = kHKChangeMagic;
	head.oldLength = (uint32_t) oldSecret.length;
	head.newLength = (uint32_t) newSecret.length;
	head.blobLength = (uint32_t) opaque.length;
	request = [NSMutableData dataWithBytes:&head length:sizeof(head)];
	[request appendData:oldSecret];
	[request appendData:newSecret];
	if (opaque.length) {
		[request appendData:opaque];
	}
	kr = IOConnectCallStructMethod(conn, kHKChangeSecret, request.bytes, request.length, NULL, NULL);
	memset(request.mutableBytes, 0, request.length);
	say("change of passcode (%s): 0x%x", newSecret.length ? (oldSecret.length ? "change" : "set") : "remove", kr);
	return mkbCode(kr);
}

static bool
status(uint64_t out[5])
{
	io_connect_t conn = keystore();
	uint32_t count = 5;

	return conn != IO_OBJECT_NULL && IOConnectCallScalarMethod(conn, kHKStatus, NULL, 0, out, &count) == KERN_SUCCESS;
}

@interface Passcoded : NSObject <NSXPCListenerDelegate, KBXPCProtocol>
@end

@implementation Passcoded

- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)connection
{
	connection.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(KBXPCProtocol)];
	connection.exportedObject = self;
	[connection resume];
	return YES;
}

/* what matters */

- (void)changeSystemSecretfromOldSecret:(NSFileHandle *)oldSecret oldSize:(unsigned long long)oldSize toNewSecret:(NSFileHandle *)newSecret newSize:(unsigned long long)newSize opaqueData:(NSData *)opaque reply:(void (^)(NSError *))reply
{
	reply(failure(changeSecret(oldSecret, oldSize, newSecret, newSize, opaque)));
}

- (void)changeSystemSecretWithEscrow:(NSData *)escrow fromOldSecret:(NSFileHandle *)oldSecret oldSize:(unsigned long long)oldSize toNewSecret:(NSFileHandle *)newSecret newSize:(unsigned long long)newSize opaqueData:(NSData *)opaque keepstate:(BOOL)keep reply:(void (^)(NSError *))reply
{
	/* no escrow bags here: the change itself is the same */
	reply(failure(changeSecret(oldSecret, oldSize, newSecret, newSize, opaque)));
}

- (void)getPasscodeBlobWithReply:(void (^)(NSData *, NSError *))reply
{
	io_connect_t conn = keystore();
	NSMutableData *blob = [NSMutableData dataWithLength:kHKMaxBlob];
	size_t size = kHKMaxBlob;
	kern_return_t kr = kIOReturnNotOpen;

	if (conn != IO_OBJECT_NULL) {
		kr = IOConnectCallStructMethod(conn, kHKCopyBlob, NULL, 0, blob.mutableBytes, &size);
	}
	if (kr != KERN_SUCCESS) {
		reply(nil, failure(-1));
		return;
	}
	blob.length = size;
	reply(blob, nil);
}

- (void)getDeviceLockState:(int)uid needsExtended:(BOOL)extended withReply:(void (^)(int, NSError *))reply
{
	uint64_t out[5];

	if (!status(out)) {
		reply(-1, failure(-1));
		return;
	}
	reply((int) out[1], nil);
}

- (void)passcodeUnlockStartWithReply:(void (^)(NSError *))reply
{
	reply(nil);
}

- (void)passcodeUnlockSuccessWithReply:(void (^)(NSError *))reply
{
	reply(nil);
}

- (void)passcodeUnlockFailedWithReply:(void (^)(NSError *))reply
{
	reply(nil);
}

- (void)Event:(int)event
{
}

- (void)isKeyRollingInProgresswithreply:(void (^)(int, NSError *))reply
{
	reply(0, nil);
}

- (void)migrationWithReply:(void (^)(NSError *))reply
{
	reply(nil);
}

/* the rest: features this device has not got */

- (void)SeshatDebugWithDebugMask:(int)mask withReply:(void (^)(NSError *))reply { reply(failure(-1)); }
- (void)SeshatEnrollWithSecret:(NSFileHandle *)secret secretSize:(unsigned long long)size withReply:(void (^)(NSError *))reply { reply(failure(-1)); }
- (void)SeshatRecoverWithSecret:(NSFileHandle *)secret secretSize:(unsigned long long)size withReply:(void (^)(int, NSError *))reply { reply(-1, failure(-1)); }
- (void)SeshatUnlockWithSecret:(NSFileHandle *)secret secretSize:(unsigned long long)size withReply:(void (^)(int, NSError *))reply { reply(-1, failure(-1)); }
- (void)backupUUIDForVolume:(NSData *)volume reply:(void (^)(NSData *, NSError *))reply { reply(nil, failure(-1)); }
- (void)changeClassKeysGenerationWithSecret:(NSFileHandle *)secret secretSize:(unsigned long long)size generationOption:(int)option reply:(void (^)(NSError *))reply { reply(nil); }
- (void)createKeybagForUserSession:(NSDictionary *)session withSessionUID:(int)uid WithSecret:(NSFileHandle *)secret secretSize:(unsigned long long)size withGracePeriod:(int)grace withOpaqeStuff:(NSData *)opaque withReply:(void (^)(NSError *))reply { reply(failure(-1)); }
- (void)createPersonaKeyForUserSession:(NSString *)session forPath:(NSString *)path withUID:(unsigned int)uid WithSecret:(NSFileHandle *)secret secretSize:(unsigned long long)size withReply:(void (^)(NSError *))reply { reply(failure(-1)); }
- (void)createSyncBagForUserSession:(NSDictionary *)session withSessionUID:(int)uid withReply:(void (^)(NSError *))reply { reply(failure(-1)); }
- (void)deleteKeybagForUserSession:(int)uid withReply:(void (^)(NSError *))reply { reply(failure(-1)); }
- (void)disableBackupForVolume:(NSData *)volume reply:(void (^)(NSError *))reply { reply(failure(-1)); }
- (void)enableBackupForVolume:(NSData *)volume withSecret:(NSFileHandle *)secret secretSize:(unsigned long long)size reply:(void (^)(NSData *, NSError *))reply { reply(nil, failure(-1)); }
- (void)getBackupBlobreply:(void (^)(NSData *, NSError *))reply { reply(nil, failure(-1)); }
- (void)getBackupkeyForVolume:(NSData *)volume andCryptoID:(unsigned long long)cryptoID withReply:(void (^)(NSData *, NSError *))reply { reply(nil, failure(-1)); }
- (void)getLockSateInfoforUser:(int)uid WithReply:(void (^)(NSDictionary *, NSError *))reply { reply(nil, failure(-1)); }
- (void)loadKeybagForUserSession:(NSDictionary *)session withSessionID:(int)sid withSecret:(NSFileHandle *)secret secretSize:(unsigned long long)size shouldSetGracePeriod:(BOOL)setGrace withGracePeriod:(int)grace isInEarlyStar:(BOOL)early withReply:(void (^)(NSError *))reply { reply(failure(-1)); }
- (void)loadSyncBagForUserSession:(NSDictionary *)session withSessionUID:(int)uid withReply:(void (^)(NSError *))reply { reply(failure(-1)); }
- (void)registerBackupBag:(NSData *)bag withSecret:(NSFileHandle *)secret secretSize:(unsigned long long)size reply:(void (^)(NSError *))reply { reply(failure(-1)); }
- (void)removePersonaKeyForUserSession:(NSString *)session withUID:(unsigned int)uid withVolumeUUIDString:(NSString *)volume withReply:(void (^)(NSError *))reply { reply(failure(-1)); }
- (void)removeSyncBagForUserSession:(NSDictionary *)session withSessionUID:(int)uid withReply:(void (^)(NSError *))reply { reply(failure(-1)); }
- (void)setVolumeToPersona:(NSString *)volume withPersonaString:(NSString *)persona withReply:(void (^)(NSError *))reply { reply(failure(-1)); }
- (void)startBackupSessionForVolume:(NSData *)volume withReply:(void (^)(NSError *))reply { reply(failure(-1)); }
- (void)stashCommitwithUID:(unsigned int)uid WithFlags:(unsigned int)flags WithReply:(void (^)(NSError *))reply { reply(failure(-1)); }
- (void)stashCreatewithSecret:(NSFileHandle *)secret secrestSize:(unsigned long long)size withMode:(int)mode withUID:(unsigned int)uid WithFlags:(unsigned int)flags reply:(void (^)(NSError *))reply { reply(failure(-1)); }
- (void)stashDestroywithReply:(void (^)(NSError *))reply { reply(nil); }
- (void)stashVerifywithUID:(unsigned int)uid WithFlags:(unsigned int)flags WithReply:(void (^)(int, NSError *))reply { reply(-1, failure(-1)); }
- (void)stopBackupSessionForVolume:(NSData *)volume withReply:(void (^)(NSError *))reply { reply(failure(-1)); }
- (void)unloadKeybagForUserSession:(int)uid withReply:(void (^)(NSError *))reply { reply(failure(-1)); }
- (void)unloadSyncBagForUserSession:(NSDictionary *)session withSessionUID:(int)uid withReply:(void (^)(NSError *))reply { reply(failure(-1)); }
- (void)verifySyncBagForUserSession:(NSDictionary *)session withSessionUID:(int)uid withReply:(void (^)(NSError *))reply { reply(failure(-1)); }

@end

/*
 * passcode_debug=1 among the kernel's arguments: two seconds after each change of the lock state, ask SpringBoard
 * (libhakios.sb.dylib, com.hakios.layers) to log whether it counts the user as authenticated and has a passcode.
 */
static bool
debugging(void)
{
	static int known = -1;
	char args[1024];
	size_t size = sizeof(args) - 1;

	if (known < 0) {
		memset(args, 0, sizeof(args));
		known = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "passcode_debug=1") != NULL;
	}
	return known == 1;
}

/* The key store's messages, as keybagd's notifications. */
static void
interest(void *refcon, io_service_t service, natural_t type, void *argument)
{
	uint64_t out[5] = { 0 };

	if (type == kAKSMessageStatusChanged) {
		notify_post("com.apple.mobile.keybagd.lock_status");
		if (status(out)) {
			say("lock state %llu (passcode %s, %llu failed attempts, %llu s of delay)", out[1], out[0] ? "set" : "none", out[2], out[3]);
		}
		if (debugging()) {
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
				notify_post("com.hakios.layers");
			});
		}
	} else if (type == kAKSMessageFirstUnlock) {
		notify_post("com.apple.mobile.keybagd.first_unlock");
		say("first unlock of this boot");
	}
}

static void
watch(void)
{
	IONotificationPortRef port = IONotificationPortCreate(kIOMainPortDefault);
	io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleKeyStore"));
	io_object_t note;

	if (port == NULL || service == IO_OBJECT_NULL) {
		say("cannot watch the key store");
		return;
	}
	IONotificationPortSetDispatchQueue(port, dispatch_get_main_queue());
	if (IOServiceAddInterestNotification(port, service, kIOGeneralInterest, interest, NULL, &note) != KERN_SUCCESS) {
		say("cannot watch the key store's messages");
	}
	IOObjectRelease(service);
}

/*
 * keybagd posts lock_status when it starts, and its clients read the state then. wifid only learns that the device
 * has been unlocked since boot from that notification (__WiFiManagerLockStateUpdate sets its unlockedSinceBoot):
 * without it, it holds Wi-Fi's power off for itself and never auto-joins, though Settings can still join by hand.
 * With no passcode (or already unlocked) the state never changes here, so nothing was ever posted. Post it now and
 * again over the first minute, for the daemons that register later (wifid among them).
 */
static void
announce_unlocked(void)
{
	static const int at[] = { 0, 3, 10, 20, 40, 60 };

	for (size_t i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) at[i] * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			notify_post("com.apple.mobile.keybagd.lock_status");
			if (at[i] == 0 || at[i] == 20) {
				notify_post("com.apple.mobile.keybagd.first_unlock");
			}
		});
	}
	say("unlocked since boot: lock_status and first_unlock posted over the first minute");
}

int
main(void)
{
	@autoreleasepool {
		static Passcoded *server;
		static NSXPCListener *listener;
		uint64_t out[5] = { 0 };

		server = [Passcoded new];
		/* the SDK marks it unavailable on iOS, where Apple's daemons use it all the same */
		listener = ((NSXPCListener * (*)(id, SEL, NSString *))objc_msgSend)([NSXPCListener alloc],
		    @selector(initWithMachServiceName:), @"com.apple.mobile.keybagd.xpc");
		listener.delegate = server;
		[listener resume];
		watch();
		if (status(out)) {
			say("serving com.apple.mobile.keybagd.xpc; passcode %s, lock state %llu", out[0] ? "set" : "none", out[1]);
			if (out[1] != 1) {
				announce_unlocked();
			}
		} else {
			say("serving com.apple.mobile.keybagd.xpc; the key store does not answer its status");
		}
	}
	dispatch_main();
}
