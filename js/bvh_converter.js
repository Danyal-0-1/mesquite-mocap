var filtersx = {};
var filtersy = {};
var filtersz = {};

const radToDeg = 180 / Math.PI;

function quaternionToEulerDegrees(q, bone) {
    var euler = new THREE.Euler().setFromQuaternion(q, "XYZ");

   // euler = smoothOrientation(euler, bone);

    return [euler.x * radToDeg, euler.y * radToDeg, euler.z * radToDeg];
}

function smoothOrientation(euler, bone) {
    if(filtersx[bone] == undefined){
        filtersx[bone] = new KalmanFilter({R: 0.01, Q: 0.01});
    }
    euler.x = filtersx[bone].filter(euler.x);

    if(filtersy[bone] == undefined){
        filtersy[bone] = new KalmanFilter({R: 0.01, Q: 0.01});
    }
    euler.y = filtersy[bone].filter(euler.y);

    if(filtersz[bone] == undefined){
        filtersz[bone] = new KalmanFilter({R: 0.01, Q: 0.01});
    }
    euler.z = filtersz[bone].filter(euler.z);

    return euler;
}


function updateMotionData() {
    const jointInfo = [];
    const rootJoint = model.getObjectByName("mmHips");
    if (rootJoint) {
        // console.log("Root joint found!", rootJoint);
        traverseHierarchy(rootJoint, jointInfo, 0);
        if (recording) {
            recordedMotionData.push(jointInfo);
        }
    } else {
        console.error("Root joint not found!");
    }
    return jointInfo;
}


function traverseHierarchy(joint, jointInfo, level = 0) {
    const jointNames = [
        "Hips",
        "LeftUpLeg",
        "RightUpLeg",
        "Spine",
        "Spine1",
        "Spine2",
        "Neck",
        "Head",
        "LeftShoulder",
        "LeftArm",
        "LeftForeArm",
        "LeftHand",
        "RightShoulder",
        "RightArm",
        "RightForeArm",
        "RightHand",
        "LeftLeg",
        "LeftFoot",
        "LeftToeBase",
        "RightLeg",
        "RightFoot",
        "RightToeBase",
    ];


    const name = joint.name.replace("mm", "");
   

    const jointExists = jointInfo.some((existingJoint) => existingJoint.name === name);

    if (jointNames.includes(name) && !jointExists) {
        var position = joint.position;
        position  = position.toArray();
        if (name === "Hips") {
            position[1] -= 0;
        }
        const rotation = quaternionToEulerDegrees(joint.quaternion, name);

        jointInfo.push({
            name: name,
            position: position,
            rotation: rotation,
            level: level,
        });
    }

    joint.children.forEach((child) => {
        if (child.type === "Bone") {
            if (!jointExists){
            traverseHierarchy(child, jointInfo, level + 1);
            }
            else{
                traverseHierarchy(child, jointInfo, level);
            }
        }
    });
}



function generateBVH(jointInfo, motionData) {
    console.log(jointInfo);
    let bvhContent = "HIERARCHY\n";

    jointInfo.forEach((joint, index) => {
        const indentation = "  ".repeat(joint.level);
        bvhContent += `${indentation}${joint.level === 0 ? "ROOT" : "JOINT"} ${joint.name}\n`;
        bvhContent += `${indentation}{\n`;
        bvhContent += `${indentation}  OFFSET ${joint.position.join(" ")}\n`;

        if (joint.name === "Hips") {
            bvhContent += `${indentation}  CHANNELS 6 Xposition Yposition Zposition Xrotation Yrotation Zrotation\n`;
        } 
        else {
            bvhContent += `${indentation}  CHANNELS 3 Xrotation Yrotation Zrotation\n`;
        }

        if (index === jointInfo.length - 1 || jointInfo[index + 1].level <= joint.level) {
            bvhContent += `${indentation}  End Site\n`;
            bvhContent += `${indentation}  {\n`;
            bvhContent += `${indentation}    OFFSET 0 0 0\n`;
            bvhContent += `${indentation}  }\n`;
            // for (let i = joint.level; i > 0; i--) {
            //     bvhContent += `${"  ".repeat(i - 1)}}\n`; // Close braces for all the parent joints
            // }
        }

        if (index < jointInfo.length - 1) {
            const nextJoint = jointInfo[index + 1];
            if (nextJoint.level <= joint.level) {
                for (let i = 0; i < joint.level - nextJoint.level + 1; i++) {
                    bvhContent += `${"  ".repeat(joint.level - i)}}\n`; 
                }
            }
        }
        if (index === jointInfo.length - 1) {
            for (let i = joint.level; i >= 0; i--) {
                bvhContent += `${"  ".repeat(i)}}\n`; 
            }
        }
    });

    // Add the MOTION section with the appropriate number of frames and frame time
    numFramesrecorded = motionData.length;
    // ===== I12 (SYNC-07 / WEB-03 / EST-05 / INT-04) =====
    // Phase 1 measured that hardcoding 1/30 over data actually sampled at
    // ~32 Hz imposes a ~7% PROGRESSIVE time dilation on every capture, which
    // is why cross-correlation against ground truth cannot lock (the existing
    // benchmark reports latencies of -300 ms and -2200 ms for two recordings
    // of the same activity, at correlations of only ~0.6).
    //
    // Use the MEASURED mean frame interval when the recorder supplied one.
    // window.mesqFrameTiming is populated by the recording path; if it is
    // absent we fall back to 1/30 AND say so in the file, so a reader can
    // tell an asserted frame time from a measured one.
    //
    // Phase 3: the guard below was `_ft.startMs && _ft.endMs && ...`, a
    // TRUTHINESS test on a numeric timestamp. A startMs of exactly 0 -- which
    // performance.now() can legitimately return, and which every synthetic
    // fixture uses -- is falsy, so the measured branch was skipped and the
    // export silently reverted to the asserted 1/30. Same defect class as
    // PHN-01's position guard. Test numbers as numbers.
    var frameTime;
    var _ft = (typeof window !== 'undefined') && window.mesqFrameTiming;
    var _ftMeasured = false;
    var _num = function (v) { return typeof v === 'number' && isFinite(v); };
    if (_ft && _num(_ft.frames) && _ft.frames > 1
             && _num(_ft.startMs) && _num(_ft.endMs) && _ft.endMs > _ft.startMs) {
        frameTime = ((_ft.endMs - _ft.startMs) / 1000) / (_ft.frames - 1);
        _ftMeasured = true;
    } else {
        frameTime = 1/30;
    }
    console.log(numFramesrecorded);
    bvhContent += "MOTION\n";
    bvhContent += `Frames: ${numFramesrecorded}\n`;
    bvhContent += `Frame Time: ${frameTime}\n`;
    // Provenance comment lines. BVH readers ignore unknown trailing header
    // text, and this is the only wall-clock anchor a capture has ever had
    // (§13: makes the tester's field log joinable to the second).
    if (_ft && _ft.startISO) {
        bvhContent += `; MESQ_CAPTURE_START ${_ft.startISO}\n`;
        bvhContent += `; MESQ_CAPTURE_END   ${_ft.endISO || ''}\n`;
    }
    bvhContent += `; MESQ_FRAME_TIME_SOURCE ${_ftMeasured ? 'measured' : 'ASSUMED_1_30_UNTRUSTWORTHY'}\n`;

    motionData.forEach(frame => {
        frame.forEach(joint => {
            if (joint.name === "Hips") {
                bvhContent += `${joint.position.join(" ")} ${joint.rotation.join(" ")} `;
            } else {
                bvhContent += `${joint.rotation.join(" ")} `;
            }
        });
        bvhContent += "\n";
    });

    return bvhContent;
}
