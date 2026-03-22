#pragma once

#include <godot_cpp/classes/script_language.hpp>
#include <godot_cpp/classes/script_extension.hpp>

#include <unordered_set>

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

    const String &get_vm_name() const;
    const String &get_namespace_name() const;
    const String &get_base_type() const;
    uint64_t get_revision() const;
    bool has_method_name(const StringName &method) const;

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
    TypedArray<Dictionary> _get_documentation() const override;
    bool _has_method(const StringName &p_method) const override;
    bool _has_static_method(const StringName &p_method) const override;
    Variant _get_script_method_argument_count(const StringName &p_method) const override;
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

protected:
    static void _bind_methods();

private:
    void refresh_method_cache();

    String source_code_;
    String vm_name_;
    String namespace_name_;
    String base_type_;
    uint64_t revision_;
    std::unordered_set<std::string> method_names_;
};

} // namespace godot
