#include "KorkScript.h"

#include "KorkScriptLanguage.h"
#include "KorkScriptVMHost.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/gdextension_interface_loader.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/core/object.hpp>

#include <vector>

namespace godot {

namespace {

struct KorkScriptInstance {
    Object *owner = nullptr;
    Ref<KorkScript> script;
    KorkScriptVMHost *host = nullptr;
    KorkApi::VMObject *vm_object = nullptr;
    bool in_native_fallback = false;
};

void set_call_error(GDExtensionCallError *r_error, GDExtensionCallErrorType error_type, int32_t argument = -1, int32_t expected = 0) {
    if (r_error == nullptr) {
        return;
    }
    r_error->error = error_type;
    r_error->argument = argument;
    r_error->expected = expected;
}

GDExtensionBool instance_set(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionConstVariantPtr p_value) {
    return false;
}

GDExtensionBool instance_get(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionVariantPtr r_ret) {
    return false;
}

const GDExtensionPropertyInfo *instance_get_property_list(GDExtensionScriptInstanceDataPtr, uint32_t *r_count) {
    *r_count = 0;
    return nullptr;
}

void instance_free_property_list(GDExtensionScriptInstanceDataPtr, const GDExtensionPropertyInfo *, uint32_t) {
}

GDExtensionBool instance_property_can_revert(GDExtensionScriptInstanceDataPtr, GDExtensionConstStringNamePtr) {
    return false;
}

GDExtensionBool instance_property_get_revert(GDExtensionScriptInstanceDataPtr, GDExtensionConstStringNamePtr, GDExtensionVariantPtr) {
    return false;
}

GDExtensionObjectPtr instance_get_owner(GDExtensionScriptInstanceDataPtr p_instance) {
    KorkScriptInstance *instance = static_cast<KorkScriptInstance *>(p_instance);
    return instance != nullptr && instance->owner != nullptr ? instance->owner->_owner : nullptr;
}

void instance_get_property_state(GDExtensionScriptInstanceDataPtr, GDExtensionScriptInstancePropertyStateAdd, void *) {
}

const GDExtensionMethodInfo *instance_get_method_list(GDExtensionScriptInstanceDataPtr, uint32_t *r_count) {
    *r_count = 0;
    return nullptr;
}

void instance_free_method_list(GDExtensionScriptInstanceDataPtr, const GDExtensionMethodInfo *, uint32_t) {
}

GDExtensionVariantType instance_get_property_type(GDExtensionScriptInstanceDataPtr, GDExtensionConstStringNamePtr, GDExtensionBool *r_is_valid) {
    *r_is_valid = false;
    return GDEXTENSION_VARIANT_TYPE_NIL;
}

GDExtensionBool instance_validate_property(GDExtensionScriptInstanceDataPtr, GDExtensionPropertyInfo *) {
    return false;
}

GDExtensionBool instance_has_method(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name) {
    KorkScriptInstance *instance = static_cast<KorkScriptInstance *>(p_instance);
    return instance != nullptr && instance->script.is_valid() && instance->script->has_method_name(*reinterpret_cast<const StringName *>(p_name));
}

GDExtensionInt instance_get_method_argument_count(GDExtensionScriptInstanceDataPtr, GDExtensionConstStringNamePtr, GDExtensionBool *r_is_valid) {
    *r_is_valid = true;
    return 0;
}

void instance_call(GDExtensionScriptInstanceDataPtr p_self, GDExtensionConstStringNamePtr p_method, const GDExtensionConstVariantPtr *p_args, GDExtensionInt p_argument_count, GDExtensionVariantPtr r_return, GDExtensionCallError *r_error) {
    KorkScriptInstance *instance = static_cast<KorkScriptInstance *>(p_self);
    if (instance == nullptr || instance->host == nullptr || instance->vm_object == nullptr) {
        set_call_error(r_error, GDEXTENSION_CALL_ERROR_INVALID_METHOD);
        return;
    }

    const StringName &method_name = *reinterpret_cast<const StringName *>(p_method);

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
    if (instance->host != nullptr && instance->vm_object != nullptr) {
        instance->host->destroy_vm_object_for(instance->owner, instance->vm_object);
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
    &instance_get_property_state,
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
    &instance_set,
    &instance_get,
    &instance_get_language,
    &instance_free
};

std::string string_name_key(const StringName &name) {
    const CharString utf8 = String(name).utf8();
    return std::string(utf8.get_data(), static_cast<size_t>(utf8.length()));
}

} // namespace

KorkScript::KorkScript() :
        vm_name_("default"),
        base_type_("Node"),
        revision_(1) {
}

KorkScript::~KorkScript() {
}

void KorkScript::set_source_code(const String &source) {
    source_code_ = source;
    ++revision_;
    refresh_method_cache();
    emit_changed();
}

void KorkScript::set_vm_name(const String &vm_name) {
    vm_name_ = vm_name.is_empty() ? String("default") : vm_name;
    emit_changed();
}

void KorkScript::set_base_type(const String &base_type) {
    base_type_ = base_type.is_empty() ? String("Node") : base_type;
    emit_changed();
}

const String &KorkScript::get_vm_name() const {
    return vm_name_;
}

const String &KorkScript::get_base_type() const {
    return base_type_;
}

uint64_t KorkScript::get_revision() const {
    return revision_;
}

bool KorkScript::has_method_name(const StringName &method) const {
    return method_names_.find(string_name_key(method)) != method_names_.end();
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
    return StringName();
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
    instance->vm_object = host->create_vm_object_for(p_for_object);
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
    ++revision_;
    refresh_method_cache();
    emit_changed();
    return OK;
}

TypedArray<Dictionary> KorkScript::_get_documentation() const {
    return TypedArray<Dictionary>();
}

bool KorkScript::_has_method(const StringName &p_method) const {
    return has_method_name(p_method);
}

bool KorkScript::_has_static_method(const StringName &) const {
    return false;
}

Variant KorkScript::_get_script_method_argument_count(const StringName &p_method) const {
    if (!has_method_name(p_method)) {
        return Variant();
    }
    return 0;
}

bool KorkScript::_is_tool() const {
    return false;
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

bool KorkScript::_has_script_signal(const StringName &) const {
    return false;
}

TypedArray<Dictionary> KorkScript::_get_script_signal_list() const {
    return TypedArray<Dictionary>();
}

bool KorkScript::_has_property_default_value(const StringName &) const {
    return false;
}

Variant KorkScript::_get_property_default_value(const StringName &) const {
    return Variant();
}

void KorkScript::_update_exports() {
}

TypedArray<Dictionary> KorkScript::_get_script_method_list() const {
    TypedArray<Dictionary> out;
    for (const std::string &name : method_names_) {
        out.push_back(MethodInfo(StringName(name.c_str())).operator Dictionary());
    }
    return out;
}

TypedArray<Dictionary> KorkScript::_get_script_property_list() const {
    return TypedArray<Dictionary>();
}

void KorkScript::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_vm_name", "vm_name"), &KorkScript::set_vm_name);
    ClassDB::bind_method(D_METHOD("get_vm_name"), &KorkScript::get_vm_name);
    ClassDB::bind_method(D_METHOD("set_base_type", "base_type"), &KorkScript::set_base_type);
    ClassDB::bind_method(D_METHOD("get_base_type"), &KorkScript::get_base_type);
    ClassDB::bind_method(D_METHOD("set_source_code", "source_code"), &KorkScript::set_source_code);
    ClassDB::bind_method(D_METHOD("get_source_code"), &KorkScript::_get_source_code);
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "source_code", PROPERTY_HINT_MULTILINE_TEXT), "set_source_code", "get_source_code");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "vm_name"), "set_vm_name", "get_vm_name");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "base_type"), "set_base_type", "get_base_type");
}

void KorkScript::refresh_method_cache() {
    method_names_.clear();

    int from = 0;
    while (true) {
        const int function_pos = source_code_.find("function ", from);
        if (function_pos < 0) {
            break;
        }

        const int ns_pos = source_code_.find("::", function_pos);
        const int open_paren = source_code_.find("(", function_pos);
        if (ns_pos > function_pos && open_paren > ns_pos) {
            const String method_name = source_code_.substr(ns_pos + 2, open_paren - (ns_pos + 2)).strip_edges();
            if (!method_name.is_empty()) {
                method_names_.insert(string_name_key(StringName(method_name)));
            }
        }

        from = function_pos + 8;
    }
}

} // namespace godot
