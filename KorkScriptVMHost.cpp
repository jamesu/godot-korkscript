#include "KorkScriptVMHost.h"

#include "KorkScript.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace godot {

namespace {

bool korkscript_debug_types_enabled() {
    const char *value = std::getenv("KORKSCRIPT_DEBUG_TYPES");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

String format_component(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.9g", value);
    return String(buffer);
}

void debug_log_console_value(const char *label, const KorkApi::ConsoleValue &value) {
    if (!korkscript_debug_types_enabled()) {
        return;
    }

    UtilityFunctions::print(String("[korkscript-debug] ")
            + String(label)
            + " type=" + String::num_int64(value.typeId)
            + " zone=" + String::num_int64(value.zoneId)
            + " raw=" + String::num_uint64(value.cvalue));
}

bool parse_space_separated_components(const char *input, int expected_components, double *out_values) {
    if (input == nullptr) {
        return false;
    }

    if (expected_components == 2) {
        return std::sscanf(input, "%lf %lf", &out_values[0], &out_values[1]) == 2;
    }
    if (expected_components == 3) {
        return std::sscanf(input, "%lf %lf %lf", &out_values[0], &out_values[1], &out_values[2]) == 3;
    }
    if (expected_components == 4) {
        return std::sscanf(input, "%lf %lf %lf %lf", &out_values[0], &out_values[1], &out_values[2], &out_values[3]) == 4;
    }
    return false;
}

template <typename TVec, int Components, typename ReadFn, typename WriteFn>
bool cast_pod_value_impl(KorkApi::Vm *vm,
        KorkApi::TypeStorageInterface *input_storage,
        KorkApi::TypeStorageInterface *output_storage,
        void *field_user_ptr,
        BitSet32 flag,
        U32 requested_type,
        U32 self_type_id,
        ReadFn read_fn,
        WriteFn write_fn) {
    const KorkApi::ConsoleValue *argv = nullptr;
    U32 argc = input_storage ? input_storage->data.argc : 0;
    bool direct_load = false;

    if (argc > 0 && input_storage->data.storageRegister) {
        argv = input_storage->data.storageRegister;
    } else {
        argc = 1;
        argv = &input_storage->data.storageAddress;
        direct_load = true;
    }

    TVec value{};

    if (input_storage->isField && direct_load) {
        const TVec *src = static_cast<const TVec *>(input_storage->data.storageAddress.evaluatePtr(vm->getAllocBase()));
        if (!src) {
            return false;
        }
        value = *src;
    } else if (argc == 1 && argv[0].typeId == self_type_id) {
        const TVec *src = static_cast<const TVec *>(argv[0].evaluatePtr(vm->getAllocBase()));
        if (!src) {
            return false;
        }
        value = *src;
    } else if (argc == Components) {
        double values[4] = {};
        for (int i = 0; i < Components; ++i) {
            values[i] = vm->valueAsFloat(argv[i]);
        }
        write_fn(value, values);
    } else if (argc == 1) {
        double values[4] = {};
        const char *text = vm->valueAsString(argv[0]);
        if (!parse_space_separated_components(text, Components, values)) {
            return false;
        }
        write_fn(value, values);
    } else {
        return false;
    }

    if (requested_type == self_type_id) {
        TVec *dst = static_cast<TVec *>(output_storage->data.storageAddress.evaluatePtr(vm->getAllocBase()));
        if (!dst) {
            return false;
        }
        *dst = value;
        if (output_storage->data.storageRegister) {
            *output_storage->data.storageRegister = output_storage->data.storageAddress;
        }
        return true;
    }

    if (requested_type == KorkApi::ConsoleValue::TypeInternalString) {
        double values[4] = {};
        read_fn(value, values);
        String text;
        if (Components == 2) {
            text = format_component(values[0]) + " " + format_component(values[1]);
        } else if (Components == 3) {
            text = format_component(values[0]) + " " + format_component(values[1]) + " " + format_component(values[2]);
        } else {
            text = format_component(values[0]) + " " + format_component(values[1]) + " " + format_component(values[2]) + " " + format_component(values[3]);
        }
        const CharString utf8 = text.utf8();
        const U32 len = static_cast<U32>(utf8.length() + 1);
        output_storage->ResizeStorage(output_storage, len);
        output_storage->FinalizeStorage(output_storage, len);
        char *out = static_cast<char *>(output_storage->data.storageAddress.evaluatePtr(vm->getAllocBase()));
        if (!out) {
            return false;
        }
        std::memcpy(out, utf8.get_data(), static_cast<size_t>(len));
        if (output_storage->data.storageRegister) {
            *output_storage->data.storageRegister = output_storage->data.storageAddress;
        }
        if (korkscript_debug_types_enabled()) {
            UtilityFunctions::print(String("[korkscript-debug] cast->string requested=")
                    + String::num_int64(requested_type)
                    + " argc=" + String::num_int64(argc)
                    + " text='" + text + "'");
        }
        return true;
    }

    KorkApi::ConsoleValue vals[Components];
    double values[4] = {};
    read_fn(value, values);
    for (int i = 0; i < Components; ++i) {
        vals[i] = KorkApi::ConsoleValue::makeNumber(values[i]);
    }
    KorkApi::TypeStorageInterface cast_input;
    vm->initRegisterTypeStorage(Components, vals, &cast_input);
    return vm->castValue(requested_type, &cast_input, output_storage, field_user_ptr, flag);
}

bool vector2_cast_value(void *user_ptr,
        KorkApi::Vm *vm,
        KorkApi::TypeStorageInterface *input_storage,
        KorkApi::TypeStorageInterface *output_storage,
        void *field_user_ptr,
        BitSet32 flag,
        U32 requested_type) {
    const U32 self_type_id = *static_cast<const U32 *>(user_ptr);
    return cast_pod_value_impl<Vector2, 2>(
            vm, input_storage, output_storage, field_user_ptr, flag, requested_type, self_type_id,
            [](const Vector2 &value, double *out_values) {
                out_values[0] = value.x;
                out_values[1] = value.y;
            },
            [](Vector2 &value, const double *in_values) {
                value.x = static_cast<real_t>(in_values[0]);
                value.y = static_cast<real_t>(in_values[1]);
            });
}

bool vector3_cast_value(void *user_ptr,
        KorkApi::Vm *vm,
        KorkApi::TypeStorageInterface *input_storage,
        KorkApi::TypeStorageInterface *output_storage,
        void *field_user_ptr,
        BitSet32 flag,
        U32 requested_type) {
    const U32 self_type_id = *static_cast<const U32 *>(user_ptr);
    return cast_pod_value_impl<Vector3, 3>(
            vm, input_storage, output_storage, field_user_ptr, flag, requested_type, self_type_id,
            [](const Vector3 &value, double *out_values) {
                out_values[0] = value.x;
                out_values[1] = value.y;
                out_values[2] = value.z;
            },
            [](Vector3 &value, const double *in_values) {
                value.x = static_cast<real_t>(in_values[0]);
                value.y = static_cast<real_t>(in_values[1]);
                value.z = static_cast<real_t>(in_values[2]);
            });
}

bool vector4_cast_value(void *user_ptr,
        KorkApi::Vm *vm,
        KorkApi::TypeStorageInterface *input_storage,
        KorkApi::TypeStorageInterface *output_storage,
        void *field_user_ptr,
        BitSet32 flag,
        U32 requested_type) {
    const U32 self_type_id = *static_cast<const U32 *>(user_ptr);
    return cast_pod_value_impl<Vector4, 4>(
            vm, input_storage, output_storage, field_user_ptr, flag, requested_type, self_type_id,
            [](const Vector4 &value, double *out_values) {
                out_values[0] = value.x;
                out_values[1] = value.y;
                out_values[2] = value.z;
                out_values[3] = value.w;
            },
            [](Vector4 &value, const double *in_values) {
                value.x = static_cast<real_t>(in_values[0]);
                value.y = static_cast<real_t>(in_values[1]);
                value.z = static_cast<real_t>(in_values[2]);
                value.w = static_cast<real_t>(in_values[3]);
            });
}

bool color_cast_value(void *user_ptr,
        KorkApi::Vm *vm,
        KorkApi::TypeStorageInterface *input_storage,
        KorkApi::TypeStorageInterface *output_storage,
        void *field_user_ptr,
        BitSet32 flag,
        U32 requested_type) {
    const U32 self_type_id = *static_cast<const U32 *>(user_ptr);
    return cast_pod_value_impl<Color, 4>(
            vm, input_storage, output_storage, field_user_ptr, flag, requested_type, self_type_id,
            [](const Color &value, double *out_values) {
                out_values[0] = value.r;
                out_values[1] = value.g;
                out_values[2] = value.b;
                out_values[3] = value.a;
            },
            [](Color &value, const double *in_values) {
                value.r = static_cast<float>(in_values[0]);
                value.g = static_cast<float>(in_values[1]);
                value.b = static_cast<float>(in_values[2]);
                value.a = static_cast<float>(in_values[3]);
            });
}

const char *vector2_type_name(void *) {
    return "Vector2";
}

const char *vector3_type_name(void *) {
    return "Vector3";
}

const char *vector4_type_name(void *) {
    return "Vector4";
}

const char *color_type_name(void *) {
    return "Color";
}

} // namespace

KorkScriptVMHost::KorkScriptVMHost(const String &vm_name) :
        vm_name_(vm_name),
        vm_(nullptr),
        godot_object_class_id_(-1),
        vector2_type_id_(-1),
        vector3_type_id_(-1),
        vector4_type_id_(-1),
        color_type_id_(-1),
        next_sim_id_(1) {
    KorkApi::Config cfg{};
    cfg.mallocFn = [](size_t size, void *) -> void * {
        return std::malloc(size);
    };
    cfg.freeFn = [](void *ptr, void *) {
        std::free(ptr);
    };
    cfg.logFn = &KorkScriptVMHost::log_callback;
    cfg.logUser = this;
    cfg.iFind.FindObjectByNameFn = &KorkScriptVMHost::find_by_name_callback;
    cfg.iFind.FindObjectByPathFn = &KorkScriptVMHost::find_by_path_callback;
    cfg.iFind.FindObjectByIdFn = &KorkScriptVMHost::find_by_id_callback;
    cfg.findUser = this;
    cfg.enableExceptions = true;
    cfg.enableTuples = true;
    cfg.enableTypes = true;
    cfg.enableStringInterpolation = true;
    cfg.warnUndefinedScriptVariables = true;
    cfg.maxFibers = 128;

    vm_ = KorkApi::createVM(&cfg);
    KorkApi::TypeInfo vector2_info{};
    vector2_info.name = vm_->internString("Vector2");
    vector2_info.userPtr = &vector2_type_id_;
    vector2_info.fieldSize = sizeof(Vector2);
    vector2_info.valueSize = sizeof(Vector2);
    vector2_info.iFuncs = { &vector2_cast_value, &vector2_type_name, nullptr, nullptr };
    vector2_type_id_ = vm_->registerType(vector2_info);

    KorkApi::TypeInfo vector3_info{};
    vector3_info.name = vm_->internString("Vector3");
    vector3_info.userPtr = &vector3_type_id_;
    vector3_info.fieldSize = sizeof(Vector3);
    vector3_info.valueSize = sizeof(Vector3);
    vector3_info.iFuncs = { &vector3_cast_value, &vector3_type_name, nullptr, nullptr };
    vector3_type_id_ = vm_->registerType(vector3_info);

    KorkApi::TypeInfo vector4_info{};
    vector4_info.name = vm_->internString("Vector4");
    vector4_info.userPtr = &vector4_type_id_;
    vector4_info.fieldSize = sizeof(Vector4);
    vector4_info.valueSize = sizeof(Vector4);
    vector4_info.iFuncs = { &vector4_cast_value, &vector4_type_name, nullptr, nullptr };
    vector4_type_id_ = vm_->registerType(vector4_info);

    KorkApi::TypeInfo color_info{};
    color_info.name = vm_->internString("Color");
    color_info.userPtr = &color_type_id_;
    color_info.fieldSize = sizeof(Color);
    color_info.valueSize = sizeof(Color);
    color_info.iFuncs = { &color_cast_value, &color_type_name, nullptr, nullptr };
    color_type_id_ = vm_->registerType(color_info);

    ensure_object_bridge_namespace();

    KorkApi::ClassInfo class_info{};
    class_info.name = vm_->internString("GodotObject");
    class_info.userPtr = this;
    class_info.numFields = 0;
    class_info.fields = nullptr;
    class_info.iCreate = {
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &KorkScriptVMHost::get_vm_object_id,
        &KorkScriptVMHost::get_vm_object_name
    };
    godot_object_class_id_ = vm_->registerClass(class_info);
}

KorkScriptVMHost::~KorkScriptVMHost() {
    if (vm_ != nullptr) {
        KorkApi::destroyVM(vm_);
        vm_ = nullptr;
    }
}

KorkApi::Vm *KorkScriptVMHost::get_vm() const {
    return vm_;
}

const String &KorkScriptVMHost::get_vm_name() const {
    return vm_name_;
}

bool KorkScriptVMHost::ensure_script_loaded(const KorkScript *script) {
    if (script == nullptr || vm_ == nullptr) {
        return false;
    }

    const uint64_t key = reinterpret_cast<uint64_t>(script);
    ScriptLoadState &state = loaded_scripts_[key];
    if (state.revision == script->get_revision()) {
        return true;
    }

    const CharString source_utf8 = script->get_source_code().utf8();
    String path = script->get_path();
    if (path.is_empty()) {
        path = vformat("res://%s_inline.ks", vm_name_);
    }
    const CharString path_utf8 = path.utf8();

    vm_->evalCode(source_utf8.get_data(), path_utf8.get_data(), "");
    if (vm_->getCurrentFiberState() == KorkApi::FiberRunResult::ERROR) {
        UtilityFunctions::push_error(vformat("Failed to load korkscript source into VM '%s'.", vm_name_));
        vm_->clearCurrentFiberError();
        return false;
    }

    state.revision = script->get_revision();
    return true;
}

KorkApi::VMObject *KorkScriptVMHost::create_vm_object_for(Object *owner) {
    if (owner == nullptr || vm_ == nullptr) {
        return nullptr;
    }

    const KorkApi::SimObjectId sim_id = ensure_sim_object_id(owner);
    KorkApi::VMObject *vm_object = vm_->createVMObject(godot_object_class_id_, owner);
    vm_->setObjectNamespace(vm_object, ensure_namespace_for_class(owner->get_class()));
    register_vm_object(owner, vm_object, sim_id);
    return vm_object;
}

void KorkScriptVMHost::destroy_vm_object_for(Object *owner, KorkApi::VMObject *vm_object) {
    unregister_vm_object(owner, vm_object);
    if (vm_ != nullptr && vm_object != nullptr) {
        vm_->decVMRef(vm_object);
    }
}

bool KorkScriptVMHost::call_method(KorkApi::VMObject *vm_object, const StringName &method, const Variant **args, GDExtensionInt arg_count, Variant &ret) const {
    if (vm_ == nullptr || vm_object == nullptr) {
        return false;
    }

    std::vector<KorkApi::ConsoleValue> argv(static_cast<size_t>(arg_count) + 2);
    const std::string method_utf8 = intern_utf8(method);
    argv[0] = console_value_from_variant_for_call(String(method_utf8.c_str()));
    argv[1] = KorkApi::ConsoleValue::makeUnsigned(0);
    for (GDExtensionInt i = 0; i < arg_count; ++i) {
        argv[static_cast<size_t>(i) + 2] = console_value_from_variant_for_call(*args[i]);
    }

    KorkApi::ConsoleValue result;
    const bool ok = vm_->callObjectFunction(vm_object, vm_->internString(method_utf8.c_str()), static_cast<int>(argv.size()), argv.data(), result, false);
    if (ok) {
        ret = variant_from_console_value(result);
    }
    return ok;
}

Variant KorkScriptVMHost::get_property(Object *owner, const StringName &property) const {
    if (owner == nullptr) {
        return Variant();
    }
    return owner->get(property);
}

bool KorkScriptVMHost::set_property(Object *owner, const StringName &property, const Variant &value) const {
    if (owner == nullptr) {
        return false;
    }
    owner->set(property, value);
    return true;
}

void KorkScriptVMHost::log_callback(uint32_t level, const char *console_line, void *user_ptr) {
    KorkScriptVMHost *self = static_cast<KorkScriptVMHost *>(user_ptr);
    if (self == nullptr) {
        return;
    }
    UtilityFunctions::print(vformat("[korkscript:%s:%d] %s", self->vm_name_, static_cast<int64_t>(level), String(console_line ? console_line : "")));
}

KorkApi::VMObject *KorkScriptVMHost::find_by_name_callback(void *user_ptr, StringTableEntry name, KorkApi::VMObject *) {
    KorkScriptVMHost *self = static_cast<KorkScriptVMHost *>(user_ptr);
    if (self == nullptr || name == nullptr) {
        return nullptr;
    }

    auto found = self->vm_objects_by_name_.find(std::string(name));
    return found != self->vm_objects_by_name_.end() ? found->second : nullptr;
}

KorkApi::VMObject *KorkScriptVMHost::find_by_path_callback(void *user_ptr, const char *path) {
    KorkScriptVMHost *self = static_cast<KorkScriptVMHost *>(user_ptr);
    if (self == nullptr || path == nullptr || path[0] == '\0') {
        return nullptr;
    }

    char *end = nullptr;
    const unsigned long ident = std::strtoul(path, &end, 10);
    if (end != nullptr && *end == '\0') {
        auto found = self->vm_objects_by_id_.find(static_cast<KorkApi::SimObjectId>(ident));
        if (found != self->vm_objects_by_id_.end()) {
            return found->second;
        }
    }

    auto found = self->vm_objects_by_name_.find(std::string(path));
    return found != self->vm_objects_by_name_.end() ? found->second : nullptr;
}

KorkApi::VMObject *KorkScriptVMHost::find_by_id_callback(void *user_ptr, KorkApi::SimObjectId ident) {
    KorkScriptVMHost *self = static_cast<KorkScriptVMHost *>(user_ptr);
    if (self == nullptr) {
        return nullptr;
    }

    auto found = self->vm_objects_by_id_.find(ident);
    return found != self->vm_objects_by_id_.end() ? found->second : nullptr;
}

KorkApi::SimObjectId KorkScriptVMHost::get_vm_object_id(KorkApi::VMObject *object) {
    if (object == nullptr || object->klass == nullptr || object->klass->userPtr == nullptr || object->userPtr == nullptr) {
        return 0;
    }

    KorkScriptVMHost *self = static_cast<KorkScriptVMHost *>(object->klass->userPtr);
    Object *owner = static_cast<Object *>(object->userPtr);
    return self->ensure_sim_object_id(owner);
}

StringTableEntry KorkScriptVMHost::get_vm_object_name(KorkApi::VMObject *object) {
    if (object == nullptr || object->klass == nullptr || object->klass->userPtr == nullptr || object->userPtr == nullptr) {
        return nullptr;
    }

    KorkScriptVMHost *self = static_cast<KorkScriptVMHost *>(object->klass->userPtr);
    Object *owner = static_cast<Object *>(object->userPtr);
    Node *node = Object::cast_to<Node>(owner);
    if (node != nullptr) {
        const CharString utf8 = String(node->get_name()).utf8();
        return self->vm_->internString(utf8.get_data());
    }

    const String generated_name = vformat("%s_%d", owner->get_class(), static_cast<int64_t>(self->ensure_sim_object_id(owner)));
    const CharString utf8 = generated_name.utf8();
    return self->vm_->internString(utf8.get_data());
}

KorkApi::ConsoleValue KorkScriptVMHost::object_call_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]) {
    return static_cast<KorkScriptVMHost *>(user_ptr)->bridge_object_call(static_cast<Object *>(obj), argc, argv);
}

KorkApi::ConsoleValue KorkScriptVMHost::object_get_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]) {
    return static_cast<KorkScriptVMHost *>(user_ptr)->bridge_object_get(static_cast<Object *>(obj), argc, argv);
}

KorkApi::ConsoleValue KorkScriptVMHost::object_set_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]) {
    return static_cast<KorkScriptVMHost *>(user_ptr)->bridge_object_set(static_cast<Object *>(obj), argc, argv);
}

KorkApi::ConsoleValue KorkScriptVMHost::object_print_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]) {
    return static_cast<KorkScriptVMHost *>(user_ptr)->bridge_object_print(static_cast<Object *>(obj), argc, argv);
}

KorkApi::NamespaceId KorkScriptVMHost::ensure_namespace_for_class(const StringName &class_name) {
    if (class_name.is_empty()) {
        return vm_->findNamespace(vm_->internString("Object"));
    }

    const std::string class_utf8 = intern_utf8(class_name);
    KorkApi::NamespaceId current = vm_->findNamespace(vm_->internString(class_utf8.c_str()));

    const StringName parent_name = ClassDBSingleton::get_singleton()->get_parent_class(class_name);
    if (!parent_name.is_empty()) {
        KorkApi::NamespaceId parent = ensure_namespace_for_class(parent_name);
        vm_->linkNamespaceById(parent, current);
    }
    return current;
}

void KorkScriptVMHost::ensure_object_bridge_namespace() {
    KorkApi::NamespaceId object_ns = ensure_namespace_for_class("Object");
    vm_->addNamespaceFunction(object_ns, vm_->internString("call"), &KorkScriptVMHost::object_call_callback, this, "(method, ...args)", 3, KorkApi::Constants::MaxArgs);
    vm_->addNamespaceFunction(object_ns, vm_->internString("get"), &KorkScriptVMHost::object_get_callback, this, "(property)", 3, 3);
    vm_->addNamespaceFunction(object_ns, vm_->internString("set"), &KorkScriptVMHost::object_set_callback, this, "(property, value)", 4, 4);
    vm_->addNamespaceFunction(object_ns, vm_->internString("print"), &KorkScriptVMHost::object_print_callback, this, "(...args)", 2, KorkApi::Constants::MaxArgs);
}

KorkApi::SimObjectId KorkScriptVMHost::ensure_sim_object_id(Object *owner) {
    const uint64_t key = owner->get_instance_id();
    auto found = sim_ids_.find(key);
    if (found != sim_ids_.end()) {
        return found->second;
    }
    const KorkApi::SimObjectId id = next_sim_id_++;
    sim_ids_[key] = id;
    return id;
}

void KorkScriptVMHost::register_vm_object(Object *owner, KorkApi::VMObject *vm_object, KorkApi::SimObjectId sim_id) {
    if (owner == nullptr || vm_object == nullptr) {
        return;
    }

    vm_objects_by_id_[sim_id] = vm_object;

    Node *node = Object::cast_to<Node>(owner);
    if (node != nullptr) {
        const CharString name_utf8 = String(node->get_name()).utf8();
        vm_objects_by_name_[std::string(name_utf8.get_data(), static_cast<size_t>(name_utf8.length()))] = vm_object;
    }
}

void KorkScriptVMHost::unregister_vm_object(Object *owner, KorkApi::VMObject *vm_object) {
    if (owner == nullptr) {
        return;
    }

    auto sim_it = sim_ids_.find(owner->get_instance_id());
    if (sim_it != sim_ids_.end()) {
        vm_objects_by_id_.erase(sim_it->second);
        sim_ids_.erase(sim_it);
    }

    Node *node = Object::cast_to<Node>(owner);
    if (node != nullptr) {
        const CharString name_utf8 = String(node->get_name()).utf8();
        auto name_it = vm_objects_by_name_.find(std::string(name_utf8.get_data(), static_cast<size_t>(name_utf8.length())));
        if (name_it != vm_objects_by_name_.end() && name_it->second == vm_object) {
            vm_objects_by_name_.erase(name_it);
        }
    }
}

KorkApi::ConsoleValue KorkScriptVMHost::bridge_object_call(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const {
    if (target == nullptr || argc < 3) {
        return KorkApi::ConsoleValue();
    }

    Array args;
    for (int32_t i = 3; i < argc; ++i) {
        args.push_back(parse_script_argument(argv[i]));
    }

    const StringName method(console_value_to_string(argv[2]));
    return console_value_from_variant(target->callv(method, args));
}

KorkApi::ConsoleValue KorkScriptVMHost::bridge_object_get(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const {
    if (target == nullptr || argc < 3) {
        return KorkApi::ConsoleValue();
    }
    const StringName property_name(console_value_to_string(argv[2]));
    Variant value = target->get(property_name);
    if (korkscript_debug_types_enabled()) {
        UtilityFunctions::print(String("[korkscript-debug] get property='")
                + String(property_name)
                + "' variant_type=" + String::num_int64(value.get_type())
                + " value='" + value.stringify() + "'");
    }
    KorkApi::ConsoleValue out = console_value_from_variant(value);
    debug_log_console_value("bridge_object_get.return", out);
    return out;
}

KorkApi::ConsoleValue KorkScriptVMHost::bridge_object_set(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const {
    if (target == nullptr || argc < 4) {
        return KorkApi::ConsoleValue::makeUnsigned(0);
    }
    target->set(StringName(console_value_to_string(argv[2])), parse_script_argument(argv[3]));
    return KorkApi::ConsoleValue::makeUnsigned(1);
}

KorkApi::ConsoleValue KorkScriptVMHost::bridge_object_print(Object *, int32_t argc, KorkApi::ConsoleValue argv[]) const {
    PackedStringArray parts;
    for (int32_t i = 2; i < argc; ++i) {
        parts.push_back(console_value_to_string(argv[i]));
    }
    UtilityFunctions::print(String(" ").join(parts));
    return KorkApi::ConsoleValue::makeString("");
}

Variant KorkScriptVMHost::variant_from_console_value(KorkApi::ConsoleValue value) const {
    if (value.isFloat()) {
        return value.getFloat();
    }
    if (value.isUnsigned()) {
        return static_cast<int64_t>(value.getInt());
    }
    if (value.isCustom()) {
        if (value.typeId == vector2_type_id_) {
            const Vector2 *vec = static_cast<const Vector2 *>(value.evaluatePtr(vm_->getAllocBase()));
            if (vec != nullptr) {
                return *vec;
            }
        }
        if (value.typeId == vector3_type_id_) {
            const Vector3 *vec = static_cast<const Vector3 *>(value.evaluatePtr(vm_->getAllocBase()));
            if (vec != nullptr) {
                return *vec;
            }
        }
        if (value.typeId == vector4_type_id_) {
            const Vector4 *vec = static_cast<const Vector4 *>(value.evaluatePtr(vm_->getAllocBase()));
            if (vec != nullptr) {
                return *vec;
            }
        }
        if (value.typeId == color_type_id_) {
            const Color *color = static_cast<const Color *>(value.evaluatePtr(vm_->getAllocBase()));
            if (color != nullptr) {
                return *color;
            }
        }
    }
    return String(vm_->valueAsString(value));
}

KorkApi::ConsoleValue KorkScriptVMHost::console_value_from_variant(const Variant &value) const {
    switch (value.get_type()) {
        case Variant::BOOL:
        {
            KorkApi::ConsoleValue out = KorkApi::ConsoleValue::makeUnsigned(static_cast<bool>(value) ? 1 : 0);
            debug_log_console_value("console_value_from_variant.bool", out);
            return out;
        }
        case Variant::INT:
        {
            KorkApi::ConsoleValue out = KorkApi::ConsoleValue::makeUnsigned(static_cast<uint64_t>(static_cast<int64_t>(value)));
            debug_log_console_value("console_value_from_variant.int", out);
            return out;
        }
        case Variant::FLOAT:
        {
            KorkApi::ConsoleValue out = KorkApi::ConsoleValue::makeNumber(static_cast<double>(value));
            debug_log_console_value("console_value_from_variant.float", out);
            return out;
        }
        case Variant::VECTOR2: {
            KorkApi::ConsoleValue out = vm_->getTypeInZone(KorkApi::ConsoleValue::ZoneReturn, vector2_type_id_, sizeof(Vector2));
            Vector2 *dst = static_cast<Vector2 *>(out.evaluatePtr(vm_->getAllocBase()));
            if (dst != nullptr) {
                *dst = static_cast<Vector2>(value);
            }
            debug_log_console_value("console_value_from_variant.vector2", out);
            return out;
        }
        case Variant::VECTOR3: {
            KorkApi::ConsoleValue out = vm_->getTypeInZone(KorkApi::ConsoleValue::ZoneReturn, vector3_type_id_, sizeof(Vector3));
            Vector3 *dst = static_cast<Vector3 *>(out.evaluatePtr(vm_->getAllocBase()));
            if (dst != nullptr) {
                *dst = static_cast<Vector3>(value);
            }
            debug_log_console_value("console_value_from_variant.vector3", out);
            return out;
        }
        case Variant::VECTOR4: {
            KorkApi::ConsoleValue out = vm_->getTypeInZone(KorkApi::ConsoleValue::ZoneReturn, vector4_type_id_, sizeof(Vector4));
            Vector4 *dst = static_cast<Vector4 *>(out.evaluatePtr(vm_->getAllocBase()));
            if (dst != nullptr) {
                *dst = static_cast<Vector4>(value);
            }
            debug_log_console_value("console_value_from_variant.vector4", out);
            return out;
        }
        case Variant::COLOR: {
            KorkApi::ConsoleValue out = vm_->getTypeInZone(KorkApi::ConsoleValue::ZoneReturn, color_type_id_, sizeof(Color));
            Color *dst = static_cast<Color *>(out.evaluatePtr(vm_->getAllocBase()));
            if (dst != nullptr) {
                *dst = static_cast<Color>(value);
            }
            debug_log_console_value("console_value_from_variant.color", out);
            return out;
        }
        case Variant::NIL:
        {
            KorkApi::ConsoleValue out;
            debug_log_console_value("console_value_from_variant.nil", out);
            return out;
        }
        default: {
            const CharString utf8 = value.stringify().utf8();
            KorkApi::ConsoleValue buffer = vm_->getStringInZone(KorkApi::ConsoleValue::ZoneReturn, static_cast<uint32_t>(utf8.length() + 1));
            char *out = static_cast<char *>(buffer.evaluatePtr(vm_->getAllocBase()));
            if (out != nullptr) {
                std::memcpy(out, utf8.get_data(), static_cast<size_t>(utf8.length() + 1));
            }
            debug_log_console_value("console_value_from_variant.stringish", buffer);
            return buffer;
        }
    }
}

KorkApi::ConsoleValue KorkScriptVMHost::console_value_from_variant_for_call(const Variant &value) const {
    switch (value.get_type()) {
        case Variant::BOOL:
            return KorkApi::ConsoleValue::makeUnsigned(static_cast<bool>(value) ? 1 : 0);
        case Variant::INT:
            return KorkApi::ConsoleValue::makeUnsigned(static_cast<uint64_t>(static_cast<int64_t>(value)));
        case Variant::FLOAT:
            return KorkApi::ConsoleValue::makeNumber(static_cast<double>(value));
        case Variant::VECTOR2: {
            KorkApi::ConsoleValue out = vm_->getTypeFunc(vector2_type_id_, sizeof(Vector2));
            Vector2 *dst = static_cast<Vector2 *>(out.evaluatePtr(vm_->getAllocBase()));
            if (dst != nullptr) {
                *dst = static_cast<Vector2>(value);
            }
            debug_log_console_value("console_value_from_variant_for_call.vector2", out);
            return out;
        }
        case Variant::VECTOR3: {
            KorkApi::ConsoleValue out = vm_->getTypeFunc(vector3_type_id_, sizeof(Vector3));
            Vector3 *dst = static_cast<Vector3 *>(out.evaluatePtr(vm_->getAllocBase()));
            if (dst != nullptr) {
                *dst = static_cast<Vector3>(value);
            }
            debug_log_console_value("console_value_from_variant_for_call.vector3", out);
            return out;
        }
        case Variant::VECTOR4: {
            KorkApi::ConsoleValue out = vm_->getTypeFunc(vector4_type_id_, sizeof(Vector4));
            Vector4 *dst = static_cast<Vector4 *>(out.evaluatePtr(vm_->getAllocBase()));
            if (dst != nullptr) {
                *dst = static_cast<Vector4>(value);
            }
            debug_log_console_value("console_value_from_variant_for_call.vector4", out);
            return out;
        }
        case Variant::COLOR: {
            KorkApi::ConsoleValue out = vm_->getTypeFunc(color_type_id_, sizeof(Color));
            Color *dst = static_cast<Color *>(out.evaluatePtr(vm_->getAllocBase()));
            if (dst != nullptr) {
                *dst = static_cast<Color>(value);
            }
            debug_log_console_value("console_value_from_variant_for_call.color", out);
            return out;
        }
        case Variant::NIL:
            return KorkApi::ConsoleValue();
        default: {
            const CharString utf8 = value.stringify().utf8();
            KorkApi::ConsoleValue buffer = vm_->getStringFuncBuffer(static_cast<uint32_t>(utf8.length() + 1));
            char *out = static_cast<char *>(buffer.evaluatePtr(vm_->getAllocBase()));
            if (out != nullptr) {
                std::memcpy(out, utf8.get_data(), static_cast<size_t>(utf8.length() + 1));
            }
            debug_log_console_value("console_value_from_variant_for_call.stringish", buffer);
            return buffer;
        }
    }
}

Variant KorkScriptVMHost::parse_script_argument(KorkApi::ConsoleValue value) const {
    if (value.isFloat()) {
        return value.getFloat();
    }
    if (value.isUnsigned()) {
        return static_cast<int64_t>(value.getInt());
    }
    if (value.isCustom()) {
        if (value.typeId == vector2_type_id_) {
            const Vector2 *vec = static_cast<const Vector2 *>(value.evaluatePtr(vm_->getAllocBase()));
            if (vec != nullptr) {
                return *vec;
            }
        }
        if (value.typeId == vector3_type_id_) {
            const Vector3 *vec = static_cast<const Vector3 *>(value.evaluatePtr(vm_->getAllocBase()));
            if (vec != nullptr) {
                return *vec;
            }
        }
        if (value.typeId == vector4_type_id_) {
            const Vector4 *vec = static_cast<const Vector4 *>(value.evaluatePtr(vm_->getAllocBase()));
            if (vec != nullptr) {
                return *vec;
            }
        }
        if (value.typeId == color_type_id_) {
            const Color *color = static_cast<const Color *>(value.evaluatePtr(vm_->getAllocBase()));
            if (color != nullptr) {
                return *color;
            }
        }
    }

    const String text = console_value_to_string(value);
    if (text == "true") {
        return true;
    }
    if (text == "false") {
        return false;
    }
    if (text == "null") {
        return Variant();
    }

    if (text.is_valid_int()) {
        return text.to_int();
    }
    if (text.is_valid_float()) {
        return text.to_float();
    }
    return text;
}

String KorkScriptVMHost::console_value_to_string(KorkApi::ConsoleValue value) const {
    const char *str = vm_->valueAsString(value);
    return String(str ? str : "");
}

std::string KorkScriptVMHost::intern_utf8(const StringName &value) const {
    return intern_utf8(String(value));
}

std::string KorkScriptVMHost::intern_utf8(const String &value) const {
    const CharString utf8 = value.utf8();
    return std::string(utf8.get_data(), static_cast<size_t>(utf8.length()));
}

} // namespace godot
