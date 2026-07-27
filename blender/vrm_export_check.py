"""Pre-flight check for a Blender character before you export it as VRM.

Run inside Blender:

    blender your-character.blend --background --python blender/vrm_export_check.py

or paste it into the Scripting tab and hit Run. It only reports — nothing is
modified — so it is safe to run on a file you have not saved.

It checks the things that actually break the desktop mascot downstream:
required humanoid bones, unapplied object transforms, the shape keys the
expression system drives, and mesh/material budget.
"""

import bpy

# The VRM humanoid bones this project relies on. Names are the VRM spec names;
# the VRM Add-on for Blender maps your rig onto these at export time, so if your
# bones are named differently that is fine — this list is what the *export* must
# end up filling in.
REQUIRED_BONES = [
    "hips", "spine", "chest", "neck", "head",
    "leftUpperArm", "leftLowerArm", "leftHand",
    "rightUpperArm", "rightLowerArm", "rightHand",
    "leftUpperLeg", "leftLowerLeg", "leftFoot",
    "rightUpperLeg", "rightLowerLeg", "rightFoot",
]

# Blink and the emotion presets come from expressions; `aa` drives lip sync.
EXPECTED_SHAPE_KEYS = ["aa", "ih", "ou", "ee", "oh", "blink", "happy", "angry", "sad", "relaxed"]

problems = []
notes = []


def check_armature():
    armatures = [o for o in bpy.data.objects if o.type == "ARMATURE"]
    if not armatures:
        problems.append("No armature in the scene — a VRM needs a humanoid rig.")
        return None
    if len(armatures) > 1:
        notes.append(f"{len(armatures)} armatures found; VRM exports a single one.")

    armature = armatures[0]
    bone_names = {b.name.lower() for b in armature.data.bones}

    # Loose match: a bone called "J_Bip_C_Head" or "Head.001" still counts.
    missing = [
        vrm_bone
        for vrm_bone in REQUIRED_BONES
        if not any(vrm_bone.lower() in name or name in vrm_bone.lower() for name in bone_names)
    ]
    if missing:
        notes.append(
            "Could not obviously match these humanoid bones by name: "
            + ", ".join(missing)
            + "\n    (fine if you map them by hand in the VRM add-on's Humanoid panel)"
        )
    return armature


def check_transforms():
    for obj in bpy.data.objects:
        if obj.type not in {"MESH", "ARMATURE"}:
            continue
        if tuple(round(v, 4) for v in obj.scale) != (1.0, 1.0, 1.0):
            problems.append(f"{obj.name}: scale is {tuple(round(v, 3) for v in obj.scale)} — apply it (Ctrl+A).")
        if tuple(round(v, 4) for v in obj.rotation_euler) != (0.0, 0.0, 0.0):
            problems.append(f"{obj.name}: rotation is not applied (Ctrl+A → Rotation).")


def check_shape_keys():
    found = set()
    for obj in bpy.data.objects:
        if obj.type != "MESH" or not obj.data.shape_keys:
            continue
        for key in obj.data.shape_keys.key_blocks:
            found.add(key.name.lower())

    if not found:
        notes.append(
            "No shape keys at all. The character will still load, but she cannot "
            "blink, change expression, or lip sync."
        )
        return

    missing = [k for k in EXPECTED_SHAPE_KEYS if not any(k in name for name in found)]
    if missing:
        notes.append(
            "No shape key obviously matching: "
            + ", ".join(missing)
            + "\n    (map whatever you do have in the VRM add-on's Expressions panel)"
        )


def check_budget():
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    tris = sum(len(o.data.loop_triangles) or len(o.data.polygons) * 2 for o in meshes)
    materials = {slot.material for o in meshes for slot in o.material_slots if slot.material}

    print(f"  meshes: {len(meshes)}   materials: {len(materials)}   ~triangles: {tris}")
    if len(materials) > 12:
        notes.append(f"{len(materials)} materials — each one is a draw call. Atlas them if it stutters.")
    if tris > 120_000:
        notes.append(f"~{tris} triangles is heavy for an always-on desktop overlay.")


def main():
    print("\n=== VRM export check ===")
    check_armature()
    check_transforms()
    check_shape_keys()
    check_budget()

    print("\n-- must fix --")
    print("\n".join(f"  ✗ {p}" for p in problems) or "  (none)")
    print("\n-- worth a look --")
    print("\n".join(f"  · {n}" for n in notes) or "  (none)")
    print(
        "\nExport: File → Export → VRM (needs the VRM Add-on for Blender),"
        "\nthen drop the .vrm into this project's models/ folder.\n"
    )


main()
