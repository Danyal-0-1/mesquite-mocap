# The mathematics of Mesquite Hips

**Picture it.** There are five frames and two corrections.

1. ARKit gives the camera's pose in a world it invented when the session started.
2. The app slides that pose from the camera to your hip centre. That is the **lever arm**.
3. It re-expresses the pose in a frame nailed to your body at the moment you stood in a T-pose. That is the **Zero**.
4. It turns the pose so that the page's tree will un-turn it correctly.
5. It rounds the result into 95 bytes.

The page then takes its own T-pose reading away, relabels the axes and smooths. Every formula below is implemented in
[MesquiteHips/Sources/MesquiteHips/Core](MesquiteHips/Sources/MesquiteHips/Core) and checked in
[MesquiteHips/Checks](MesquiteHips/Checks). The checks' names are given with each result.

```
 ARKit world A ──(lever arm l, in camera frame C)──▶ hip centre h(t) in A
        │
        └──(Zero: turn by A0 = Y(-ψ0), shift to the hip at T0, lift to height H)──▶ reported frame R
                                                                                     │ (tree turn)
                                                                                     ▼
                                       page: Δ = q q_T⁻¹, axis map, posswap, scale ×100, smoothing ──▶ scene S
```

| Symbol | Meaning |
|---|---|
| A, C, B, R, S | ARKit's world, the camera, the body (pelvis), the reported frame, the page's three.js scene |
| p_C(t), q_C(t) | the camera's position (m) and orientation (A ← C) from `ARFrame.camera.transform` |
| l | the lever arm: camera → hip centre, in C (m) |
| h(t) | the hip centre in A |
| T0, T_p | the app's Zero; the page's Set T-pose |
| Y(θ), X(θ) | rotations by θ about +Y, +X (right-hand rule) |
| ψ(v) | the heading of a horizontal direction v |
| H | the reported hip height (m), 1.00 by default |

## Contents

1. [Conventions](#1-conventions)
2. [The frames](#2-the-frames)
3. [The Zero frame](#3-the-zero-frame)
4. [The lever arm](#4-the-lever-arm)
5. [The message: 95 bytes](#5-the-message-95-bytes)
6. [Rate and the page's smoothing](#6-rate-and-the-pages-smoothing)
7. [Tracking policy and the sign of q](#7-tracking-policy-and-the-sign-of-q)
8. [Time](#8-time)
9. [References](#9-references)

---

## 1. Conventions

These are the conventions of three.js, which the page uses
([Geometry.swift](MesquiteHips/Sources/MesquiteHips/Core/Geometry.swift)).

- A quaternion is q = (x, y, z, w), with w the scalar part. The vector part is u = (x, y, z).
- The product is Hamilton's. q ⊗ r means "rotate by r, then by q".
  - It is three.js `multiplyQuaternions(q, r)`, and ARKit's `simd_quatf` multiplies the same way.
  - In components: (q⊗r).w = q_w r_w − q_x r_x − q_y r_y − q_z r_z, and so on, as in `Quat.*`.
- A unit quaternion rotates a vector v as v' = q v q⁻¹, computed as t = 2 u × v and v' = v + w t + u × t.
- q and −q are the same rotation.
- `ARFrame.camera.transform` is a 4×4 matrix whose columns 0, 1 and 2 are the camera's axes in A, and column 3 its
  position. The app turns the 3×3 block into a quaternion with **Shepperd's method**: of the four candidate
  formulas, it uses the one whose square root is largest, so it never divides by a number near zero. These are the
  same branches as three.js `setFromRotationMatrix`.
  - *Check `geometry`:* a matrix → quaternion → matrix round trip over 2,000 random rotations returns within
    1.1 × 10⁻¹⁵ rad.
- **The angle between two orientations** is θ = 2 atan2(|u(a⁻¹⊗b)|, |w(a⁻¹⊗b)|).
  - The textbook form 2 acos|a·b| loses half its digits near 1: it cannot see angles below about 10⁻⁸ rad, and the
    first version of the checks failed on exactly that.

## 2. The frames

| Frame | Origin | Axes | Source |
|---|---|---|---|
| **A**, ARKit world with `.gravity` | the camera's position when the session started | +Y up, against gravity. −Z points the way the camera looked at the start, made level. +X completes a right-handed frame | Apple, `ARConfiguration.WorldAlignment.gravity` |
| **C**, the camera | the camera | +X along the phone's long axis toward the Home-button end; +Y "up" in landscape; +Z out of the screen. The rear camera looks along **−Z** | Apple, `ARCamera.transform` |
| **B**, the body | the hip centre (the rig's `mmHips`) | +X your right, +Y up, −Z your forward | this app |
| **R**, reported | on the floor under the hip centre at T0 | B's axes at T0, made level: −Z your forward then, +Y up | this app, [§3](#3-the-zero-frame) |
| **S**, the page's scene | | +Y up. The avatar faces **+Z**: its toes point +Z in `glbs/scene2.glb`. So its right is −X | three.js |

With the screen against your body, the camera's +Z points **into** you. So "the hip centre is d behind the screen" is
the vector l = (0, 0, d) in C, whatever way up the phone is. That is the manual lever arm.

**Heading.** For a direction v with a horizontal part, ψ(v) = atan2(−v_x, −v_z). It is 0 for −Z and +90° for −X; it
is positive to the **left**, as Y(ψ) turns −Z. `Vec3.heading()` returns nothing when the horizontal part is too
short.

## 3. The Zero frame

### 3.1 The hip centre

The phone is rigid on the pelvis, so the hip centre is a fixed vector from the camera, in the camera's own frame:

```
h(t) = p_C(t) + q_C(t) l q_C(t)⁻¹
```

### 3.2 Your forward at the Zero

Find the camera's view direction in A, and keep its horizontal part:

```
f   = q_C(T0) (0, 0, −1) q_C(T0)⁻¹
f_h = (f_x, 0, f_z)
```

The Zero is refused unless |f_h| ≥ 0.3, that is, unless the camera is within 72.5° of level (cos 72.5° = 0.3).
Looking at the floor gives no forward direction.

Your forward is the view direction turned by β:

- β = 0 if the camera looks forward;
- β = π if it looks backward;
- β = −π/2 if it looks to your left;
- β = +π/2 if it looks to your right.

Turning adds to a heading, so your heading at the Zero is ψ0 = ψ(f_h) + β. The alignment that sends your forward to
−Z is a pure turn about the vertical:

```
A0 = Y(−ψ0)
```

### 3.3 The reported pose

```
p_R(t) = A0 (h(t) − h(T0)) + (0, H, 0)
q_R(t) = A0 ⊗ q_C(t) ⊗ q_C(T0)⁻¹ ⊗ A0⁻¹
```

**Where q_R comes from.**

1. The camera is fixed on the pelvis, so q_C(t) = q_B(t) ⊗ M, with M (B ← C) constant.
2. At the Zero you stand upright facing forward, so the body frame *is* the reported frame: q_B(T0) = A0⁻¹.
3. Therefore M = A0 ⊗ q_C(T0).
4. The body's orientation in R is q_R(t) = A0 ⊗ q_B(t) = A0 ⊗ q_C(t) ⊗ M⁻¹, which is the formula above.

M cancels, so how the phone is mounted (portrait, landscape, tilted) does not matter for orientation. It matters for
position only through l.

**Properties.** Each is checked in *`reported frame`*, over 4 ways of wearing the phone (front portrait tilted 8°,
front landscape, back, left hip) × 50 random headings of the wearer and of the session:

| Motion after the Zero | Reported | Worst error |
|---|---|---|
| none | p_R = (0, H, 0), q_R = identity | exact |
| 1 m forward, 0.5 m right, 0.1 m up | p_R = (0.5, H + 0.1, −1) | 1.1 × 10⁻¹⁵ m |
| 90° left turn on the spot (lever known) | p_R unchanged, q_R = Y(+90°) | 3 × 10⁻¹⁶ m, 3 × 10⁻¹⁴ ° |
| 20° forward lean | q_R = X(−20°): +Y tips toward −Z | 0 |
| a new l after the Zero | the Zero point stays at (0, H, 0) (the origin h(T0) is recomputed with the new l) | exact |

**Why H = 1.00 m.**

- The page's root target is 100·p − `initialPosition` + c, with c = (0, 100, 0) cm (`js/custom_icm.js:946-951`), and
  the hips move 0.1 of the way per message (`:956`).
- `calibrate()` sets `initialPosition` to the hips' own `position` object (`:228`), so the target contains the value
  being moved, x. Each message therefore does

  ```
  x ← x + 0.1 (100 p − x + c − x) = 0.8 x + 0.1 (100 p + c)
  ```

  This converges to **x\* = (100 p + c) / 2**: half the travel.
- At the T-pose p = (0, H, 0), so the hips' height is (100 H + 100) / 2 = 100 cm for H = 1 m. The rig's hips stand at
  99.7 cm.
- If the page copied the phone's position at its T-pose instead, the target would be 100 (p − p_T) + c, which is
  100 cm whatever H is.
- So H = 1.00 m stands the avatar on the floor under the page as it is and as it would be after that fix.
- *Real page code:* `mmHips` y = 100.0 cm after the T-pose, and a 1 m walk renders 49.9 to 50.0 cm.

### 3.4 The page trees

The page relabels the phone's orientation change Δ = q ⊗ q_T⁻¹ (`js/custom_icm.js:584-590`) with the tree's `axis`
map, and its position with `posswap` if the tree has one (`:932-939`). For `Hips`, in `trees/<id>/meta.json`:

| Tree | `axis` | `posswap` |
|---|---|---|
| velvet, spiny, honey, velvetGreen, bc | XYZ −+− | none |
| smooth | XYZ −+− | XYZ −+− |
| thorny | XYZ +++ | none |

"XYZ −+−" negates x and z. On a vector that is the rotation Y(180°). On a quaternion's vector part it is conjugation
by Y(180°), the same physical turn applied to a rotation. Both are proper rotations (determinant +1).

**What is correct.** R has −Z forward and S has +Z forward, so the frame change from R to S is Y(180°). It has to be
applied to the position and the orientation alike, because the phone reports both in one frame.

| Tree | The page applies (orientation, position) | Missing | The app sends |
|---|---|---|---|
| smooth | Y(180°), Y(180°) | nothing | p_R, q_R |
| velvet family | Y(180°), I | the position's turn | Y(180°) p_R, q_R |
| thorny | I, I | both | Y(180°) p_R, Y(180°) q_R Y(180°)⁻¹ |

This is `PageTree.wire` in [HipsFrame.swift](MesquiteHips/Sources/MesquiteHips/Core/HipsFrame.swift).

**The page change that would remove the setting.** Give every tree's `Hips` a `posswap` equal to its `axis`, as
`smooth` has. Then one frame change applies to both, and every tree is "Smooth" to the app.

**Checks.**

- *Check `page trees`:* for each setting, a forward walk lands along +Z and a forward lean tips toward +Z.
- *Real page code*, for every tree on `main` and on `a1-implementation`:
  - a 1 m walk moves the avatar along its facing, within 0.1°;
  - a 20° lean tips it 20.0° toward its facing;
  - a left turn reads +90.0°;
  - with the setting mismatched on purpose, the walk comes out at 180°.

## 4. The lever arm

### 4.1 Why it matters

If the phone is L from the hip centre, turning on the spot by θ moves it along a chord of 2L sin(θ/2). A half turn
with L = 12 cm gives 24 cm of false travel. On the real page, with the half-distance problem, that is 12.0 cm; with
the lever arm known, it is 0.1 cm.

### 4.2 Pivot calibration

While you turn on the spot, your hip centre c stays put, so every sample i gives

```
p_i + R_i l = c          (R_i is the rotation of q_C at sample i)
```

These are three equations per sample in six unknowns: l and c. The app minimises

```
J = Σ_i |p_i + R_i l − c|²  +  λ N |l − l0|²
```

1. **∂J/∂c = 0** gives c = p̄ + R̄ l, where the bars are means over the N samples.
2. **∂J/∂l = 0** uses R_iᵀR_i = I and substitutes c:

   ```
   [(1 + λ) I − R̄ᵀR̄] l = λ l0 − (1/N) Σ_i R_iᵀ (p_i − p̄)
   ```

3. The left-hand matrix is 3×3, symmetric, and at least λI: R̄ is an average of rotations, so the eigenvalues of R̄ᵀR̄
   are at most 1. The system is solved by Cramer's rule
   ([LeverArmFit.swift](MesquiteHips/Sources/MesquiteHips/Core/LeverArmFit.swift)).

### 4.3 What a turn can and cannot see

For a full turn about the vertical, R_i = Y(θ_i) R_0. The mean of Y(θ) over a full circle is diag(0, 1, 0), so
R̄ᵀR̄ = v vᵀ, with v = R_0ᵀ ŷ the vertical expressed in the camera frame. The matrix I − R̄ᵀR̄ therefore has:

- eigenvalue **0 along v**: a turn says nothing about how far the hip centre sits above or below the camera;
- eigenvalue 1 across it: the horizontal offset is fully seen.

For a turn spread evenly over an arc Θ, the horizontal eigenvalue is 1 − (sin(Θ/2)/(Θ/2))²:

| Arc Θ | Horizontal eigenvalue |
|---|---|
| a full turn | 1 |
| a half turn | 0.59 |
| a 36° twist | 0.03 |

So the app asks for at least half a turn and stops at 1.05 turns (or 30 s).

The ridge λ = 10⁻³ pins the unseen vertical part to the prior l0 (the manual depth, (0, 0, d)), and shrinks the seen
part by only 1/(1 + λ), 0.1 %.

### 4.4 No samples stored

The residual expands into running sums:

```
Σ|p_i + R_i l − c|² = Σ|p_i|² + N|l|² + N|c|² + 2 (Σ R_iᵀp_i)·l − 2 (Σ p_i)·c − 2N (R̄ l)·c
```

So the fit keeps Σp, ΣR, ΣRᵀp, Σ|p|² and N. Its memory does not grow, however long the turn.

### 4.5 Gates, and what the checks show

The fit is refused with a reason unless all of these hold:

- N ≥ 60 samples;
- the heading covered at least 180°;
- |l| ≤ 50 cm;
- the RMS residual is ≤ 3 cm.

*Check `lever arm`*: a phone 12 cm in front of the hip centre and 4 cm above it, tilted 10°, turned at 60 Hz with 2 mm
of position noise.

| Case | Result |
|---|---|
| hips still while turning | within 3 mm of the truth where a turn can see (RMS 4.9 mm) |
| hips swaying ±1 cm while turning | within 1.5 cm; a half turn's false travel falls from 25.2 cm to 2.9 cm |
| a half turn only | within 2 cm |
| a 36° twist / a "turn" that walks 60 cm / half a second | refused (not enough turn / implausible lever / too few samples) |

Sway during the turn is the limit. The method cannot tell a hip that sways in step with the turn from a lever arm, so
turn slowly, with your feet in one place.

## 5. The message: 95 bytes

### 5.1 The budget

The hub forwards a line of at most 95 bytes ([HOW_IT_WORKS.md §1](HOW_IT_WORKS.md#1-the-path-of-one-message)). The
line is made of:

| Part | Bytes |
|---|---|
| `{"bone":"Hips",` | 15 |
| four quaternion fields `"x":` … | 4 + number + 1 comma each |
| three position fields `"px":` … | 5 + number + 1 comma each |
| no comma after the last field | −1 |
| `}` | 1 |

A number with d decimals and k integer digits takes at most 1 (sign) + k + 1 (point) + d characters. The worst cases:

| Level | Quaternion | Position | Worst length | Guarantee |
|---|---|---|---|---|
| 0 | 4 decimals | 3 decimals (1 mm) | 15 + 4·12 + 3·12 = **99** (\|p\| < 10 m) | fits when the numbers are not all long |
| 1 | 3 | 3 | 15 + 4·11 + 3·12 = **95** | always fits within 10 m |
| 2 | 3 | 2 (1 cm) | 15 + 4·11 + 3·12 = **95** with \|p\| < 100 m | always fits within 100 m |
| 3 | 4 | none | 15 + 4·12 = **63** | always fits; the page holds the root |

`HubMessage.encode` uses the first level that fits. Trailing zeros are dropped and small numbers are short, so level 0
is the usual case. Every message of the scripted takes (walking, leaning, turning, swaying) was level 0, the longest
80 bytes.

*Check `hub message`:* 180,000 uniformly random poses, the worst case for length, in boxes of ±5, ±50 and ±500 m.

- Every one fits, and the longest is exactly 95 bytes.
- Every one is valid JSON with exactly the page's keys and no exponent, and every value is within its level's
  rounding step.
- The levels used were 60,268 / 91,703 / 26,840 / 1,189.

### 5.2 What rounding costs

- **Orientation.** Rounding each component to d decimals moves q by e, with |e_i| ≤ 0.5·10⁻ᵈ. The rotation moves by
  about 2|e_⊥| ≤ 2·10⁻ᵈ rad: **0.011°** at 4 decimals and 0.11° at 3. The pods' int16 encoding resolves about 0.006°.
- **Position.** ±0.5 mm at 3 decimals; ±5 mm at 2.

### 5.3 Never an exact zero

The page skips a position with a 0 in it (`js/custom_icm.js:931`, `obj.px && obj.py && obj.pz`). After a Zero the hips
start at x = z = 0, so `HubMessage.fixed(…, avoidZero: true)` sends a value that rounds to 0 as one step of its own
sign: ±0.001 m, an error of at most 1 mm.

| Take | Plain rounding: positions the page skips | The app |
|---|---|---|
| the walk-lean-turn take | all 361 (x stays exactly 0: the walk is straight) | 0 |
| a 2 cm sway around the Zero point | 106 of 361 | 0 |

The sway result is from the real page code.

### 5.4 No exponent, no locale

Numbers are built with integer arithmetic: round(v·10ᵈ), split into whole and fraction, trailing zeros removed, and
"−0" written as "0". Library formatting could print `1e-05`, which is legal JSON but costs bytes, or use a decimal
comma under some locales.

## 6. Rate and the page's smoothing

The page smooths **per message**:

- the position moves 0.1 of the way to its target (`js/custom_icm.js:956`). Because the target chases the hips
  ([§3.3](#33-the-reported-pose)), the effective fraction is 0.2;
- the orientation moves 0.35 of the way (`slerpDict.Hips`, `index.html:139`).

A fraction α per message at f messages a second is a first-order lag with time constant

```
τ = −1 / (f · ln(1 − α))          (63 % of a step in τ; 90 % in 2.3 τ)
```

| Rate | Position, page as it is (α = 0.2) | Position, if the page copied its origin (α = 0.1) | Orientation (α = 0.35) |
|---|---|---|---|
| 15 Hz | 299 ms | 633 ms | 155 ms |
| 30 Hz | **149 ms** | 316 ms | **77 ms** |
| 60 Hz | 75 ms | 158 ms | 39 ms |

So the rate changes how the hips feel. At 30 Hz, near the pods' ~32 Hz, the hips lag like the limbs do.

**The rate gate** (`PosePipeline.isDue`). A frame is sent when t − t_last ≥ 1/f − 1/240 s, a quarter of a 60 Hz frame
early, so jitter in ARKit's timestamps never skips a frame.

*Check `pipeline`*, on 10 s of 60 Hz frames with ±1.5 ms jitter:

| Rate | Messages | Worst gap error |
|---|---|---|
| 15 Hz | 150 | 2.9 ms |
| 30 Hz | 300 | 2.8 ms |
| 60 Hz | 600 | 2.9 ms |

## 7. Tracking policy and the sign of q

**Policy.** See [HOW_IT_WORKS.md §5](HOW_IT_WORKS.md#5-what-is-sent-and-when).

- With *excessive motion* or *insufficient features*, only the orientation is sent. Visual tracking, and with it
  position, degrades first; the gyroscope still integrates rotation.
- While *initializing* or *relocalizing*, nothing is sent: the world itself may be about to change.

*Check `pipeline`* confirms each state, and that a requested Zero waits for normal tracking.

**Sign.** q and −q are the same rotation, but a stream that flips between them is hard to filter or plot. Each new q
is negated if q·q_previous < 0, so consecutive samples stay in one hemisphere.

- *Check:* two full turns show no sign change.
- The page does not need this, because three.js `slerp` already takes the short way. The log and any later filter do.

## 8. Time

`ARFrame.timestamp` is the capture time on the device's host clock, the one behind `CACurrentMediaTime` and
`ProcessInfo.systemUptime`. At **Start** the app records the offset

```
offset = (Unix time now) − systemUptime
wall_s = t_s + offset
```

and writes it into the log's header. A page recording keeps browser `Date.now()` times, so the two line up by wall
time. The error is the phone's wall-clock error, set by the network time service when the phone was last online, plus
its drift since: tens of milliseconds per hour.

For a closer alignment, put a sharp, visible event at the start of the take, such as a quick hip twist. It shows in the
phone's log and in the pods at once.

## 9. References

- Apple Developer Documentation:
  - [`ARConfiguration.WorldAlignment.gravity`](https://developer.apple.com/documentation/arkit/arconfiguration/worldalignment-swift.enum/gravity): the world's axes;
  - [`ARCamera.transform`](https://developer.apple.com/documentation/arkit/arcamera/transform): the camera's axes;
  - [`ARFrame.timestamp`](https://developer.apple.com/documentation/arkit/arframe/timestamp);
  - [`ARCamera.TrackingState`](https://developer.apple.com/documentation/arkit/arcamera/trackingstate-swift.enum);
  - [Understanding world tracking](https://developer.apple.com/documentation/arkit/understanding-world-tracking).
- three.js `Quaternion` ([documentation](https://threejs.org/docs/#api/en/math/Quaternion)); the page uses r157
  (`build-static/three.module.js`).
- S. W. Shepperd, "Quaternion from rotation matrix", *Journal of Guidance and Control* 1(3):223–224, 1978.
- J. Diebel, *Representing Attitude: Euler Angles, Unit Quaternions, and Rotation Vectors*, Stanford University, 2006.
- J. Solà, *Quaternion kinematics for the error-state Kalman filter*, arXiv:1711.02508, 2017.
- K. M. Lynch and F. C. Park, *Modern Robotics: Mechanics, Planning, and Control*, Cambridge University Press, 2017,
  ch. 3 (rigid-body motions and frames). The preprint is free from the authors.
- Z. Yaniv, "Which pivot calibration?", *Proc. SPIE 9415, Medical Imaging 2015: Image-Guided Procedures*, 2015. It
  compares the algebraic and geometric pivot-calibration methods; §4 is the algebraic one.
- K. Shoemake, "Uniform random rotations", in *Graphics Gems III*, 1992. The checks draw their random rotations this
  way.
- RFC 6455, *The WebSocket Protocol*: frames, masking, fragmentation. RFC 8259, *JSON*: number syntax.
