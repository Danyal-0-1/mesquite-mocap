# How Mesquite Hips works

**Picture it.** The suit has 17 pods, and each one knows only how it is turned, not where it is. Somebody has to say
where the body is in the room. That somebody is the phone on the belt. Its camera watches the room go by, and its motion
sensors feel every turn and step. ARKit fuses the two into the phone's pose (position and orientation), about 60 times
a second. The app does three things with that pose:

- it moves the pose from the phone to the hip centre;
- it expresses the pose in a frame tied to your body at the moment you Zeroed;
- it posts the result to the hub as a short line of text.

The page treats that line as the bone `Hips`.

```
 room ──camera images──▶ ARKit (visual-inertial odometry) ──camera pose, 60 Hz──▶ PosePipeline
                                                                                      │ lever arm: phone -> hip centre
                                                                                      │ Zero frame: your T-pose
                                                                                      │ page-tree turn
                                                                                      │ rate gate, tracking policy
                                                                                      ▼
                                             HubLink (Network.framework) ──WebSocket text, ≤ 95 B──▶ hub /ws
                                                                                      │
                                                SessionLog: every frame, on the phone ◀┘
```

## Contents

1. [The path of one message](#1-the-path-of-one-message)
2. [What the phone measures](#2-what-the-phone-measures)
3. [What the app reports](#3-what-the-app-reports)
4. [A session, step by step](#4-a-session-step-by-step)
5. [What is sent, and when](#5-what-is-sent-and-when)
6. [The session log](#6-the-session-log)
7. [The screen and the settings](#7-the-screen-and-the-settings)
8. [What the page does with the message](#8-what-the-page-does-with-the-message)
9. [When something goes wrong](#9-when-something-goes-wrong)
10. [Testing without the suit](#10-testing-without-the-suit)

---

## 1. The path of one message

| Hop | Where | What happens | The rule it imposes |
|---|---|---|---|
| 1. Phone | [HubLink.swift](MesquiteHips/Sources/MesquiteHips/Transport/HubLink.swift) | one `send` with `isComplete: true` becomes one final WebSocket **text** frame, over TCP with no Nagle delay | never queue: if two messages are still in flight, the frame is skipped |
| 2. Wi-Fi | the hub's soft-AP (`MM-…`, channel 1) | the phone joins it like any network; the hub is `192.168.4.1` (the ESP32 soft-AP default) | the network has no Internet, so iOS keeps the cellular route for everything else; the app forbids cellular for this connection |
| 3. Hub WebSocket | [Dongle_Binary.ino:239-240](../Dongle_Binary/Dongle_Binary.ino) (`AsyncWebServer server(80)`, `AsyncWebSocket ws("/ws")`) | `handleWebSocketMessage` (`:477`) accepts a frame only if it is final, arrived whole and is text | fragmented, binary or split frames are ignored without a trace |
| 4. Hub USB queue | [Dongle_Binary.ino:488](../Dongle_Binary/Dongle_Binary.ino) and [mesq_hub_core.h:173](../Dongle_Binary/mesq_hub_core.h) | the hub appends `\n` and pushes the line into the same USB queue as the pods' 16-byte frames | **`len + 1 > 96` returns silently**: a message of more than 95 bytes never reaches the page |
| 5. Browser parser | [js/mesq_parser.js](../../js/mesq_parser.js) Branch B (`:259`) | a record starting with `{` up to `\n` is parsed as JSON; `commitJsonLine` (`:187`) drops it if `x`, `y`, `z` or `w` is not a finite number (`:202`) | JSON only; quaternion components must be numbers |
| 6. Dispatch | [js/webserialnative.js](../../js/webserialnative.js) `onJsonObject` (`:123`) → `_dispatch` (`:90`) | `handleWSMessage(obj)` inside a `try` | an exception is logged once per bone and the frame is lost |
| 7. The rig | [js/custom_icm.js](../../js/custom_icm.js) `handleWSMessage` (`:500`) | `bone: "Hips"` selects the rig node `mmHips` (`:506`); orientation and position are applied as in [§8](#8-what-the-page-does-with-the-message) | `px`, `py`, `pz` are skipped if any one is exactly `0` (`:931`) |

The app keeps every message within these rules: at most 95 bytes, plain JSON numbers, never an exact `0` in a
position, always a finite quaternion. [MATH.md §5](MATH.md#5-the-message-95-bytes) shows how the precision is fitted
to the byte budget. The older JSON dongle ([Device code/Dongle__JSON](../../Device%20code/Dongle__JSON/Dongle__JSON.ino))
forwards lines of any length, so the app's messages work with both hubs.

## 2. What the phone measures

ARKit **world tracking** combines the camera images with the gyroscope and accelerometer. This is visual-inertial
odometry: the features the camera sees in the room hold the drift of the inertial sensors in check.

- **Position**, in metres.
- **Orientation**, including heading. A pod's 6-axis IMU cannot hold its heading; the camera can, because the room
  does not turn.

The app configures ARKit for pose and nothing else (`PhoneController.configuration()`):

| Setting | Value | Why |
|---|---|---|
| `worldAlignment` | `.gravity` | +Y is up, and the heading is wherever the camera pointed at the session's start ([MATH.md §2](MATH.md#2-the-frames)). Compass alignment (`.gravityAndHeading`) would bring in the magnetometer, which the suit avoids |
| plane detection, environment texturing, light estimation | off | not needed; every feature left off saves heat and battery |
| a camera view | none | nothing is drawn. The status rows show whether tracking is good |

Each frame brings `ARFrame.camera.transform`, the camera's pose in ARKit's world; `ARFrame.timestamp`, the capture
time; and `camera.trackingState`. The app copies those numbers out at once and never keeps the frame, because ARKit
stops delivering frames to an app that holds on to them.

**Where to wear it.** Strap it rigidly to the pelvis, with the camera unobstructed and looking out level. The front of
the belt is what the page's wear guide shows ([trees/velvet/wearguide/threejsscene.js](../../trees/velvet/wearguide/threejsscene.js),
`loadAllPods`, the phone at (0, 100, 12) and turned 180°). The back of the belt also works: set **Rear camera looks:
Backward**. So does either hip. Portrait or landscape makes no difference.

## 3. What the app reports

Three corrections turn "where the camera is" into "where your hips are".

**The lever arm.** The camera is not at your hip joint. It sits about 10 to 15 cm in front of it. When you turn on the
spot, the phone swings around your hips. Uncorrected, a half turn would show as 20 to 30 cm of walking. The app adds
the fixed vector from the camera to the hip centre, measured in the phone's own axes. There are two ways to set it:

- **Manual:** the hip centre is *d* cm behind the screen. The default is 10 cm.
- **Measured:** **Measure lever arm** fits it from one slow turn on the spot ([MATH.md §4](MATH.md#4-the-lever-arm)).

**The Zero frame.** ARKit's world starts wherever the phone was when you pressed Start, pointing wherever the camera
looked. That frame means nothing to the page. **Zero in T-pose** redefines it at a moment you choose:

- the origin is on the floor under your hip centre, so the reported height equals your hip height (100 cm by default,
  the avatar's);
- **−Z** is your forward at that moment, and **+X** your right;
- the orientation is measured from that moment, so the pose reads as identity, `{x:0, y:0, z:0, w:1}`, at the Zero.

The page's own **Set T-pose** assumes the same thing: at that moment the wearer is upright, facing the avatar's
forward. So if both happen in the same T-pose, the phone's frame and the page's frame agree. The direction you walk
then matches the direction the avatar faces. ([MATH.md §3](MATH.md#3-the-zero-frame))

**The page-tree turn.** Each suit tree in `trees/<id>/meta.json` tells the page how to relabel the phone's axes. Most
trees relabel the orientation by a 180° turn but leave the position alone, so one of the two would come out reversed.
The app's **Suit tree** setting applies the missing half before sending, so that walking and leaning both come out the
right way ([MATH.md §3.4](MATH.md#34-the-page-trees)):

| Page tree (the page's suit menu) | App setting | What the app turns |
|---|---|---|
| Velvet, Spiny, Honey, VelvetGreen (`velvet`, `spiny`, `honey`, `velvetGreen`) | *Velvet, Spiny, Honey, VelvetGreen* (default) | the position, by 180° about the vertical |
| Smooth (`smooth`) | *Smooth (or a tree with a matching posswap)* | nothing |
| Thorny (`thorny`) | *Thorny* | the position and the orientation |

If a tree is changed so that its `"posswap"` equals its `"axis"` for `Hips`, as `smooth` has it, choose the Smooth
setting for it.

## 4. A session, step by step

| Step | You do | The app does | Why in this order |
|---|---|---|---|
| 1 | Join the hub's Wi-Fi, open the app, **Start streaming** | starts ARKit, connects to `ws://192.168.4.1:80/ws`, opens a log, keeps the screen awake; the first good frame is an automatic Zero (epoch 1) so data flows at once | the page shows **Hips (phone) ON** as soon as messages arrive |
| 2 | Strap the phone on | | |
| 3 | **Measure lever arm**, then turn slowly once around on the spot | counts down aloud; collects the turn; fits; says "Lever arm measured, *n* centimetres" (or why not) | the lever arm must be known before the Zero, because it moves the hips' origin |
| 4 | Stand in a T-pose facing the avatar's forward; **Zero in T-pose** | counts down aloud; Zeros on the next good frame; says "Zeroed. Now set T-pose on the computer." | the page's T-pose must see the phone's new frame |
| 5 | Still in the T-pose, press the page's **Set T-pose** | | the page records each device's reading as "T-pose" five seconds later |
| 6 | Move, record in the page | sends 30 messages a second, logs every frame | |
| 7 | **Stop** | closes the connection and the log | |

**Zeroing again** starts a new frame (a new *epoch* in the log). The page must then be given a new T-pose, or the hips
jump by whatever moved between the two Zeros. The same holds after the app has been in the background: ARKit tries to
pick up the same world, and if the avatar jumped, Zero and T-pose again.

## 5. What is sent, and when

**The rate.** 30 messages a second by default; 15 and 60 are offered. The page smooths the hips by a fixed fraction
**per message** (`lerp 0.1` for position, `slerp 0.35` for orientation), so the rate sets how smooth and how laggy the
hips look ([MATH.md §6](MATH.md#6-rate-and-the-pages-smoothing)). 30 is close to the pods' own rate of about 32 Hz,
and it leaves the hub's radio, shared with 17 pods on channel 1, mostly to the pods.

**What each tracking state allows** (`Tracking.allows` in [PosePipeline.swift](MesquiteHips/Sources/MesquiteHips/Core/PosePipeline.swift)):

| ARKit says | The app sends | Because |
|---|---|---|
| normal | orientation and position | |
| limited: excessive motion, or insufficient features | orientation only (no `px`, `py`, `pz`) | camera tracking, and with it position, is the first thing to degrade; the gyroscope still holds the rotation. A message without `px` is ignored by the page's position step, so the hips stay where they are instead of jumping |
| limited: initializing or relocalizing; not available | nothing | the pose may belong to a world that is about to be replaced |

**The latest pose wins.** A message that cannot go now is dropped, never queued. A queued pose would arrive late and the
page would draw the past as the present. At most two sends may be in flight; beyond that the frame is counted as
*skipped busy*, and the next frame tries again.

**Reconnecting.** Any failure closes the connection and retries after 0.5 s, doubling up to 4 s. TCP keepalive notices
a hub that lost power within about 5 s. The status row says why it is not connected.

## 6. The session log

Every **Start** opens `hips_YYYYMMDD_HHMMSS.csv` in the app's Documents folder. It is visible in the Files app, under On
My iPhone, then Mesquite Hips, and it can be shared from the main screen. There is one row per ARKit frame, whether or
not that frame was sent: about 60 rows a second, each about 170 bytes, which comes to 10 KB a second or about 40 MB an
hour. It begins like this:

```
# Mesquite Hips session log, format 1
# app=1.0.0
# device=iPhone15,2
# ios=26.1
# video=1920x1440@60
# world_alignment=gravity
# hub=ws://192.168.4.1:80/ws
# rate_hz=30
# page_tree=velvetFamily
# camera_facing=forward
# hip_height_m=1.000
# lever_cam_m=0.0080,-0.0150,0.1230
# lever_source=measured
# wall_s = t_s + 1763120000.123456
t_s,wall_s,epoch,tracking,cam_px_m,…,cam_qw,hip_px_m,…,hip_qw,sent,level,bytes
#E,5123.016667,tracking normal
#E,5123.016667,zero epoch=1 heading_deg=-37.20
5123.016667,1763125123.140,1,normal,0.00000,…
```

- `t_s` is ARKit's capture time on the phone's uptime clock.
- `wall_s` is the same moment in Unix time. Use it to line the log up with a page recording.
- `cam_*` is the raw ARKit camera pose.
- `hip_*` is what the app reported, before the page-tree turn.
- `sent`, `level` and `bytes` describe the message.
- Lines starting `#E` are events: Zero, lever fit, link state, tracking changes, background.

Read it with `pandas.read_csv(path, comment='#')`.

The hub message can carry a pose and nothing else, so this log is the only record of the phone's tracking quality,
timing and Zero history.

## 7. The screen and the settings

**Status rows.** Each has a green tick when all is well:

- **Tracking:** ARKit's state.
- **Hub:** connected, or why not.
- **Sending:** messages per second.
- **Zero:** automatic, or done in a T-pose.
- **Lever arm:** manual depth, or measured.

**Buttons.**

- **Start streaming** / **Stop**.
- **Zero in T-pose** and **Measure lever arm**, each with a spoken countdown (8 s by default).
- **Pocket screen**: a black screen; triple-tap to wake.

**Live.** The hips in cm, the heading, the last message's size and precision level, sent and skipped counts, and the
log's size.

**Settings.**

| Setting | Default | Effect |
|---|---|---|
| Hub address | `ws://192.168.4.1:80/ws` | applied when you press *Use this address*; for the hub stand-in use `ws://<computer>:8765/ws` |
| Messages per second | 30 | at once |
| Suit tree | Velvet, Spiny, Honey, VelvetGreen | at once; must match the page |
| Rear camera looks | Forward | at the next Zero |
| Hip height | 100 cm | at once (redo the page's T-pose) |
| Hip centre behind the screen | 10 cm | at once, unless a measured lever arm is set |
| Record a session log | on | from the next Start |
| Spoken cues | on | at once |
| Countdown | 8 s | next countdown |

## 8. What the page does with the message

Line numbers are in [js/custom_icm.js](../../js/custom_icm.js) on `main` at `7bdcf1c`.

**Position, every message (`:920-958`):**

1. `if (bone == "Hips" && obj.px && obj.py && obj.pz)` (`:931`). This is a truthiness test, so a coordinate of exactly
   `0` skips the whole position (this is PHN-01 in [system_assessment/07_phone_slam/REPORT.md](../../system_assessment/07_phone_slam/REPORT.md)).
   After a Zero your hips start *at* x = z = 0, and a straight walk keeps x at 0. If the app rounded honestly, the
   page would ignore every position of that walk. So the app never sends an exact 0: a value that rounds to 0 goes
   out as ±0.001 m, an error of at most 1 mm. In a simulated sway around the origin, plain rounding loses 106 of 361
   positions; the app loses none.
2. The tree's `posswap`, if it has one, relabels the axes (`getTransformedPosition`, `:478`).
3. The target is 100·p − `initialPosition` + (0, 100, 0) cm (`:946-951`), and the hips move a tenth of the way there
   per message (`hipsBone.position.lerp(…, 0.1)`, `:956`).
4. **The half-distance problem.** `calibrate()` sets `initialPosition = model.getObjectByName("mmHips").position`
   (`:228`). That is the hips' own position object, not a copy, so after the T-pose the target subtracts the very value
   it is moving. The hips settle at (100·p + (0, 100, 0)) / 2.
   - Walking 1 m moves the avatar 50 cm.
   - The app sends a hip height of 1.00 m, so at the T-pose the hips sit at (100 + 100) / 2 = 100 cm, standing on the
     floor. A copied `initialPosition` would also give 100 cm, from 100·(p − p_T) + 100, so the app's choice is right
     either way.

**Orientation:**

- `Hips` drives the hips' orientation only when the HipsAlt pod has been silent for more than a second
  (`hipsAltLast + 1000 < now`, `:582`).
  - `velvet` has a HipsAlt entry, so on `main` the pod draws the hips while it works, and the phone gives position
    only.
  - `honey`, `spiny` and `velvetGreen` disable their HipsAlt entry, and `thorny` has none. The HipsAlt branch then
    throws, so the phone always draws the hips.
  - On the `a1-implementation` branch the phone always draws the hips.
- When the phone draws them:
  - the change since the page's T-pose is Δ = q ⊗ q_T⁻¹;
  - Δ is relabelled by the tree's `axis` map (`getTransformedQuaternion`, `:439`);
  - a mounting offset is applied (`applyMountingOffset`, `:37`);
  - the hips turn 0.35 of the way per message (`slerpDict`, [index.html:139](../../index.html)).
- **Failover on `main`.**
  - `calibrate()` computes the phone's mounting offset against whatever is drawing the hips at the T-pose (`:227-325`).
    If that is the HipsAlt pod, the offset is the pod's arbitrary boot heading.
  - When the pod then stops for more than a second, the page hands the hips to the phone with that offset. In a
    simulation with the wearer standing still, the body turned by a median 102° and up to 150° at the switch.
  - Nothing the phone sends can prevent this. Keep the pod running, or redo the T-pose after a dropout.
  - If the pod is absent from the start, the phone draws the hips at the T-pose and the offset is the identity, so this
    does not happen.

**The status row.** `parseFloat(obj.batt)*100` (`:542`) is `NaN` when there is no `batt` field, so the
"Hips (phone)" row reads `NaN%`. A battery field would cost 12 of the 95 bytes. The row still turns **ON** with
every message.

## 9. When something goes wrong

| You see | Why | What to do |
|---|---|---|
| Hub: *no route to the hub: join its Wi-Fi* | the phone is not on the `MM-…` network | Settings, Wi-Fi, join it; if iOS asks, keep using the network without Internet |
| Hub: *hub refused the connection* or *timed out* | hub off, wrong address, or a different hub | check power and the address; the hub is always `192.168.4.1` on its own network |
| Hub keeps connecting, iOS asked about the Local Network | the permission was refused | Settings, Privacy & Security, Local Network, turn on Mesquite Hips |
| *Camera access is off* | the camera permission was refused | Settings, Mesquite Hips, Camera |
| Tracking: *limited excessive motion* | fast motion blurs the images | orientation still goes out; position resumes when tracking does |
| Tracking: *limited insufficient features* | a blank wall, darkness, or the camera covered by clothing | uncover the lens, add light, face a textured part of the room |
| Tracking: *limited relocalizing* | after an interruption (the app went to the background) | wait a moment in a place the camera saw before; if the avatar jumped, Zero and set the T-pose again |
| *Zero failed: the camera must look straight out* | the camera faced the floor or the ceiling | wear the phone upright, camera level |
| *Lever arm not measured: only n degrees of turn* | the turn was too small | turn at least half way round; once around is better |
| *Lever arm not measured: the feet moved too much* | walking during the turn | keep your feet in one place and turn slowly |
| The avatar walks backwards | the app's tree setting is not the page's tree | set the same tree in both |
| The avatar walks half as far as you | the page's half-distance problem (§8) | expected on `main` |
| The body turns by a large angle when the hip pod drops | the page's failover (§8) | redo the T-pose |
| The page's Hips (phone) row stays *Not Connected* while the app says connected | the page is not reading the hub, or the hub firmware does not forward | check Link Pods; test the phone with the hub stand-in (§10) |

## 10. Testing without the suit

[tools/mock_hub.js](tools/mock_hub.js) is the hub's phone path and the page's parser in one Node script, with no
packages to install. Run it on a computer on the same Wi-Fi as the phone:

```sh
node new_device_code/phone_app/tools/mock_hub.js
# [hub] mock hub (binary rule, messages up to 95 bytes)
# [hub] listening on port 8765, path /ws
# [hub]   in the app: ws://192.168.1.23:8765/ws
```

Enter that address in the app (Settings, Hub, *Use this address*) and press Start. Every second it prints:

- the message rate;
- what was accepted, and what was refused and why: oversize, fragmented, binary or split, by the hub's rule in
  `Dongle_Binary.ino`;
- the largest message;
- how many objects the page's parser produced;
- how many positions the page's zero test would have skipped;
- the last pose.

Options:

| Option | What it does |
|---|---|
| `--out capture.jsonl` | saves every accepted line with its arrival time |
| `--hub json` | applies the older JSON dongle's rule (no length limit) |
| `--replay take.jsonl` | checks a file of lines offline |
| `--self-test` | checks the stand-in itself |

To check everything that needs no phone, run [tools/run_checks.sh](tools/run_checks.sh) (details in
[CODE.md §5](CODE.md#5-the-checks)).
