#include "KorkScriptLanguage.h"

#include "KorkScript.h"
#include "KorkScriptVMHost.h"
#include "ext/korkscript/engine/console/ast.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource.hpp>

#include <cstdlib>
#include <vector>

namespace godot {

#ifndef KORKSCRIPT_LANGUAGE_NAME
#define KORKSCRIPT_LANGUAGE_NAME "KorkScript"
#endif

KorkScriptLanguage *KorkScriptLanguage::singleton_ = nullptr;

namespace {
bool debug_global_classes_enabled() {
    static const bool enabled = []() {
        const char *value = std::getenv("KORKSCRIPT_DEBUG_GLOBAL_CLASSES");
        return value != nullptr && *value != '\0' && String(value) != "0";
    }();
    return enabled;
}

void debug_global_classes_log(const String &message) {
    if (debug_global_classes_enabled()) {
        UtilityFunctions::print(vformat("[korkscript-global] %s", message));
    }
}

struct FunctionEntry {
    String name;
    int32_t line = -1;
};

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

KorkApi::AstEnumerationControl collect_function_entries(void *user_ptr, const KorkApi::AstEnumerationInfo *info) {
    if (user_ptr == nullptr || info == nullptr || info->kind != KorkApi::AstEnumerationNodeStmt || info->nodeType != ASTNodeFunctionDeclStmt) {
        return KorkApi::AstEnumerationContinue;
    }

    std::vector<FunctionEntry> *out = static_cast<std::vector<FunctionEntry> *>(user_ptr);
    const FunctionDeclStmtNode *function_node = static_cast<const FunctionDeclStmtNode *>(info->stmtNode);
    if (function_node == nullptr || function_node->fnName == nullptr || function_node->isSignal) {
        return KorkApi::AstEnumerationContinue;
    }

    const String method_name = String(function_node->fnName).strip_edges();
    if (!method_name.is_empty()) {
        FunctionEntry entry;
        entry.name = method_name;
        entry.line = function_node->dbgLineNumber > 0 ? function_node->dbgLineNumber : -1;
        out->push_back(entry);
    }
    return KorkApi::AstEnumerationContinue;
}

std::vector<FunctionEntry> scan_function_entries(KorkScriptVMHost *host, const String &code) {
    std::vector<FunctionEntry> out;
    if (host == nullptr) {
        return out;
    }

    host->enumerate_ast(code, String("[KorkScriptLanguageValidate]"), &out, &collect_function_entries);
    return out;
}

int32_t find_function_line(KorkScriptVMHost *host, const String &p_function, const String &p_code) {
    const String normalized_name = normalize_function_name(p_function);
    if (normalized_name.is_empty()) {
        return -1;
    }

    const std::vector<FunctionEntry> functions = scan_function_entries(host, p_code);
    for (const FunctionEntry &entry : functions) {
        if (entry.name == normalized_name) {
            return entry.line;
        }
    }

    return -1;
}

PackedStringArray scan_functions(KorkScriptVMHost *host, const String &code) {
    PackedStringArray out;
    const std::vector<FunctionEntry> functions = scan_function_entries(host, code);
    for (const FunctionEntry &entry : functions) {
        out.push_back(vformat("%s:%d", entry.name, entry.line));
    }
    return out;
}

void collect_kork_script_paths(const String &path, PackedStringArray &out_paths) {
    Ref<DirAccess> dir = DirAccess::open(path);
    if (dir.is_null()) {
        return;
    }

    if (dir->list_dir_begin() != OK) {
        return;
    }

    for (String entry = dir->get_next(); !entry.is_empty(); entry = dir->get_next()) {
        if (entry == "." || entry == "..") {
            continue;
        }

        const String child_path = path.path_join(entry);
        if (dir->current_is_dir()) {
            collect_kork_script_paths(child_path, out_paths);
            continue;
        }

        const String ext = entry.get_extension().to_lower();
        if (ext == "ks" || ext == "tscript") {
            out_paths.push_back(child_path);
        }
    }

    dir->list_dir_end();
}

void refresh_kork_global_class_cache() {
    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    ResourceLoader *loader = ResourceLoader::get_singleton();
    if (project_settings == nullptr || loader == nullptr) {
        debug_global_classes_log("cache refresh skipped: missing ProjectSettings or ResourceLoader");
        return;
    }

    Array merged = project_settings->get_global_class_list();
    Array kept;
    kept.resize(0);
    for (int i = 0; i < merged.size(); ++i) {
        Dictionary entry = merged[i];
        const String language = String(entry.get("language", ""));
        const String path = String(entry.get("path", ""));
        const String ext = path.get_extension().to_lower();
        if (language == String(KORKSCRIPT_LANGUAGE_NAME) || ext == "ks" || ext == "tscript") {
            continue;
        }
        kept.push_back(entry);
    }

    PackedStringArray paths;
    collect_kork_script_paths(String("res://"), paths);
    debug_global_classes_log(vformat("cache refresh scanning %d kork scripts", paths.size()));
    for (int i = 0; i < paths.size(); ++i) {
        const String &path = paths[i];
        Ref<Resource> resource = loader->load(path, String(), ResourceLoader::CACHE_MODE_REUSE);
        Ref<KorkScript> script = resource;
        if (!script.is_valid()) {
            debug_global_classes_log(vformat("cache refresh load failed for %s", path));
            continue;
        }

        const String class_name = script->get_effective_namespace_name();
        if (class_name.is_empty()) {
            debug_global_classes_log(vformat("cache refresh no class name for %s", path));
            continue;
        }

        Dictionary entry;
        entry["class"] = class_name;
        entry["language"] = String(KORKSCRIPT_LANGUAGE_NAME);
        entry["path"] = path;
        entry["base"] = script->get_base_type();
        entry["icon"] = String();
        entry["is_abstract"] = false;
        entry["is_tool"] = script->is_tool_enabled();
        kept.push_back(entry);
        debug_global_classes_log(vformat("cache refresh registered %s from %s base=%s", class_name, path, script->get_base_type()));
    }

    project_settings->call(StringName("store_global_class_list"), kept);
    project_settings->call(StringName("refresh_global_class_list"));
    debug_global_classes_log(vformat("cache refresh stored %d total global classes", kept.size()));
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
    refresh_kork_global_class_cache();
}

String KorkScriptLanguage::_get_name() const {
    return KORKSCRIPT_LANGUAGE_NAME;
}

void KorkScriptLanguage::_init() {
    refresh_kork_global_class_cache();
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
    KorkScriptVMHost *host = const_cast<KorkScriptLanguage *>(this)->get_vm_host(String("default"));
    result["functions"] = p_validate_functions ? Variant(scan_functions(host, p_script)) : Variant(PackedStringArray());
    return result;
}

String KorkScriptLanguage::_validate_path(const String &p_path) const {
    String path = p_path.strip_edges();
    if (path.is_empty()) {
        return "Path is empty.";
    }

    if (!path.begins_with("res://") && !path.begins_with("user://")) {
        return "Path is not local.";
    }

    const String lower_path = path.to_lower();
    if (!lower_path.ends_with(".ks") && !lower_path.ends_with(".tscript")) {
        return "Invalid extension.";
    }

    return String();
}

Object *KorkScriptLanguage::_create_script() const {
    return memnew(KorkScript);
}

bool KorkScriptLanguage::_has_named_classes() const {
    return true;
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
    KorkScriptVMHost *host = const_cast<KorkScriptLanguage *>(this)->get_vm_host(String("default"));
    return find_function_line(host, p_function, p_code);
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

bool KorkScriptLanguage::_handles_global_class_type(const String &p_type) const {
    const bool handled = p_type == _get_type() || p_type == String("Script") || p_type == String("Resource");
    if (debug_global_classes_enabled()) {
        debug_global_classes_log(vformat("handles_global_class_type type=%s handled=%s", p_type, handled ? "true" : "false"));
    }
    return handled;
}

Dictionary KorkScriptLanguage::_get_global_class_name(const String &p_path) const {
    Dictionary out;
    if (p_path.is_empty()) {
        return out;
    }

    ResourceLoader *loader = ResourceLoader::get_singleton();
    if (loader == nullptr || !loader->exists(p_path)) {
        debug_global_classes_log(vformat("get_global_class_name no loader/exists for %s", p_path));
        return out;
    }

    Ref<Resource> resource = loader->load(p_path, String(), ResourceLoader::CACHE_MODE_REUSE);
    Ref<KorkScript> script = resource;
    if (!script.is_valid()) {
        debug_global_classes_log(vformat("get_global_class_name load failed for %s", p_path));
        return out;
    }

    const String effective_name = script->get_effective_namespace_name();
    const StringName global_name = effective_name.is_empty() ? StringName() : StringName(effective_name);
    if (global_name.is_empty()) {
        debug_global_classes_log(vformat("get_global_class_name empty class for %s", p_path));
        return out;
    }

    out["name"] = String(global_name);
    out["base_type"] = script->get_base_type();
    out["icon_path"] = String();
    out["is_abstract"] = false;
    out["is_tool"] = script->is_tool_enabled();
    debug_global_classes_log(vformat("get_global_class_name path=%s class=%s base=%s", p_path, String(global_name), script->get_base_type()));
    return out;
}

void KorkScriptLanguage::_bind_methods() {
}

} // namespace godot
