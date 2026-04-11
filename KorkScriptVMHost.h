#pragma once

#include "embed/api.h"

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
    bool has_signal(KorkApi::VMObject *vm_object, const StringName &signal) const;
    void trigger_signal(KorkApi::VMObject *vm_object, const StringName &signal, const Variant **args, GDExtensionInt arg_count) const;
    bool has_script_method(const KorkScript *script, const StringName &method);
    Dictionary get_script_method_info(const KorkScript *script, const StringName &method);
    TypedArray<Dictionary> get_script_method_list(const KorkScript *script);
    bool has_script_signal(const KorkScript *script, const StringName &signal);
    TypedArray<Dictionary> get_script_signal_list(const KorkScript *script);
    bool set_instance_field(KorkApi::VMObject *vm_object, const StringName &field, const Variant &value);
    bool get_instance_field(KorkApi::VMObject *vm_object, const StringName &field, Variant &value) const;
    Variant::Type get_instance_field_type(KorkApi::VMObject *vm_object, const StringName &field, bool *r_exists = nullptr) const;
    TypedArray<Dictionary> get_instance_field_list(KorkApi::VMObject *vm_object) const;
    void add_instance_property_state(KorkApi::VMObject *vm_object, GDExtensionScriptInstancePropertyStateAdd add_func, void *userdata) const;
    bool is_current_execution_target(KorkApi::VMObject *vm_object) const;

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
    static KorkApi::ConsoleValue global_m_sin_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);
    static KorkApi::ConsoleValue global_m_cos_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);
    static KorkApi::ConsoleValue global_m_tan_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);
    static KorkApi::ConsoleValue global_get_word_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);
    static KorkApi::ConsoleValue global_is_object_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);
    static bool custom_field_iterate_callback(KorkApi::Vm *vm, KorkApi::VMObject *object, KorkApi::VMIterator &state, StringTableEntry *name);
    static KorkApi::ConsoleValue custom_field_get_by_iterator_callback(KorkApi::Vm *vm, KorkApi::VMObject *object, KorkApi::VMIterator &state);
    static KorkApi::ConsoleValue custom_field_get_by_name_callback(KorkApi::Vm *vm, KorkApi::VMObject *object, const char *name, KorkApi::ConsoleValue array);
    static void custom_field_set_by_name_callback(KorkApi::Vm *vm, KorkApi::VMObject *object, const char *name, KorkApi::ConsoleValue array, U32 argc, KorkApi::ConsoleValue *argv);
    static bool custom_field_set_type_callback(KorkApi::Vm *vm, KorkApi::VMObject *object, const char *name, KorkApi::ConsoleValue array, U32 type_id);
    static KorkApi::VMObject *find_by_name_callback(void *user_ptr, StringTableEntry name, KorkApi::VMObject *parent);
    static KorkApi::VMObject *find_by_path_callback(void *user_ptr, const char *path);
    static KorkApi::VMObject *find_by_id_callback(void *user_ptr, KorkApi::SimObjectId ident);
    static KorkApi::SimObjectId get_vm_object_id(KorkApi::VMObject *object);
    static StringTableEntry get_vm_object_name(KorkApi::VMObject *object);

    static KorkApi::ConsoleValue object_call_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);
    static KorkApi::ConsoleValue object_get_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);
    static KorkApi::ConsoleValue object_set_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);
    static KorkApi::ConsoleValue object_print_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);
    static KorkApi::ConsoleValue object_get_id_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);
    static KorkApi::ConsoleValue object_get_name_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);
    static KorkApi::ConsoleValue object_dump_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);
    static KorkApi::ConsoleValue object_find_object_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);
    static KorkApi::ConsoleValue object_get_parent_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);
    static KorkApi::ConsoleValue object_get_object_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);
    static KorkApi::ConsoleValue object_get_count_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);

    void initialize_vm();
    void reset_vm();
    bool eval_script_source(const KorkScript *script);
    bool reload_known_scripts(const KorkScript *extra_script = nullptr);
    void ensure_global_math_namespace();
    KorkApi::NamespaceId ensure_namespace_for_class(const StringName &class_name);
    void install_object_bridge_methods(KorkApi::NamespaceId ns_id);
    void ensure_object_bridge_namespace();
    bool has_godot_property(Object *owner, const StringName &property) const;
    void notify_owner_property_list_changed(Object *owner) const;
    void push_execution_target(uint64_t owner_key) const;
    void pop_execution_target() const;
    uint64_t get_dynamic_field_owner_key(const KorkApi::VMObject *vm_object) const;
    uint64_t get_dynamic_field_owner_key(const Object *owner) const;
    KorkApi::SimObjectId ensure_sim_object_id(Object *owner) const;
    KorkApi::NamespaceId resolve_object_namespace(Object *owner, const KorkScript *script);
    KorkApi::VMObject *get_or_create_vm_object(Object *owner, const KorkScript *script);
    void register_vm_object(Object *owner, KorkApi::VMObject *vm_object, KorkApi::SimObjectId sim_id);
    void unregister_vm_object(Object *owner, KorkApi::VMObject *vm_object);
    KorkApi::Vm *get_vm_for_generation(uint64_t generation) const;
    const KorkScript *get_attached_korkscript(Object *owner) const;
    String get_object_name(Object *owner) const;
    Object *resolve_object_reference(Object *context_owner, const String &query) const;
    KorkApi::NamespaceId get_script_namespace(const KorkScript *script) const;

    KorkApi::ConsoleValue bridge_object_call(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const;
    KorkApi::ConsoleValue bridge_object_get(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const;
    KorkApi::ConsoleValue bridge_object_set(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const;
    KorkApi::ConsoleValue bridge_object_print(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const;
    KorkApi::ConsoleValue bridge_object_get_id(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const;
    KorkApi::ConsoleValue bridge_object_get_name(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const;
    KorkApi::ConsoleValue bridge_object_dump(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const;
    KorkApi::ConsoleValue bridge_object_find_object(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]);
    KorkApi::ConsoleValue bridge_object_get_parent(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]);
    KorkApi::ConsoleValue bridge_object_get_object(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]);
    KorkApi::ConsoleValue bridge_object_get_count(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const;
    KorkApi::ConsoleValue bridge_global_trig(int32_t argc, KorkApi::ConsoleValue argv[], real_t (*fn)(real_t)) const;
    KorkApi::ConsoleValue bridge_global_get_word(int32_t argc, KorkApi::ConsoleValue argv[]) const;
    KorkApi::ConsoleValue bridge_global_is_object(int32_t argc, KorkApi::ConsoleValue argv[]) const;
    void trigger_godot_signal(Object *owner, StringTableEntry signal_name, int argc, KorkApi::ConsoleValue *argv) const;

    Variant variant_from_console_value(KorkApi::ConsoleValue value) const;
    KorkApi::ConsoleValue console_value_from_variant(const Variant &value) const;
    KorkApi::ConsoleValue console_value_from_variant_for_call(const Variant &value) const;
    Variant parse_script_argument(KorkApi::ConsoleValue value) const;
    String console_value_to_string(KorkApi::ConsoleValue value) const;

    std::string intern_utf8(const StringName &value) const;
    std::string intern_utf8(const String &value) const;

    struct DynamicFieldEntry {
        Variant value;
        Variant::Type type = Variant::NIL;
    };

    struct DynamicFieldState {
        std::unordered_map<std::string, DynamicFieldEntry> by_name;
        std::vector<std::string> order;
    };

    DynamicFieldState *get_dynamic_field_state_for_key(uint64_t owner_key);
    const DynamicFieldState *get_dynamic_field_state_for_key(uint64_t owner_key) const;
    DynamicFieldEntry *upsert_dynamic_field_entry(uint64_t owner_key, std::string_view field_name);
    const DynamicFieldEntry *find_dynamic_field_entry(uint64_t owner_key, std::string_view field_name) const;
    void erase_dynamic_field_state(uint64_t owner_key);

    String vm_name_;
    KorkApi::Vm *vm_;
    KorkApi::ClassId godot_object_class_id_;
    KorkApi::TypeId vector2_type_id_;
    KorkApi::TypeId vector3_type_id_;
    KorkApi::TypeId vector4_type_id_;
    KorkApi::TypeId color_type_id_;
    mutable std::unordered_map<uint64_t, KorkApi::SimObjectId> sim_ids_;
    std::unordered_map<uint64_t, KorkApi::VMObject *> vm_objects_by_owner_id_;
    std::unordered_map<KorkApi::SimObjectId, KorkApi::VMObject *> vm_objects_by_id_;
    std::unordered_map<std::string, KorkApi::VMObject *> vm_objects_by_name_;
    std::unordered_map<std::string, KorkApi::VMObject *> vm_objects_by_path_;
    std::unordered_map<uint64_t, DynamicFieldState> dynamic_fields_by_owner_id_;
    std::unordered_map<uint64_t, ActiveScriptState> active_scripts_;
    std::unordered_map<uint64_t, const KorkScript *> known_scripts_;
    std::unordered_map<uint64_t, ScriptLoadState> loaded_scripts_;
    std::vector<KorkApi::Vm *> retired_vms_;
    mutable KorkApi::SimObjectId next_sim_id_;
    uint64_t generation_;
    bool reload_pending_;
    mutable std::vector<uint64_t> execution_target_stack_;
};

} // namespace godot
