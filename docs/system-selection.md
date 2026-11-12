# What the system image leaves out of Darwin's root filesystem

`sysimage/select-system.py` decides which files of the 20F66 root filesystem (7.9 GB) go into the system image. This page
lists everything it leaves out, grouped by feature, and says what each part would cost to add back and whether that is
worth doing. It was made on 13 November 2026 from `sysimage/rootfs.slt`, against select-system.py as of commit 2217837
(the Files fix). To make it again, run every regular file of the listing through `wanted_file()`, the same way `main()`
does (staged applications counted as `/Applications`).

**Totals.** The image takes 43 338 files, 3 047 MB. It leaves out about 120 000 files, 4 855 MB. The SYSTEM partition
holds 7.0 GB and the image is about 2.7 GB of it on the phone, so about 4.3 GB is free. Space on the flash partition
(`system_apfs=ufs_SYSTEM`) is no longer the limit it was when the image was only a RAM disk (`/dev/md0`): the "~1 GB
of headroom" in select-system.py's comments is about a RAM-disk boot. Everything below would fit on the flash. The
real costs are a longer `system.apfs` build and upload, and files that do nothing without hardware or services this
port does not have.

**What breaks without it.** A missing *data* file usually means a feature that quietly does nothing. A missing
*executable* (a daemon, or an app extension's binary) means a registered service that cannot start: lookups fail, or
"the requested app extension could not be found". Before 2217837 the image had five of those, and they were the
Files fix: app-extension binaries over 4 MB inside frameworks had been dropped.

## Executables still left out (the class that breaks things)

| File | Size | Feature | Effect of leaving it out | Recommendation |
|---|---|---|---|---|
| `TelephonyUtilities.framework/callservicesd` | 5.0 MB | call state for the Phone app, FaceTime, SpringBoard's call UI, CallKit apps (VoIP calls in WhatsApp and similar) | its lookups fail; CallKit apps cannot report or answer calls | **Add back** (KEEP_DAEMONS). Cellular calls still need the IPC route ([cellular-star2lte.md](cellular-star2lte.md)), but VoIP/CallKit does not |
| `AssistantServices.framework/assistantd` | 5.2 MB | Siri, the server side of dictation | its lookups fail. Siri cannot work anyway: `System/Library/Assistant` and the voices are left out too | Add back only together with Siri; on its own it only stops lookups |
| `usr/libexec/searchpartyd` | 13.1 MB | Find My (devices, AirTags), offline finding | Find My has no backend | Leave out (needs an Apple ID and Bluetooth LE beacons) |
| `usr/libexec/lskdd` | 20.0 MB | purpose not identified here; never missed in a lookup so far | none seen | Leave out |
| `usr/sbin/fairplayd.H2` | 18.4 MB | FairPlay DRM | analysed in mkjobs.py: needs an IOKit user client this kernel does not have | Leave out |
| `System/Applications/Family/InviteMessageBubbleExtension.appex` | 0.3 MB | Family Sharing's invitation bubble in Messages | `System/Applications` is not under WHOLE | Leave out (nothing else lives there) |
| `System/DriverKit/...` | 8 MB | the DriverKit runtime and headers | not used by this kernel | Leave out |

There are no more over-size app-extension or XPC-service executables: every `PlugIns/*.appex`, `XPCServices`, `Support`
and `Helpers` binary is now in the image. The one extension the phone still shows without a binary,
`SmartChargingWidget.appex`, has none in Apple's image either.

## Data left out because of size (over SMALL, 4 MB, in frameworks)

| What | Size | Feature | Effect | Recommendation |
|---|---|---|---|---|
| `DataDetectorsCore.framework/com.apple.datadetectorscore.cache.full.{asia,europe,middleeast,easterneurope}.system` | 18.9 MB | finding addresses, dates, phone and flight numbers in text (Messages, Mail, Notes, Safari) | regional detection is weaker; the smaller caches are in the image | Optional: `europe` and `asia` (10 MB) if detection looks poor |
| `CognitiveHealth.framework/CustomCategoryMapping.sqlite` | 28.2 MB | Screen Time's app categories | Screen Time cannot categorise apps | Leave out |
| `CoreAnalytics.framework/defaultConfig.json` | 17.6 MB | analyticsd's telemetry configuration | no telemetry, which is what we want | Leave out |
| `WebContentRestrictions.framework/WCRFilter-*.plist` | 11.4 MB | Screen Time's adult-site filter | the filter is inactive | Leave out |
| `QuartzCore.framework/default.pipelinelib/pipelines.g*` (14 files) | 88.5 MB | pipelines precompiled for Apple GPUs (g4-g15) | of no use on a Mali | Leave out |
| `AppleCV3D` PCA tables, `AppleCVA` tensor.plist, `CoreRE` cubemap, `TuriCore` model | 53 MB | ARKit, face tracking, RealityKit | AR and face features (no TrueDepth camera anyway) | Leave out |
| `ContentKit.framework/magic.mgc` | 6.3 MB | Shortcuts' file-type detection | the "Get Type of File" action fails | Optional (6 MB) |
| `iWorkImport.framework/shape_library.json` | 4.9 MB | Quick Look of Pages/Keynote shapes | shapes are missing in those previews | Leave out |
| asset catalogs over 4 MB: HomeUI 21.5, HeadphoneConfigs 12.0, NanoTimeKitAssetsCompanion 11.1, MapsUI 8.1, FitnessUI 7.1, HeartRhythmUI 6.5, NanoTimeKit 4.5, SeymourUI 4.0 | 74.8 MB | the pictures of Home, AirPods setup, Watch faces, Maps, Fitness, ECG, Fitness+ | those screens are drawn without some of their images | **Add back `MapsUI.framework/Assets.car` (8.1 MB)**; leave the rest |

## Categories left out on purpose

| Category (rule) | Size | What it is | Effect | Recommendation |
|---|---|---|---|---|
| `System/Library/LinguisticData` | 957 MB | language assets per language: lexicons, autocorrection and prediction models, tokenisers. `RequiredAssets_en` 84.6, `_ko` 35.1, `_zh-Hans` 85.1, `_ja` 61.9 ... | **the keyboard's autocorrection, predictive text and spell checking are weak or absent**; natural-language tagging is limited | **Add back `RequiredAssets_en.bundle` (84.6 MB)**, and `_ko` (35.1 MB) if the owner types Korean |
| `System/Library/PreinstalledAssetsV2/RequiredByOs` | 660 MB | preinstalled MobileAssets: Siri voices (VoiceResources 151, CustomVoice 132), "Hey Siri" (VoiceTrigger 52 + 32), SharingDeviceAssets 153 (device pictures in AirDrop/Bluetooth), ContextKit 100, GameController_DB1 12, GeoPolygonDataAssets 11, LanguageDetectorAssets 9, SpeakerRecognition 6, CoreLocationConfig 1 | no Siri or system voices, no device pictures, no language detection | Optional and cheap: **GameController_DB1 (12 MB)**, **LanguageDetectorAssets (9 MB)**, **CoreLocationConfig (1 MB)**. Leave the voices unless Siri or VoiceOver is a goal |
| framework ML weights (`.weights`, `.weights_nonane`, `.espresso.*`, `.mlmodelc`, the `.dat` of ML frameworks) | ~1 100 MB | Vision 255, MediaAnalysis 242, CVNLP 75, TextRecognition 73, CoreHandwriting 39, ARKitCore 35, SoundAnalysis 13, TextToSpeechMauiSupport 106 ... | **Live Text, Photos search and faces, Visual Look Up, Sound Recognition, handwriting** do not work. They fail feature by feature; nothing is known to crash-loop (the Spotlight models that did are already kept, 911622f) | Leave out by default. For Live Text: TextRecognition's weights, `.dat`, `.net` and `.shape`, about 75 MB |
| non-English `.lproj` | 248 MB | the interface in other languages (`ko.lproj` is only 5.1 MB) | English only | Optional: `ko` in KEEP_LANG (+5 MB) for a Korean interface |
| `System/Library/NanoTimeKit`, `NanoPreferenceBundles`, the `.art` of NanoUniverse/NanoTimeKit | 275 MB | Apple Watch faces and settings | no Watch | Leave out |
| `usr/standalone` | 166 MB | the restore ramdisk, Apple's baseband firmware | not this hardware | Leave out |
| `.metallib` over 2 MB | 115 MB | MPS neural network/NDArray/matrix (48), Espresso (28), AvatarKit (16), CoreImage (4), Metal (3.4), VectorKit (2.4) ... | a Metal client that asks for one aborts ([metal.md](metal.md)). None does on the default boot | Leave out until a METAL=vk client asks; CoreImage's and VectorKit's are the next candidates |
| videos and sounds (`.mov`, `.mp4`, `.caf`, `.wav`, `.m4a`) in apps and frameworks | ~240 MB | setup and pairing films (Bridge 45, FindMy 32, Setup 12), Face ID enrolment, Siri, headphone setup, ToneLibrary | those screens show no film; a few interface sounds are silent | Leave out |
| `Library/Ringtones` | 17.2 MB | the ringtones and alert tones (`.m4r`) | **Settings > Sounds has no ringtones to choose; alarms and timers have only the default sound** | **Add back (17 MB)** |
| `System/Library/Health/Assets/FitnessUIAssets.bundle` | 101 MB | the pictures and films of activity awards | the awards have no artwork | Leave out |
| `System/Library/Assistant` | 72 MB, 78 535 files | Siri's plug-ins and interface | no Siri | Leave out (Siri also needs the voices, assistantd and Apple's servers) |
| `System/Library/Audio` (other models' tunings, MIDI drivers, the ML denoisers) | 105 MB | the D22 and Generic tunings are kept | none seen | Leave out |
| `System/Library/AccessibilityBundles`, `ScreenReader` | 91 MB | VoiceOver, Switch Control, Braille | **VoiceOver and the other accessibility features do not work** | Leave out unless accessibility is a goal (then the voices too) |
| `usr/share/tokenizer` | 54 MB | Japanese, Chinese and Korean word segmentation (`ko` 7.8) | CJK search and typing are weaker | With Korean typing: `ko` (7.8 MB) |
| `System/Library/VideoProcessors`, `ImagingNetworks` | 45 MB | Apple's camera processing: noise reduction, portrait, stabilisation | not used by our camera path | Leave out |
| `System/Library/Photos/Resources` | 29.5 MB | the music of Memories (FlexAudio 23.5), the Photos map atlas (4.0) | Memories are silent; the Photos map lacks its atlas | Optional: PhotosAtlas (4 MB) |
| `System/Library/Carrier Bundles` | 25.7 MB | carrier settings (APN, MMS, carrier name) | read through CommCenter, which this port replaces ([cellular-star2lte.md](cellular-star2lte.md)) | Leave out until a telephony daemon reads them |
| `System/Library/UserNotifications/Bundles` | 13.6 MB | the notification sections of system services (IDS firewall 6.7, Bluetooth 1.6, Wallet 1.1, Do Not Disturb 0.6, Game Center, Exposure Notification) | those services cannot post notifications | Optional: Do Not Disturb and Bluetooth (2.2 MB) |
| `System/Library/AppPlaceholders` | 10.6 MB | placeholders for deleted system apps (the image ships the apps themselves) | none | Leave out |
| `System/Library/ProductDocuments` | 6.2 MB | warranty and regulatory texts (OnBoardingBundles, the privacy notices, *are* kept) | Settings > Legal has fewer pages | Leave out |
| `usr/share/firmware/isp` | 12.5 MB | Apple's camera ISP firmware | not this hardware | Leave out |
| GPUCompiler `.pcm`, AGXCompilerCore `.ds`, `.scube`, `.scnz`, AvatarKit `.heic` | 98 MB | Apple's GPU compiler, Memoji/Animoji | not this GPU | Leave out |
| CarPlayUIServices `.heic` | 23.8 MB | CarPlay wallpapers | no CarPlay | Leave out |
| VoiceServices `.bin` and `.caf` | 24.7 MB | system text-to-speech voices, Speak Screen | no speech output | Only with accessibility |

## Recommendation

Add back, in one select-system.py change (one re-extraction and one system.apfs rebuild):

| Item | MB | Why |
|---|---|---|
| `callservicesd` (KEEP_DAEMONS) | 5.0 | CallKit/VoIP calls, the Phone and FaceTime call state; its lookups stop failing |
| `LinguisticData/RequiredAssets_en.bundle` | 84.6 | the keyboard's autocorrection, predictions and spell checking |
| `Library/Ringtones` | 17.2 | ringtones and alert tones |
| `MapsUI.framework/Assets.car` | 8.1 | Maps' own pictures |
| `PreinstalledAssetsV2/RequiredByOs/com_apple_MobileAsset_{GameController_DB1,LanguageDetectorAssets,CoreLocationConfig}` | 22.2 | game controllers, language detection, locationd's configuration |
| for a Korean setup: `ko` in KEEP_LANG, `RequiredAssets_ko.bundle`, `usr/share/tokenizer/ko` | 48.0 | the Korean interface and typing |

That is **137 MB** (185 MB with Korean) of about 4.3 GB free. Check each item on the phone after the build, as with the
Files fix, because a feature can need more than its data: the keyboard's autocorrection, for example, also depends on
its own daemons.

Fine to leave out: the Apple-GPU pipelines and compilers, Watch, CarPlay, Siri and its voices, the ML models of Photos,
Live Text and AR, the setup films, other models' audio tunings and firmware, Screen Time's tables, the carrier bundles
(until a telephony daemon reads them) and the other languages.
