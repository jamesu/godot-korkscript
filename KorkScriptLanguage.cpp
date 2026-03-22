#include "KorkScriptLanguage.h"

#include "KorkScript.h"
#include "KorkScriptVMHost.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>

namespace godot {

KorkScriptLanguage *KorkScriptLanguage::singleton_ = nullptr;

KorkScriptLanguage::KorkScriptLanguage() {
    singleton_ = this;
}

KorkScriptLanguage::~KorkScriptLanguage() {
    if (singleton_ == this) {
        singleton_ = nullptr;
    }
}

KorkScriptLanguage *KorkScriptLanguage::get_singleton() {
    return singleton_;
}

KorkScriptVMHost *KorkScriptLanguage::get_vm_host(const String &vm_name) {
    const CharString utf8 = vm_name.utf8();
    const std::string key = vm_name.is_empty() ? std::string("default") : std::string(utf8.get_data(), static_cast<size_t>(utf8.length()));
    auto found = vm_hosts_.find(key);
    if (found != vm_hosts_.end()) {
        return found->second.get();
    }

    const String resolved_name = vm_name.is_empty() ? String("default") : vm_name;
    std::unique_ptr<KorkScriptVMHost> host = std::make_unique<KorkScriptVMHost>(resolved_name);
    KorkScriptVMHost *host_ptr = host.get();
    vm_hosts_[key] = std::move(host);
    return host_ptr;
}

String KorkScriptLanguage::_get_name() const {
    return "KorkScript";
}

void KorkScriptLanguage::_init() {
}

String KorkScriptLanguage::_get_type() const {
    return "KorkScript";
}

String KorkScriptLanguage::_get_extension() const {
    return "ks";
}

void KorkScriptLanguage::_finish() {
}

PackedStringArray KorkScriptLanguage::_get_reserved_words() const {
    PackedStringArray out;
    out.push_back("function");
    out.push_back("package");
    out.push_back("return");
    out.push_back("if");
    out.push_back("else");
    out.push_back("while");
    out.push_back("for");
    return out;
}

bool KorkScriptLanguage::_is_control_flow_keyword(const String &p_keyword) const {
    return p_keyword == "if" || p_keyword == "else" || p_keyword == "while" || p_keyword == "for" || p_keyword == "return";
}

PackedStringArray KorkScriptLanguage::_get_comment_delimiters() const {
    PackedStringArray out;
    out.push_back("//");
    return out;
}

PackedStringArray KorkScriptLanguage::_get_string_delimiters() const {
    PackedStringArray out;
    out.push_back("\" \"");
    return out;
}

Ref<Script> KorkScriptLanguage::_make_template(const String &, const String &, const String &p_base_class_name) const {
    Ref<KorkScript> script;
    script.instantiate();
    script->set_base_type(p_base_class_name.is_empty() ? String("Node") : p_base_class_name);
    script->set_source_code(vformat("function %s::_ready(%%this)\n{\n}\n", script->get_base_type()));
    return script;
}

bool KorkScriptLanguage::_is_using_templates() {
    return false;
}

Dictionary KorkScriptLanguage::_validate(const String &, const String &, bool, bool, bool, bool) const {
    Dictionary result;
    result["valid"] = true;
    return result;
}

String KorkScriptLanguage::_validate_path(const String &p_path) const {
    return p_path;
}

Object *KorkScriptLanguage::_create_script() const {
    return memnew(KorkScript);
}

bool KorkScriptLanguage::_supports_builtin_mode() const {
    return true;
}

bool KorkScriptLanguage::_supports_documentation() const {
    return false;
}

bool KorkScriptLanguage::_can_inherit_from_file() const {
    return false;
}

bool KorkScriptLanguage::_can_make_function() const {
    return false;
}

Error KorkScriptLanguage::_open_in_external_editor(const Ref<Script> &, int32_t, int32_t) {
    return ERR_UNAVAILABLE;
}

bool KorkScriptLanguage::_overrides_external_editor() {
    return false;
}

Dictionary KorkScriptLanguage::_complete_code(const String &, const String &, Object *) const {
    Dictionary result;
    result["result"] = ERR_UNAVAILABLE;
    result["options"] = Array();
    result["force"] = false;
    result["call_hint"] = "";
    result["call_hint_from"] = 0;
    return result;
}

Dictionary KorkScriptLanguage::_lookup_code(const String &, const String &, const String &, Object *) const {
    Dictionary result;
    result["result"] = ERR_UNAVAILABLE;
    result["type"] = LOOKUP_RESULT_MAX;
    return result;
}

void KorkScriptLanguage::_thread_enter() {
}

void KorkScriptLanguage::_thread_exit() {
}

TypedArray<Dictionary> KorkScriptLanguage::_debug_get_current_stack_info() {
    return TypedArray<Dictionary>();
}

PackedStringArray KorkScriptLanguage::_get_recognized_extensions() const {
    PackedStringArray out;
    out.push_back("ks");
    return out;
}

TypedArray<Dictionary> KorkScriptLanguage::_get_public_functions() const {
    return TypedArray<Dictionary>();
}

Dictionary KorkScriptLanguage::_get_public_constants() const {
    return Dictionary();
}

TypedArray<Dictionary> KorkScriptLanguage::_get_public_annotations() const {
    return TypedArray<Dictionary>();
}

void KorkScriptLanguage::_frame() {
}

bool KorkScriptLanguage::_handles_global_class_type(const String &) const {
    return false;
}

Dictionary KorkScriptLanguage::_get_global_class_name(const String &) const {
    return Dictionary();
}

void KorkScriptLanguage::_bind_methods() {
}

} // namespace godot
