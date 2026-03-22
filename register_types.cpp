#include "register_types.h"

#include "KorkScript.h"
#include "KorkScriptLanguage.h"
#include "KorkScriptVMNode.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/memory.hpp>

namespace godot {

static KorkScriptLanguage *language_singleton = nullptr;

void initialize_korkscript_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }

    ClassDB::register_class<KorkScriptLanguage>();
    ClassDB::register_class<KorkScript>();
    ClassDB::register_class<KorkScriptVMNode>();

    language_singleton = memnew(KorkScriptLanguage);
    Engine::get_singleton()->register_script_language(language_singleton);
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
}

} // namespace godot
