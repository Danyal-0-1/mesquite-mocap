// =========================================================================
//  SESSION SIMULATOR  --  Checks/Simulator.swift
//
//  The truth the checks compare against: a wearer in a room, a phone
//  strapped to the pelvis, and the world an ARKit session would define.
//
//  Frames (all right-handed, +Y up):
//    room    the true, fixed world of the simulation
//    pelvis  x = wearer's right, y = up, -z = wearer's forward; origin at
//            the hip centre (the rig's "Hips" joint)
//    camera  ARKit's camera axes (Apple, ARCamera.transform): +x toward the
//            Home-button end, +z out of the screen; the rear camera looks
//            along -z
//    ARKit   origin at the camera's position when the session started, -Z
//            along the camera's view at that moment, projected level
//            (Apple, ARConfiguration.WorldAlignment.gravity)
// =========================================================================
import Foundation

struct Wearer {
    var hip: Vec3            // hip centre in the room, metres
    var pelvis: Quat         // room <- pelvis
}

struct Mount {
    var camInPelvis: Quat    // pelvis <- camera
    var camOffset: Vec3      // camera position relative to the hip centre, pelvis frame

    /// The lever arm the app needs: camera -> hip centre, in the camera frame.
    var leverCam: Vec3 { camInPelvis.conjugate.rotate(-camOffset) }

    /// The wear guide's mount: front of the belt, screen to the body, camera
    /// looking forward, phone upright (portrait): camera +x points down,
    /// +y to the wearer's right, +z into the body.
    static func frontPortrait(offset: Vec3 = Vec3(0, 0.03, -0.12), tilt: Quat = .identity) -> Mount {
        let q = Quat(columns: Vec3(0, -1, 0), Vec3(1, 0, 0), Vec3(0, 0, 1))
        return Mount(camInPelvis: tilt * q, camOffset: offset)
    }

    /// Lying sideways (landscape), as the wear guide's phone model is drawn.
    static func frontLandscape(offset: Vec3 = Vec3(0, 0.03, -0.12)) -> Mount {
        let q = Quat(columns: Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1))
        return Mount(camInPelvis: q, camOffset: offset)
    }

    /// On the lower back, camera looking backward, screen to the body.
    static func backPortrait(offset: Vec3 = Vec3(0, 0.05, 0.10)) -> Mount {
        // camera +z (into the body) = pelvis -z; +x down; +y = +z x +x
        let cz = Vec3(0, 0, -1), cx = Vec3(0, -1, 0)
        return Mount(camInPelvis: Quat(columns: cx, cz.cross(cx), cz), camOffset: offset)
    }

    /// On the left hip, camera looking to the wearer's left.
    static func leftSide(offset: Vec3 = Vec3(-0.16, 0.0, 0)) -> Mount {
        let cz = Vec3(1, 0, 0), cx = Vec3(0, -1, 0)
        return Mount(camInPelvis: Quat(columns: cx, cz.cross(cx), cz), camOffset: offset)
    }
}

struct ARWorld {
    var roomFromWorld: Quat
    var originInRoom: Vec3

    /// The world a session started with the camera at this room pose defines.
    init(cameraInRoom q: Quat, at p: Vec3) {
        let view = q.rotate(Vec3(0, 0, -1))
        roomFromWorld = .yaw(view.heading() ?? 0)
        originInRoom = p
    }

    /// What ARFrame.camera.transform would say for this wearer.
    func camera(_ w: Wearer, _ m: Mount) -> (p: Vec3, q: Quat) {
        let qRoom = w.pelvis * m.camInPelvis
        let pRoom = w.hip + w.pelvis.rotate(m.camOffset)
        return (roomFromWorld.conjugate.rotate(pRoom - originInRoom), (roomFromWorld.conjugate * qRoom).normalized)
    }
}

/// A scripted take: piecewise motion of the wearer, sampled at ARKit's 60 Hz.
struct Take {
    struct Sample {
        var t: Double
        var wearer: Wearer
        var tracking: Tracking
        var zero = false          // the wearer asks the app to Zero here
        var calibrate = false     // the page's Set T-pose fires here
    }
    var samples: [Sample] = []
}

/// Facing `heading` in the room, at hip height 0.98 m, upright.
func standing(at p: Vec3 = Vec3(0, 0.98, 0), heading: Double) -> Wearer {
    Wearer(hip: p, pelvis: .yaw(heading))
}

func smoothstep(_ x: Double) -> Double {
    let c = min(1, max(0, x))
    return c * c * (3 - 2 * c)
}
