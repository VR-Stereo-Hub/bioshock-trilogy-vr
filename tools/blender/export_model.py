# export_model.py - write one object (or its armature's actions) out of a .blend, the format
# chosen by the --out extension. Run through tools\blender-run.ps1 with -Blend:
#
#   .\tools\blender-run.ps1 tools\blender\export_model.py -Blend <work.blend> -- --object <Name> --out <file>
#       .psk  skinned mesh + skeleton (ActorX)  - needs the PSK/PSA add-on
#       .psa  the armature's actions (every action, or --action A [--action B])
#       .obj  static mesh + .mtl
#       .glb / .gltf / .fbx  for other tools
#
# A relative --out lands in <model_workspace>\exports. Prints BVR_EXPORT lines; a missing
# object, an empty selection or an exporter error exits non-zero. There is no route back
# into the games' cooked packages: anything exported is for offline verification or for a
# mod-side loader, and it stays in the model workspace, never the repo.
# Ported from the Dishonored VR mod.
import bpy, sys, os, argparse

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ap = argparse.ArgumentParser(prog="export_model.py")
ap.add_argument("--object", required=True, help="mesh object (or armature for .psa)")
ap.add_argument("--out", required=True)
ap.add_argument("--action", action="append", default=[], help=".psa: limit to these actions")
args = ap.parse_args(argv)

ws = os.environ.get("BVR_MODEL_WS", os.getcwd())
out = args.out if os.path.isabs(args.out) else os.path.join(ws, "exports", args.out)
os.makedirs(os.path.dirname(out), exist_ok=True)
ext = os.path.splitext(out)[1].lower()

obj = bpy.data.objects.get(args.object)
if obj is None:
    print("BVR_EXPORT *** no object named %r; objects: %s" % (args.object, sorted(o.name for o in bpy.data.objects)))
    sys.exit(2)

bpy.ops.object.select_all(action="DESELECT")
sel = [obj]
rig = obj if obj.type == "ARMATURE" else obj.find_armature()
if ext == ".psk" and rig is not None:
    sel.append(rig)
for o in sel:
    o.hide_set(False)
    o.select_set(True)
bpy.context.view_layer.objects.active = obj

if ext == ".psk":
    res = bpy.ops.psk.export(filepath=out)
elif ext == ".psa":
    if rig is None:
        print("BVR_EXPORT *** %s has no armature; a PSA needs one" % obj.name)
        sys.exit(2)
    # bpy.ops.psa.export only works through its UI invoke (it collects the armatures and
    # the action list there; execute alone raises "No armatures"). Headless, do what
    # invoke + execute do, through the add-on's own module (io_scene_psk_psa 9.1.x).
    import importlib
    base = next((m for m in sys.modules if m.endswith("io_scene_psk_psa")), None)
    if base is None:
        print("BVR_EXPORT *** io_scene_psk_psa is not loaded - run tools\\blender-run.ps1 -Setup")
        sys.exit(2)
    psa_ops = importlib.import_module(base + ".psa.export.operators")
    ctx = bpy.context
    ctx.view_layer.objects.active = rig
    if rig.animation_data is None:
        rig.animation_data_create()
    pg = ctx.scene.psa_export
    psa_ops.update_actions_and_timeline_markers(ctx, [rig], pg)
    psa_ops.populate_bone_collection_list(pg.bone_collection_list, [rig],
                                          primary_key="DATA" if pg.sequence_source == "ACTIVE_ACTION" else "OBJECT")
    for item in pg.action_list:
        item.is_selected = (item.action.name in args.action) if args.action else True
    chosen = [i.action.name for i in pg.action_list if i.is_selected]
    if not chosen:
        print("BVR_EXPORT *** no actions to export; actions on file: %s" % sorted(a.name for a in bpy.data.actions))
        sys.exit(2)
    options = psa_ops.create_psa_export_options(ctx, [rig], pg)
    psa_ops.write_psa_to_file(psa_ops.build_psa(ctx, options), out)
    print("BVR_EXPORT sequences: %s" % ", ".join(chosen))
    res = {"FINISHED"}
elif ext == ".obj":
    res = bpy.ops.wm.obj_export(filepath=out, export_selected_objects=True, export_materials=True,
                                export_uv=True, export_normals=True, export_triangulated_mesh=True)
elif ext in (".glb", ".gltf"):
    res = bpy.ops.export_scene.gltf(filepath=out, use_selection=True,
                                    export_format="GLB" if ext == ".glb" else "GLTF_SEPARATE")
elif ext == ".fbx":
    res = bpy.ops.export_scene.fbx(filepath=out, use_selection=True)
else:
    print("BVR_EXPORT *** unsupported extension %s" % ext)
    sys.exit(2)

if "FINISHED" not in res or not os.path.exists(out):
    print("BVR_EXPORT *** exporter returned %s, file %s" % (sorted(res), "present" if os.path.exists(out) else "MISSING"))
    sys.exit(1)
print("BVR_EXPORT %s -> %s (%d bytes)" % (obj.name, out, os.path.getsize(out)))
