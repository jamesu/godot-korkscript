#include "KorkScriptResourceFormat.h"

#include "KorkScript.h"
#include "KorkScriptLanguage.h"
#include "KorkScriptVMHost.h"
#include "ext/korkscript/engine/console/ast.h"

#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <unordered_map>
#include <unordered_set>

namespace godot {

namespace {

bool is_ks_path(const String &path) {
    const String lower_path = path.to_lower();
    return lower_path.ends_with(".ks") || lower_path.ends_with(".tscript");
}

void add_referenced_type_name(std::unordered_set<std::string> &type_names, const char *type_name) {
    if (type_name == nullptr) {
        return;
    }

    const String normalized = String(type_name).strip_edges();
    if (normalized.is_empty()) {
        return;
    }

    const String lower = normalized.to_lower();
    if (lower == "bool" ||
            lower == "int" ||
            lower == "float" ||
            lower == "uint" ||
            lower == "string" ||
            lower == "vector2" ||
            lower == "vector3" ||
            lower == "vector4" ||
            lower == "color") {
        return;
    }

    const CharString utf8 = normalized.utf8();
    type_names.emplace(utf8.get_data(), static_cast<size_t>(utf8.length()));
}

KorkApi::AstEnumerationControl collect_referenced_type_names(void *user_ptr, const KorkApi::AstEnumerationInfo *info) {
    if (user_ptr == nullptr || info == nullptr) {
        return KorkApi::AstEnumerationContinue;
    }

    std::unordered_set<std::string> &type_names = *static_cast<std::unordered_set<std::string> *>(user_ptr);
    if (info->kind == KorkApi::AstEnumerationNodeStmt) {
        if (info->nodeType == ASTNodeClassDeclStmt) {
            const ClassDeclStmtNode *class_node = static_cast<const ClassDeclStmtNode *>(info->stmtNode);
            if (class_node != nullptr) {
                add_referenced_type_name(type_names, class_node->parentName);
            }
        } else if (info->nodeType == ASTNodeFunctionDeclStmt) {
            const FunctionDeclStmtNode *function_node = static_cast<const FunctionDeclStmtNode *>(info->stmtNode);
            if (function_node != nullptr) {
                add_referenced_type_name(type_names, function_node->returnTypeName);
                for (const VarNode *arg = function_node->args; arg != nullptr; arg = static_cast<const VarNode *>(arg->getNext())) {
                    add_referenced_type_name(type_names, arg->varType);
                }
            }
        } else if (info->nodeType == ASTNodeVar) {
            const VarNode *var_node = static_cast<const VarNode *>(info->stmtNode);
            if (var_node != nullptr) {
                add_referenced_type_name(type_names, var_node->varType);
            }
        } else if (info->nodeType == ASTNodeAssignExpr) {
            const AssignExprNode *assign_node = static_cast<const AssignExprNode *>(info->stmtNode);
            if (assign_node != nullptr) {
                add_referenced_type_name(type_names, assign_node->assignTypeName);
            }
        } else if (info->nodeType == ASTNodeSlotAssign) {
            const SlotAssignNode *slot_assign_node = static_cast<const SlotAssignNode *>(info->stmtNode);
            if (slot_assign_node != nullptr) {
                add_referenced_type_name(type_names, slot_assign_node->varType);
            }
        }
    } else if (info->kind == KorkApi::AstEnumerationNodeScriptClassField) {
        const ScriptClassFieldDecl *field_node = info->scriptClassFieldNode;
        if (field_node != nullptr) {
            add_referenced_type_name(type_names, field_node->typeName);
        }
    }

    return KorkApi::AstEnumerationContinue;
}

std::unordered_set<std::string> collect_type_names_from_source(const String &source) {
    std::unordered_set<std::string> out;
    KorkScriptLanguage *language = KorkScriptLanguage::get_singleton();
    if (language == nullptr) {
        return out;
    }

    KorkScriptVMHost *host = language->get_vm_host(String("default"));
    if (host == nullptr) {
        return out;
    }

    host->enumerate_ast(source, String("[KorkScriptLoaderDeps]"), &out, &collect_referenced_type_names);
    return out;
}

std::unordered_map<std::string, String> build_kork_global_class_path_map() {
    std::unordered_map<std::string, String> out;
    ProjectSettings *settings = ProjectSettings::get_singleton();
    if (settings == nullptr) {
        return out;
    }

    const TypedArray<Dictionary> classes = settings->get_global_class_list();
    for (int i = 0; i < classes.size(); ++i) {
        const Dictionary entry = classes[i];
        const String class_name = String(entry.get("class", entry.get("name", ""))).strip_edges();
        const String path = String(entry.get("path", "")).strip_edges();
        const String language_name = String(entry.get("language", "")).strip_edges();
        if (class_name.is_empty() || path.is_empty()) {
            continue;
        }

        if (language_name != "KorkScript" && !is_ks_path(path)) {
            continue;
        }

        const CharString class_utf8 = class_name.utf8();
        out.emplace(std::string(class_utf8.get_data(), static_cast<size_t>(class_utf8.length())), path);
    }

    return out;
}

} // namespace

PackedStringArray KorkScriptResourceFormatLoader::_get_recognized_extensions() const {
    PackedStringArray out;
    out.push_back("ks");
    out.push_back("tscript");
    return out;
}

bool KorkScriptResourceFormatLoader::_recognize_path(const String &p_path, const StringName &p_type) const {
    return is_ks_path(p_path) && (p_type.is_empty() || _handles_type(p_type));
}

bool KorkScriptResourceFormatLoader::_handles_type(const StringName &p_type) const {
    return p_type == StringName("KorkScript") || p_type == StringName("Script") || p_type == StringName("Resource");
}

String KorkScriptResourceFormatLoader::_get_resource_type(const String &p_path) const {
    return is_ks_path(p_path) ? String("KorkScript") : String();
}

String KorkScriptResourceFormatLoader::_get_resource_script_class(const String &p_path) const {
    if (!is_ks_path(p_path)) {
        return String();
    }

    Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
    if (file.is_null()) {
        return String();
    }

    Ref<KorkScript> script;
    script.instantiate();
    script->set_source_code_silent(file->get_as_text());
    return script->has_declared_script_class() ? script->get_declared_script_class_name() : String();
}

PackedStringArray KorkScriptResourceFormatLoader::_get_dependencies(const String &p_path, bool p_add_types) const {
    PackedStringArray out;
    if (!is_ks_path(p_path)) {
        return out;
    }

    Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
    if (file.is_null()) {
        return out;
    }

    const String source = file->get_as_text();
    const std::unordered_set<std::string> referenced_types = collect_type_names_from_source(source);
    if (referenced_types.empty()) {
        return out;
    }

    const std::unordered_map<std::string, String> class_to_path = build_kork_global_class_path_map();
    std::unordered_set<std::string> seen_paths;
    for (const std::string &type_name : referenced_types) {
        const auto found = class_to_path.find(type_name);
        if (found == class_to_path.end()) {
            continue;
        }

        const String dep_path = found->second;
        if (dep_path.is_empty() || dep_path == p_path) {
            continue;
        }

        const CharString dep_path_utf8 = dep_path.utf8();
        if (!seen_paths.insert(std::string(dep_path_utf8.get_data(), static_cast<size_t>(dep_path_utf8.length()))).second) {
            continue;
        }

        if (p_add_types) {
            out.push_back(String("KorkScript::") + dep_path);
        } else {
            out.push_back(dep_path);
        }
    }

    return out;
}

bool KorkScriptResourceFormatLoader::_exists(const String &p_path) const {
    return is_ks_path(p_path) && FileAccess::file_exists(p_path);
}

PackedStringArray KorkScriptResourceFormatLoader::_get_classes_used(const String &p_path) const {
    PackedStringArray out;
    if (!is_ks_path(p_path)) {
        return out;
    }

    Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
    if (file.is_null()) {
        return out;
    }

    const std::unordered_set<std::string> referenced_types = collect_type_names_from_source(file->get_as_text());
    for (const std::string &type_name : referenced_types) {
        out.push_back(String(type_name.c_str()));
    }
    return out;
}

Variant KorkScriptResourceFormatLoader::_load(const String &p_path, const String &, bool, int32_t) const {
    if (!is_ks_path(p_path)) {
        return Variant();
    }

    Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
    if (file.is_null()) {
        return Variant();
    }

    Ref<KorkScript> script;
    script.instantiate();
    script->set_source_code_silent(file->get_as_text());
    return script;
}

void KorkScriptResourceFormatLoader::_bind_methods() {
}

Error KorkScriptResourceFormatSaver::_save(const Ref<Resource> &p_resource, const String &p_path, uint32_t) {
    Ref<KorkScript> script = p_resource;
    if (script.is_null()) {
        return ERR_INVALID_PARAMETER;
    }

    Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
    if (file.is_null()) {
        return FileAccess::get_open_error();
    }

    if (!file->store_string(script->_get_source_code())) {
        return file->get_error();
    }

    script->set_path(p_path);
    return OK;
}

Error KorkScriptResourceFormatSaver::_set_uid(const String &, int64_t) {
    return ERR_UNAVAILABLE;
}

bool KorkScriptResourceFormatSaver::_recognize(const Ref<Resource> &p_resource) const {
    Ref<KorkScript> script = p_resource;
    return script.is_valid();
}

PackedStringArray KorkScriptResourceFormatSaver::_get_recognized_extensions(const Ref<Resource> &p_resource) const {
    PackedStringArray out;
    if (_recognize(p_resource)) {
        out.push_back("ks");
        out.push_back("tscript");
    }
    return out;
}

bool KorkScriptResourceFormatSaver::_recognize_path(const Ref<Resource> &p_resource, const String &p_path) const {
    return _recognize(p_resource) && is_ks_path(p_path);
}

void KorkScriptResourceFormatSaver::_bind_methods() {
}

} // namespace godot
