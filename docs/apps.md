# Apps without an App Store account

The aim: a person with a Scion phone can put apps on it without an Apple account, with nothing cracked and nothing decrypted. The
route is four steps, in this order:

1. **Web apps as home screen icons** (web clips). Built; off by default; never run on a phone. [Below](#1-web-apps-web-clips).
2. **Open-source native apps built from their source.** A survey, one pilot build. [Below](#2-open-source-native-apps).
3. **Push without Apple's push service** (a Scion push daemon over UnifiedPush or ntfy, patched into source-built apps). A sketch only.
4. **An App Library front end** over the first three. Not started.

Not part of this and never will be: FairPlay decryption, decrypted-IPA stores, cracks of paid apps, an Apple account made up or
borrowed, Widevine or attestation spoofing.

What is verified in this page and what is not: a line with a commit or a command was read or run on 7 November 2026; "(guess)" is
what I expect and have not seen. Nothing on this page had run on a phone until the [Linux pilot](#the-linux-pilot-into-the-sileo-repository)
of 12 November, which did.

## 1. Web apps (web clips)

A web clip is what Safari's Add to Home Screen makes: a directory `<UUID>.webclip` in `/var/mobile/Library/WebClips` with an
`Info.plist` (title, address) and an `icon.png`. It needs no configuration profile (`.mobileconfig`), no signing and no account.

- **In the image**, behind a flag: `WEBCLIPS=1 userland/build.sh`, or `build-installer.sh --webclips` in scion-installer. `tools/mk-webclips.py`
  reads `userland/webclips/manifest.json` and writes the clips next to `userland/var/`; `build.sh` adds them to `etc/var.skel`, the list
  from which init copies files into `/private/var` at each boot (with a kept `/private/var`, only if the file is not there yet). Eight
  clips: Wikipedia, Mastodon (mastodon.social), Element web, OpenStreetMap, Excalidraw, Jitsi Meet, Bluesky, and Scion's own page.
  The list has room for about five more (`etc/var.skel` is read into 16 384 bytes, `build.sh` stops at 15 000; with these eight it is
  about 11 000).
- **On a phone that is installed already**: the Sileo repository in scion-installer (`repo/`) has one package for each clip and one that
  pulls them all in. See its README.
- **Icons**: drawn by the script from plain shapes (a book, a speech bubble, a map pin, a camera ...), the same bytes every build. They are not
  the services' logos on purpose: a logo is its owner's trademark, some under licences of their own (Wikipedia's globe, Mastodon's mark,
  Element's, OpenStreetMap's), and a shortcut is no reason to ask. Licence of the icons and the script: MIT.
- **Addresses** are each service's own public web page. Mastodon needs a server: mastodon.social is a placeholder, a person edits the manifest
  (or the clip's `Info.plist`) for their own.
- **They open in Safari**, not as stand-alone web apps (`FullScreen` false). The launcher for stand-alone apps is not in the system image's
  file list (`rootfs.slt` has MobileSafari and SafariViewService, and no WebApp bundle); `WEBCLIPS_FULLSCREEN=1` turns it on to try.

Checked here: `tools/webclips-test.py` (16 checks: manifest, plist, PNG, same bytes twice, budget of the skeleton), and the line of `build.sh` run
by hand on a copy of a real image's `var.skel` (10 992 bytes with the eight clips). **Not checked (guess):** that SpringBoard on 16.5 shows
a clip that was put in the directory by hand and not by Safari (it should, and place it after the apps, but the exact keys of `Info.plist` are
from memory of what Safari writes, not from a booted system), that Safari's WebKit runs the sites at all on this port, that WebKit has the
network and the certificates it needs (`rc.jobs.safari` opens yandex.ru and example.com; no log of it is in hand). The first run on a phone settles all three.

## 2. Open-source native apps

What the kernel does about signing: it replaces Apple's code-signing policy with a stub (`libkern/amfi/miniamfi.c`, see
[the legal review](legal-review.md)), so a Mach-O that nobody signed is allowed to run. **So an app needs no signature, no AppSync and no
Apple account to start.** (Guess, to be tried with the first pilot app on a phone: entitlements. Without a signature an app has none, and
the ones that matter to real apps are the keychain, app groups and push; a pseudo-signature with `ldid -S` carrying the entitlements may be needed
for those, and whether the stub looks at them is unread.) The route for an app is a rootless deb with the `.app` under `/var/jb/Applications`, which
`prep.sh` in the system image registers with `uicache`, as it already does for Sileo.

**The finding that shapes everything: the Galaxy S9 runs iOS 16.5, and nearly every current app wants iOS 17 or 18.** The minimum iOS of each
app below was read from its project files at the commit named (blobless clone, 7 November 2026). On the phones with iOS 26 (the PAC chips) it is
a different table; for the S9 only the first rows are candidates, and anything else would mean building with a lower deployment target
and hoping the code does not call newer APIs (that is what the pilot below tries).

| App | Licence | Min iOS | Build | Signing on Scion | The service's terms for a third-party client | Push | Verdict for the S9 (16.5) |
|---|---|---|---|---|---|---|---|
| **Organic Maps** | Apache-2.0 for the code; map data under a separate licence (`DATA_LICENSE.txt`); the About screen must carry the notice | 16.2 | Xcode project + CMake and git submodules (`configure.sh`) | none to start | no account; maps are downloaded from the project's servers: read their terms before a fork uses them (not read) | no | **First native candidate, on a Mac** (Swift and XIBs: not a Linux build, see [the Linux pilot](#the-linux-pilot-into-the-sileo-repository)): runs on 16.5, works offline (the phone has no cellular), no account. Location needs a GPS the port does not list (guess: browsing only) |
| **VLC for iOS** | GPL-2.0-or-later and MPL-2.0 (bi-licensed, `COPYING`) | 15.0 (some targets 17.0) | CocoaPods + VLCKit, which compiles libvlc from source (hours) or takes a binary pod | none to start | none; the name VLC and the cone are VideoLAN's: a modified build is renamed (not read) | no | Feasible, size M. Interesting for the hardware decoder work (the MFC driver in `drivers/media/mfc`) |
| **Telegram-iOS** | GPL (v2 or later, per telegram.org/apps; the repository has no LICENSE file at its top, which is worth fixing upstream) | 15.0 | Bazel (`build-system/Make/Make.py`), pins an Xcode version (`--overrideXcodeVersion`), `--disableProvisioningProfiles` exists | none to start | **Allowed and asked for**: your own `api_id`, do not use the name Telegram or the paper-plane logo, publish your source. A marked unofficial client is the stated route | APNs only (see step 3); without it, messages arrive when the app is open | Feasible, size M to L: Bazel's cache is tens of GB, a first build hours; not tried, Xcode 27.2 against a pinned Xcode is the first risk |
| **Signal-iOS** | AGPL-3.0 | 15.0 | Xcode + CocoaPods + SwiftPM, `make dependencies` | entitlements: keychain, app groups | **Unclear, and the owner of the account is the one who can say**: the terms bar using the service "in unauthorized manners"; the project has a history of asking that unofficial builds not use its servers (2016). Nothing on its site invites a third-party build | APNs only | **Do not ship until Signal agrees in writing.** Buildable for oneself |
| **Element Classic** (`element-ios`) | AGPL-3.0, dual with a commercial licence | 15.1 | CocoaPods | keychain | Matrix is open: any client may use any homeserver | APNs via Sygnal | Feasible, size M; the project is in maintenance (last push 24 Sep 2026). The web clip covers it meanwhile |
| **Firefox for iOS** | MPL-2.0 | 15.0 (some 16.4) | Xcode + SwiftPM, `bootstrap.sh` | none to start | Mozilla's name and logos are not under the MPL: a build is rebranded | no | Low value: it is WKWebView again, as Safari is. Sync uses Mozilla's servers (terms not read) |
| **Wikipedia** | MIT (`LICENSE.txt`, commit `0b9980f`) | **17.6** | Xcode project + SwiftPM (Rive, SDWebImage, RxSwift, CocoaLumberjack, swift-log, HCaptcha) | none (the build below ran with `CODE_SIGNING_ALLOWED=NO`) | public API, free to use with a User-Agent that says who you are; "Wikipedia" and the globe are Wikimedia's marks: rename and re-icon | no | **Pilot, see below**: the deployment target would have to be lowered by one major version |
| NetNewsWire | MIT | 18 | Xcode + SwiftPM | none | it reads any feed | no | Not on 16.5. Fine on iOS 26 |
| Ice Cubes (Mastodon) | AGPL-3.0 | 18.5 | SwiftPM only | none to start | Mastodon's API lets any client register itself | UnifiedPush-style relay needed | Not on 16.5. Fine on iOS 26 |
| Mastodon (official) | GPL-3.0 | 18.6 | Xcode + SwiftPM | none to start | same | APNs via the project's relay | Not on 16.5 |
| Element X | AGPL-3.0, dual | 18.5 and 26.0 (by target) | Xcode + XcodeGen + the Matrix Rust SDK | keychain | Matrix is open | APNs via Sygnal | **Not on 16.5.** On iOS 26 maybe |
| Nextcloud | GPL-3.0 | 17.0 (and 27.0 by target) | Xcode + SwiftPM | keychain | needs a server of one's own | the project's push proxy serves only its own app id | Not on 16.5 |
| Kiwix | GPL-3.0 | 17.6 | XcodeGen + libkiwix | none | content is offline ZIM files, each under its own licence | no | Not on 16.5. A good offline fit on iOS 26 |
| Bluesky | MIT | not read (React Native, Expo) | Node + CocoaPods | keychain | AT Protocol is open | APNs | Not read; the web clip covers it |

Excluded and why: Ivory and Mona (closed source, nothing to build); anything from the App Store (a decrypted IPA is the thing this project does not
do).

**Licence habit for what is built**: a GPL or AGPL app goes out with its source (the exact commit and our patches, a link that stays up as long as
the binary does) and its licence text in the package; a trademark in an app's name or icon is replaced for a build that is not the project's own;
every package gets a row in `repo/LICENSES.md` of scion-installer: SPDX id, source, commit, the service's terms, what was changed.

### The pilot build

Wikipedia's iOS app, unsigned, on the Mac mini (Xcode 27.2, iPhoneOS 27.2 SDK, Apple silicon), no Apple ID of any kind:

```
xcodebuild -scheme Wikipedia -configuration Release -destination 'generic/platform=iOS' -derivedDataPath pilot/dd -jobs 4 \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" IPHONEOS_DEPLOYMENT_TARGET=16.5 build
```

The project says 17.6; the override to 16.5 is the experiment: whether an app written for 17.6 builds for the S9's 16.5 when the SDK is newer.

**Result of the 16.5 build (7 November, about 15 minutes on a busy machine): it fails to compile.** `WMFComponents`, Wikipedia's SwiftUI layer,
calls iOS 17 APIs without a fallback: 110 errors in 36 Swift files, 15 or so distinct calls (`onChange(of:initial:_:)` 60 times, then
`contentMargins`, `scrollTargetLayout`, `scrollTargetBehavior`, `scrollPosition(id:anchor:)`, `listSectionSpacing`, `searchable(text:isPresented:...)`,
`containerRelativeFrame`, `registerForTraitChanges`, `UITraitVerticalSizeClass`). Nothing after that target was compiled. So for the S9 the app would need
a fork with `if #available` fallbacks or a hand-written replacement of each call in 36 files: possible, not small, and each fix is a behaviour change nobody has
run. A lower deployment target is not a switch you can flip; this is the same for the other iOS 17 and 18 apps in the table. The log is
`pilot/build1.log` on the mini (`~/s9/agents/scion-apps/`).

The second build is the same command with the project's own deployment target (17.6), to find out whether the unsigned build path works at all, which is what
the phones with iOS 26 need. Its result is in the report that came with this change; a build that finishes is not an app that runs, and none of it has been on a phone.

### The Linux pilot, into the Sileo repository

12 November 2026, on the S9+ (star2lte, 16.5). The question: can an app go from its source to a Sileo install with no Mac at all.

- **Organic Maps is not a Linux build.** Read at `79674626` (blobless clone): the deployment target is 16.2, which fits, but `iphone/`
  is 410 Swift files, 68 XIBs plus storyboards and asset catalogs. Theos's Linux toolchain (clang 11, cctools, `ldid`) has no
  `swiftc`, `ibtool` or `actool`, and the build host has no macOS reachable (the Mac mini of the pilot above is not reachable from
  the WSL host). It stays the first candidate for a Mac build; `"host": "macos"` in a recipe is how it would go in (build there,
  package with `mkapps.py --no-build`).
- **What was built instead: Simon Tatham's Portable Puzzle Collection**, Greg Hewgill's iOS port
  ([ghewgill/puzzles](https://github.com/ghewgill/puzzles) at `01c3e382`, MIT): Objective-C and C with its UI in code, deployment target 9.0,
  so the only Xcode-only parts are the launch screen storyboard and the icon catalog, both replaced in Info.plist
  (`UILaunchScreen`, loose `AppIcon60x60` files). No source file changed. Theos clang with the iPhoneOS 16.5 SDK compiles the
  67 files of the project's Sources phase in about 7 s; `ldid -S` for an ad-hoc signature (no entitlements).
- **On the phone**: scion-installer's repository (`repo/build.sh` with `APPS_WORK`, `repo/mkapps.py`, recipe `repo/apps/puzzles`)
  served from WSL, added as an apt source; `apt-get install com.scion.app.puzzles` unpacked it into `/var/jb/Applications`, the postinst's
  `uicache -p` registered it, the icon is on the home screen, and `uiopen --bundleid com.scion.oss.puzzles` opens it to its list of
  39 games, full screen. Sileo lists it under the repository's name with its description. Removal (`prerm`: `uicache -u`) takes the
  icon away. **So: no signature, no entitlements and no Apple account were needed for a UIKit app.** Not tried: opening a game (there
  is no touch injection in the tools); an app that needs the keychain or app groups.
- **Serving it on the LAN**: WSL2 with mirrored networking does not take inbound connections from the LAN by default (Hyper-V
  firewall); the test used a reverse tunnel over the phone's SSH. Publishing is the owner's decision; nothing is hosted.

The next ones that fit 16.5 and Linux are the same shape (Objective-C or C, UI in code). Swift apps (most of the table above) need
the Mac, or a Swift toolchain for Theos on Linux plus no XIBs, which none of the table's apps is.

## 3. Push without Apple's push service

Apps built here cannot use APNs: it needs the `aps-environment` entitlement signed by a certificate tied to an Apple developer team, and a
topic the app's team owns. So a source-built messenger gets no pushes and only hears from its server while it is open. The plan (a sketch, nothing
written): a launchd daemon on the phone holds one connection to a UnifiedPush or ntfy server (the person's own, or ntfy.sh), and when something
arrives it wakes the app the way a local notification does. Every app needs a patch that registers with that daemon instead of APNs; Telegram and Element
both have a server side that can speak a webhook or a UnifiedPush gateway (Element's Sygnal; Telegram's own notification server is theirs, so
for Telegram it means polling). Cost to count first: a held connection against the S9's battery, and whether the kernel lets a background daemon wake an app.

## 4. The App Library

Sileo is on by default in the builds. The Scion repository ([`repo/` in scion-installer](https://github.com/Scion-Project/scion-installer))
is its first source. An App Library front end (one place to list the web apps, the built apps and what is installed) comes after there is more
than a handful of things to list.
