# Mesquite Hips: the iPhone app that is the suit's hips

**Picture it.** You clip an iPhone to the front of your belt: the screen faces your body and the camera looks ahead,
as the page's wear guide shows. Sixty times a second, Apple's ARKit uses the camera and the motion sensors to work
out where the phone is in the room and which way it points. The app turns that into where your hips are and how they
are turned, measured from the moment you stood in a T-pose. Thirty times a second it sends that over the hub's Wi-Fi.
The hub passes each message down its USB cable, alongside the pods' data, and the Mesquite page moves the avatar's
hips around the room.

```
 iPhone: ARKit camera pose, 60 Hz
   │  the app: Zero at your T-pose, lever arm, page-tree turn, a 95-byte JSON line
   │  WebSocket text frame, 30 per second (Wi-Fi, ws://192.168.4.1:80/ws)
   ▼
 hub (ESP32 soft-AP): forwards the line, adds '\n', same USB stream as the 17 pods
   ▼
 page: js/mesq_parser.js ─▶ js/custom_icm.js handleWSMessage({bone:"Hips", ...})
   ▼
 avatar root: position from the phone; orientation from the HipsAlt pod (velvet on main)
              or from the phone (other trees, failover, the a1-implementation branch)
```

| Goes in | Comes out |
|---|---|
| ARKit's camera pose (position in metres, rotation) and its tracking state, about 60 times a second | `{"bone":"Hips","x":…,"y":…,"z":…,"w":…,"px":…,"py":…,"pz":…}` (orientation as a quaternion, hips position in metres), 30 per second by default, at most 95 bytes each |
| Your T-pose (**Zero**), one slow turn on the spot (**lever arm**), the suit tree you use in the page | A CSV log of every frame on the phone, for lining up with a recording later |

## The files

| File | What it is | Read it when |
|---|---|---|
| [HOW_IT_WORKS.md](HOW_IT_WORKS.md) | the app inside the system: the path of one message through the hub and the page, a session step by step, what is sent and when, the log, what the page does with the data, and what you see when something fails | you are using the app, or want to know why it behaves as it does |
| [MATH.md](MATH.md) | the mathematics: ARKit's frames, the Zero frame, the lever-arm fit, why each page tree needs its own setting, the 95-byte budget, the page's smoothing as a function of the rate, with numbers from the checks | you want to understand or change what the app computes |
| [CODE.md](CODE.md) | the code and the tools: Swift, SwiftUI, ARKit, Network.framework, SwiftPM and xtool; every file and its job; the threads; the checks; how to change the message safely; where to learn each piece | you want to read or change the code |
| [BUILD_ON_LINUX.md](BUILD_ON_LINUX.md) | building and installing the app on your iPhone from a Linux machine with xtool, step by step, with troubleshooting | you want the app on your phone |
| [MesquiteHips/](MesquiteHips/) | the app: a Swift package that xtool turns into an iOS app | |
| [tools/](tools/) | `run_checks.sh` (every check that needs no iPhone) and `mock_hub.js` (a hub stand-in for testing the phone without the suit) | |

## Quick start, once the app is on the phone

1. Power the hub, plug it into the computer, open the page and press **Link Pods**.
2. On the iPhone, open Settings, then Wi-Fi, and join the hub's network. Its name is `MM-` followed by the hub's MAC
   address, and its password is the one set in the `WiFi.softAP()` call in
   [Dongle_Binary.ino](../Dongle_Binary/Dongle_Binary.ino).
3. Open **Mesquite Hips**. In **Settings**, pick the suit tree you use in the page. The app's default is
   *Velvet, Spiny, Honey, VelvetGreen*, which matches the page's default tree, `velvet`.
4. Press **Start streaming**, and allow Camera and Local Network the first time.
5. Strap the phone on: front of the belt, screen against your body, camera looking ahead.
6. Once per strap-on (optional, recommended): press **Measure lever arm**. After the countdown, turn slowly on the spot,
   once around, keeping your feet in one place.
7. Stand in a T-pose facing the way the avatar should face. Press **Zero in T-pose**. The phone counts down aloud and
   says "Zeroed".
8. Still in the T-pose, press **Set T-pose** in the page. Then walk.

Every timed step is spoken, because you cannot see the screen while the phone is strapped on. To keep the screen
from wasting battery, use **Pocket screen**, which turns it black; triple-tap to wake it.

## What has been checked, and what has not

| Check | Result |
|---|---|
| The app's Core logic (`tools/run_checks.sh`, 45 checks) | ✓ quaternion conventions identical to three.js and ARKit. ✓ 180,000 random poses all encode in ≤ 95 bytes as JSON with exactly the page's keys and no coordinate exactly 0. ✓ The Zero frame tested on 4 ways of wearing the phone × 50 random headings. ✓ Lever-arm fit, rate gate, tracking policy, sign continuity |
| The hub's acceptance rule and the page's own parser (`mock_hub.js --self-test` and `--replay`) | ✓ a 95-byte line is accepted, a 96-byte line is dropped, as the real hub does. ✓ Every message of six scripted takes is accepted and parsed (largest 80 bytes) |
| End to end on a Mac (`run_checks.sh --e2e`): the app's own Network.framework link → hub stand-in → `js/mesq_parser.js` | ✓ 361 of 361 messages arrived and parsed, identical to what was sent |
| The whole app compiled and linked against the iOS frameworks (ARKit, SwiftUI, UIKit, AVFoundation), through Mac Catalyst (`run_checks.sh --catalyst`) | ✓ no errors or warnings, in the Swift 5 language mode the package uses |
| The app's messages replayed into the page's real `custom_icm.js` on the real avatar, for every tree, on `main` and on `a1-implementation` | ✓ walking, leaning and turning all come out along the avatar's facing. ✓ No frame dropped |
| A build with xtool on Linux, and a run on an iPhone with the real hub | **not done yet**: it needs your Linux machine and your phone. [BUILD_ON_LINUX.md](BUILD_ON_LINUX.md) is the procedure |

## Four things the page does that you will notice

These are in the page's code, not in the app. Each is explained, with its line, in
[HOW_IT_WORKS.md §8](HOW_IT_WORKS.md#8-what-the-page-does-with-the-message).

1. **The avatar travels half the distance you walk.** `js/custom_icm.js:228` keeps a live reference to the hips'
   position instead of a copy, so the target chases itself and settles half way.
2. **The suit trees label the phone's axes differently.** If the app's tree setting does not match the page's tree,
   walking comes out backwards. That is what the setting is for.
3. **On `main`, failover can turn the body.** Suppose the HipsAlt pod drew the hips at the T-pose and then stops for
   more than a second. The page then hands the hips' orientation to the phone, with an offset it computed against the
   pod, and the body turns, by about 100° in simulation. If that happens, redo the T-pose.
4. **The "Hips (phone)" row shows a battery of `NaN%`.** A 95-byte message has no room for a battery field. The row
   still shows **ON** while messages arrive.
