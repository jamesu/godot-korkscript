# Godot Bridge

** IMPORTANT NOTE **

This extension has largely been vibe coded via ChatGPT codex in order to provide an ususual testbed for the korkscript embedding API, so exercise relevant skepticism regarding reliability and functionality.

At the moment you can indeed run KorkScript/TorqueScript scripts in godot, however it DOES NOT feature any of the torque APIs so things may appear slightly unusual in usage. Likely a fair few revisions of the interface are required for this to be truly usable in a real project.

** END IMPORTANT NOTE **

This folder contains a minimal Godot 4 GDExtension bridge for `korkscript`.

Two interfaces are currently implemented:

1) `KorkScriptVMNode` owns a single `KorkApi::Vm` instance and uses only the public embedding API from [`engine/embed/api.h`](../engine/embed/api.h).
2) Nodes can have a `KorkScript` script type attached to them.

The extension registers a `KorkScriptLanguage`/`KorkScript` pair so Godot can load `.ks` scripts through `ScriptLanguageExtension`. Scripts can choose a shared VM by name with the `vm_name` property on the script resource.

## What it exposes

From Godot:

- `initialize_vm() -> bool`
- `shutdown_vm()`
- `has_vm() -> bool`
- `execute_script(source: String, filename := "res://inline.ks") -> Variant`
- `console_output(level: int, line: String)` signal

From korkscript:

- `godot_call(target_path, method, ...args)`
- `godot_get(target_path, property)`
- `godot_set(target_path, property, value)`
- `godot_print(...args)`

Target resolution rules:

- `"."` or `"self"` targets the `KorkScriptVMNode` instance itself
- relative `NodePath`s resolve from the node
- absolute paths are retried from `/root`
- `"node:<path>"` can be used in script arguments to pass a Godot object reference into a method call

## Build

Example configure:

```sh
cmake -S . -B build \
  -DGODOT_CPP_DIR=/path/to/godot-cpp
cmake --build build --target korkscript_godot
```

The build also stages a Godot-ready addon folder automatically at:

```text
build/godot-addon/addons/korkscript/
```

or whatever you set with `KORKSCRIPT_GODOT_ADDON_DIR`.

You can build and stage in one step with:

```sh
cmake --build build --target stage_korkscript_godot_addon
```

Then copy that staged `addons/korkscript/` folder into your Godot project's `addons/` directory.

The `godot-cpp` dependency can come from either:

- a checkout passed with `GODOT_CPP_DIR`
- a CMake package that exports `godot-cpp` or `godot::cpp`

## Example

Attach `KorkScriptVMNode` to a scene and run:

```gdscript
var vm := KorkScriptVMNode.new()
add_child(vm)

var result = vm.execute_script("""
godot_print("hello from korkscript");
godot_set(".", "name", "VMNode");
return godot_get(".", "name");
""")

print(result)
```

## Example (script mode)

Attach a "KorkScript" script to a `Sprite2D` node and enter:

```
   function Sprite2D::_process(%this)
   {
     $lx = 0;
   }

   function Sprite2D::_process(%this)
    {
    $lx += 1;
    if ($lx > 200) {
      $lx = 0;
    }
     %next : Vector2 = $lx, 50;
     %this.set("position", %next);
    }
```

## Godot setup

1. Copy `addons/korkscript/` into your Godot project.
2. Open the project in Godot.
3. Restart the editor if the extension was copied in while Godot was already open.
4. Add a node of type `KorkScriptVMNode` to a scene, or create one from GDScript with `KorkScriptVMNode.new()`. OR
5. Add a script of type `KorkScript` to a node.

If Godot does not see the class, check:

- the `.gdextension` file and library are both present in `res://addons/korkscript/`
- the library filename matches the built artifact name
- the architecture matches the Godot editor you are running
