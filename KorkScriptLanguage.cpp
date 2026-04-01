#include "KorkScriptLanguage.h"

#include "KorkScript.h"
#include "KorkScriptVMHost.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>

#include <vector>

namespace godot {

#ifndef KORKSCRIPT_LANGUAGE_NAME
#define KORKSCRIPT_LANGUAGE_NAME "KorkScript"
#endif

KorkScriptLanguage *KorkScriptLanguage::singleton_ = nullptr;

namespace {

String normalize_function_name(const String &p_function) {
    String normalized_name = p_function.strip_edges();
    if (normalized_name.is_empty()) {
        return String();
    }

    const int brace_pos = normalized_name.find("{");
    if (brace_pos >= 0) {
        normalized_name = normalized_name.substr(0, brace_pos).strip_edges();
    }
    const int paren_pos = normalized_name.find("(");
    if (paren_pos >= 0) {
        normalized_name = normalized_name.substr(0, paren_pos).strip_edges();
    }
    const int ns_lookup_pos = normalized_name.rfind("::");
    if (ns_lookup_pos >= 0) {
        normalized_name = normalized_name.substr(ns_lookup_pos + 2).strip_edges();
    }

    return normalized_name;
}

struct FunctionEntry {
    String name;
    int32_t line = -1;
};

std::vector<FunctionEntry> scan_function_entries(const String &code) {
    std::vector<FunctionEntry> out;

    int from = 0;
    while (true) {
        const int function_pos = code.find("function ", from);
        if (function_pos < 0) {
            break;
        }

        const int ns_pos = code.find("::", function_pos);
        const int open_paren = code.find("(", function_pos);
        if (ns_pos > function_pos && open_paren > ns_pos) {
            const String method_name = code.substr(ns_pos + 2, open_paren - (ns_pos + 2)).strip_edges();
            if (!method_name.is_empty()) {
                FunctionEntry entry;
                entry.name = method_name;
                entry.line = 1 + code.count("\n", 0, function_pos);
                out.push_back(entry);
            }
        }

        from = function_pos + 8;
    }

    return out;
}

int32_t find_function_line(const String &p_function, const String &p_code) {
    const String normalized_name = normalize_function_name(p_function);
    if (normalized_name.is_empty()) {
        return -1;
    }

    const std::vector<FunctionEntry> functions = scan_function_entries(p_code);
    for (const FunctionEntry &entry : functions) {
        if (entry.name == normalized_name) {
            return entry.line;
        }
    }

    return -1;
}

PackedStringArray scan_functions(const String &code) {
    PackedStringArray out;
    const std::vector<FunctionEntry> functions = scan_function_entries(code);
    for (const FunctionEntry &entry : functions) {
        out.push_back(vformat("%s:%d", entry.name, entry.line));
    }
    return out;
}

} // namespace

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

void KorkScriptLanguage::notify_script_changed(const KorkScript *script) {
    if (script == nullptr) {
        return;
    }

    KorkScriptVMHost *host = get_vm_host(script->get_vm_name());
    if (host != nullptr) {
        host->notify_script_changed(script);
    }
}

String KorkScriptLanguage::_get_name() const {
    return KORKSCRIPT_LANGUAGE_NAME;
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
    return true;
}

Dictionary KorkScriptLanguage::_validate(const String &p_script, const String &, bool p_validate_functions, bool, bool, bool) const {
    Dictionary result;
    result["valid"] = true;
    result["errors"] = TypedArray<Dictionary>();
    result["warnings"] = TypedArray<Dictionary>();
    result["safe_lines"] = PackedInt32Array();
    result["functions"] = p_validate_functions ? Variant(scan_functions(p_script)) : Variant(PackedStringArray());
    return result;
}

String KorkScriptLanguage::_validate_path(const String &p_path) const {
    String path = p_path.strip_edges();
    if (path.is_empty()) {
        return "res://new_korkscript.ks";
    }

    if (!path.begins_with("res://") && !path.begins_with("user://")) {
        path = "res://" + path;
    }

    const String lower_path = path.to_lower();
    if (!lower_path.ends_with(".ks") && !lower_path.ends_with(".tscript")) {
        const String extension = "." + _get_extension();
        path += extension;
    }

    return path;
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

int32_t KorkScriptLanguage::_find_function(const String &p_function, const String &p_code) const {
    return find_function_line(p_function, p_code);
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

ScriptLanguage::ScriptNameCasing KorkScriptLanguage::_preferred_file_name_casing() const {
    return SCRIPT_NAME_CASING_AUTO;
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
    out.push_back("tscript");
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
