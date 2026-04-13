#include "KorkScript.h"

#include "KorkScriptLanguage.h"
#include "KorkScriptVMHost.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/gdextension_interface_loader.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/core/object.hpp>

#include <unordered_map>
#include <vector>

namespace godot {

namespace {

std::string string_name_key(const StringName &name);

String strip_script_comments(const String &source) {
    String out;

    bool in_string = false;
    bool in_line_comment = false;
    bool in_block_comment = false;

    for (int i = 0; i < source.length(); ++i) {
        const char32_t c = source[i];
        const char32_t next = i + 1 < source.length() ? source[i + 1] : 0;

        if (in_line_comment) {
            if (c == '\n') {
                in_line_comment = false;
                out += String::chr(c);
            }
            continue;
        }

        if (in_block_comment) {
            if (c == '*' && next == '/') {
                in_block_comment = false;
                ++i;
            }
            continue;
        }

        if (!in_string && c == '/' && next == '/') {
            in_line_comment = true;
            ++i;
            continue;
        }

        if (!in_string && c == '/' && next == '*') {
            in_block_comment = true;
            ++i;
            continue;
        }

        if (c == '"' && (i == 0 || source[i - 1] != '\\')) {
            in_string = !in_string;
        }

        out += String::chr(c);
    }

    return out;
}

Variant::Type parse_class_field_variant_type(const String &type_name) {
    const String type = type_name.strip_edges();
    if (type == "bool" || type == "Bool") {
        return Variant::BOOL;
    }
    if (type == "int" || type == "Int") {
        return Variant::INT;
    }
    if (type == "float") {
        return Variant::FLOAT;
    }
    if (type == "uint") {
        return Variant::INT;
    }
    if (type == "string") {
        return Variant::STRING;
    }
    if (type == "Vector2") {
        return Variant::VECTOR2;
    }
    if (type == "Vector3") {
        return Variant::VECTOR3;
    }
    if (type == "Vector4") {
        return Variant::VECTOR4;
    }
    if (type == "Color") {
        return Variant::COLOR;
    }
    return Variant::NIL;
}

Variant parse_class_field_default_value(const String &value_text, Variant::Type type, bool *r_ok = nullptr) {
    if (r_ok != nullptr) {
        *r_ok = false;
    }

    const String text = value_text.strip_edges();
    if (text.is_empty()) {
        return Variant();
    }

    switch (type) {
        case Variant::BOOL:
            if (text == "true" || text == "1") {
                if (r_ok != nullptr) {
                    *r_ok = true;
                }
                return true;
            }
            if (text == "false" || text == "0") {
                if (r_ok != nullptr) {
                    *r_ok = true;
                }
                return false;
            }
            return Variant();
        case Variant::FLOAT:
            if (r_ok != nullptr) {
                *r_ok = true;
            }
            return text.to_float();
        case Variant::INT:
            if (r_ok != nullptr) {
                *r_ok = true;
            }
            return static_cast<int64_t>(text.to_int());
        case Variant::STRING:
            if (text.length() >= 2 && text.begins_with("\"") && text.ends_with("\"")) {
                if (r_ok != nullptr) {
                    *r_ok = true;
                }
                return text.substr(1, text.length() - 2);
            }
            return Variant();
        default:
            return Variant();
    }
}

void parse_script_class_fields(const String &source_code,
        std::unordered_map<std::string, KorkScript::ClassFieldMetadata> &out_metadata,
        std::vector<std::string> &out_order) {
    out_metadata.clear();
    out_order.clear();

    const String sanitized_source = strip_script_comments(source_code);
    const int class_pos = sanitized_source.find("class ");
    if (class_pos < 0) {
        return;
    }

    const int open_brace = sanitized_source.find("{", class_pos);
    const int close_brace = open_brace >= 0 ? sanitized_source.find("};", open_brace) : -1;
    if (open_brace < 0 || close_brace < 0 || close_brace <= open_brace) {
        return;
    }

    const String body = sanitized_source.substr(open_brace + 1, close_brace - (open_brace + 1));
    PackedStringArray declarations = body.split(";", false);
    for (int i = 0; i < declarations.size(); ++i) {
        const String declaration = declarations[i].strip_edges();
        if (declaration.is_empty()) {
            continue;
        }

        const int colon_pos = declaration.find(":");
        if (colon_pos <= 0) {
            continue;
        }

        const String field_name = declaration.substr(0, colon_pos).strip_edges();
        const String remainder = declaration.substr(colon_pos + 1).strip_edges();
        const int equals_pos = remainder.find("=");
        const String type_name = (equals_pos >= 0 ? remainder.substr(0, equals_pos) : remainder).strip_edges();
        const String default_text = equals_pos >= 0 ? remainder.substr(equals_pos + 1).strip_edges() : String();
        if (field_name.is_empty() || type_name.is_empty()) {
            continue;
        }

        KorkScript::ClassFieldMetadata metadata;
        metadata.name = StringName(field_name);
        metadata.type = parse_class_field_variant_type(type_name);
        bool has_default = false;
        metadata.default_value = parse_class_field_default_value(default_text, metadata.type, &has_default);
        metadata.has_default = has_default;

        const std::string key = string_name_key(StringName(field_name));
        if (out_metadata.find(key) == out_metadata.end()) {
            out_order.push_back(key);
        }
        out_metadata[key] = metadata;
    }
}

struct KorkScriptInstance {
    Object *owner = nullptr;
    Ref<KorkScript> script;
    KorkScriptVMHost *host = nullptr;
    KorkApi::VMObject *vm_object = nullptr;
    uint64_t host_generation = 0;
    bool in_native_fallback = false;
};

struct ScriptPropertyListAllocation {
    GDExtensionPropertyInfo *infos = nullptr;
    StringName *names = nullptr;
    StringName *class_names = nullptr;
    String *hint_strings = nullptr;
    uint32_t count = 0;
};

std::unordered_map<const GDExtensionPropertyInfo *, ScriptPropertyListAllocation> g_script_property_lists;

bool is_editor_lifecycle_method(const StringName &name) {
    return name == StringName("_ready") ||
            name == StringName("_enter_tree") ||
            name == StringName("_exit_tree") ||
            name == StringName("_process") ||
            name == StringName("_physics_process") ||
            name == StringName("_internal_process") ||
            name == StringName("_internal_physics_process") ||
            name == StringName("_input") ||
            name == StringName("_shortcut_input") ||
            name == StringName("_unhandled_input") ||
            name == StringName("_unhandled_key_input") ||
            name == StringName("_input_event") ||
            name == StringName("_gui_input") ||
            name == StringName("_draw");
}

bool should_skip_editor_callback(const KorkScriptInstance *instance, const StringName &method_name) {
    if (instance == nullptr || !instance->script.is_valid() || instance->script->is_tool_enabled()) {
        return false;
    }

    Engine *engine = Engine::get_singleton();
    if (engine == nullptr || !engine->is_editor_hint()) {
        return false;
    }

    return is_editor_lifecycle_method(method_name);
}

bool owner_has_godot_property(const KorkScriptInstance *instance, const StringName &property_name) {
    if (instance == nullptr || instance->owner == nullptr || property_name.is_empty()) {
        return false;
    }

    const TypedArray<Dictionary> properties = ClassDBSingleton::get_singleton()->class_get_property_list(instance->owner->get_class());
    for (int i = 0; i < properties.size(); ++i) {
        const Dictionary info = properties[i];
        if (StringName(info.get("name", Variant())) == property_name) {
            return true;
        }
    }
    return false;
}

bool sync_instance_vm_object(KorkScriptInstance *instance) {
    if (instance == nullptr || instance->host == nullptr || !instance->script.is_valid() || instance->owner == nullptr) {
        return false;
    }

    if (!instance->host->ensure_script_loaded(instance->script.ptr())) {
        return false;
    }

    const uint64_t generation = instance->host->get_generation();
    if (instance->vm_object != nullptr && instance->host_generation == generation) {
        return true;
    }

    if (instance->vm_object != nullptr) {
        instance->host->release_vm_object_for_generation(instance->vm_object, instance->host_generation);
    }
    instance->vm_object = nullptr;
    instance->vm_object = instance->host->create_vm_object_for(instance->owner, instance->script.ptr());
    instance->host_generation = generation;
    return instance->vm_object != nullptr;
}

void set_call_error(GDExtensionCallError *r_error, GDExtensionCallErrorType error_type, int32_t argument = -1, int32_t expected = 0) {
    if (r_error == nullptr) {
        return;
    }
    r_error->error = error_type;
    r_error->argument = argument;
    r_error->expected = expected;
}

GDExtensionBool instance_set(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionConstVariantPtr p_value) {
    KorkScriptInstance *instance = static_cast<KorkScriptInstance *>(p_instance);
    if (!sync_instance_vm_object(instance)) {
        return false;
    }

    const StringName &name = *reinterpret_cast<const StringName *>(p_name);
    if (owner_has_godot_property(instance, name)) {
        return false;
    }

    const Variant &value = *reinterpret_cast<const Variant *>(p_value);
    return instance->host->set_instance_field(instance->vm_object, name, value);
}

GDExtensionBool instance_get(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionVariantPtr r_ret) {
    KorkScriptInstance *instance = static_cast<KorkScriptInstance *>(p_instance);
    if (!sync_instance_vm_object(instance)) {
        return false;
    }

    const StringName &name = *reinterpret_cast<const StringName *>(p_name);
    if (owner_has_godot_property(instance, name)) {
        return false;
    }

    Variant value;
    if (!instance->host->get_instance_field(instance->vm_object, name, value)) {
        return false;
    }

    *reinterpret_cast<Variant *>(r_ret) = value;
    return true;
}

const GDExtensionPropertyInfo *instance_get_property_list(GDExtensionScriptInstanceDataPtr p_instance, uint32_t *r_count) {
    KorkScriptInstance *instance = static_cast<KorkScriptInstance *>(p_instance);
    if (r_count == nullptr) {
        return nullptr;
    }
    *r_count = 0;

    if (!sync_instance_vm_object(instance)) {
        return nullptr;
    }

    const TypedArray<Dictionary> properties = instance->host->get_instance_field_list(instance->vm_object);
    const uint32_t count = static_cast<uint32_t>(properties.size());
    if (count == 0) {
        return nullptr;
    }

    ScriptPropertyListAllocation allocation;
    allocation.count = count;
    allocation.infos = memnew_arr(GDExtensionPropertyInfo, count);
    allocation.names = memnew_arr(StringName, count);
    allocation.class_names = memnew_arr(StringName, count);
    allocation.hint_strings = memnew_arr(String, count);

    for (uint32_t i = 0; i < count; ++i) {
        const Dictionary property = properties[static_cast<int32_t>(i)];
        allocation.names[i] = StringName(property.get("name", ""));
        allocation.class_names[i] = StringName();
        allocation.hint_strings[i] = String(property.get("hint_string", ""));

        allocation.infos[i].type = static_cast<GDExtensionVariantType>(static_cast<int64_t>(property.get("type", static_cast<int64_t>(Variant::NIL))));
        allocation.infos[i].name = &allocation.names[i];
        allocation.infos[i].class_name = &allocation.class_names[i];
        allocation.infos[i].hint = static_cast<uint32_t>(static_cast<int64_t>(property.get("hint", static_cast<int64_t>(PROPERTY_HINT_NONE))));
        allocation.infos[i].hint_string = &allocation.hint_strings[i];
        allocation.infos[i].usage = static_cast<uint32_t>(static_cast<int64_t>(property.get("usage", static_cast<int64_t>(PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SCRIPT_VARIABLE))));
    }

    const GDExtensionPropertyInfo *list = allocation.infos;
    g_script_property_lists.emplace(list, std::move(allocation));
    *r_count = count;
    return list;
}

void instance_free_property_list(GDExtensionScriptInstanceDataPtr, const GDExtensionPropertyInfo *p_list, uint32_t) {
    auto found = g_script_property_lists.find(p_list);
    if (found == g_script_property_lists.end()) {
        return;
    }

    ScriptPropertyListAllocation &allocation = found->second;
    memdelete_arr(allocation.infos);
    memdelete_arr(allocation.names);
    memdelete_arr(allocation.class_names);
    memdelete_arr(allocation.hint_strings);
    g_script_property_lists.erase(found);
}

GDExtensionBool instance_property_can_revert(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name) {
    KorkScriptInstance *instance = static_cast<KorkScriptInstance *>(p_instance);
    if (instance == nullptr || !instance->script.is_valid()) {
        return false;
    }

    const StringName &name = *reinterpret_cast<const StringName *>(p_name);
    if (owner_has_godot_property(instance, name)) {
        return false;
    }

    return instance->script->_has_property_default_value(name);
}

GDExtensionBool instance_property_get_revert(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionVariantPtr r_ret) {
    KorkScriptInstance *instance = static_cast<KorkScriptInstance *>(p_instance);
    if (instance == nullptr || !instance->script.is_valid()) {
        return false;
    }

    const StringName &name = *reinterpret_cast<const StringName *>(p_name);
    if (owner_has_godot_property(instance, name) || !instance->script->_has_property_default_value(name)) {
        return false;
    }

    *reinterpret_cast<Variant *>(r_ret) = instance->script->_get_property_default_value(name);
    return true;
}

GDExtensionObjectPtr instance_get_owner(GDExtensionScriptInstanceDataPtr p_instance) {
    KorkScriptInstance *instance = static_cast<KorkScriptInstance *>(p_instance);
    return instance != nullptr && instance->owner != nullptr ? instance->owner->_owner : nullptr;
}

const GDExtensionMethodInfo *instance_get_method_list(GDExtensionScriptInstanceDataPtr, uint32_t *r_count) {
    *r_count = 0;
    return nullptr;
}

void instance_free_method_list(GDExtensionScriptInstanceDataPtr, const GDExtensionMethodInfo *, uint32_t) {
}

GDExtensionVariantType instance_get_property_type(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionBool *r_is_valid) {
    KorkScriptInstance *instance = static_cast<KorkScriptInstance *>(p_instance);
    if (!sync_instance_vm_object(instance)) {
        *r_is_valid = false;
        return GDEXTENSION_VARIANT_TYPE_NIL;
    }

    const StringName &name = *reinterpret_cast<const StringName *>(p_name);
    if (owner_has_godot_property(instance, name)) {
        *r_is_valid = false;
        return GDEXTENSION_VARIANT_TYPE_NIL;
    }

    bool exists = false;
    const Variant::Type type = instance->host->get_instance_field_type(instance->vm_object, name, &exists);
    *r_is_valid = exists;
    return static_cast<GDExtensionVariantType>(type);
}

GDExtensionBool instance_validate_property(GDExtensionScriptInstanceDataPtr, GDExtensionPropertyInfo *) {
    return false;
}

GDExtensionBool instance_has_method(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name) {
    KorkScriptInstance *instance = static_cast<KorkScriptInstance *>(p_instance);
    if (instance == nullptr || !instance->script.is_valid()) {
        return false;
    }

    const StringName &name = *reinterpret_cast<const StringName *>(p_name);
    if (should_skip_editor_callback(instance, name)) {
        return false;
    }

    if (instance->script->has_method_name(name)) {
        return true;
    }

    return false;
}

GDExtensionInt instance_get_method_argument_count(GDExtensionScriptInstanceDataPtr, GDExtensionConstStringNamePtr, GDExtensionBool *r_is_valid) {
    *r_is_valid = true;
    return 0;
}

void instance_call(GDExtensionScriptInstanceDataPtr p_self, GDExtensionConstStringNamePtr p_method, const GDExtensionConstVariantPtr *p_args, GDExtensionInt p_argument_count, GDExtensionVariantPtr r_return, GDExtensionCallError *r_error) {
    KorkScriptInstance *instance = static_cast<KorkScriptInstance *>(p_self);
    if (!sync_instance_vm_object(instance)) {
        set_call_error(r_error, GDEXTENSION_CALL_ERROR_INVALID_METHOD);
        return;
    }

    const StringName &method_name = *reinterpret_cast<const StringName *>(p_method);
    if (should_skip_editor_callback(instance, method_name)) {
        *reinterpret_cast<Variant *>(r_return) = Variant();
        set_call_error(r_error, GDEXTENSION_CALL_OK);
        return;
    }

    if (!instance->script.is_valid() || !instance->script->has_method_name(method_name)) {
        if (instance->in_native_fallback) {
            set_call_error(r_error, GDEXTENSION_CALL_ERROR_INVALID_METHOD);
            return;
        }

        if (!instance->owner->has_method(method_name)) {
            set_call_error(r_error, GDEXTENSION_CALL_ERROR_INVALID_METHOD);
            return;
        }

        instance->in_native_fallback = true;
        Array native_args;
        for (GDExtensionInt i = 0; i < p_argument_count; ++i) {
            native_args.push_back(*reinterpret_cast<const Variant *>(p_args[i]));
        }
        Variant native_result = instance->owner->callv(method_name, native_args);
        instance->in_native_fallback = false;
        *reinterpret_cast<Variant *>(r_return) = native_result;
        set_call_error(r_error, GDEXTENSION_CALL_OK);
        return;
    }

    std::vector<const Variant *> args(static_cast<size_t>(p_argument_count));
    for (GDExtensionInt i = 0; i < p_argument_count; ++i) {
        args[static_cast<size_t>(i)] = reinterpret_cast<const Variant *>(p_args[i]);
    }

    Variant ret;
    const bool ok = instance->host->call_method(instance->vm_object, method_name, args.data(), p_argument_count, ret);
    if (!ok) {
        set_call_error(r_error, GDEXTENSION_CALL_ERROR_INVALID_METHOD);
        return;
    }

    *reinterpret_cast<Variant *>(r_return) = ret;
    set_call_error(r_error, GDEXTENSION_CALL_OK);
}

void instance_notification(GDExtensionScriptInstanceDataPtr p_instance, int32_t p_what, GDExtensionBool) {
    KorkScriptInstance *instance = static_cast<KorkScriptInstance *>(p_instance);
    if (instance == nullptr || !instance->script.is_valid() || !instance->script->has_method_name("_notification")) {
        return;
    }
    if (!sync_instance_vm_object(instance)) {
        return;
    }

    Variant arg = p_what;
    const Variant *args[] = { &arg };
    Variant ret;
    instance->host->call_method(instance->vm_object, "_notification", args, 1, ret);
}

void instance_to_string(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionBool *r_is_valid, GDExtensionStringPtr p_out) {
    KorkScriptInstance *instance = static_cast<KorkScriptInstance *>(p_instance);
    *r_is_valid = true;
    *reinterpret_cast<String *>(p_out) = instance != nullptr ? vformat("[KorkScriptInstance:%s]", instance->owner->get_class()) : String("[KorkScriptInstance]");
}

void instance_refcount_incremented(GDExtensionScriptInstanceDataPtr) {
}

GDExtensionBool instance_refcount_decremented(GDExtensionScriptInstanceDataPtr) {
    return false;
}

GDExtensionObjectPtr instance_get_script(GDExtensionScriptInstanceDataPtr p_instance) {
    KorkScriptInstance *instance = static_cast<KorkScriptInstance *>(p_instance);
    return instance != nullptr && instance->script.is_valid() ? instance->script->_owner : nullptr;
}

GDExtensionBool instance_is_placeholder(GDExtensionScriptInstanceDataPtr) {
    return false;
}

GDExtensionScriptLanguagePtr instance_get_language(GDExtensionScriptInstanceDataPtr) {
    KorkScriptLanguage *language = KorkScriptLanguage::get_singleton();
    return language != nullptr ? language->_owner : nullptr;
}

void instance_free(GDExtensionScriptInstanceDataPtr p_instance) {
    KorkScriptInstance *instance = static_cast<KorkScriptInstance *>(p_instance);
    if (instance == nullptr) {
        return;
    }
    if (instance->host != nullptr && instance->vm_object != nullptr &&
            instance->host_generation == instance->host->get_generation()) {
        instance->host->destroy_vm_object_for(instance->owner, instance->script.ptr(), instance->vm_object);
    }
    if (instance->host != nullptr) {
        instance->host->release_script(instance->script.ptr());
    }
    memdelete(instance);
}

const GDExtensionScriptInstanceInfo3 script_instance_info = {
    &instance_set,
    &instance_get,
    &instance_get_property_list,
    &instance_free_property_list,
    nullptr,
    &instance_property_can_revert,
    &instance_property_get_revert,
    &instance_get_owner,
    nullptr,
    &instance_get_method_list,
    &instance_free_method_list,
    &instance_get_property_type,
    &instance_validate_property,
    &instance_has_method,
    &instance_get_method_argument_count,
    &instance_call,
    &instance_notification,
    &instance_to_string,
    &instance_refcount_incremented,
    &instance_refcount_decremented,
    &instance_get_script,
    &instance_is_placeholder,
    nullptr,
    nullptr,
    &instance_get_language,
    &instance_free
};

std::string string_name_key(const StringName &name) {
    const CharString utf8 = String(name).utf8();
    return std::string(utf8.get_data(), static_cast<size_t>(utf8.length()));
}

Variant::Type variant_type_from_name(const String &type_name) {
    const String normalized = type_name.strip_edges();
    if (normalized.is_empty()) {
        return Variant::NIL;
    }
    if (normalized == "bool" || normalized == "Bool") {
        return Variant::BOOL;
    }
    if (normalized == "int" || normalized == "Int") {
        return Variant::INT;
    }
    if (normalized == "float" || normalized == "Float") {
        return Variant::FLOAT;
    }
    if (normalized == "String" || normalized == "string") {
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

int32_t find_method_line_in_code(const String &code, const String &method_name) {
    String normalized_name = method_name.strip_edges();
    if (normalized_name.is_empty()) {
        return -1;
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

    int from = 0;
    while (true) {
        const int function_pos = code.find("function ", from);
        if (function_pos < 0) {
            break;
        }

        const int ns_pos = code.find("::", function_pos);
        const int open_paren = code.find("(", function_pos);
        if (ns_pos > function_pos && open_paren > ns_pos) {
            const String candidate_name = code.substr(ns_pos + 2, open_paren - (ns_pos + 2)).strip_edges();
            if (candidate_name == normalized_name) {
                return 1 + code.count("\n", 0, function_pos);
            }
        }

        from = function_pos + 8;
    }

    return -1;
}

String infer_class_name_from_code(const String &code) {
    int from = 0;
    while (true) {
        const int class_pos = code.find("class ", from);
        if (class_pos < 0) {
            return String();
        }

        int name_start = class_pos + 6;
        while (name_start < code.length() && String::chr(code[name_start]).strip_edges().is_empty()) {
            ++name_start;
        }

        int name_end = name_start;
        while (name_end < code.length()) {
            const char32_t c = code[name_end];
            const bool valid_ident = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            if (!valid_ident) {
                break;
            }
            ++name_end;
        }

        const String class_name = code.substr(name_start, name_end - name_start).strip_edges();
        if (!class_name.is_empty()) {
            return class_name;
        }

        from = class_pos + 6;
    }
}

String infer_namespace_from_signal_decls(const String &code) {
    int from = 0;
    while (true) {
        const int signal_pos = code.find("signal ", from);
        if (signal_pos < 0) {
            return String();
        }

        const int ns_pos = code.find("::", signal_pos);
        const int open_paren = code.find("(", signal_pos);
        if (ns_pos > signal_pos && open_paren > ns_pos) {
            const String namespace_name = code.substr(signal_pos + 7, ns_pos - (signal_pos + 7)).strip_edges();
            if (!namespace_name.is_empty()) {
                return namespace_name;
            }
        }

        from = signal_pos + 7;
    }
}

KorkScript::MethodArgumentMetadata parse_method_argument(const String &argument_text) {
    KorkScript::MethodArgumentMetadata metadata;
    String token = argument_text.strip_edges();
    if (token.is_empty()) {
        return metadata;
    }

    const int colon_pos = token.find(":");
    String name_part = colon_pos >= 0 ? token.substr(0, colon_pos) : token;
    String type_part = colon_pos >= 0 ? token.substr(colon_pos + 1) : String();

    name_part = name_part.strip_edges();
    if (name_part.begins_with("%")) {
        name_part = name_part.substr(1);
    }
    metadata.name = StringName(name_part);

    type_part = type_part.strip_edges();
    metadata.type = variant_type_from_name(type_part);
    if (metadata.type == Variant::NIL && !type_part.is_empty()) {
        metadata.class_name = StringName(type_part);
    }

    return metadata;
}

} // namespace

KorkScript::KorkScript() :
        vm_name_("default"),
        namespace_name_(""),
        inferred_namespace_name_(""),
        base_type_("Node"),
        tool_enabled_(false),
        revision_(1) {
}

KorkScript::~KorkScript() {
}

void KorkScript::set_source_code(const String &source) {
    source_code_ = source;
    ++revision_;
    refresh_method_cache();
    KorkScriptLanguage *language = KorkScriptLanguage::get_singleton();
    if (language != nullptr) {
        language->notify_script_changed(this);
    }
    emit_changed();
}

void KorkScript::set_vm_name(const String &vm_name) {
    vm_name_ = vm_name.is_empty() ? String("default") : vm_name;
    emit_changed();
}

void KorkScript::set_namespace_name(const String &namespace_name) {
    namespace_name_ = namespace_name.strip_edges();
    emit_changed();
}

void KorkScript::set_base_type(const String &base_type) {
    base_type_ = base_type.is_empty() ? String("Node") : base_type;
    emit_changed();
}

void KorkScript::set_tool_enabled(bool enabled) {
    if (tool_enabled_ == enabled) {
        return;
    }

    tool_enabled_ = enabled;
    emit_changed();
}

const String &KorkScript::get_vm_name() const {
    return vm_name_;
}

const String &KorkScript::get_namespace_name() const {
    return namespace_name_;
}

const String &KorkScript::get_base_type() const {
    return base_type_;
}

bool KorkScript::get_tool_enabled() const {
    return tool_enabled_;
}

String KorkScript::get_effective_namespace_name() const {
    if (!namespace_name_.is_empty()) {
        return namespace_name_;
    }
    if (!inferred_namespace_name_.is_empty()) {
        return inferred_namespace_name_;
    }
    return base_type_;
}

uint64_t KorkScript::get_revision() const {
    return revision_;
}

bool KorkScript::has_method_name(const StringName &method) const {
    return method_names_.find(string_name_key(method)) != method_names_.end();
}

bool KorkScript::is_tool_enabled() const {
    return _is_tool();
}

bool KorkScript::has_class_field(const StringName &field) const {
    return get_class_field_metadata(field) != nullptr;
}

Variant::Type KorkScript::get_class_field_type(const StringName &field, bool *r_exists) const {
    const ClassFieldMetadata *metadata = get_class_field_metadata(field);
    if (r_exists != nullptr) {
        *r_exists = metadata != nullptr;
    }
    return metadata != nullptr ? metadata->type : Variant::NIL;
}

bool KorkScript::get_class_field_default_value(const StringName &field, Variant *r_value) const {
    const ClassFieldMetadata *metadata = get_class_field_metadata(field);
    if (metadata == nullptr || !metadata->has_default) {
        return false;
    }
    if (r_value != nullptr) {
        *r_value = metadata->default_value;
    }
    return true;
}

bool KorkScript::get_previous_class_field_default_value(const StringName &field, Variant *r_value) const {
    const ClassFieldMetadata *metadata = get_previous_class_field_metadata(field);
    if (metadata == nullptr || !metadata->has_default) {
        return false;
    }
    if (r_value != nullptr) {
        *r_value = metadata->default_value;
    }
    return true;
}

PackedStringArray KorkScript::get_class_field_names() const {
    PackedStringArray out;
    for (const std::string &field_name : class_field_order_) {
        const auto found = class_field_metadata_.find(field_name);
        if (found != class_field_metadata_.end()) {
            out.push_back(String(found->second.name));
        }
    }
    return out;
}

bool KorkScript::_editor_can_reload_from_file() {
    return true;
}

bool KorkScript::_can_instantiate() const {
    return true;
}

Ref<Script> KorkScript::_get_base_script() const {
    return Ref<Script>();
}

StringName KorkScript::_get_global_name() const {
    const String effective_namespace = get_effective_namespace_name();
    return effective_namespace.is_empty() ? StringName() : StringName(effective_namespace);
}

StringName KorkScript::_get_instance_base_type() const {
    return StringName(base_type_);
}

void *KorkScript::_instance_create(Object *p_for_object) const {
    KorkScriptLanguage *language = KorkScriptLanguage::get_singleton();
    if (language == nullptr) {
        return nullptr;
    }

    KorkScriptVMHost *host = language->get_vm_host(vm_name_);
    if (host == nullptr || !host->ensure_script_loaded(this)) {
        return nullptr;
    }

    KorkScriptInstance *instance = memnew(KorkScriptInstance);
    instance->owner = p_for_object;
    instance->script = Ref<KorkScript>(const_cast<KorkScript *>(this));
    instance->host = host;
    instance->host->retain_script(this);
    instance->vm_object = host->create_vm_object_for(p_for_object, this);
    instance->host_generation = host->get_generation();
    return gdextension_interface::script_instance_create3(&script_instance_info, instance);
}

void *KorkScript::_placeholder_instance_create(Object *) const {
    return nullptr;
}

bool KorkScript::_instance_has(Object *) const {
    return false;
}

bool KorkScript::_has_source_code() const {
    return true;
}

String KorkScript::_get_source_code() const {
    return source_code_;
}

void KorkScript::_set_source_code(const String &p_code) {
    set_source_code(p_code);
}

Error KorkScript::_reload(bool) {
    KorkScriptLanguage *language = KorkScriptLanguage::get_singleton();
    KorkScriptVMHost *host = language != nullptr ? language->get_vm_host(vm_name_) : nullptr;
    ++revision_;
    refresh_method_cache();
    if (host != nullptr) {
        host->refresh_script_class_defaults(this);
    }
    if (language != nullptr) {
        language->notify_script_changed(this);
    }
    emit_changed();
    return OK;
}

StringName KorkScript::_get_doc_class_name() const {
    const String effective_namespace = get_effective_namespace_name();
    return effective_namespace.is_empty() ? StringName("KorkScript") : StringName(effective_namespace);
}

TypedArray<Dictionary> KorkScript::_get_documentation() const {
    return TypedArray<Dictionary>();
}

bool KorkScript::_has_method(const StringName &p_method) const {
    if (has_method_name(p_method)) {
        return true;
    }

    KorkScriptLanguage *language = KorkScriptLanguage::get_singleton();
    if (language == nullptr) {
        return false;
    }

    KorkScriptVMHost *host = language->get_vm_host(vm_name_);
    return host != nullptr && (host->has_script_method(this, p_method) || host->has_script_signal(this, p_method));
}

bool KorkScript::_has_static_method(const StringName &) const {
    return false;
}

Variant KorkScript::_get_script_method_argument_count(const StringName &p_method) const {
    const MethodMetadata *metadata = get_method_metadata(p_method);
    if (metadata == nullptr) {
        KorkScriptLanguage *language = KorkScriptLanguage::get_singleton();
        if (language == nullptr) {
            return Variant();
        }

        KorkScriptVMHost *host = language->get_vm_host(vm_name_);
        if (host == nullptr) {
            return Variant();
        }

        const Dictionary method_info = host->get_script_method_info(this, p_method);
        if (method_info.is_empty()) {
            return Variant();
        }

        const Array arguments = method_info.get("args", Array());
        return static_cast<int64_t>(arguments.size());
    }
    return static_cast<int64_t>(metadata->arguments.size());
}

Dictionary KorkScript::_get_method_info(const StringName &p_method) const {
    const MethodMetadata *metadata = get_method_metadata(p_method);
    if (metadata == nullptr) {
        KorkScriptLanguage *language = KorkScriptLanguage::get_singleton();
        if (language == nullptr) {
            return Dictionary();
        }

        KorkScriptVMHost *host = language->get_vm_host(vm_name_);
        return host != nullptr ? host->get_script_method_info(this, p_method) : Dictionary();
    }

    MethodInfo method_info(p_method);
    for (const MethodArgumentMetadata &argument : metadata->arguments) {
        PropertyInfo property_info(argument.type, String(argument.name), PROPERTY_HINT_NONE, String(argument.class_name));
        method_info.arguments.push_back(property_info);
    }
    return method_info.operator Dictionary();
}

bool KorkScript::_is_tool() const {
    return tool_enabled_;
}

bool KorkScript::_is_valid() const {
    return !source_code_.is_empty();
}

bool KorkScript::_is_abstract() const {
    return false;
}

ScriptLanguage *KorkScript::_get_language() const {
    return KorkScriptLanguage::get_singleton();
}

bool KorkScript::_has_script_signal(const StringName &p_signal) const {
    KorkScriptLanguage *language = KorkScriptLanguage::get_singleton();
    if (language == nullptr) {
        return false;
    }

    KorkScriptVMHost *host = language->get_vm_host(vm_name_);
    return host != nullptr && host->has_script_signal(this, p_signal);
}

TypedArray<Dictionary> KorkScript::_get_script_signal_list() const {
    KorkScriptLanguage *language = KorkScriptLanguage::get_singleton();
    if (language == nullptr) {
        return TypedArray<Dictionary>();
    }

    KorkScriptVMHost *host = language->get_vm_host(vm_name_);
    return host != nullptr ? host->get_script_signal_list(this) : TypedArray<Dictionary>();
}

bool KorkScript::_has_property_default_value(const StringName &p_property) const {
    const ClassFieldMetadata *metadata = get_class_field_metadata(p_property);
    return metadata != nullptr && metadata->has_default;
}

Variant KorkScript::_get_property_default_value(const StringName &p_property) const {
    const ClassFieldMetadata *metadata = get_class_field_metadata(p_property);
    if (metadata == nullptr || !metadata->has_default) {
        return Variant();
    }
    return metadata->default_value;
}

void KorkScript::_update_exports() {
}

TypedArray<Dictionary> KorkScript::_get_script_method_list() const {
    TypedArray<Dictionary> out;
    std::unordered_set<std::string> seen_methods;
    for (const std::string &method_name : method_order_) {
        Dictionary method_info = _get_method_info(StringName(method_name.c_str()));
        if (!method_info.is_empty()) {
            seen_methods.insert(method_name);
            out.push_back(method_info);
        }
    }

    KorkScriptLanguage *language = KorkScriptLanguage::get_singleton();
    if (language == nullptr) {
        return out;
    }

    KorkScriptVMHost *host = language->get_vm_host(vm_name_);
    if (host == nullptr) {
        return out;
    }

    const TypedArray<Dictionary> host_methods = host->get_script_method_list(this);
    for (int i = 0; i < host_methods.size(); ++i) {
        const Dictionary method_info = host_methods[i];
        const String method_name = method_info.get("name", "");
        if (method_name.is_empty()) {
            continue;
        }

        const std::string key = string_name_key(StringName(method_name));
        if (!seen_methods.insert(key).second) {
            continue;
        }

        out.push_back(method_info);
    }

    return out;
}

TypedArray<Dictionary> KorkScript::_get_script_property_list() const {
    TypedArray<Dictionary> out;
    for (const std::string &field_name : class_field_order_) {
        const auto found = class_field_metadata_.find(field_name);
        if (found == class_field_metadata_.end()) {
            continue;
        }

        Dictionary prop;
        prop["name"] = String(found->second.name);
        prop["type"] = static_cast<int64_t>(found->second.type);
        prop["hint"] = static_cast<int64_t>(PROPERTY_HINT_NONE);
        prop["hint_string"] = String();
        prop["usage"] = static_cast<int64_t>(PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SCRIPT_VARIABLE);
        out.push_back(prop);
    }
    return out;
}

int32_t KorkScript::_get_member_line(const StringName &p_member) const {
    const MethodMetadata *metadata = get_method_metadata(p_member);
    if (metadata != nullptr && metadata->line > 0) {
        return metadata->line;
    }
    return find_method_line_in_code(source_code_, String(p_member));
}

Dictionary KorkScript::_get_constants() const {
    return Dictionary();
}

TypedArray<StringName> KorkScript::_get_members() const {
    TypedArray<StringName> out;
    std::unordered_set<std::string> seen_members;
    for (const std::string &method_name : method_order_) {
        seen_members.insert(method_name);
        out.push_back(StringName(method_name.c_str()));
    }

    KorkScriptLanguage *language = KorkScriptLanguage::get_singleton();
    if (language == nullptr) {
        return out;
    }

    KorkScriptVMHost *host = language->get_vm_host(vm_name_);
    if (host == nullptr) {
        return out;
    }

    const TypedArray<Dictionary> host_methods = host->get_script_method_list(this);
    for (int i = 0; i < host_methods.size(); ++i) {
        const Dictionary method_info = host_methods[i];
        const String method_name = method_info.get("name", "");
        if (method_name.is_empty()) {
            continue;
        }

        const std::string key = string_name_key(StringName(method_name));
        if (!seen_members.insert(key).second) {
            continue;
        }

        out.push_back(StringName(method_name));
    }

    return out;
}

void KorkScript::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_vm_name", "vm_name"), &KorkScript::set_vm_name);
    ClassDB::bind_method(D_METHOD("get_vm_name"), &KorkScript::get_vm_name);
    ClassDB::bind_method(D_METHOD("set_namespace_name", "namespace_name"), &KorkScript::set_namespace_name);
    ClassDB::bind_method(D_METHOD("get_namespace_name"), &KorkScript::get_namespace_name);
    ClassDB::bind_method(D_METHOD("set_base_type", "base_type"), &KorkScript::set_base_type);
    ClassDB::bind_method(D_METHOD("get_base_type"), &KorkScript::get_base_type);
    ClassDB::bind_method(D_METHOD("set_tool_enabled", "enabled"), &KorkScript::set_tool_enabled);
    ClassDB::bind_method(D_METHOD("get_tool_enabled"), &KorkScript::get_tool_enabled);
    ClassDB::bind_method(D_METHOD("set_source_code", "source_code"), &KorkScript::set_source_code);
    ClassDB::bind_method(D_METHOD("get_source_code"), &KorkScript::_get_source_code);
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "source_code", PROPERTY_HINT_MULTILINE_TEXT), "set_source_code", "get_source_code");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "vm_name"), "set_vm_name", "get_vm_name");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "namespace_name"), "set_namespace_name", "get_namespace_name");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "base_type"), "set_base_type", "get_base_type");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "tool_enabled"), "set_tool_enabled", "get_tool_enabled");
}

void KorkScript::refresh_method_cache() {
    previous_class_field_metadata_ = class_field_metadata_;
    method_names_.clear();
    method_metadata_.clear();
    method_order_.clear();
    class_field_metadata_.clear();
    class_field_order_.clear();
    inferred_namespace_name_ = String();

    const String inferred_class_name = infer_class_name_from_code(source_code_);
    if (!inferred_class_name.is_empty()) {
        inferred_namespace_name_ = inferred_class_name;
    }

    int from = 0;
    while (true) {
        const int function_pos = source_code_.find("function ", from);
        if (function_pos < 0) {
            break;
        }

        const int ns_pos = source_code_.find("::", function_pos);
        const int open_paren = source_code_.find("(", function_pos);
        const int close_paren = open_paren >= 0 ? source_code_.find(")", open_paren) : -1;
        if (ns_pos > function_pos && open_paren > ns_pos && close_paren > open_paren) {
            const String namespace_name = source_code_.substr(function_pos + 9, ns_pos - (function_pos + 9)).strip_edges();
            const String method_name = source_code_.substr(ns_pos + 2, open_paren - (ns_pos + 2)).strip_edges();
            if (!method_name.is_empty()) {
                const std::string method_key = string_name_key(StringName(method_name));
                method_names_.insert(method_key);
                if (method_metadata_.find(method_key) == method_metadata_.end()) {
                    method_order_.push_back(method_key);
                }
                if (inferred_namespace_name_.is_empty() && !namespace_name.is_empty()) {
                    inferred_namespace_name_ = namespace_name;
                }

                MethodMetadata metadata;
                const String arguments_text = source_code_.substr(open_paren + 1, close_paren - (open_paren + 1));
                PackedStringArray arguments = arguments_text.split(",");
                for (int i = 0; i < arguments.size(); ++i) {
                    MethodArgumentMetadata argument_metadata = parse_method_argument(arguments[i]);
                    if (argument_metadata.name.is_empty()) {
                        continue;
                    }
                    if (metadata.arguments.empty() && argument_metadata.name == StringName("this")) {
                        continue;
                    }
                    metadata.arguments.push_back(argument_metadata);
                }
                metadata.line = 1 + source_code_.count("\n", 0, function_pos);
                method_metadata_[method_key] = metadata;
            }
        }

        from = function_pos + 8;
    }

    if (inferred_namespace_name_.is_empty()) {
        inferred_namespace_name_ = infer_namespace_from_signal_decls(source_code_);
    }

    parse_script_class_fields(source_code_, class_field_metadata_, class_field_order_);
}

const KorkScript::MethodMetadata *KorkScript::get_method_metadata(const StringName &method) const {
    const auto found = method_metadata_.find(string_name_key(method));
    return found != method_metadata_.end() ? &found->second : nullptr;
}

const KorkScript::ClassFieldMetadata *KorkScript::get_class_field_metadata(const StringName &field) const {
    const auto found = class_field_metadata_.find(string_name_key(field));
    return found != class_field_metadata_.end() ? &found->second : nullptr;
}

const KorkScript::ClassFieldMetadata *KorkScript::get_previous_class_field_metadata(const StringName &field) const {
    const auto found = previous_class_field_metadata_.find(string_name_key(field));
    return found != previous_class_field_metadata_.end() ? &found->second : nullptr;
}

} // namespace godot
