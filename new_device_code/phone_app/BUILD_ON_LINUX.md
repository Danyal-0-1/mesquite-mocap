# Building Mesquite Hips on Linux and installing it on your iPhone

**Picture it.** Apple's Xcode only runs on a Mac, but an iOS app is "just" Swift compiled for the iPhone's processor,
packed into a folder, signed, and copied over USB. **xtool** does each of those steps on Linux:

1. it borrows the iOS SDK from Xcode's own download, `Xcode.xip`;
2. it builds the app with the Swift toolchain for Linux;
3. it asks Apple, with your Apple ID, for a certificate and a provisioning profile;
4. it signs the app and installs it through `usbmuxd`, the same USB service iTunes used.

```
 Linux:  swift 6.4 + darwin SDK (from Xcode.xip) ──build──▶ MesquiteHips.app
                                                             │ sign (your Apple ID, free or paid)
                                                             ▼
                                usbmuxd ──USB cable──▶ iPhone (Developer Mode on)
```

| You need | Notes |
|---|---|
| a Linux machine, x86_64 or aarch64 | Windows works through WSL, with USB passthrough |
| an iPhone with iOS 16 or later and ARKit (any iPhone that runs iOS 16 does) | plus a USB cable |
| an Apple ID | a free one works, with limits ([§6](#6-free-and-paid-apple-ids)) |
| room to unpack Xcode during setup (tens of GB) | `Xcode.xip` is several GB and is unpacked once; both can be deleted afterwards |

The commands below follow xtool's own guide as of xtool **1.24.0** (October 2026). If xtool's
[Linux installation page](https://xtool.sh/documentation/xtooldocs/installation-linux) names a newer Swift or Xcode
version, use that one.

## 1. Install the prerequisites (once)

**Swift.** Install the Swift 6.4 toolchain for your distribution from <https://www.swift.org/install/linux/>. The
`swiftly` installer described there is the easiest way. Then check it:

```sh
swift --version
# Swift version 6.4 (swift-6.4-RELEASE) ...
```

**usbmuxd**, which talks to the iPhone over USB, and the libimobiledevice tools, which are handy for checking the
connection:

```sh
sudo apt-get install usbmuxd libimobiledevice-utils     # Debian / Ubuntu
# Fedora: sudo dnf install usbmuxd libimobiledevice-utils
# Arch:   sudo pacman -S usbmuxd libimobiledevice
```

**FUSE 2**, if your distribution does not have it, for running AppImages:

```sh
sudo apt-get install libfuse2        # Ubuntu 22.04; on 24.04 the package is libfuse2t64
```

**Node.js 18 or later**, optional, for the checks and the hub stand-in in [tools/](tools/):
`sudo apt-get install nodejs`, or your preferred way.

## 2. Download Xcode.xip (once per Xcode version)

Open <https://developer.apple.com/download/all/?q=Xcode> **in a browser**. Sign in with your Apple ID, accept the
license, and download the Xcode version xtool's guide names (**Xcode 27.1** at the time of writing). Note where
`Xcode_27.1.xip` was saved.

The download needs a signed-in browser, so `curl` cannot fetch it. Nothing from it ends up in this repository.

## 3. Install and set up xtool (once)

```sh
curl -fL "https://github.com/xtool-org/xtool/releases/latest/download/xtool-$(uname -m).AppImage" -o xtool
chmod +x xtool
sudo mv xtool /usr/local/bin/
xtool --help          # OVERVIEW: Cross-platform Xcode replacement ...
```

Then:

```sh
xtool setup
```

1. **Log in.** xtool asks for a login mode.
   - **0: API Key** needs a paid Apple Developer Program membership. It uses Apple's public App Store Connect API, with
     a Team Key that has the *App Manager* role.
   - **1: Password** works with any Apple ID, including a free one. It uses Apple's private sign-in protocol, as
     Xcode does, so you may prefer a separate Apple ID made for development.

   Your credentials go only to Apple.
2. **The SDK.** xtool asks for the path to `Xcode.xip`, for example `~/Downloads/Xcode_27.1.xip`. It unpacks it and
   installs an iOS SDK for Swift. Check it:

   ```sh
   swift sdk list
   # darwin
   ```

## 4. Get the code

```sh
git clone https://github.com/Danyal-0-1/mesquite-mocap.git
cd mesquite-mocap
git checkout phone-app-ios          # the branch this app is on, until it is merged into main
```

Optionally, run the checks that need no phone. They take about a minute and use `swiftc` and `node`:

```sh
new_device_code/phone_app/tools/run_checks.sh
# ... 45 passed, 0 failed ... all checks passed
```

## 5. Build, sign and install

1. Plug the iPhone in with the cable and unlock it.
2. If you installed `libimobiledevice-utils`, `ideviceinfo | head` should print the phone's name.
3. From the package folder, run:

   ```sh
   cd new_device_code/phone_app/MesquiteHips
   xtool dev
   ```

**What happens the first time:**

| Step | What you see | What to do |
|---|---|---|
| Build | `Planning... Building for debugging...` takes several minutes the first time, while SwiftPM builds the iOS SDK's modules; after that a rebuild takes seconds | wait |
| Pairing | the iPhone asks **Trust This Computer?** | tap *Trust*, enter the passcode; if xtool then stops with an error, run `xtool dev` again |
| Provisioning | `[Logging in] [Preparing device] [Provisioning] [Signing] [Installing]` | xtool registers the phone with your Apple ID and makes a certificate, an App ID and a profile for `cc.mesquite.hips` |
| Developer Mode | an error asking for Developer Mode | on the iPhone: Settings, Privacy & Security, **Developer Mode**, On; the phone restarts; confirm; run `xtool dev` again |
| First launch | **Untrusted Developer** | Settings, General, **VPN & Device Management**, your Apple ID, *Trust* |

The app is now on the home screen as **Mesquite Hips**. The first time you press *Start streaming*, iOS asks for the
**camera** and, at the first connection, the **local network**: allow both. Then follow the
[quick start](README.md#quick-start-once-the-app-is-on-the-phone).

**Rebuilding after a change.** Run `xtool dev` again; it reinstalls in seconds.

**Release build.** It is optimised, so it uses less CPU per frame on long takes:

```sh
xtool dev -c release
```

**More than one phone.** `xtool devices` lists them; pick one with `xtool dev --udid <udid>`.

**An .ipa file without installing**, for example to install from another machine with `xtool install`:

```sh
xtool dev build --ipa            # writes xtool/MesquiteHips.ipa
xtool install xtool/MesquiteHips.ipa
```

## 6. Free and paid Apple IDs

|  | Free Apple ID (login mode 1) | Apple Developer Program (mode 0 or 1) |
|---|---|---|
| Cost | none | yearly fee |
| The app keeps working for | **7 days**, then it will not open until you run `xtool dev` again (your settings and logs stay) | the profile's life (a year) |
| Apps installed this way at once | 3 | many |
| New App IDs | 10 per week | many |

**If xtool says the App ID is not available.** Bundle identifiers are unique across all of Apple's accounts. If
`cc.mesquite.hips` is already registered by someone else, for instance a collaborator who built it first with their
own Apple ID, change the last part in [xtool.yml](MesquiteHips/xtool.yml), for example `bundleID:
cc.mesquite.hips.yourname`, and run `xtool dev` again. Nothing else in the app depends on the identifier.

## 7. Testing the phone without the suit

With the phone and the Linux machine on the same Wi-Fi:

```sh
node new_device_code/phone_app/tools/mock_hub.js
# [hub]   in the app: ws://192.168.1.23:8765/ws
```

In the app, open Settings, then Hub, enter that address, press *Use this address*, then *Start streaming*. The stand-in
prints the rate, what the real hub would accept or drop, and what the page's parser makes of each message
([HOW_IT_WORKS.md §10](HOW_IT_WORKS.md#10-testing-without-the-suit)). If nothing arrives, check that the firewall lets
the phone reach port 8765, for example `sudo ufw allow 8765/tcp`. Afterwards set the address back to
`ws://192.168.4.1:80/ws` for the real hub.

## 8. Troubleshooting

| Problem | Fix |
|---|---|
| `xtool: ... FUSE` or `dlopen(): error loading libfuse.so.2` | install FUSE 2 ([§1](#1-install-the-prerequisites-once)), or run `xtool --appimage-extract-and-run …` |
| `swift sdk list` shows nothing after setup | rerun `xtool sdk install ~/Downloads/Xcode_27.1.xip` |
| `Darwin SDK is out of date` | a newer xtool wants a newer SDK. Download the Xcode version its guide names, then run `xtool sdk install <path>` (a "slim" SDK must be reinstalled this way) |
| `Waiting for device to be connected...` forever | check the cable and that the phone is unlocked; `ideviceinfo` must see it. Make sure `usbmuxd` is running (`systemctl status usbmuxd`; plugging the phone in usually starts it) |
| `AFCClient.Error.muxError` (WSL) | follow the WSL note in xtool's installation guide (relay the connection through iTunes on Windows) |
| errors about pairing or lockdown | unplug, unlock, plug in, tap *Trust* again; as a last resort `idevicepair unpair && idevicepair pair` |
| `An App ID with identifier … is not available` | change `bundleID` ([§6](#6-free-and-paid-apple-ids)) |
| the app opens and closes at once after a week | a free profile expired; run `xtool dev` again |
| the build stops in the app's own code | run `tools/run_checks.sh` (and `--catalyst` on a Mac) to see whether Core or the screens break. Report the first `error:` line |
| the app runs but the page shows no "Hips (phone)" | see [HOW_IT_WORKS.md §9](HOW_IT_WORKS.md#9-when-something-goes-wrong); test with the stand-in first |

## 9. On a Mac instead

xtool works on macOS too (`brew install xtool-org/tap/xtool`), using the installed Xcode's SDK, so no `.xip` is
needed. `xtool dev` is the same; `xtool dev generate-xcode-project` makes an Xcode project if you prefer Xcode. On
macOS, the Xcode license must have been accepted once (`sudo xcodebuild -license`).
