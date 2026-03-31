#pragma once

#include "embed/api.h"

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace godot {

class Object;
class KorkScript;

class KorkScriptVMHost {
public:
    explicit KorkScriptVMHost(const String &vm_name);
    ~KorkScriptVMHost();

    KorkApi::Vm *get_vm() const;
    const String &get_vm_name() const;
    uint64_t get_generation() const;

    bool ensure_script_loaded(const KorkScript *script);
    void notify_script_changed(const KorkScript *script);
    void retain_script(const KorkScript *script);
    void release_script(const KorkScript *script);

    KorkApi::VMObject *create_vm_object_for(Object *owner, const KorkScript *script);
    void destroy_vm_object_for(Object *owner, const KorkScript *script, KorkApi::VMObject *vm_object);
    void release_vm_object_for_generation(KorkApi::VMObject *vm_object, uint64_t generation);

    bool call_method(KorkApi::VMObject *vm_object, const StringName &method, const Variant **args, GDExtensionInt arg_count, Variant &ret) const;

    Variant get_property(Object *owner, const StringName &property) const;
    bool set_property(Object *owner, const StringName &property, const Variant &value) const;

private:
    struct ActiveScriptState {
        const KorkScript *script = nullptr;
        uint32_t ref_count = 0;
    };

    struct ScriptLoadState {
        uint64_t revision = 0;
    };

    static void log_callback(uint32_t level, const char *console_line, void *user_ptr);
    static KorkApi::VMObject *find_by_name_callback(void *user_ptr, StringTableEntry name, KorkApi::VMObject *parent);
    static KorkApi::VMObject *find_by_path_callback(void *user_ptr, const char *path);
    static KorkApi::VMObject *find_by_id_callback(void *user_ptr, KorkApi::SimObjectId ident);
    static KorkApi::SimObjectId get_vm_object_id(KorkApi::VMObject *object);
    static StringTableEntry get_vm_object_name(KorkApi::VMObject *object);

    static KorkApi::ConsoleValue object_call_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);
    static KorkApi::ConsoleValue object_get_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);
    static KorkApi::ConsoleValue object_set_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);
    static KorkApi::ConsoleValue object_print_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);

    void initialize_vm();
    void reset_vm();
    bool eval_script_source(const KorkScript *script);
    bool reload_known_scripts(const KorkScript *extra_script = nullptr);
    KorkApi::NamespaceId ensure_namespace_for_class(const StringName &class_name);
    void ensure_object_bridge_namespace();
    KorkApi::SimObjectId ensure_sim_object_id(Object *owner);
    KorkApi::NamespaceId resolve_object_namespace(Object *owner, const KorkScript *script);
    void register_vm_object(Object *owner, KorkApi::VMObject *vm_object, KorkApi::SimObjectId sim_id);
    void unregister_vm_object(Object *owner, KorkApi::VMObject *vm_object);
    KorkApi::Vm *get_vm_for_generation(uint64_t generation) const;

    KorkApi::ConsoleValue bridge_object_call(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const;
    KorkApi::ConsoleValue bridge_object_get(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const;
    KorkApi::ConsoleValue bridge_object_set(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const;
    KorkApi::ConsoleValue bridge_object_print(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const;

    Variant variant_from_console_value(KorkApi::ConsoleValue value) const;
    KorkApi::ConsoleValue console_value_from_variant(const Variant &value) const;
    KorkApi::ConsoleValue console_value_from_variant_for_call(const Variant &value) const;
    Variant parse_script_argument(KorkApi::ConsoleValue value) const;
    String console_value_to_string(KorkApi::ConsoleValue value) const;

    std::string intern_utf8(const StringName &value) const;
    std::string intern_utf8(const String &value) const;

    String vm_name_;
    KorkApi::Vm *vm_;
    KorkApi::ClassId godot_object_class_id_;
    KorkApi::TypeId vector2_type_id_;
    KorkApi::TypeId vector3_type_id_;
    KorkApi::TypeId vector4_type_id_;
    KorkApi::TypeId color_type_id_;
    mutable std::unordered_map<uint64_t, KorkApi::SimObjectId> sim_ids_;
    std::unordered_map<KorkApi::SimObjectId, KorkApi::VMObject *> vm_objects_by_id_;
    std::unordered_map<std::string, KorkApi::VMObject *> vm_objects_by_name_;
    std::unordered_map<std::string, KorkApi::VMObject *> vm_objects_by_path_;
    std::unordered_map<uint64_t, ActiveScriptState> active_scripts_;
    std::unordered_map<uint64_t, const KorkScript *> known_scripts_;
    std::unordered_map<uint64_t, ScriptLoadState> loaded_scripts_;
    std::vector<KorkApi::Vm *> retired_vms_;
    KorkApi::SimObjectId next_sim_id_;
    uint64_t generation_;
    bool reload_pending_;
};

} // namespace godot
