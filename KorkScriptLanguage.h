#pragma once

#include <godot_cpp/classes/script_language_extension.hpp>

#include <memory>
#include <unordered_map>

namespace godot {

class KorkScript;
class KorkScriptVMHost;

class KorkScriptLanguage : public ScriptLanguageExtension {
    GDCLASS(KorkScriptLanguage, ScriptLanguageExtension);

public:
    KorkScriptLanguage();
    ~KorkScriptLanguage() override;

    static KorkScriptLanguage *get_singleton();
    KorkScriptVMHost *get_vm_host(const String &vm_name);
    void notify_script_changed(const KorkScript *script);

    String _get_name() const override;
    void _init() override;
    String _get_type() const override;
    String _get_extension() const override;
    void _finish() override;
    PackedStringArray _get_reserved_words() const override;
    bool _is_control_flow_keyword(const String &p_keyword) const override;
    PackedStringArray _get_comment_delimiters() const override;
    PackedStringArray _get_string_delimiters() const override;
    Ref<Script> _make_template(const String &p_template, const String &p_class_name, const String &p_base_class_name) const override;
    bool _is_using_templates() override;
    Dictionary _validate(const String &p_script, const String &p_path, bool p_validate_functions, bool p_validate_errors, bool p_validate_warnings, bool p_validate_safe_lines) const override;
    String _validate_path(const String &p_path) const override;
    Object *_create_script() const override;
    bool _supports_builtin_mode() const override;
    bool _supports_documentation() const override;
    bool _can_inherit_from_file() const override;
    bool _can_make_function() const override;
    Error _open_in_external_editor(const Ref<Script> &p_script, int32_t p_line, int32_t p_column) override;
    bool _overrides_external_editor() override;
    Dictionary _complete_code(const String &p_code, const String &p_path, Object *p_owner) const override;
    Dictionary _lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner) const override;
    void _thread_enter() override;
    void _thread_exit() override;
    TypedArray<Dictionary> _debug_get_current_stack_info() override;
    PackedStringArray _get_recognized_extensions() const override;
    TypedArray<Dictionary> _get_public_functions() const override;
    Dictionary _get_public_constants() const override;
    TypedArray<Dictionary> _get_public_annotations() const override;
    void _frame() override;
    bool _handles_global_class_type(const String &p_type) const override;
    Dictionary _get_global_class_name(const String &p_path) const override;

protected:
    static void _bind_methods();

private:
    static KorkScriptLanguage *singleton_;
    std::unordered_map<std::string, std::unique_ptr<KorkScriptVMHost>> vm_hosts_;
};

} // namespace godot
