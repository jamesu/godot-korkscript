#include "register_types.h"

#include "KorkScript.h"
#include "KorkScriptLanguage.h"
#include "KorkScriptResourceFormat.h"
#include "KorkScriptVMNode.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/core/memory.hpp>

namespace godot {

static KorkScriptLanguage *language_singleton = nullptr;
static Ref<KorkScriptResourceFormatLoader> resource_loader_singleton;
static Ref<KorkScriptResourceFormatSaver> resource_saver_singleton;
static bool classes_registered = false;

void initialize_korkscript_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }

    if (!classes_registered) {
        ClassDB::register_class<KorkScriptLanguage>();
        ClassDB::register_class<KorkScript>();
        ClassDB::register_class<KorkScriptResourceFormatLoader>();
        ClassDB::register_class<KorkScriptResourceFormatSaver>();
        ClassDB::register_class<KorkScriptVMNode>();
        classes_registered = true;
    }

    if (language_singleton == nullptr) {
        language_singleton = memnew(KorkScriptLanguage);
        Engine::get_singleton()->register_script_language(language_singleton);
    }

    if (resource_loader_singleton.is_null()) {
        resource_loader_singleton.instantiate();
        ResourceLoader::get_singleton()->add_resource_format_loader(resource_loader_singleton);
    }

    if (resource_saver_singleton.is_null()) {
        resource_saver_singleton.instantiate();
        ResourceSaver::get_singleton()->add_resource_format_saver(resource_saver_singleton);
    }
}

void uninitialize_korkscript_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }

    if (language_singleton != nullptr) {
        Engine::get_singleton()->unregister_script_language(language_singleton);
        memdelete(language_singleton);
        language_singleton = nullptr;
    }

    if (resource_loader_singleton.is_valid()) {
        ResourceLoader::get_singleton()->remove_resource_format_loader(resource_loader_singleton);
        resource_loader_singleton.unref();
    }

    if (resource_saver_singleton.is_valid()) {
        ResourceSaver::get_singleton()->remove_resource_format_saver(resource_saver_singleton);
        resource_saver_singleton.unref();
    }
}

} // namespace godot
