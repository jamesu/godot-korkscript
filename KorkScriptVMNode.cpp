#include "KorkScriptVMNode.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstdlib>
#include <cstring>

namespace godot {

namespace {

constexpr const char *kDefaultFilename = "res://inline.ks";

String level_prefix(uint32_t level) {
    if (level == 0) {
        return "[korkscript]";
    }
    return vformat("[korkscript:%d]", static_cast<int64_t>(level));
}

} // namespace

KorkScriptVMNode::KorkScriptVMNode() : vm_(nullptr) {
}

KorkScriptVMNode::~KorkScriptVMNode() {
    shutdown_vm();
}

void KorkScriptVMNode::_bind_methods() {
    ClassDB::bind_method(D_METHOD("initialize_vm"), &KorkScriptVMNode::initialize_vm);
    ClassDB::bind_method(D_METHOD("shutdown_vm"), &KorkScriptVMNode::shutdown_vm);
    ClassDB::bind_method(D_METHOD("has_vm"), &KorkScriptVMNode::has_vm);
    ClassDB::bind_method(D_METHOD("execute_script", "source", "filename"), &KorkScriptVMNode::execute_script, DEFVAL(String(kDefaultFilename)));

    ADD_SIGNAL(MethodInfo("console_output", PropertyInfo(Variant::INT, "level"), PropertyInfo(Variant::STRING, "line")));
}

void KorkScriptVMNode::_notification(int p_what) {
    if (p_what == NOTIFICATION_EXIT_TREE) {
        shutdown_vm();
    }
}

bool KorkScriptVMNode::initialize_vm() {
    if (vm_ != nullptr) {
        return true;
    }

    KorkApi::Config cfg{};
    cfg.mallocFn = [](size_t size, void *) -> void * {
        return std::malloc(size);
    };
    cfg.freeFn = [](void *ptr, void *) {
        std::free(ptr);
    };
    cfg.logFn = &KorkScriptVMNode::log_callback;
    cfg.logUser = this;
    cfg.enableExceptions = true;
    cfg.enableTuples = true;
    cfg.enableTypes = true;
    cfg.enableStringInterpolation = true;
    cfg.warnUndefinedScriptVariables = true;
    cfg.maxFibers = 64;

    vm_ = KorkApi::createVM(&cfg);
    if (vm_ == nullptr) {
        UtilityFunctions::push_error("Failed to create korkscript VM.");
        return false;
    }

    KorkApi::NamespaceId global_ns = vm_->getGlobalNamespace();
    vm_->addNamespaceFunction(global_ns, vm_->internString("godot_call"), &KorkScriptVMNode::godot_call_callback, this, "(target_path, method, ...args)", 3, KorkApi::Constants::MaxArgs);
    vm_->addNamespaceFunction(global_ns, vm_->internString("godot_get"), &KorkScriptVMNode::godot_get_callback, this, "(target_path, property)", 3, 3);
    vm_->addNamespaceFunction(global_ns, vm_->internString("godot_set"), &KorkScriptVMNode::godot_set_callback, this, "(target_path, property, value)", 4, 4);
    vm_->addNamespaceFunction(global_ns, vm_->internString("godot_print"), &KorkScriptVMNode::godot_print_callback, this, "(...args)", 1, KorkApi::Constants::MaxArgs);
    return true;
}

void KorkScriptVMNode::shutdown_vm() {
    if (vm_ == nullptr) {
        return;
    }
    KorkApi::destroyVM(vm_);
    vm_ = nullptr;
}

bool KorkScriptVMNode::has_vm() const {
    return vm_ != nullptr;
}

Variant KorkScriptVMNode::execute_script(const String &source, const String &filename) {
    if (!initialize_vm()) {
        return Variant();
    }

    const CharString source_utf8 = source.utf8();
    const CharString file_utf8 = filename.utf8();
    KorkApi::ConsoleValue result = vm_->evalCode(source_utf8.get_data(), filename.is_empty() ? kDefaultFilename : file_utf8.get_data(), "");

    if (vm_->getCurrentFiberState() == KorkApi::FiberRunResult::ERROR) {
        StringTableEntry file_name = nullptr;
        uint32_t line = 0;
        if (vm_->getCurrentFiberFileLine(&file_name, &line) && file_name != nullptr) {
            UtilityFunctions::push_error(vformat("korkscript runtime error at %s:%d", String(file_name), static_cast<int64_t>(line)));
        } else {
            UtilityFunctions::push_error("korkscript runtime error.");
        }
        vm_->clearCurrentFiberError();
        return Variant();
    }

    return variant_from_console_value(result);
}

void KorkScriptVMNode::log_callback(uint32_t level, const char *console_line, void *user_ptr) {
    KorkScriptVMNode *self = static_cast<KorkScriptVMNode *>(user_ptr);
    if (self == nullptr) {
        return;
    }

    const String line = String(console_line ? console_line : "");
    self->emit_signal("console_output", static_cast<int64_t>(level), line);
    UtilityFunctions::print(level_prefix(level), " ", line);
}

KorkApi::ConsoleValue KorkScriptVMNode::godot_call_callback(void *, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]) {
    return static_cast<KorkScriptVMNode *>(user_ptr)->bridge_call(argc, argv);
}

KorkApi::ConsoleValue KorkScriptVMNode::godot_get_callback(void *, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]) {
    return static_cast<KorkScriptVMNode *>(user_ptr)->bridge_get(argc, argv);
}

KorkApi::ConsoleValue KorkScriptVMNode::godot_set_callback(void *, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]) {
    return static_cast<KorkScriptVMNode *>(user_ptr)->bridge_set(argc, argv);
}

KorkApi::ConsoleValue KorkScriptVMNode::godot_print_callback(void *, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]) {
    return static_cast<KorkScriptVMNode *>(user_ptr)->bridge_print(argc, argv);
}

KorkApi::ConsoleValue KorkScriptVMNode::bridge_call(int32_t argc, KorkApi::ConsoleValue argv[]) {
    if (argc < 3) {
        UtilityFunctions::push_error("godot_call requires at least a target path and method name.");
        return KorkApi::ConsoleValue();
    }

    Object *target = resolve_target(console_value_to_string(argv[1]));
    if (target == nullptr) {
        UtilityFunctions::push_error("godot_call target could not be resolved.");
        return KorkApi::ConsoleValue();
    }

    Array args;
    for (int32_t i = 3; i < argc; ++i) {
        args.push_back(parse_script_argument(argv[i]));
    }

    Variant result = target->callv(StringName(console_value_to_string(argv[2])), args);
    return console_value_from_variant(result);
}

KorkApi::ConsoleValue KorkScriptVMNode::bridge_get(int32_t argc, KorkApi::ConsoleValue argv[]) {
    if (argc != 3) {
        UtilityFunctions::push_error("godot_get requires a target path and property name.");
        return KorkApi::ConsoleValue();
    }

    Object *target = resolve_target(console_value_to_string(argv[1]));
    if (target == nullptr) {
        UtilityFunctions::push_error("godot_get target could not be resolved.");
        return KorkApi::ConsoleValue();
    }

    return console_value_from_variant(target->get(StringName(console_value_to_string(argv[2]))));
}

KorkApi::ConsoleValue KorkScriptVMNode::bridge_set(int32_t argc, KorkApi::ConsoleValue argv[]) {
    if (argc != 4) {
        UtilityFunctions::push_error("godot_set requires a target path, property name, and value.");
        return KorkApi::ConsoleValue::makeUnsigned(0);
    }

    Object *target = resolve_target(console_value_to_string(argv[1]));
    if (target == nullptr) {
        UtilityFunctions::push_error("godot_set target could not be resolved.");
        return KorkApi::ConsoleValue::makeUnsigned(0);
    }

    target->set(StringName(console_value_to_string(argv[2])), parse_script_argument(argv[3]));
    return KorkApi::ConsoleValue::makeUnsigned(1);
}

KorkApi::ConsoleValue KorkScriptVMNode::bridge_print(int32_t argc, KorkApi::ConsoleValue argv[]) {
    PackedStringArray parts;
    for (int32_t i = 1; i < argc; ++i) {
        parts.push_back(console_value_to_string(argv[i]));
    }

    const String line = String(" ").join(parts);
    emit_signal("console_output", static_cast<int64_t>(0), line);
    UtilityFunctions::print("[korkscript] ", line);
    return KorkApi::ConsoleValue::makeString("");
}

Object *KorkScriptVMNode::resolve_target(const String &path) const {
    if (path.is_empty() || path == "." || path == "self") {
        return const_cast<KorkScriptVMNode *>(this);
    }
    if (path == "/root") {
        SceneTree *tree = get_tree();
        Window *root = tree ? tree->get_root() : nullptr;
        return root;
    }

    Node *node = get_node_or_null(NodePath(path));
    if (node != nullptr) {
        return node;
    }

    SceneTree *tree = get_tree();
    if (tree != nullptr) {
        Window *root = tree->get_root();
        return root ? root->get_node_or_null(NodePath(path)) : nullptr;
    }
    return nullptr;
}

Variant KorkScriptVMNode::variant_from_console_value(KorkApi::ConsoleValue value) const {
    if (value.isFloat()) {
        return value.getFloat();
    }
    if (value.isUnsigned()) {
        return static_cast<int64_t>(value.getInt());
    }
    return String(vm_ ? vm_->valueAsString(value) : "");
}

KorkApi::ConsoleValue KorkScriptVMNode::console_value_from_variant(const Variant &value) {
    if (vm_ == nullptr) {
        return KorkApi::ConsoleValue();
    }

    switch (value.get_type()) {
        case Variant::BOOL:
            return KorkApi::ConsoleValue::makeUnsigned(static_cast<bool>(value) ? 1 : 0);
        case Variant::INT:
            return KorkApi::ConsoleValue::makeUnsigned(static_cast<uint64_t>(static_cast<int64_t>(value)));
        case Variant::FLOAT:
            return KorkApi::ConsoleValue::makeNumber(static_cast<double>(value));
        case Variant::STRING: {
            const CharString utf8 = static_cast<String>(value).utf8();
            KorkApi::ConsoleValue buffer = vm_->getStringReturnBuffer(static_cast<uint32_t>(utf8.length() + 1));
            char *out = static_cast<char *>(buffer.evaluatePtr(vm_->getAllocBase()));
            if (out != nullptr) {
                std::memcpy(out, utf8.get_data(), static_cast<size_t>(utf8.length() + 1));
            }
            return buffer;
        }
        case Variant::STRING_NAME: {
            const CharString utf8 = String(static_cast<StringName>(value)).utf8();
            KorkApi::ConsoleValue buffer = vm_->getStringReturnBuffer(static_cast<uint32_t>(utf8.length() + 1));
            char *out = static_cast<char *>(buffer.evaluatePtr(vm_->getAllocBase()));
            if (out != nullptr) {
                std::memcpy(out, utf8.get_data(), static_cast<size_t>(utf8.length() + 1));
            }
            return buffer;
        }
        case Variant::NODE_PATH: {
            const CharString utf8 = String(static_cast<NodePath>(value)).utf8();
            KorkApi::ConsoleValue buffer = vm_->getStringReturnBuffer(static_cast<uint32_t>(utf8.length() + 1));
            char *out = static_cast<char *>(buffer.evaluatePtr(vm_->getAllocBase()));
            if (out != nullptr) {
                std::memcpy(out, utf8.get_data(), static_cast<size_t>(utf8.length() + 1));
            }
            return buffer;
        }
        case Variant::NIL:
            return KorkApi::ConsoleValue();
        default: {
            const CharString utf8 = value.stringify().utf8();
            KorkApi::ConsoleValue buffer = vm_->getStringReturnBuffer(static_cast<uint32_t>(utf8.length() + 1));
            char *out = static_cast<char *>(buffer.evaluatePtr(vm_->getAllocBase()));
            if (out != nullptr) {
                std::memcpy(out, utf8.get_data(), static_cast<size_t>(utf8.length() + 1));
            }
            return buffer;
        }
    }
}

Variant KorkScriptVMNode::parse_script_argument(KorkApi::ConsoleValue value) const {
    if (value.isFloat()) {
        return value.getFloat();
    }
    if (value.isUnsigned()) {
        return static_cast<int64_t>(value.getInt());
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
    if (text.begins_with("node:")) {
        return resolve_target(text.substr(5, text.length()));
    }
    if (text.is_valid_int()) {
        return text.to_int();
    }
    if (text.is_valid_float()) {
        return text.to_float();
    }
    return text;
}

String KorkScriptVMNode::console_value_to_string(KorkApi::ConsoleValue value) const {
    if (vm_ == nullptr) {
        return "";
    }
    const char *str = vm_->valueAsString(value);
    return String(str ? str : "");
}

} // namespace godot
