#include "KorkScriptLanguage.h"

#include "KorkScript.h"
#include "KorkScriptVMHost.h"
#include "ext/korkscript/engine/console/ast.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/color.hpp>

#include <unordered_set>
#include <vector>

namespace godot {

#ifndef KORKSCRIPT_LANGUAGE_NAME
#define KORKSCRIPT_LANGUAGE_NAME "KorkScript"
#endif

KorkScriptLanguage *KorkScriptLanguage::singleton_ = nullptr;

namespace {
struct FunctionEntry {
    String name;
    int32_t line = -1;
};

struct LookupMetadata {
    String script_namespace_name;
    String script_class_name;
    String script_parent_name;
    int32_t script_class_line = -1;
    std::unordered_map<std::string, int32_t> scoped_functions;
    std::unordered_map<std::string, int32_t> scoped_signals;
    std::unordered_map<std::string, int32_t> global_functions;
    std::unordered_map<std::string, int32_t> global_signals;
    std::unordered_map<std::string, int32_t> class_fields;
};

struct ScriptCompletionMetadata {
    bool parse_succeeded = false;
    String script_class_name;
    String script_parent_name;
    String script_namespace_name;
    struct SignatureArgument {
        String name;
        Variant::Type type = Variant::NIL;
        String class_name;
    };
    struct Signature {
        String name;
        std::vector<SignatureArgument> arguments;
        Variant::Type return_type = Variant::NIL;
        String return_class_name;
        bool has_return_value = false;
    };
    std::unordered_set<std::string> fields;
    std::unordered_set<std::string> scoped_methods;
    std::unordered_set<std::string> scoped_signals;
    std::unordered_set<std::string> global_methods;
    std::unordered_set<std::string> global_signals;
    std::unordered_map<std::string, Signature> scoped_method_signatures;
    std::unordered_map<std::string, Signature> scoped_signal_signatures;
    std::unordered_map<std::string, Signature> global_method_signatures;
    std::unordered_map<std::string, Signature> global_signal_signatures;
};

Variant::Type variant_type_from_declared_name(const String &type_name);

Dictionary make_validation_error(const String &message, int32_t line, int32_t column, const String &path = String()) {
    Dictionary error;
    if (!path.is_empty()) {
        error["path"] = path;
    }
    error["line"] = line;
    error["column"] = column;
    error["message"] = message;
    return error;
}

Dictionary make_validation_warning(const String &message, int32_t line, const String &string_code = String("korkscript_semantic"), int32_t code = 0) {
    Dictionary warning;
    warning["start_line"] = line;
    warning["end_line"] = line;
    warning["code"] = code;
    warning["string_code"] = string_code;
    warning["message"] = message;
    return warning;
}

Dictionary make_completion_option(const String &display, KorkScriptLanguage::CodeCompletionKind kind, int location, const String &insert_text = String()) {
    Dictionary option;
    option["kind"] = static_cast<int64_t>(kind);
    option["display"] = display;
    option["insert_text"] = insert_text.is_empty() ? display : insert_text;
    option["font_color"] = Color(1, 1, 1, 1);
    option["icon"] = Ref<Resource>();
    option["default_value"] = Variant();
    option["location"] = location;
    return option;
}

bool is_completion_identifier_char(char32_t c) {
    return (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '%' || c == '$';
}

String trim_completion_prefix(const String &input) {
    int start = 0;
    while (start < input.length() && (input[start] == '%' || input[start] == '$')) {
        ++start;
    }
    return input.substr(start);
}

struct MemberCompletionContext {
    bool valid = false;
    String target;
    String prefix;
};

struct IdentifierCompletionContext {
    bool valid = false;
    String prefix;
};

struct CallHintContext {
    bool valid = false;
    String target;
    String method_name;
    int argument_index = 0;
};

struct LookupTargetContext {
    bool has_member_target = false;
    String target;
};

MemberCompletionContext extract_member_completion_context(const String &before_cursor) {
    MemberCompletionContext ctx;
    if (before_cursor.is_empty()) {
        return ctx;
    }

    int end = before_cursor.length() - 1;
    while (end >= 0 && is_completion_identifier_char(before_cursor[end])) {
        --end;
    }

    if (end < 0 || before_cursor[end] != '.') {
        return ctx;
    }

    ctx.prefix = before_cursor.substr(end + 1);
    int target_end = end - 1;
    while (target_end >= 0 && before_cursor[target_end] == ' ') {
        --target_end;
    }
    if (target_end < 0) {
        return ctx;
    }

    int target_start = target_end;
    while (target_start >= 0 && is_completion_identifier_char(before_cursor[target_start])) {
        --target_start;
    }
    ++target_start;
    if (target_start > target_end) {
        return ctx;
    }

    ctx.target = before_cursor.substr(target_start, target_end - target_start + 1);
    ctx.valid = !ctx.target.is_empty();
    return ctx;
}

IdentifierCompletionContext extract_identifier_completion_context(const String &before_cursor) {
    IdentifierCompletionContext ctx;
    if (before_cursor.is_empty()) {
        return ctx;
    }

    int end = before_cursor.length() - 1;
    while (end >= 0 && is_completion_identifier_char(before_cursor[end])) {
        --end;
    }

    ctx.prefix = before_cursor.substr(end + 1);

    int prev = end;
    while (prev >= 0 && (before_cursor[prev] == ' ' || before_cursor[prev] == '\t')) {
        --prev;
    }

    if (prev < 0) {
        ctx.valid = true;
        return ctx;
    }

    const char32_t c = before_cursor[prev];
    ctx.valid = c == '=' || c == '(' || c == ',' || c == ':' || c == '[' || c == '{' || c == '+' || c == '-' || c == '*' || c == '/' || c == '!' || c == '&' || c == '|';
    return ctx;
}

CallHintContext extract_call_hint_context(const String &before_cursor) {
    CallHintContext ctx;
    if (before_cursor.is_empty()) {
        return ctx;
    }

    int depth = 0;
    for (int i = before_cursor.length() - 1; i >= 0; --i) {
        const char32_t c = before_cursor[i];
        if (c == ')') {
            ++depth;
            continue;
        }
        if (c == '(') {
            if (depth == 0) {
                int callee_end = i - 1;
                while (callee_end >= 0 && before_cursor[callee_end] == ' ') {
                    --callee_end;
                }
                if (callee_end < 0) {
                    return ctx;
                }

                int callee_start = callee_end;
                while (callee_start >= 0 && (is_completion_identifier_char(before_cursor[callee_start]) || before_cursor[callee_start] == '.')) {
                    --callee_start;
                }
                ++callee_start;
                if (callee_start > callee_end) {
                    return ctx;
                }

                const String callee = before_cursor.substr(callee_start, callee_end - callee_start + 1).strip_edges();
                if (callee.is_empty()) {
                    return ctx;
                }

                const int dot = callee.rfind(".");
                if (dot >= 0) {
                    ctx.target = callee.substr(0, dot);
                    ctx.method_name = callee.substr(dot + 1);
                } else {
                    ctx.method_name = callee;
                }

                ctx.argument_index = 0;
                int nested = 0;
                for (int j = i + 1; j < before_cursor.length(); ++j) {
                    const char32_t inner = before_cursor[j];
                    if (inner == '(') {
                        ++nested;
                    } else if (inner == ')') {
                        if (nested > 0) {
                            --nested;
                        }
                    } else if (inner == ',' && nested == 0) {
                        ++ctx.argument_index;
                    }
                }

                ctx.valid = !ctx.method_name.is_empty();
                return ctx;
            }
            --depth;
        }
    }

    return ctx;
}

LookupTargetContext extract_lookup_target_context(const String &before_cursor) {
    LookupTargetContext ctx;
    const MemberCompletionContext member_ctx = extract_member_completion_context(before_cursor);
    if (member_ctx.valid) {
        ctx.has_member_target = true;
        ctx.target = member_ctx.target;
    }
    return ctx;
}

String normalize_lookup_symbol(const String &symbol) {
    return symbol.strip_edges();
}

String bare_lookup_symbol(const String &symbol) {
    const String normalized = normalize_lookup_symbol(symbol);
    if (normalized.begins_with("%") || normalized.begins_with("$")) {
        return normalized.substr(1);
    }
    return normalized;
}

bool symbol_matches_lookup_name(const String &candidate, const String &symbol) {
    const String normalized_candidate = normalize_lookup_symbol(candidate);
    const String normalized_symbol = normalize_lookup_symbol(symbol);
    if (normalized_candidate == normalized_symbol) {
        return true;
    }
    return bare_lookup_symbol(normalized_candidate) == bare_lookup_symbol(normalized_symbol);
}

int32_t find_variable_definition_line(const String &code, const String &symbol, int cursor_offset) {
    const String target = normalize_lookup_symbol(symbol);
    if (target.is_empty()) {
        return -1;
    }

    const PackedStringArray lines = code.left(cursor_offset >= 0 ? cursor_offset : code.length()).split("\n", false);
    for (int line_idx = lines.size() - 1; line_idx >= 0; --line_idx) {
        const String &line = lines[line_idx];
        int search_from = 0;
        while (true) {
            const int pos = line.find(target, search_from);
            if (pos < 0) {
                break;
            }

            const int before = pos - 1;
            const int after = pos + target.length();
            const bool left_ok = before < 0 || !is_completion_identifier_char(line[before]);
            const bool right_ok = after >= line.length() || !is_completion_identifier_char(line[after]);
            if (left_ok && right_ok) {
                int cursor = after;
                while (cursor < line.length() && line[cursor] == ' ') {
                    ++cursor;
                }

                const bool looks_like_assignment = cursor < line.length() && (line[cursor] == ':' || line[cursor] == '=');
                const bool looks_like_param = line.find("function") >= 0 || line.find("signal") >= 0;
                if (looks_like_assignment || looks_like_param) {
                    return line_idx + 1;
                }
            }

            search_from = pos + target.length();
        }
    }

    return -1;
}

bool lookup_godot_class_member(const StringName &class_name, const String &symbol, Dictionary &result) {
    ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
    if (class_db == nullptr || class_name.is_empty() || !class_db->class_exists(class_name)) {
        return false;
    }

    const String member = bare_lookup_symbol(symbol);

    StringName current = class_name;
    while (!current.is_empty() && class_db->class_exists(current)) {
        const TypedArray<Dictionary> properties = class_db->class_get_property_list(current);
        for (int i = 0; i < properties.size(); ++i) {
            const Dictionary property = properties[i];
            if (String(property.get("name", "")).strip_edges() == member) {
                result["result"] = static_cast<int64_t>(OK);
                result["type"] = static_cast<int64_t>(KorkScriptLanguage::LOOKUP_RESULT_CLASS_PROPERTY);
                result["class_name"] = String(current);
                result["class_member"] = member;
                return true;
            }
        }

        const TypedArray<Dictionary> methods = class_db->class_get_method_list(current);
        for (int i = 0; i < methods.size(); ++i) {
            const Dictionary method = methods[i];
            if (String(method.get("name", "")).strip_edges() == member) {
                result["result"] = static_cast<int64_t>(OK);
                result["type"] = static_cast<int64_t>(KorkScriptLanguage::LOOKUP_RESULT_CLASS_METHOD);
                result["class_name"] = String(current);
                result["class_member"] = member;
                return true;
            }
        }

        const TypedArray<Dictionary> signals = class_db->class_get_signal_list(current);
        for (int i = 0; i < signals.size(); ++i) {
            const Dictionary signal = signals[i];
            if (String(signal.get("name", "")).strip_edges() == member) {
                result["result"] = static_cast<int64_t>(OK);
                result["type"] = static_cast<int64_t>(KorkScriptLanguage::LOOKUP_RESULT_CLASS_SIGNAL);
                result["class_name"] = String(current);
                result["class_member"] = member;
                return true;
            }
        }

        current = ClassDB::get_parent_class(current);
    }

    return false;
}

String find_explicit_type_for_variable(const String &code_before_cursor, const String &variable_name) {
    if (code_before_cursor.is_empty() || variable_name.is_empty()) {
        return String();
    }

    const PackedStringArray lines = code_before_cursor.split("\n", false);
    for (int line_idx = lines.size() - 1; line_idx >= 0; --line_idx) {
        const String &line = lines[line_idx];
        int search_from = 0;
        while (true) {
            const int pos = line.find(variable_name, search_from);
            if (pos < 0) {
                break;
            }

            const int before = pos - 1;
            const int after = pos + variable_name.length();
            const bool left_ok = before < 0 || !is_completion_identifier_char(line[before]);
            const bool right_ok = after >= line.length() || !is_completion_identifier_char(line[after]);
            if (left_ok && right_ok) {
                int cursor = after;
                while (cursor < line.length() && line[cursor] == ' ') {
                    ++cursor;
                }
                if (cursor < line.length() && line[cursor] == ':') {
                    ++cursor;
                    while (cursor < line.length() && line[cursor] == ' ') {
                        ++cursor;
                    }
                    const int type_start = cursor;
                    while (cursor < line.length() && ((line[cursor] >= 'a' && line[cursor] <= 'z') ||
                                                              (line[cursor] >= 'A' && line[cursor] <= 'Z') ||
                                                              (line[cursor] >= '0' && line[cursor] <= '9') ||
                                                              line[cursor] == '_')) {
                        ++cursor;
                    }
                    if (cursor > type_start) {
                        return line.substr(type_start, cursor - type_start);
                    }
                }
            }

            search_from = pos + variable_name.length();
        }
    }

    return String();
}

KorkApi::AstEnumerationControl collect_completion_metadata(void *user_ptr, const KorkApi::AstEnumerationInfo *info) {
    if (user_ptr == nullptr || info == nullptr) {
        return KorkApi::AstEnumerationContinue;
    }

    ScriptCompletionMetadata *out = static_cast<ScriptCompletionMetadata *>(user_ptr);
    if (info->kind == KorkApi::AstEnumerationNodeStmt) {
        if (info->nodeType == ASTNodeClassDeclStmt) {
            const ClassDeclStmtNode *class_node = static_cast<const ClassDeclStmtNode *>(info->stmtNode);
            if (class_node != nullptr) {
                if (class_node->className != nullptr) {
                    out->script_class_name = String(class_node->className);
                    out->script_namespace_name = out->script_class_name;
                }
                if (class_node->parentName != nullptr) {
                    out->script_parent_name = String(class_node->parentName);
                }
            }
        } else if (info->nodeType == ASTNodeFunctionDeclStmt) {
            const FunctionDeclStmtNode *function_node = static_cast<const FunctionDeclStmtNode *>(info->stmtNode);
            if (function_node != nullptr && function_node->fnName != nullptr) {
                const String name = String(function_node->fnName).strip_edges();
                if (!name.is_empty()) {
                    const std::string key = name.utf8().get_data();
                    const String namespace_name = function_node->nameSpace != nullptr ? String(function_node->nameSpace).strip_edges() : String();
                    if (out->script_namespace_name.is_empty() && !namespace_name.is_empty()) {
                        out->script_namespace_name = namespace_name;
                    }
                    const bool is_global_scope = namespace_name.is_empty();
                    const bool is_script_scope = !namespace_name.is_empty() && namespace_name == out->script_namespace_name;
                    ScriptCompletionMetadata::Signature signature;
                    signature.name = name;
                    for (const VarNode *arg = function_node->args; arg != nullptr; arg = static_cast<const VarNode *>(arg->getNext())) {
                        ScriptCompletionMetadata::SignatureArgument arg_meta;
                        if (arg->varName == nullptr) {
                            continue;
                        }
                        String arg_name = String(arg->varName).strip_edges();
                        if (arg_name.begins_with("%")) {
                            arg_name = arg_name.substr(1);
                        }
                        if (signature.arguments.empty() && arg_name == "this") {
                            continue;
                        }
                        arg_meta.name = arg_name;
                        const String type_name = arg->varType != nullptr ? String(arg->varType).strip_edges() : String();
                        arg_meta.type = variant_type_from_declared_name(type_name);
                        if (arg_meta.type == Variant::NIL && !type_name.is_empty()) {
                            arg_meta.class_name = type_name;
                        }
                        signature.arguments.push_back(arg_meta);
                    }
                    const String return_type_name = function_node->returnTypeName != nullptr ? String(function_node->returnTypeName).strip_edges() : String();
                    signature.return_type = variant_type_from_declared_name(return_type_name);
                    if (signature.return_type == Variant::NIL && !return_type_name.is_empty()) {
                        signature.return_class_name = return_type_name;
                    }
                    signature.has_return_value = function_node->returnTypeName != nullptr;
                    if (function_node->isSignal) {
                        if (is_script_scope) {
                            out->scoped_signals.insert(key);
                            out->scoped_signal_signatures[key] = signature;
                        } else if (is_global_scope) {
                            out->global_signals.insert(key);
                            out->global_signal_signatures[key] = signature;
                        }
                    } else {
                        if (is_script_scope) {
                            out->scoped_methods.insert(key);
                            out->scoped_method_signatures[key] = signature;
                        } else if (is_global_scope) {
                            out->global_methods.insert(key);
                            out->global_method_signatures[key] = signature;
                        }
                    }
                }
            }
        }
    } else if (info->kind == KorkApi::AstEnumerationNodeScriptClassField) {
        const ScriptClassFieldDecl *field_node = info->scriptClassFieldNode;
        if (field_node != nullptr && field_node->fieldName != nullptr) {
            const String name = String(field_node->fieldName).strip_edges();
            if (!name.is_empty()) {
                out->fields.insert(name.utf8().get_data());
            }
        }
    }

    return KorkApi::AstEnumerationContinue;
}

KorkApi::AstEnumerationControl collect_lookup_metadata(void *user_ptr, const KorkApi::AstEnumerationInfo *info) {
    if (user_ptr == nullptr || info == nullptr) {
        return KorkApi::AstEnumerationContinue;
    }

    LookupMetadata *out = static_cast<LookupMetadata *>(user_ptr);
    if (info->kind == KorkApi::AstEnumerationNodeStmt) {
        if (info->nodeType == ASTNodeClassDeclStmt) {
            const ClassDeclStmtNode *class_node = static_cast<const ClassDeclStmtNode *>(info->stmtNode);
            if (class_node != nullptr && class_node->className != nullptr) {
                out->script_class_name = String(class_node->className).strip_edges();
                out->script_namespace_name = out->script_class_name;
                out->script_class_line = class_node->dbgLineNumber > 0 ? class_node->dbgLineNumber : -1;
                if (class_node->parentName != nullptr) {
                    out->script_parent_name = String(class_node->parentName).strip_edges();
                }
            }
        } else if (info->nodeType == ASTNodeFunctionDeclStmt) {
            const FunctionDeclStmtNode *function_node = static_cast<const FunctionDeclStmtNode *>(info->stmtNode);
            if (function_node == nullptr || function_node->fnName == nullptr) {
                return KorkApi::AstEnumerationContinue;
            }

            const String name = String(function_node->fnName).strip_edges();
            if (name.is_empty()) {
                return KorkApi::AstEnumerationContinue;
            }

            const String namespace_name = function_node->nameSpace != nullptr ? String(function_node->nameSpace).strip_edges() : String();
            if (out->script_namespace_name.is_empty() && !namespace_name.is_empty()) {
                out->script_namespace_name = namespace_name;
            }

            const std::string key = name.utf8().get_data();
            const int32_t line = function_node->dbgLineNumber > 0 ? function_node->dbgLineNumber : -1;
            const bool is_global_scope = namespace_name.is_empty();
            const bool is_script_scope = !namespace_name.is_empty() && namespace_name == out->script_namespace_name;
            if (function_node->isSignal) {
                if (is_script_scope) {
                    out->scoped_signals[key] = line;
                } else if (is_global_scope) {
                    out->global_signals[key] = line;
                }
            } else {
                if (is_script_scope) {
                    out->scoped_functions[key] = line;
                } else if (is_global_scope) {
                    out->global_functions[key] = line;
                }
            }
        }
    } else if (info->kind == KorkApi::AstEnumerationNodeScriptClassField) {
        const ScriptClassFieldDecl *field_node = info->scriptClassFieldNode;
        if (field_node != nullptr && field_node->fieldName != nullptr) {
            const String field_name = String(field_node->fieldName).strip_edges();
            if (!field_name.is_empty()) {
                out->class_fields[field_name.utf8().get_data()] = field_node->dbgLineNumber > 0 ? field_node->dbgLineNumber : -1;
            }
        }
    }

    return KorkApi::AstEnumerationContinue;
}

ScriptCompletionMetadata scan_completion_metadata(KorkScriptVMHost *host, const String &code) {
    ScriptCompletionMetadata metadata;
    if (host == nullptr) {
        return metadata;
    }
    const KorkApi::AstEnumerationResult parse_result = host->enumerate_ast(
            code,
            String("[KorkScriptLanguageComplete]"),
            &metadata,
            &collect_completion_metadata,
            nullptr);
    metadata.parse_succeeded = parse_result == KorkApi::AstEnumerationCompleted;
    return metadata;
}

LookupMetadata scan_lookup_metadata(KorkScriptVMHost *host, const String &code, bool *r_parse_succeeded = nullptr) {
    LookupMetadata metadata;
    if (r_parse_succeeded != nullptr) {
        *r_parse_succeeded = false;
    }
    if (host == nullptr) {
        return metadata;
    }
    const KorkApi::AstEnumerationResult parse_result = host->enumerate_ast(
            code,
            String("[KorkScriptLanguageLookup]"),
            &metadata,
            &collect_lookup_metadata,
            nullptr);
    if (r_parse_succeeded != nullptr) {
        *r_parse_succeeded = parse_result == KorkApi::AstEnumerationCompleted;
    }
    return metadata;
}

ScriptCompletionMetadata completion_metadata_from_script_resource(const Ref<KorkScript> &script) {
    ScriptCompletionMetadata metadata;
    if (!script.is_valid()) {
        return metadata;
    }

    KorkScriptLanguage *language = KorkScriptLanguage::get_singleton();
    KorkScriptVMHost *host = language != nullptr ? language->get_vm_host(script->get_vm_name()) : nullptr;
    if (host != nullptr) {
        metadata = scan_completion_metadata(host, script->get_source_code_ref());
    }

    if (!metadata.parse_succeeded) {
        return metadata;
    }

    if (metadata.script_class_name.is_empty()) {
        metadata.script_class_name = script->get_declared_script_class_name().strip_edges();
    }
    if (metadata.script_namespace_name.is_empty()) {
        metadata.script_namespace_name = script->get_effective_namespace_name().strip_edges();
    }
    if (metadata.script_parent_name.is_empty()) {
        metadata.script_parent_name = script->get_declared_script_class_parent_name().strip_edges();
    }
    if (metadata.script_parent_name.is_empty()) {
        metadata.script_parent_name = script->get_base_type();
    }

    return metadata;
}

Ref<KorkScript> load_kork_script_resource(const String &path) {
    if (path.is_empty()) {
        return Ref<KorkScript>();
    }

    ResourceLoader *loader = ResourceLoader::get_singleton();
    if (loader == nullptr || !loader->exists(path)) {
        return Ref<KorkScript>();
    }

    Ref<Resource> resource = loader->load(path, String(), ResourceLoader::CACHE_MODE_REUSE);
    Ref<KorkScript> script = resource;
    return script;
}

void push_unique_completion(Array &options, std::unordered_set<std::string> &seen, const String &display, KorkScriptLanguage::CodeCompletionKind kind, int location, const String &insert_text);
bool completion_matches_prefix(const String &name, const String &prefix);

void collect_script_variable_names(const String &code, std::unordered_set<std::string> &locals, std::unordered_set<std::string> &globals) {
    for (int i = 0; i < code.length(); ++i) {
        const char32_t c = code[i];
        if (c != '%' && c != '$') {
            continue;
        }

        const int start = i;
        const bool is_global = c == '$';
        int cursor = i + 1;
        if (cursor >= code.length()) {
            continue;
        }
        const char32_t first = code[cursor];
        if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '_')) {
            continue;
        }
        while (cursor < code.length()) {
            const char32_t ch = code[cursor];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_')) {
                break;
            }
            ++cursor;
        }

        const String var_name = code.substr(start, cursor - start);
        const std::string key = var_name.utf8().get_data();
        if (is_global) {
            globals.insert(key);
        } else {
            locals.insert(key);
        }
        i = cursor - 1;
    }
}

void add_identifier_completion_items(Array &options,
        std::unordered_set<std::string> &seen,
        const ScriptCompletionMetadata &metadata,
        const String &code_before_cursor,
        const String &prefix) {
    std::unordered_set<std::string> local_vars;
    std::unordered_set<std::string> global_vars;
    collect_script_variable_names(code_before_cursor, local_vars, global_vars);

    const auto variable_matches_prefix = [&](const String &full_name) {
        if (completion_matches_prefix(full_name, prefix)) {
            return true;
        }
        if ((full_name.begins_with("%") || full_name.begins_with("$")) && prefix.is_empty() == false) {
            return completion_matches_prefix(full_name.substr(1), prefix);
        }
        return false;
    };

    if (variable_matches_prefix("%this")) {
        push_unique_completion(options, seen, "%this", KorkScriptLanguage::CODE_COMPLETION_KIND_VARIABLE, KorkScriptLanguage::LOCATION_LOCAL, String());
    }

    for (const std::string &name : local_vars) {
        const String display(name.c_str());
        if (variable_matches_prefix(display)) {
            push_unique_completion(options, seen, display, KorkScriptLanguage::CODE_COMPLETION_KIND_VARIABLE, KorkScriptLanguage::LOCATION_LOCAL, String());
        }
    }

    for (const std::string &name : global_vars) {
        const String display(name.c_str());
        if (variable_matches_prefix(display)) {
            push_unique_completion(options, seen, display, KorkScriptLanguage::CODE_COMPLETION_KIND_VARIABLE, KorkScriptLanguage::LOCATION_OTHER_USER_CODE, String());
        }
    }

    for (const std::string &method : metadata.global_methods) {
        const String name(method.c_str());
        if (completion_matches_prefix(name, prefix)) {
            push_unique_completion(options, seen, name, KorkScriptLanguage::CODE_COMPLETION_KIND_FUNCTION, KorkScriptLanguage::LOCATION_OTHER_USER_CODE, name + String("("));
        }
    }
}

void push_unique_completion(Array &options, std::unordered_set<std::string> &seen, const String &display, KorkScriptLanguage::CodeCompletionKind kind, int location, const String &insert_text = String()) {
    if (display.is_empty()) {
        return;
    }
    const std::string key = display.utf8().get_data();
    if (!seen.insert(key).second) {
        return;
    }
    options.push_back(make_completion_option(display, kind, location, insert_text));
}

bool completion_matches_prefix(const String &name, const String &prefix) {
    if (prefix.is_empty()) {
        return true;
    }
    return name.begins_with(prefix);
}

void add_script_completion_members(Array &options, std::unordered_set<std::string> &seen, const ScriptCompletionMetadata &metadata, const String &prefix) {
    for (const std::string &field : metadata.fields) {
        const String name(field.c_str());
        if (completion_matches_prefix(name, prefix)) {
            push_unique_completion(options, seen, name, KorkScriptLanguage::CODE_COMPLETION_KIND_MEMBER, KorkScriptLanguage::LOCATION_OTHER_USER_CODE);
        }
    }
    for (const std::string &signal : metadata.scoped_signals) {
        const String name(signal.c_str());
        if (completion_matches_prefix(name, prefix)) {
            push_unique_completion(options, seen, name, KorkScriptLanguage::CODE_COMPLETION_KIND_SIGNAL, KorkScriptLanguage::LOCATION_OTHER_USER_CODE);
        }
    }
    for (const std::string &method : metadata.scoped_methods) {
        const String name(method.c_str());
        if (completion_matches_prefix(name, prefix)) {
            push_unique_completion(options, seen, name, KorkScriptLanguage::CODE_COMPLETION_KIND_FUNCTION, KorkScriptLanguage::LOCATION_OTHER_USER_CODE, name + String("("));
        }
    }
}

void add_godot_class_completion_members(Array &options, std::unordered_set<std::string> &seen, const StringName &class_name, const String &prefix) {
    ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
    if (class_db == nullptr || class_name.is_empty() || !class_db->class_exists(class_name)) {
        return;
    }

    const TypedArray<Dictionary> properties = class_db->class_get_property_list(class_name);
    for (int i = 0; i < properties.size(); ++i) {
        const Dictionary property = properties[i];
        const String name = property.get("name", "");
        if (completion_matches_prefix(name, prefix)) {
            push_unique_completion(options, seen, name, KorkScriptLanguage::CODE_COMPLETION_KIND_MEMBER, KorkScriptLanguage::LOCATION_PARENT_MASK);
        }
    }

    const TypedArray<Dictionary> signals = class_db->class_get_signal_list(class_name);
    for (int i = 0; i < signals.size(); ++i) {
        const Dictionary signal = signals[i];
        const String name = signal.get("name", "");
        if (completion_matches_prefix(name, prefix)) {
            push_unique_completion(options, seen, name, KorkScriptLanguage::CODE_COMPLETION_KIND_SIGNAL, KorkScriptLanguage::LOCATION_PARENT_MASK);
        }
    }

    const TypedArray<Dictionary> methods = class_db->class_get_method_list(class_name);
    for (int i = 0; i < methods.size(); ++i) {
        const Dictionary method = methods[i];
        const String name = method.get("name", "");
        if (completion_matches_prefix(name, prefix)) {
            push_unique_completion(options, seen, name, KorkScriptLanguage::CODE_COMPLETION_KIND_FUNCTION, KorkScriptLanguage::LOCATION_PARENT_MASK, name + String("("));
        }
    }
}

String format_completion_variant_type(Variant::Type type, const String &class_name, bool allow_void = false) {
    if (!class_name.is_empty()) {
        return class_name;
    }
    switch (type) {
        case Variant::BOOL:
            return "bool";
        case Variant::INT:
            return "int";
        case Variant::FLOAT:
            return "float";
        case Variant::STRING:
            return "String";
        case Variant::VECTOR2:
            return "Vector2";
        case Variant::VECTOR3:
            return "Vector3";
        case Variant::VECTOR4:
            return "Vector4";
        case Variant::COLOR:
            return "Color";
        case Variant::OBJECT:
            return "Object";
        case Variant::ARRAY:
            return "Array";
        case Variant::DICTIONARY:
            return "Dictionary";
        case Variant::NIL:
        default:
            return allow_void ? "void" : "Variant";
    }
}

String build_call_hint_for_signature(const ScriptCompletionMetadata::Signature &signature, int argument_index) {
    String hint = format_completion_variant_type(signature.return_type, signature.return_class_name, !signature.has_return_value);
    hint += " " + signature.name + "(";
    for (int i = 0; i < static_cast<int>(signature.arguments.size()); ++i) {
        if (i > 0) {
            hint += ", ";
        }
        if (i == argument_index) {
            hint += String::chr(0xFFFF);
        }
        const ScriptCompletionMetadata::SignatureArgument &arg = signature.arguments[static_cast<size_t>(i)];
        hint += (arg.name.is_empty() ? String("arg") + itos(i) : arg.name);
        hint += ": " + format_completion_variant_type(arg.type, arg.class_name, false);
        if (i == argument_index) {
            hint += String::chr(0xFFFF);
        }
    }
    hint += ")";
    return hint;
}

bool find_script_signature(const ScriptCompletionMetadata &metadata, const String &name, bool include_signals, bool global_scope, ScriptCompletionMetadata::Signature &out_signature) {
    const std::string key = name.utf8().get_data();
    const auto &method_signatures = global_scope ? metadata.global_method_signatures : metadata.scoped_method_signatures;
    const auto &signal_signatures = global_scope ? metadata.global_signal_signatures : metadata.scoped_signal_signatures;
    const auto method_found = method_signatures.find(key);
    if (method_found != method_signatures.end()) {
        out_signature = method_found->second;
        return true;
    }
    if (include_signals) {
        const auto signal_found = signal_signatures.find(key);
        if (signal_found != signal_signatures.end()) {
            out_signature = signal_found->second;
            return true;
        }
    }
    return false;
}

bool build_godot_class_call_hint(const StringName &class_name, const String &method_name, int argument_index, String &out_hint) {
    ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
    if (class_db == nullptr || class_name.is_empty() || !class_db->class_exists(class_name)) {
        return false;
    }

    const TypedArray<Dictionary> methods = class_db->class_get_method_list(class_name);
    for (int i = 0; i < methods.size(); ++i) {
        const Dictionary method_info = methods[i];
        if (String(method_info.get("name", "")).strip_edges() != method_name) {
            continue;
        }

        ScriptCompletionMetadata::Signature signature;
        signature.name = method_name;
        const Array args = method_info.get("args", Array());
        for (int arg_idx = 0; arg_idx < args.size(); ++arg_idx) {
            const Dictionary arg_info = args[arg_idx];
            ScriptCompletionMetadata::SignatureArgument arg_meta;
            arg_meta.name = String(arg_info.get("name", "")).strip_edges();
            arg_meta.type = static_cast<Variant::Type>(static_cast<int>(arg_info.get("type", static_cast<int64_t>(Variant::NIL))));
            arg_meta.class_name = String(arg_info.get("class_name", "")).strip_edges();
            signature.arguments.push_back(arg_meta);
        }

        const Dictionary return_info = method_info.get("return", Dictionary());
        if (!return_info.is_empty()) {
            signature.return_type = static_cast<Variant::Type>(static_cast<int>(return_info.get("type", static_cast<int64_t>(Variant::NIL))));
            signature.return_class_name = String(return_info.get("class_name", "")).strip_edges();
            signature.has_return_value = signature.return_type != Variant::NIL || !signature.return_class_name.is_empty();
        }

        out_hint = build_call_hint_for_signature(signature, argument_index);
        return true;
    }

    return false;
}

Variant::Type variant_type_from_declared_name(const String &type_name) {
    const String normalized = type_name.strip_edges();
    if (normalized.is_empty()) {
        return Variant::NIL;
    }
    if (normalized == "bool" || normalized == "Bool") {
        return Variant::BOOL;
    }
    if (normalized == "int" || normalized == "Int" || normalized == "uint" || normalized == "UInt") {
        return Variant::INT;
    }
    if (normalized == "float" || normalized == "Float") {
        return Variant::FLOAT;
    }
    if (normalized == "string" || normalized == "String") {
        return Variant::STRING;
    }
    if (normalized == "Vector2") {
        return Variant::VECTOR2;
    }
    if (normalized == "Vector3") {
        return Variant::VECTOR3;
    }
    if (normalized == "Vector4") {
        return Variant::VECTOR4;
    }
    if (normalized == "Color") {
        return Variant::COLOR;
    }
    return Variant::NIL;
}

Variant::Type infer_literal_expr_type(const ExprNode *expr) {
    if (expr == nullptr) {
        return Variant::NIL;
    }

    switch (expr->getASTNodeType()) {
        case ASTNodeInt:
            return Variant::INT;
        case ASTNodeFloat:
            return Variant::FLOAT;
        case ASTNodeStrConst:
            return Variant::STRING;
        case ASTNodeConstant: {
            const ConstantNode *node = static_cast<const ConstantNode *>(expr);
            if (node == nullptr || node->value == nullptr) {
                return Variant::NIL;
            }
            const String value = String(node->value).to_lower();
            if (value == "true" || value == "false") {
                return Variant::BOOL;
            }
            return Variant::STRING;
        }
        default:
            return Variant::NIL;
    }
}

bool are_semantic_types_compatible(Variant::Type declared_type, Variant::Type actual_type) {
    if (declared_type == Variant::NIL || actual_type == Variant::NIL) {
        return true;
    }
    if (declared_type == actual_type) {
        return true;
    }
    if (declared_type == Variant::FLOAT && actual_type == Variant::INT) {
        return true;
    }
    if (declared_type == Variant::BOOL && actual_type == Variant::INT) {
        return true;
    }
    return false;
}

struct SemanticFunctionScope {
    int32_t depth = -1;
    String display_name;
    Variant::Type return_type = Variant::NIL;
    bool has_declared_return_type = false;
    bool is_signal = false;
};

struct SemanticClassScope {
    int32_t depth = -1;
    String class_name;
    std::unordered_map<std::string, int32_t> fields;
};

struct SemanticValidationState {
    TypedArray<Dictionary> *errors = nullptr;
    TypedArray<Dictionary> *warnings = nullptr;
    String path;
    std::unordered_map<std::string, int32_t> symbols;
    std::unordered_map<std::string, bool> symbol_is_signal;
    std::unordered_map<std::string, int32_t> classes;
    std::vector<SemanticFunctionScope> function_stack;
    std::vector<SemanticClassScope> class_stack;
};

void semantic_pop_scopes_for_depth(SemanticValidationState *state, int32_t depth) {
    if (state == nullptr) {
        return;
    }

    while (!state->function_stack.empty() && state->function_stack.back().depth >= depth) {
        state->function_stack.pop_back();
    }
    while (!state->class_stack.empty() && state->class_stack.back().depth >= depth) {
        state->class_stack.pop_back();
    }
}

KorkApi::AstEnumerationControl collect_semantic_validation(void *user_ptr, const KorkApi::AstEnumerationInfo *info) {
    if (user_ptr == nullptr || info == nullptr) {
        return KorkApi::AstEnumerationContinue;
    }

    SemanticValidationState *state = static_cast<SemanticValidationState *>(user_ptr);
    semantic_pop_scopes_for_depth(state, static_cast<int32_t>(info->depth));

    if (info->kind == KorkApi::AstEnumerationNodeStmt) {
        if (info->nodeType == ASTNodeClassDeclStmt) {
            const ClassDeclStmtNode *class_node = static_cast<const ClassDeclStmtNode *>(info->stmtNode);
            if (class_node == nullptr || class_node->className == nullptr) {
                return KorkApi::AstEnumerationContinue;
            }

            const String class_name = String(class_node->className);
            const std::string class_key = class_name.utf8().get_data();
            auto found = state->classes.find(class_key);
            if (found != state->classes.end()) {
                state->errors->push_back(make_validation_error(
                        vformat("Duplicate script class '%s'. First declared on line %d.", class_name, found->second),
                        class_node->dbgLineNumber > 0 ? class_node->dbgLineNumber : -1,
                        -1,
                        state->path));
            } else {
                state->classes.emplace(class_key, class_node->dbgLineNumber);
            }

            SemanticClassScope scope;
            scope.depth = static_cast<int32_t>(info->depth);
            scope.class_name = class_name;
            state->class_stack.push_back(std::move(scope));
            return KorkApi::AstEnumerationContinue;
        }

        if (info->nodeType == ASTNodeFunctionDeclStmt) {
            const FunctionDeclStmtNode *function_node = static_cast<const FunctionDeclStmtNode *>(info->stmtNode);
            if (function_node == nullptr || function_node->fnName == nullptr) {
                return KorkApi::AstEnumerationContinue;
            }

            const String namespace_name = function_node->nameSpace != nullptr ? String(function_node->nameSpace) : String();
            const String function_name = String(function_node->fnName);
            const String qualified_name = namespace_name.is_empty() ? function_name : namespace_name + String("::") + function_name;
            const std::string symbol_key = qualified_name.utf8().get_data();

            auto found = state->symbols.find(symbol_key);
            if (found != state->symbols.end()) {
                const bool first_is_signal = state->symbol_is_signal[symbol_key];
                state->errors->push_back(make_validation_error(
                        vformat("Duplicate %s '%s'. First declared on line %d.",
                                first_is_signal ? String("signal") : String("function"),
                                qualified_name,
                                found->second),
                        function_node->dbgLineNumber > 0 ? function_node->dbgLineNumber : -1,
                        -1,
                        state->path));
            } else {
                state->symbols.emplace(symbol_key, function_node->dbgLineNumber);
                state->symbol_is_signal.emplace(symbol_key, function_node->isSignal);
            }

            if (function_node->isSignal) {
                if (function_node->returnTypeName != nullptr && function_node->returnTypeName[0] != '\0') {
                state->errors->push_back(make_validation_error(
                        vformat("Signal '%s' cannot declare a return type.", qualified_name),
                        function_node->dbgLineNumber > 0 ? function_node->dbgLineNumber : -1,
                        -1,
                        state->path));
                }
                if (function_node->stmts != nullptr) {
                    state->warnings->push_back(make_validation_warning(
                            vformat("Signal '%s' has a body; Godot signal metadata only uses the declaration signature.", qualified_name),
                            function_node->dbgLineNumber > 0 ? function_node->dbgLineNumber : -1));
                }
            }

            SemanticFunctionScope scope;
            scope.depth = static_cast<int32_t>(info->depth);
            scope.display_name = qualified_name;
            scope.is_signal = function_node->isSignal;
            if (function_node->returnTypeName != nullptr && function_node->returnTypeName[0] != '\0') {
                scope.has_declared_return_type = true;
                scope.return_type = variant_type_from_declared_name(String(function_node->returnTypeName));
            }
            state->function_stack.push_back(std::move(scope));
            return KorkApi::AstEnumerationContinue;
        }

        if (info->nodeType == ASTNodeReturnStmt) {
            const ReturnStmtNode *return_node = static_cast<const ReturnStmtNode *>(info->stmtNode);
            if (return_node == nullptr || state->function_stack.empty()) {
                return KorkApi::AstEnumerationContinue;
            }

            const SemanticFunctionScope &scope = state->function_stack.back();
            if (scope.is_signal) {
                state->errors->push_back(make_validation_error(
                        vformat("Signal '%s' cannot return a value.", scope.display_name),
                        return_node->dbgLineNumber > 0 ? return_node->dbgLineNumber : -1,
                        -1,
                        state->path));
                return KorkApi::AstEnumerationContinue;
            }

            if (!scope.has_declared_return_type) {
                return KorkApi::AstEnumerationContinue;
            }

            if (return_node->expr == nullptr) {
                state->errors->push_back(make_validation_error(
                        vformat("Function '%s' declares a return type but has a bare return.", scope.display_name),
                        return_node->dbgLineNumber > 0 ? return_node->dbgLineNumber : -1,
                        -1,
                        state->path));
                return KorkApi::AstEnumerationContinue;
            }

            const Variant::Type actual_type = infer_literal_expr_type(return_node->expr);
            if (!are_semantic_types_compatible(scope.return_type, actual_type)) {
                state->warnings->push_back(make_validation_warning(
                        vformat("Function '%s' returns a value that does not match its declared return type.", scope.display_name),
                        return_node->dbgLineNumber > 0 ? return_node->dbgLineNumber : -1));
            }
            return KorkApi::AstEnumerationContinue;
        }
    }

    if (info->kind == KorkApi::AstEnumerationNodeScriptClassField) {
        const ScriptClassFieldDecl *field_node = info->scriptClassFieldNode;
        if (field_node == nullptr || field_node->fieldName == nullptr || state->class_stack.empty()) {
            return KorkApi::AstEnumerationContinue;
        }

        SemanticClassScope &scope = state->class_stack.back();
        const String field_name = String(field_node->fieldName);
        const std::string field_key = field_name.utf8().get_data();
        auto found = scope.fields.find(field_key);
        if (found != scope.fields.end()) {
            state->errors->push_back(make_validation_error(
                    vformat("Duplicate field '%s' in script class '%s'. First declared on line %d.", field_name, scope.class_name, found->second),
                    field_node->dbgLineNumber > 0 ? field_node->dbgLineNumber : -1,
                    -1,
                    state->path));
        } else {
            scope.fields.emplace(field_key, field_node->dbgLineNumber);
        }

        const Variant::Type declared_type = variant_type_from_declared_name(String(field_node->typeName));
        const Variant::Type default_type = infer_literal_expr_type(field_node->defaultExpr);
        if (field_node->defaultExpr != nullptr && !are_semantic_types_compatible(declared_type, default_type)) {
            state->warnings->push_back(make_validation_warning(
                    vformat("Default value for field '%s' in script class '%s' does not match its declared type.", field_name, scope.class_name),
                    field_node->dbgLineNumber > 0 ? field_node->dbgLineNumber : -1));
        }
    }

    return KorkApi::AstEnumerationContinue;
}

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

Dictionary KorkScriptLanguage::_validate(const String &p_script, const String &p_path, bool p_validate_functions, bool p_validate_errors, bool p_validate_warnings, bool) const {
    Dictionary result;
    TypedArray<Dictionary> errors;
    TypedArray<Dictionary> warnings;
    result["warnings"] = warnings;
    result["safe_lines"] = PackedInt32Array();
    result["errors"] = errors;
    result["functions"] = PackedStringArray();

    KorkScriptVMHost *host = const_cast<KorkScriptLanguage *>(this)->get_vm_host(String("default"));
    if (host == nullptr) {
        errors.push_back(make_validation_error("KorkScript VM host is unavailable.", -1, -1, p_path));
        result["errors"] = errors;
        result["warnings"] = warnings;
        result["valid"] = false;
        return result;
    }

    KorkApi::AstParseErrorInfo parse_error{};
    const KorkApi::AstEnumerationResult parse_result = host->enumerate_ast(
            p_script,
            String("[KorkScriptLanguageValidate]"),
            nullptr,
            [](void *, const KorkApi::AstEnumerationInfo *) {
                return KorkApi::AstEnumerationContinue;
            },
            &parse_error);

    if (parse_result == KorkApi::AstEnumerationParseFailed) {
        String message = "Parse error.";
        if (parse_error.message != nullptr && parse_error.message[0] != '\0') {
            message = String(parse_error.message);
        } else if (parse_error.stage == KorkApi::AstParseErrorLexer) {
            message = "Lexer error.";
        } else if (parse_error.stage == KorkApi::AstParseErrorParser) {
            message = "Parser error.";
        }

        if (parse_error.tokenText != nullptr && parse_error.tokenText[0] != '\0') {
            message += vformat(" Near '%s'.", String(parse_error.tokenText));
        }

        errors.push_back(make_validation_error(
                message,
                parse_error.line > 0 ? static_cast<int32_t>(parse_error.line) : -1,
                parse_error.column > 0 ? static_cast<int32_t>(parse_error.column) : -1,
                p_path));
        result["errors"] = errors;
        result["warnings"] = warnings;
        result["valid"] = false;
        return result;
    }

    if (parse_result != KorkApi::AstEnumerationCompleted) {
        errors.push_back(make_validation_error("KorkScript AST enumeration did not complete.", -1, -1, p_path));
        result["errors"] = errors;
        result["warnings"] = warnings;
        result["valid"] = false;
        return result;
    }

    if (p_validate_errors || p_validate_warnings) {
        SemanticValidationState semantic_state;
        semantic_state.errors = &errors;
        semantic_state.warnings = &warnings;
        semantic_state.path = p_path;
        host->enumerate_ast(
                p_script,
                String("[KorkScriptLanguageValidateSemantic]"),
                &semantic_state,
                &collect_semantic_validation,
                nullptr);
    }

    result["errors"] = errors;
    result["warnings"] = warnings;
    result["valid"] = errors.is_empty();
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

Dictionary KorkScriptLanguage::_complete_code(const String &p_code, const String &p_path, Object *p_owner) const {
    Dictionary result;
    Array options;
    result["options"] = options;
    result["result"] = ERR_UNAVAILABLE;
    result["force"] = false;
    result["call_hint"] = "";
    const String cursor_token = String::chr(0xFFFF);
    const int cursor = p_code.find(cursor_token);
    if (cursor < 0) {
        return result;
    }

    const String before_cursor = p_code.substr(0, cursor);
    KorkScriptVMHost *host = const_cast<KorkScriptLanguage *>(this)->get_vm_host(String("default"));
    const String code_without_cursor = p_code.substr(0, cursor) + p_code.substr(cursor + cursor_token.length());
    ScriptCompletionMetadata metadata = scan_completion_metadata(host, code_without_cursor);
    if (!metadata.parse_succeeded) {
        metadata = completion_metadata_from_script_resource(load_kork_script_resource(p_path));
    }

    String fallback_base_type = p_owner != nullptr ? p_owner->get_class() : String("Node");
    if (!metadata.script_parent_name.is_empty()) {
        fallback_base_type = metadata.script_parent_name;
    }

    bool has_call_hint = false;
    const CallHintContext call_ctx = extract_call_hint_context(before_cursor);
    if (call_ctx.valid) {
        ScriptCompletionMetadata::Signature signature;
        bool found_hint = false;

        if (call_ctx.target.is_empty()) {
            found_hint = find_script_signature(metadata, call_ctx.method_name, true, true, signature);
        } else if (call_ctx.target == "%this") {
            found_hint = find_script_signature(metadata, call_ctx.method_name, true, false, signature);
            if (!found_hint) {
                String godot_hint;
                if (build_godot_class_call_hint(StringName(fallback_base_type), call_ctx.method_name, call_ctx.argument_index, godot_hint)) {
                    result["call_hint"] = godot_hint;
                    result["force"] = true;
                    has_call_hint = true;
                }
            }
        } else {
            const String explicit_call_target_type = find_explicit_type_for_variable(before_cursor, call_ctx.target);
            if (!explicit_call_target_type.is_empty() && !metadata.script_class_name.is_empty() && explicit_call_target_type == metadata.script_class_name) {
                found_hint = find_script_signature(metadata, call_ctx.method_name, true, false, signature);
                if (!found_hint) {
                    String godot_hint;
                    if (build_godot_class_call_hint(StringName(fallback_base_type), call_ctx.method_name, call_ctx.argument_index, godot_hint)) {
                        result["call_hint"] = godot_hint;
                        result["force"] = true;
                        has_call_hint = true;
                    }
                }
            } else {
                const StringName call_target_type = explicit_call_target_type.is_empty() ? StringName(fallback_base_type) : StringName(explicit_call_target_type);
                String godot_hint;
                if (build_godot_class_call_hint(call_target_type, call_ctx.method_name, call_ctx.argument_index, godot_hint)) {
                    result["call_hint"] = godot_hint;
                    result["force"] = true;
                    has_call_hint = true;
                }
            }
        }

        if (found_hint) {
            result["call_hint"] = build_call_hint_for_signature(signature, call_ctx.argument_index);
            result["force"] = true;
            has_call_hint = true;
        }
    }

    const MemberCompletionContext ctx = extract_member_completion_context(before_cursor);
    if (!ctx.valid) {
        const IdentifierCompletionContext ident_ctx = extract_identifier_completion_context(before_cursor);
        if (ident_ctx.valid) {
            std::unordered_set<std::string> seen;
            add_identifier_completion_items(options, seen, metadata, before_cursor, ident_ctx.prefix);
            result["options"] = options;
            result["force"] = true;
            result["result"] = (!options.is_empty() || has_call_hint) ? static_cast<int64_t>(OK) : static_cast<int64_t>(ERR_UNAVAILABLE);
            return result;
        }
        if (has_call_hint) {
            result["result"] = static_cast<int64_t>(OK);
        }
        return result;
    }

    std::unordered_set<std::string> seen;
    const String prefix = ctx.prefix;
    const String target = ctx.target;
    const String explicit_type = find_explicit_type_for_variable(before_cursor, target);

    bool produced = false;
    if (target == "%this") {
        add_script_completion_members(options, seen, metadata, prefix);
        add_godot_class_completion_members(options, seen, StringName(fallback_base_type), prefix);
        produced = options.size() > 0;
    } else if (!explicit_type.is_empty()) {
        if (!metadata.script_class_name.is_empty() && explicit_type == metadata.script_class_name) {
            add_script_completion_members(options, seen, metadata, prefix);
            add_godot_class_completion_members(options, seen, StringName(fallback_base_type), prefix);
            produced = options.size() > 0;
        } else {
            add_godot_class_completion_members(options, seen, StringName(explicit_type), prefix);
            produced = options.size() > 0;
        }
    } else {
        add_godot_class_completion_members(options, seen, StringName(fallback_base_type), prefix);
        produced = options.size() > 0;
    }

    result["options"] = options;
    result["force"] = true;
    result["result"] = (produced || has_call_hint) ? static_cast<int64_t>(OK) : static_cast<int64_t>(ERR_UNAVAILABLE);
    return result;
}

Dictionary KorkScriptLanguage::_lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner) const {
    Dictionary result;
    result["result"] = ERR_UNAVAILABLE;
    result["type"] = LOOKUP_RESULT_MAX;
    if (p_symbol.is_empty()) {
        return result;
    }

    const String cursor_token = String::chr(0xFFFF);
    const int cursor = p_code.find(cursor_token);
    const String code_without_cursor = cursor >= 0 ? (p_code.substr(0, cursor) + p_code.substr(cursor + cursor_token.length())) : p_code;
    const String before_cursor = cursor >= 0 ? p_code.substr(0, cursor) : p_code;

    KorkScriptVMHost *host = const_cast<KorkScriptLanguage *>(this)->get_vm_host(String("default"));
    bool parse_succeeded = false;
    LookupMetadata metadata = scan_lookup_metadata(host, code_without_cursor, &parse_succeeded);
    if (!parse_succeeded) {
        const Ref<KorkScript> script = load_kork_script_resource(p_path);
        if (script.is_valid()) {
            metadata = scan_lookup_metadata(host, script->get_source_code_ref(), &parse_succeeded);
        }
    }

    if (!parse_succeeded) {
        return result;
    }

    const String normalized_symbol = normalize_lookup_symbol(p_symbol);
    const std::string symbol_key = bare_lookup_symbol(normalized_symbol).utf8().get_data();
    if (symbol_key.empty()) {
        return result;
    }

    int32_t location = -1;
    const LookupTargetContext target_ctx = extract_lookup_target_context(before_cursor);

    if (symbol_matches_lookup_name(metadata.script_class_name, normalized_symbol) && metadata.script_class_line > 0) {
        location = metadata.script_class_line;
    } else if (target_ctx.has_member_target) {
        const auto lookup_scoped_script_symbol = [&]() {
            const auto field_found = metadata.class_fields.find(symbol_key);
            if (field_found != metadata.class_fields.end()) {
                return field_found->second;
            }
            const auto method_found = metadata.scoped_functions.find(symbol_key);
            if (method_found != metadata.scoped_functions.end()) {
                return method_found->second;
            }
            const auto signal_found = metadata.scoped_signals.find(symbol_key);
            if (signal_found != metadata.scoped_signals.end()) {
                return signal_found->second;
            }
            return -1;
        };

        if (target_ctx.target == "%this") {
            location = lookup_scoped_script_symbol();
            if (location < 0) {
                const StringName godot_type = !metadata.script_parent_name.is_empty() ? StringName(metadata.script_parent_name) : StringName(p_owner != nullptr ? p_owner->get_class() : String("Node"));
                if (lookup_godot_class_member(godot_type, normalized_symbol, result)) {
                    return result;
                }
            }
        } else {
            const String explicit_type = find_explicit_type_for_variable(before_cursor, target_ctx.target);
            if (!explicit_type.is_empty() && explicit_type == metadata.script_class_name) {
                location = lookup_scoped_script_symbol();
                if (location < 0) {
                    const StringName godot_type = !metadata.script_parent_name.is_empty() ? StringName(metadata.script_parent_name) : StringName(p_owner != nullptr ? p_owner->get_class() : String("Node"));
                    if (lookup_godot_class_member(godot_type, normalized_symbol, result)) {
                        return result;
                    }
                }
            } else if (!explicit_type.is_empty()) {
                if (lookup_godot_class_member(StringName(explicit_type), normalized_symbol, result)) {
                    return result;
                }
            }
        }
    } else {
        const auto global_function = metadata.global_functions.find(symbol_key);
        if (global_function != metadata.global_functions.end()) {
            location = global_function->second;
        }
        if (location < 0) {
            const auto global_signal = metadata.global_signals.find(symbol_key);
            if (global_signal != metadata.global_signals.end()) {
                location = global_signal->second;
            }
        }
        if (location < 0 && (normalized_symbol.begins_with("%") || normalized_symbol.begins_with("$"))) {
            location = find_variable_definition_line(code_without_cursor, normalized_symbol, cursor);
            if (location > 0) {
                result["result"] = static_cast<int64_t>(OK);
                result["type"] = static_cast<int64_t>(LOOKUP_RESULT_LOCAL_VARIABLE);
                result["script_path"] = p_path;
                result["location"] = location;
                result["description"] = normalized_symbol;
                result["doc_type"] = "Variant";
                result["value"] = "";
                return result;
            }
        }
    }

    if (location <= 0) {
        return result;
    }

    result["result"] = static_cast<int64_t>(OK);
    result["type"] = static_cast<int64_t>(LOOKUP_RESULT_SCRIPT_LOCATION);
    result["script_path"] = p_path;
    result["location"] = location;
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
    return p_type == _get_type();
}

Dictionary KorkScriptLanguage::_get_global_class_name(const String &p_path) const {
    Dictionary out;
    if (p_path.is_empty()) {
        return out;
    }

    ResourceLoader *loader = ResourceLoader::get_singleton();
    if (loader == nullptr || !loader->exists(p_path)) {
        return out;
    }

    Ref<Resource> resource = loader->load(p_path, String(), ResourceLoader::CACHE_MODE_REUSE);
    Ref<KorkScript> script = resource;
    if (!script.is_valid()) {
        return out;
    }

    if (!script->has_declared_script_class()) {
        return out;
    }

    const String declared_name = script->get_declared_script_class_name().strip_edges();
    const StringName global_name = declared_name.is_empty() ? StringName() : StringName(declared_name);
    if (global_name.is_empty()) {
        return out;
    }

    out["name"] = String(global_name);
    out["base_type"] = script->get_base_type();
    out["icon_path"] = String();
    out["is_abstract"] = false;
    out["is_tool"] = script->is_tool_enabled();
    return out;
}

void KorkScriptLanguage::_bind_methods() {
}

} // namespace godot
