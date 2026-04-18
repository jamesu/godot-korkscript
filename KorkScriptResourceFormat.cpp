#include "KorkScriptResourceFormat.h"

#include "KorkScript.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {

namespace {

bool is_ks_path(const String &path) {
    const String lower_path = path.to_lower();
    return lower_path.ends_with(".ks") || lower_path.ends_with(".tscript");
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

bool KorkScriptResourceFormatLoader::_exists(const String &p_path) const {
    return is_ks_path(p_path) && FileAccess::file_exists(p_path);
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
