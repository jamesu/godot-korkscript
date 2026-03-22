#pragma once

#include "embed/api.h"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

class Object;

class KorkScriptVMNode : public Node {
    GDCLASS(KorkScriptVMNode, Node);

public:
    KorkScriptVMNode();
    ~KorkScriptVMNode() override;

    bool initialize_vm();
    void shutdown_vm();
    bool has_vm() const;

    Variant execute_script(const String &source, const String &filename = "res://inline.ks");

protected:
    static void _bind_methods();
    void _notification(int p_what);

private:
    static void log_callback(uint32_t level, const char *console_line, void *user_ptr);
    static KorkApi::ConsoleValue godot_call_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);
    static KorkApi::ConsoleValue godot_get_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);
    static KorkApi::ConsoleValue godot_set_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);
    static KorkApi::ConsoleValue godot_print_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]);

    KorkApi::ConsoleValue bridge_call(int32_t argc, KorkApi::ConsoleValue argv[]);
    KorkApi::ConsoleValue bridge_get(int32_t argc, KorkApi::ConsoleValue argv[]);
    KorkApi::ConsoleValue bridge_set(int32_t argc, KorkApi::ConsoleValue argv[]);
    KorkApi::ConsoleValue bridge_print(int32_t argc, KorkApi::ConsoleValue argv[]);

    Object *resolve_target(const String &path) const;
    Variant variant_from_console_value(KorkApi::ConsoleValue value) const;
    KorkApi::ConsoleValue console_value_from_variant(const Variant &value);
    Variant parse_script_argument(KorkApi::ConsoleValue value) const;
    String console_value_to_string(KorkApi::ConsoleValue value) const;

    KorkApi::Vm *vm_;
};

} // namespace godot
