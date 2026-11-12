# telephonyd: CoreTelephony's server, saying "no SIM"

`userland/src/telephonyd.m`, launchd job `userland/com.hakios.telephonyd.plist` (`/redwood/bin/telephonyd`, started on
demand). It stands in for Apple's CommCenter, which stays out of the image (`userland/mkjobs.py`'s DENY: it needs an Apple
baseband on PCIe). telephonyd serves CommCenter's two client services and gives every CoreTelephony client the answers of
a device with no SIM and no service.

## Why

With nothing behind those services, every CoreTelephony client's lookup failed (about 190 to 200 failed lookups in a
session). SpringBoard's `SBTelephonyManager` took each failed `CTServerConnection` call as a broken connection
("CTServerConnection returned error: (2, 5). Destroying connection"), built a new one, and asked again. It did this about
once a second while the display was on. Each attempt reset the idle timer (`updateIdleTimerForReason:"IdleTimerReset"`),
so the screen never turned itself off and auto-lock never fired.

## The two services (CoreTelephony, Darwin userland 16.5 / 20F66)

### com.apple.commcenter.xpc: `_CTServerConnection*`

This service uses plain XPC.

- **Request.** A dictionary. `"kRequest"` holds the request's name; there are about 270 names, such as
  `kRegistrationGetProperty`, `kPacketCreateDormancySuspendAssertion`, `kWhois`, `kRegisterNotification` and
  `kSubscriberHomeCountryList`. Each request adds keys of its own; for example, `kRegistrationMCC` names the property
  being asked for.
- **Reply.** Requests that want an answer are sent with `xpc_connection_send_message_with_reply_sync`. CoreTelephony
  treats the answer as an error in only two cases:
  - The answer is not a dictionary. That gives `(2, 5)` (`kCTErrorDomainMach`, `KERN_FAILURE`), the error SpringBoard
    looped on.
  - The answer carries `"kPosixError"` or `"kMachError"`.

  Otherwise CoreTelephony reads the keys it wants, and a missing key means "no value".
- **Requests without a reply.** `kWhois`, `kRegisterNotification` and the assertions are sent without a reply. An
  assertion (dormancy suspend, keep-alive, ...) is a connection of its own and lives as long as it is held.

telephonyd answers every request that wants a reply with an empty dictionary. That gives no country code, no operator, and
"not in emergency callback mode". `serve_ctserver()` is the place for requests that need more. telephonyd never sends
events.

### com.apple.commcenter.coretelephony.xpc: `CoreTelephonyClient`

This service uses NSXPC with the protocol `CTXPCServiceInterface`: 31 incorporated protocols, 384 methods. All methods
but one (`SIMUnlockProcedureDidComplete`) have exactly one reply block. Every reply argument is an object, an integer or a
BOOL.

- **Interface.** telephonyd builds the interface the way `-[CoreTelephonyClientMux _connect_sync]` does: the protocol,
  plus the property-list classes (NSArray, NSDictionary, NSString, NSNumber, NSData) for the 13 carrier-bundle and
  preference arguments and replies.
- **Methods.** telephonyd implements none of them directly. NSXPC forwards each call to `forwardInvocation:`. That method
  reads the reply block's signature from the protocol's extended method types at run time, and always calls the block.
- **Answers that set the "no SIM" state:**

| method | answer |
|---|---|
| `ping:`, `registerForNotifications:completion:` | done (no error) |
| `getSubscriptionInfo:` | `CTXPCServiceSubscriptionInfo` with no subscriptions |
| `getActiveContexts:` | `CTXPCContexts` with no subscriptions |
| `getDescriptorsForDomain:completion:` | `CTServiceDescriptorContainer` with no descriptors |
| `getDualSimCapability:` | `kCTServerConnectionDualSimCapabilityNotSupported` (3) |
| everything else | no value (nil / NO / 0) and `NSError` NSPOSIXErrorDomain `ENODEV` |

With no subscription, a client has no context to ask about. The few requests that still come in (operator name, signal,
last known MCC) are about a SIM or a network, so they get "not on this device". Some requests never reach the server:
CoreTelephonyClient answers them itself because the device says it has no cellular ("Device does not support cellular").

The other services in CommCenter's plist (`cupolicy`, `carrierspace`, `atcs`, the CellularPlan daemon, ...) are looked up
a handful of times per session. telephonyd leaves them alone.

## The seam for the modem

`modem_read()` reads the kernel's `kern.hakios.cellular_up`, `_registered`, `_bars`, `_dbm`, `_radio` and `_operator`
(published by `drivers/modem/hakios.mpss.c` on redwood, and later by the star2lte Shannon driver; see
[cellular-star2lte.md](cellular-star2lte.md)). `have_subscription()` decides whether there is a SIM to describe. It is
false today, because the kernel publishes registration and signal but no SIM state. When the kernel publishes a SIM, the
work goes in these places:

- `have_subscription()`, to turn it on.
- The `getSubscriptionInfo:` / `getActiveContexts:` answers in `answer_for()`, to describe the subscription.
- `answer_for()` and `serve_ctserver()`, for that subscription's operator, MCC/MNC, registration and signal.

The status bar keeps getting its bars from `inject_sb.c` until then.

## Debugging

`kill -USR1 <telephonyd's pid>` prints every request and method seen so far, with counts and the asking processes, to
the console. The first occurrence of each is printed as it arrives.

## Measured (13 November, build 40, telephonyd loaded as a test job)

**SpringBoard before** (15 s of `oslog`): 15 `IdleTimerReset`, 28 "CTServerConnection returned error (2, 5)", and a new
connection plus dormancy assertion every second.

**SpringBoard started with telephonyd present:**
- No `IdleTimerReset` and no CTServerConnection errors. Over the next 20 s, no new connections or assertions.
- SpringBoard asked `kRegistrationMCC`, `kRegistrationEmergencyCallBackMode`, `kRegistrationBrickMode`,
  `getSubscriptionInfo:`, `getActiveContexts:` and `getUserDefaultVoiceSubscriptionContext:` once each.

A SpringBoard that started before the service existed keeps failing, so the fix shows from the boot on.

**Screen off:**
- A lock-screen wake from the lock button went back off by itself after 6 s ("Animating backlight to state inactive
  ... (idle timer)").
- With Auto-Lock at 1 minute (`maxInactivity` 60), the phone auto-locked.

**Other clients:**
- Clients in that session included SpringBoard, Preferences, bluetoothd, chronod, promotedcontentd, StocksWidget and
  NewsToday2. None crashed.
- chronod asks `kCUPolicyGet` about 7 times a minute.
- Settings > Privacy & Security still shows Location Services ("On").

**telephonyd:** 0.1 s of CPU in 15 minutes.
