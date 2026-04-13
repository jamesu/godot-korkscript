#pragma once

#include <godot_cpp/classes/script_language.hpp>
#include <godot_cpp/classes/script_extension.hpp>

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace godot {

class KorkScriptLanguage;

class KorkScript : public ScriptExtension {
    GDCLASS(KorkScript, ScriptExtension);

public:
    KorkScript();
    ~KorkScript() override;

    void set_source_code(const String &source);
    void set_vm_name(const String &vm_name);
    void set_namespace_name(const String &namespace_name);
    void set_base_type(const String &base_type);
    void set_tool_enabled(bool enabled);

    const String &get_vm_name() const;
    const String &get_namespace_name() const;
    const String &get_base_type() const;
    bool get_tool_enabled() const;
    String get_effective_namespace_name() const;
    uint64_t get_revision() const;
    bool has_method_name(const StringName &method) const;
    bool is_tool_enabled() const;
    bool has_class_field(const StringName &field) const;
    Variant::Type get_class_field_type(const StringName &field, bool *r_exists = nullptr) const;
    bool get_class_field_default_value(const StringName &field, Variant *r_value = nullptr) const;
    bool get_previous_class_field_default_value(const StringName &field, Variant *r_value = nullptr) const;
    PackedStringArray get_class_field_names() const;

    bool _editor_can_reload_from_file() override;
    bool _can_instantiate() const override;
    Ref<Script> _get_base_script() const override;
    StringName _get_global_name() const override;
    StringName _get_instance_base_type() const override;
    void *_instance_create(Object *p_for_object) const override;
    void *_placeholder_instance_create(Object *p_for_object) const override;
    bool _instance_has(Object *p_object) const override;
    bool _has_source_code() const override;
    String _get_source_code() const override;
    void _set_source_code(const String &p_code) override;
    Error _reload(bool p_keep_state) override;
    StringName _get_doc_class_name() const override;
    TypedArray<Dictionary> _get_documentation() const override;
    bool _has_method(const StringName &p_method) const override;
    bool _has_static_method(const StringName &p_method) const override;
    Variant _get_script_method_argument_count(const StringName &p_method) const override;
    Dictionary _get_method_info(const StringName &p_method) const override;
    bool _is_tool() const override;
    bool _is_valid() const override;
    bool _is_abstract() const override;
    ScriptLanguage *_get_language() const override;
    bool _has_script_signal(const StringName &p_signal) const override;
    TypedArray<Dictionary> _get_script_signal_list() const override;
    bool _has_property_default_value(const StringName &p_property) const override;
    Variant _get_property_default_value(const StringName &p_property) const override;
    void _update_exports() override;
    TypedArray<Dictionary> _get_script_method_list() const override;
    TypedArray<Dictionary> _get_script_property_list() const override;
    int32_t _get_member_line(const StringName &p_member) const override;
    Dictionary _get_constants() const override;
    TypedArray<StringName> _get_members() const override;

protected:
    static void _bind_methods();

public:
    struct MethodArgumentMetadata {
        StringName name;
        Variant::Type type = Variant::NIL;
        StringName class_name;
    };

    struct MethodMetadata {
        std::vector<MethodArgumentMetadata> arguments;
        Variant::Type return_type = Variant::NIL;
        StringName return_class_name;
        bool has_return_value = false;
        int32_t line = -1;
    };

    struct SignalMetadata {
        std::vector<MethodArgumentMetadata> arguments;
        int32_t line = -1;
    };

    struct ClassFieldMetadata {
        StringName name;
        Variant::Type type = Variant::NIL;
        Variant default_value;
        bool has_default = false;
    };

private:
    void refresh_method_cache();
    const MethodMetadata *get_method_metadata(const StringName &method) const;
    const SignalMetadata *get_signal_metadata(const StringName &signal) const;
    const ClassFieldMetadata *get_class_field_metadata(const StringName &field) const;
    const ClassFieldMetadata *get_previous_class_field_metadata(const StringName &field) const;

    String source_code_;
    String vm_name_;
    String namespace_name_;
    String inferred_namespace_name_;
    String base_type_;
    bool tool_enabled_;
    uint64_t revision_;
    std::unordered_set<std::string> method_names_;
    std::unordered_map<std::string, MethodMetadata> method_metadata_;
    std::vector<std::string> method_order_;
    std::unordered_set<std::string> signal_names_;
    std::unordered_map<std::string, SignalMetadata> signal_metadata_;
    std::vector<std::string> signal_order_;
    std::unordered_map<std::string, ClassFieldMetadata> class_field_metadata_;
    std::vector<std::string> class_field_order_;
    std::unordered_map<std::string, ClassFieldMetadata> previous_class_field_metadata_;
};

} // namespace godot
