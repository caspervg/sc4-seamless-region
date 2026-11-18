# View math: how the game projects the city

All addresses are SimCity 4 1.1.641 for Windows. The Mac build (symbolized) agrees on every
constant below. Code: `src/dll/view/ViewMath.*` (pure) and `src/dll/view/CameraReader.*`
(live). Tests: `tests/ViewMathTests.cpp`.

## Correction to docs/plan.md

**The view is trimetric, not isometric.** The yaw offset and the view angle table are
-pi/8 (-22.5 degrees), not -pi/4.

| Item | plan.md says | Game bytes | Value |
|---|---|---|---|
| Camera +0x118 (constructor `0x007cc990`) | -45 deg | `0xbec90fdb` | -0.3926991 rad = -22.5 deg |
| View angle table `0x00abcfc4` (`kSC4YViewAngle` on Mac), 5 entries | -45 deg | `0xbec90fdb` x5 | -22.5 deg |
| `GetCityWidthFromOrthoView(16, z)` `0x007f8300` | 22.6274 | 16 (cos 22.5 + sin 22.5) | 20.905008 |
| Pixels per metre, zoom 1..5 | 0.354 .. 6.452 | cellPixels / 20.905 | 0.38268, 0.76537, 1.53073, 3.49199, 6.98397 |
| Yaw | (rot + 1) 90 - 45 | (rot + 1) 90 - 22.5 | 67.5, 157.5, 247.5, 337.5 deg |

Evidence: `ghx dec 0x007cc990 -p win` writes `0xbec90fdb` to +0x118, `ghx mem 0x00abcfc4 20 -p win`
gives `db 0f c9 be` five times, and the Mac constructor `0x0050d78a` writes the same value.
`0x3f490fdb` would be pi/4. Confidence: high (bytes read directly, same on both builds).
Consequence: one 16 m cell is exactly `cellPixels` wide on screen at every rotation (test
`CellWidthOnScreenIsTheTablePixels`), and a region step of 1024 m is **not** a whole number
of pixels. Neighbor offsets are fractional in both x and y.

## Constants

| Address | Content |
|---|---|
| `0x00abace0` | int32[5] cell pixel width: 8, 16, 32, 73, 146 |
| `0x00abaccc` | float[5] pitch radians: `0x3f060a92` 30, `0x3f1c61aa` 35, `0x3f32b8c2` 40, `0x3f490fdb` 45, 45 |
| `0x00abcfc4` | float[5] view angle: -pi/8 for every zoom |
| camera +0x120 | eye distance 8192 (constructor) |
| camera +0xf0 | magnification, 1.0 normally (`SetCustomMagnification` `0x007cd6e0`) |

## UpdateCameraZoomAndRotationParams `0x007cbe40`

With `z` = zoom index (+0x108), `r` = rotation (+0x10c), `m` = magnification (+0xf0):

```
W        = GetCityWidthFromOrthoView(16, z)       = 16 (cos 22.5 + sin 22.5) = 20.905
cell     = cellPixels[z] * m                      (float)
+0x134   = W / cell                               metres per pixel
+0x138   = cell / W                               pixels per metre (ppm)
yaw      = ((float)r + 1) * 1.5707964 + (+0x118)  (float)
pitch    = pitchTable[z]                          also stored at +0x11c
slant    = (float)(8192 / cos pitch)
+0x13c   = cos(yaw) * 8192                        eye offset x
+0x140   = sin(pitch) * slant                     eye offset y (= 8192 tan pitch)
+0x144   = sin(yaw) * 8192                        eye offset z
near     = (slant - sqrt(sx^2 + sz^2) / cos pitch) * 0.95, sx, sz = max(4096, city size)
far      = slant * 1.5
SetOrtho(L = -vpW mpp / 2, R = vpW mpp / 2, B = -vpH mpp / 2, T = vpH mpp / 2, near, far)
           vpW, vpH = +0x12c, +0x130 (viewport pixels), mpp = +0x134
```

So the ortho volume is exactly the viewport in pixels times metres per pixel.

## UpdateCameraPositionParams `0x007cc290`

```
base     = GetBaseTargetPositionForRotation(r)    (+0x1c, y = 0)
             r0 (0,0)  r1 (sizeX,0)  r2 (sizeX,sizeZ)  r3 (0,sizeZ)   (0x007cba10)
+0x10    = base + eyeOffset                       base eye
T        = +0x44                                  view target
k        = 1 - dot(T - base, eyeOffset) / |eyeOffset|^2
eye      = T + k * eyeOffset                      +0x38, so the eye stays on one plane
LookAt(eye -> T, up (0,1,0))                      cS3DCamera::LookAt 0x007ffd60
SetViewportOffset(0, 0); Project(base) -> (px, py)
offset.x =  (px - floor px) + tweakX              tweak = +0xb0/+0xb4 (screen shake)
offset.y = -(py - floor py) + tweakY
SetViewportOffset(offset)                          cS3DCamera +0x19c/+0x1a0
+0x5c = forward, +0x68 = up, +0x74 = right         copied from the view transform rows
+0x90 = ConvertWorldPointToCameraPlanePoint(eye)   (dot(eye - baseEye, right), dot(.., up))
+0x98 = ppm * +0x90 + offset                       camera plane position in pixels
```

The floor is the game's `(x - 0.5) + 12582912.0` float rounding trick with an exact-integer
fix, which equals floor(x). Note for the camera module: when the target moves more than about
`|eyeOffset|` (8192 / cos pitch, 9.5 to 11.6 km) along the horizontal view direction from the
base corner, k turns negative and the eye passes the target. The ortho image does not
change, but the near and far planes then clip the scene.

## LookAt `0x007ffd60` and the view transform

```
back  = normalize(eye - target)
right = normalize(cross(up0, back)) = normalize(back.z, 0, -back.x)
up    = cross(back, right)
```

They are stored as the columns of the camera-to-world rotation (camera +0x2c..+0x4c).
`_UpdateXform` `0x007ff3d0` transposes it into the world-to-view transform, so view rows are
right, up, back. `ConvertWorldPointToCameraPlanePoint` `0x007cbce0` reads rows 0 and 1.

Closed form (b = back):

```
back    = (cos p cos y, sin p, cos p sin y)
right   = (sin y, 0, -cos y)
up      = (-sin p cos y, cos p, -sin p sin y)
forward = -back
```

## cS3DCamera::Project `0x007fff10` and _UpdateProjection `0x007ff580`

For the ortho projection (type 1) with viewport W x H (camera +0x20/+0x24, the full viewport
unless a subview is set) and viewport offset o:

```
v        = viewXform * p                    v.x = dot(p - eye, right), v.y = dot(p - eye, up)
ndc.x    = 2 v.x / (R - L) - 2 o.x / W
ndc.y    = 2 v.y / (T - B) - 2 o.y / H
screen.x = (ndc.x + 1) W / 2 + left   = W/2 + ppm v.x - o.x
screen.y = (1 - ndc.y) H / 2 + top    = H/2 - ppm v.y + o.y
```

With a subview (`SetSubview` `0x007ff080`, flag +0x14) the matrix is adjusted so the result is
the same full-viewport pixel. Because eye - T is parallel to back, `dot(eye - T, right) = 0`,
so `v.x = dot(p - T, right)` and the target lands on the viewport centre.

## The plugin's canvas

```
canvas(p).x =  ppm * dot(p, right)
canvas(p).y = -ppm * dot(p, up)          (y down)
screen      = canvas(p) + C
C.x         = W/2 - o.x - canvas(T).x
C.y         = H/2 + o.y - canvas(T).y
```

`canvas` is linear in world metres with the city origin at (0,0). For a neighbor at world
offset O (`RegionIndex::OriginOffset`), its own canvas is shifted by `canvas(O)`.

Snap: `canvas(base) + C = floor(px) - tweakX` and `floor(py) + tweakY`, so without shake the
base corner is on a whole screen pixel. Screen pixel edges are at
`canvas = canvas(base) + integer` for every view target. `ViewMath::TileGrid` puts tile
(0,0) at `canvas(base) + floor(min - canvas(base))`, so captured tile pixels line up with
screen pixels exactly. The base corner is the far corner of the city: the top of its diamond
on screen.

Sign conventions: world y up, canvas and screen y down, rotation 0 has the eye at +x +z of the
target (yaw 67.5 deg), so +x goes right and down on screen, +z goes left and down.

Height: a point h metres up moves up by `h cos(pitch) ppm` pixels.

Zoom 5 is zoom 4 at exactly twice the size (146 = 2 x 73, same 45 degree pitch).

## Live values (CameraReader)

`CameraReader::Update` reads +0x108, +0x10c, +0xf0, +0x134, +0x138, +0x118, +0x11c, +0x12c,
+0x130, +0x44, +0x38, +0x1c, +0x158, +0x15c, +0x5c, +0x68, +0x74, +0xb0, +0xb4, +0x148 of the
camera control and +0x19c/+0x1a0 of its cS3DCamera (+0xc). It uses the live viewport offset
instead of recomputing it, so screen shake and every other effect is included. The Diagnostics
tab compares the result with `cIGZS3DCameraService::WorldToScreen`, which calls the game's own
`cS3DCamera::Project`.

## Open points

- In-game confirmation of the half-pixel match is pending (Diagnostics tab, docs/testing.md).
- `+0xb0/+0xb4` is set from cSC43DRender +0x80/+0x84 and +0x258/+0x25c in `Draw` `0x007cb530`
  (shake offsets and translated view deltas). Zero when nothing shakes.
