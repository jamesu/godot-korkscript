#include "KorkScriptVMHost.h"

#include "KorkScript.h"
#include "embed/compilerOpcodes.h"
#include "ext/korkscript/engine/console/ast.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <cstdio>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace godot {

namespace {

KorkApi::FieldInfo g_empty_field_info{};

std::string make_node_path_key(Node *node) {
    if (node == nullptr || !node->is_inside_tree()) {
        return std::string();
    }

    const CharString utf8 = String(node->get_path()).utf8();
    return std::string(utf8.get_data(), static_cast<size_t>(utf8.length()));
}

bool is_numeric_lookup(const String &value) {
    if (value.is_empty()) {
        return false;
    }
    for (int i = 0; i < value.length(); ++i) {
        const char32_t c = value[i];
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

PackedStringArray parse_usage_argument_names(const char *usage) {
    PackedStringArray out;
    String usage_text = String(usage ? usage : "").strip_edges();
    if (!usage_text.begins_with("(") || !usage_text.ends_with(")")) {
        return out;
    }

    usage_text = usage_text.substr(1, usage_text.length() - 2).strip_edges();
    if (usage_text.is_empty()) {
        return out;
    }

    PackedStringArray parts = usage_text.split(",", false);
    for (int i = 0; i < parts.size(); ++i) {
        String arg_name = parts[i].strip_edges();
        if (arg_name.begins_with("...")) {
            arg_name = arg_name.substr(3).strip_edges();
        }
        if (!arg_name.is_empty()) {
            out.push_back(arg_name);
        }
    }

    return out;
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

String format_component(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.9g", value);
    return String(buffer);
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

bool cast_object_reference_type(void *,
        KorkApi::Vm *vm,
        KorkApi::TypeStorageInterface *input_storage,
        KorkApi::TypeStorageInterface *output_storage,
        void *,
        BitSet32,
        U32 requested_type) {
    if (vm == nullptr || input_storage == nullptr || output_storage == nullptr) {
        return false;
    }

    KorkApi::ConsoleValue input = input_storage->data.storageAddress;
    if (input_storage->data.storageRegister != nullptr) {
        input = *input_storage->data.storageRegister;
    }
    const U64 object_ref = static_cast<U64>(vm->valueAsInt(input));

    if (requested_type == KorkApi::ConsoleValue::TypeInternalUnsigned) {
        if (output_storage->data.storageRegister != nullptr) {
            *output_storage->data.storageRegister = KorkApi::ConsoleValue::makeUnsigned(object_ref);
            return true;
        }
        return false;
    }

    if (requested_type == KorkApi::ConsoleValue::TypeInternalNumber) {
        if (output_storage->data.storageRegister != nullptr) {
            *output_storage->data.storageRegister = KorkApi::ConsoleValue::makeNumber(static_cast<F64>(object_ref));
            return true;
        }
        return false;
    }

    if (requested_type == KorkApi::ConsoleValue::TypeInternalString) {
        const String text = String::num_uint64(object_ref);
        const CharString utf8 = text.utf8();
        output_storage->ResizeStorage(output_storage, static_cast<U32>(utf8.length() + 1));
        char *dst = static_cast<char *>(output_storage->data.storageAddress.evaluatePtr(vm->getAllocBase()));
        if (dst == nullptr) {
            return false;
        }
        memcpy(dst, utf8.get_data(), static_cast<size_t>(utf8.length() + 1));
        if (output_storage->data.storageRegister != nullptr) {
            *output_storage->data.storageRegister = output_storage->data.storageAddress;
        }
        return true;
    }

    if (requested_type >= KorkApi::ConsoleValue::TypeBeginCustom) {
        if (output_storage->data.storageRegister != nullptr) {
            *output_storage->data.storageRegister = KorkApi::ConsoleValue::makeRaw(object_ref, static_cast<U16>(requested_type), KorkApi::ConsoleValue::ZoneExternal);
        }

        void *dst_ptr = output_storage->data.storageAddress.evaluatePtr(vm->getAllocBase());
        if (dst_ptr != nullptr) {
            *static_cast<U64 *>(dst_ptr) = object_ref;
        }
        return true;
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

template <typename TVec, int Components, typename ReadFn, typename WriteFn>
KorkApi::ConsoleValue perform_pod_math_op_impl(void *user_ptr,
        KorkApi::Vm *vm,
        U32 op,
        KorkApi::ConsoleValue lhs,
        KorkApi::ConsoleValue rhs,
        ReadFn read_fn,
        WriteFn write_fn) {
    const U32 self_type_id = *static_cast<const U32 *>(user_ptr);
    TVec *lhs_value = lhs.typeId == self_type_id ? static_cast<TVec *>(lhs.evaluatePtr(vm->getAllocBase())) : nullptr;
    TVec *rhs_value = rhs.typeId == self_type_id ? static_cast<TVec *>(rhs.evaluatePtr(vm->getAllocBase())) : nullptr;

    KorkApi::ConsoleValue out_value;
    TVec *out_ptr = nullptr;
    double lhs_components[4] = {};
    double rhs_components[4] = {};

    if (lhs_value != nullptr) {
        read_fn(*lhs_value, lhs_components);
        out_value = lhs;
        out_ptr = lhs_value;
    } else if (rhs_value != nullptr) {
        const double scalar = vm->valueAsFloat(lhs);
        for (int i = 0; i < Components; ++i) {
            lhs_components[i] = scalar;
        }
        out_value = rhs;
        out_ptr = rhs_value;
    } else {
        return KorkApi::ConsoleValue();
    }

    if (rhs_value != nullptr) {
        read_fn(*rhs_value, rhs_components);
    } else {
        const double scalar = vm->valueAsFloat(rhs);
        for (int i = 0; i < Components; ++i) {
            rhs_components[i] = scalar;
        }
    }

    double out_components[4] = {};
    switch (op) {
        case Compiler::OP_ADD:
            for (int i = 0; i < Components; ++i) {
                out_components[i] = lhs_components[i] + rhs_components[i];
            }
            break;
        case Compiler::OP_SUB:
            for (int i = 0; i < Components; ++i) {
                out_components[i] = lhs_components[i] - rhs_components[i];
            }
            break;
        case Compiler::OP_MUL:
            for (int i = 0; i < Components; ++i) {
                out_components[i] = lhs_components[i] * rhs_components[i];
            }
            break;
        case Compiler::OP_DIV:
            for (int i = 0; i < Components; ++i) {
                out_components[i] = rhs_components[i] == 0.0 ? 0.0 : (lhs_components[i] / rhs_components[i]);
            }
            break;
        case Compiler::OP_NEG:
            for (int i = 0; i < Components; ++i) {
                out_components[i] = -lhs_components[i];
            }
            break;
        case Compiler::OP_CMPEQ:
        {
            bool equal = true;
            for (int i = 0; i < Components; ++i) {
                if (lhs_components[i] != rhs_components[i]) {
                    equal = false;
                    break;
                }
            }
            return KorkApi::ConsoleValue::makeUnsigned(equal ? 1 : 0);
        }
        case Compiler::OP_CMPNE:
        {
            bool not_equal = false;
            for (int i = 0; i < Components; ++i) {
                if (lhs_components[i] != rhs_components[i]) {
                    not_equal = true;
                    break;
                }
            }
            return KorkApi::ConsoleValue::makeUnsigned(not_equal ? 1 : 0);
        }
        default:
            return out_value;
    }

    if (out_ptr == nullptr) {
        return KorkApi::ConsoleValue();
    }

    write_fn(*out_ptr, out_components);
    return out_value;
}

KorkApi::ConsoleValue vector2_perform_op(void *user_ptr, KorkApi::Vm *vm, U32 op, KorkApi::ConsoleValue lhs, KorkApi::ConsoleValue rhs) {
    return perform_pod_math_op_impl<Vector2, 2>(
            user_ptr, vm, op, lhs, rhs,
            [](const Vector2 &value, double *out_values) {
                out_values[0] = value.x;
                out_values[1] = value.y;
            },
            [](Vector2 &value, const double *in_values) {
                value.x = static_cast<real_t>(in_values[0]);
                value.y = static_cast<real_t>(in_values[1]);
            });
}

KorkApi::ConsoleValue vector3_perform_op(void *user_ptr, KorkApi::Vm *vm, U32 op, KorkApi::ConsoleValue lhs, KorkApi::ConsoleValue rhs) {
    return perform_pod_math_op_impl<Vector3, 3>(
            user_ptr, vm, op, lhs, rhs,
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

KorkApi::ConsoleValue vector4_perform_op(void *user_ptr, KorkApi::Vm *vm, U32 op, KorkApi::ConsoleValue lhs, KorkApi::ConsoleValue rhs) {
    return perform_pod_math_op_impl<Vector4, 4>(
            user_ptr, vm, op, lhs, rhs,
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

KorkApi::ConsoleValue color_perform_op(void *user_ptr, KorkApi::Vm *vm, U32 op, KorkApi::ConsoleValue lhs, KorkApi::ConsoleValue rhs) {
    return perform_pod_math_op_impl<Color, 4>(
            user_ptr, vm, op, lhs, rhs,
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

Variant::Type variant_type_from_kork_type_id(U32 type_id, U32 vector2_type_id, U32 vector3_type_id, U32 vector4_type_id, U32 color_type_id) {
    if (type_id == KorkApi::ConsoleValue::TypeInternalString) {
        return Variant::STRING;
    }
    if (type_id == KorkApi::ConsoleValue::TypeInternalUnsigned) {
        return Variant::INT;
    }
    if (type_id == KorkApi::ConsoleValue::TypeInternalNumber) {
        return Variant::FLOAT;
    }
    if (type_id == vector2_type_id) {
        return Variant::VECTOR2;
    }
    if (type_id == vector3_type_id) {
        return Variant::VECTOR3;
    }
    if (type_id == vector4_type_id) {
        return Variant::VECTOR4;
    }
    if (type_id == color_type_id) {
        return Variant::COLOR;
    }
    return Variant::NIL;
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
        next_sim_id_(1),
        generation_(1),
        reload_pending_(false),
        script_instance_field_write_depth_(0),
        script_instance_field_read_depth_(0) {
    initialize_vm();
}

void KorkScriptVMHost::initialize_vm() {
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
    cfg.enableSignals = true;
    cfg.enableScriptClasses = true;
    cfg.maxFibers = 128;
    cfg.defaultScriptClass = "GodotObject";

    vm_ = KorkApi::createVM(&cfg);
    KorkApi::TypeInfo vector2_info{};
    vector2_info.name = vm_->internString("Vector2");
    vector2_info.userPtr = &vector2_type_id_;
    vector2_info.fieldSize = sizeof(Vector2);
    vector2_info.valueSize = sizeof(Vector2);
    vector2_info.iFuncs = { &vector2_cast_value, &vector2_type_name, nullptr, &vector2_perform_op };
    vector2_type_id_ = vm_->registerType(vector2_info);

    KorkApi::TypeInfo vector3_info{};
    vector3_info.name = vm_->internString("Vector3");
    vector3_info.userPtr = &vector3_type_id_;
    vector3_info.fieldSize = sizeof(Vector3);
    vector3_info.valueSize = sizeof(Vector3);
    vector3_info.iFuncs = { &vector3_cast_value, &vector3_type_name, nullptr, &vector3_perform_op };
    vector3_type_id_ = vm_->registerType(vector3_info);

    KorkApi::TypeInfo vector4_info{};
    vector4_info.name = vm_->internString("Vector4");
    vector4_info.userPtr = &vector4_type_id_;
    vector4_info.fieldSize = sizeof(Vector4);
    vector4_info.valueSize = sizeof(Vector4);
    vector4_info.iFuncs = { &vector4_cast_value, &vector4_type_name, nullptr, &vector4_perform_op };
    vector4_type_id_ = vm_->registerType(vector4_info);

    KorkApi::TypeInfo color_info{};
    color_info.name = vm_->internString("Color");
    color_info.userPtr = &color_type_id_;
    color_info.fieldSize = sizeof(Color);
    color_info.valueSize = sizeof(Color);
    color_info.iFuncs = { &color_cast_value, &color_type_name, nullptr, &color_perform_op };
    color_type_id_ = vm_->registerType(color_info);

    ensure_global_math_namespace();
    ensure_object_bridge_namespace();

    KorkApi::ClassInfo class_info{};
    class_info.name = vm_->internString("GodotObject");
    class_info.userPtr = this;
    class_info.numFields = 0;
    class_info.fields = &g_empty_field_info;
    class_info.iCreate = {
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &KorkScriptVMHost::get_vm_object_id,
        &KorkScriptVMHost::get_vm_object_name
    };
    class_info.iSignals = {
        [](void *, KorkApi::VMObject *object, StringTableEntry signal_name, int argc, KorkApi::ConsoleValue *argv) {
            if (object == nullptr || object->klass == nullptr || object->klass->userPtr == nullptr) {
                return;
            }

            KorkScriptVMHost *self = static_cast<KorkScriptVMHost *>(object->klass->userPtr);
            self->trigger_godot_signal(static_cast<Object *>(object->userPtr), signal_name, argc, argv);
        }
    };
    class_info.iCustomFields = {
        &KorkScriptVMHost::custom_field_iterate_callback,
        &KorkScriptVMHost::custom_field_get_by_iterator_callback,
        &KorkScriptVMHost::custom_field_get_by_name_callback,
        &KorkScriptVMHost::custom_field_set_by_name_callback,
        &KorkScriptVMHost::custom_field_set_type_callback
    };
    godot_object_class_id_ = vm_->registerClass(class_info);
}

KorkScriptVMHost::~KorkScriptVMHost() {
    if (vm_ != nullptr) {
        KorkApi::destroyVM(vm_);
        vm_ = nullptr;
    }
    for (KorkApi::Vm *retired_vm : retired_vms_) {
        if (retired_vm != nullptr) {
            KorkApi::destroyVM(retired_vm);
        }
    }
    retired_vms_.clear();
}

KorkApi::Vm *KorkScriptVMHost::get_vm() const {
    return vm_;
}

const String &KorkScriptVMHost::get_vm_name() const {
    return vm_name_;
}

uint64_t KorkScriptVMHost::get_generation() const {
    return generation_;
}

KorkApi::AstEnumerationResult KorkScriptVMHost::enumerate_ast(const String &code, const String &filename, void *user_ptr, KorkApi::AstEnumerationCallback callback, KorkApi::AstParseErrorInfo *out_error) const {
    if (vm_ == nullptr || callback == nullptr) {
        return KorkApi::AstEnumerationParseFailed;
    }

    const CharString code_utf8 = code.utf8();
    const CharString filename_utf8 = filename.utf8();
    return vm_->enumerateAst(code_utf8.get_data(), filename_utf8.get_data(), user_ptr, callback, out_error);
}

KorkApi::Vm *KorkScriptVMHost::get_vm_for_generation(uint64_t generation) const {
    if (generation == generation_) {
        return vm_;
    }
    if (generation == 0 || generation > retired_vms_.size()) {
        return nullptr;
    }
    return retired_vms_[static_cast<size_t>(generation - 1)];
}

void KorkScriptVMHost::reset_vm() {
    if (vm_ != nullptr) {
        retired_vms_.push_back(vm_);
        vm_ = nullptr;
    }

    godot_class_ids_by_name_.clear();
    godot_type_ids_by_name_.clear();
    vm_objects_by_owner_id_.clear();
    vm_objects_by_id_.clear();
    vm_objects_by_name_.clear();
    vm_objects_by_path_.clear();
    loaded_scripts_.clear();
    ++generation_;
    initialize_vm();
}

bool KorkScriptVMHost::eval_script_source(const KorkScript *script) {
    if (script == nullptr || vm_ == nullptr) {
        return false;
    }

    known_scripts_[reinterpret_cast<uint64_t>(script)] = Ref<KorkScript>(const_cast<KorkScript *>(script));

    const String base_type = script->get_base_type();
    if (!base_type.is_empty()) {
        ensure_type_for_godot_type(base_type);
        ensure_class_for_godot_type(base_type);
    }

    const String source_code = script->get_source_code_ref();
    const String declared_parent = script->get_declared_script_class_parent_name().strip_edges();
    if (!declared_parent.is_empty()) {
        ensure_type_for_godot_type(declared_parent);
        ensure_class_for_godot_type(declared_parent);
    }
    {
        std::unordered_set<std::string> referenced_type_names;
        const CharString source_utf8_for_ast = source_code.utf8();
        constexpr const char *kAstSourceName = "[KorkScriptTypeScan]";
        const KorkApi::AstEnumerationResult ast_result = vm_->enumerateAst(
                source_utf8_for_ast.get_data(),
                kAstSourceName,
                &referenced_type_names,
                &collect_referenced_type_names,
                nullptr);
        if (ast_result == KorkApi::AstEnumerationCompleted) {
            for (const std::string &type_name : referenced_type_names) {
                ensure_type_for_godot_type(StringName(type_name.c_str()));
                ensure_class_for_godot_type(StringName(type_name.c_str()));
            }
        }
    }

    const CharString source_utf8 = source_code.utf8();
    String filename = script->get_effective_namespace_name().strip_edges();
    if (filename.is_empty()) {
        filename = String("KorkScript");
    }
    filename = vformat("res://%s_%s.ks", vm_name_, filename);
    const CharString path_utf8 = filename.utf8();

    vm_->evalCode(source_utf8.get_data(), path_utf8.get_data(), "");
    if (vm_->getCurrentFiberState() == KorkApi::FiberRunResult::ERROR) {
        UtilityFunctions::push_error(vformat("Failed to load korkscript source into VM '%s'.", vm_name_));
        vm_->clearCurrentFiberError();
        return false;
    }

    loaded_scripts_[reinterpret_cast<uint64_t>(script)].revision = script->get_revision();
    return true;
}

bool KorkScriptVMHost::reload_known_scripts(const KorkScript *extra_script) {
    std::vector<Ref<KorkScript>> pending;
    pending.reserve(known_scripts_.size() + (extra_script != nullptr ? 1 : 0));

    for (const auto &entry : known_scripts_) {
        if (entry.second.is_valid()) {
            pending.push_back(entry.second);
        }
    }

    if (extra_script != nullptr) {
        const uint64_t key = reinterpret_cast<uint64_t>(extra_script);
        if (known_scripts_.find(key) == known_scripts_.end()) {
            pending.push_back(Ref<KorkScript>(const_cast<KorkScript *>(extra_script)));
        }
    }

    while (!pending.empty()) {
        bool progress = false;
        for (size_t i = 0; i < pending.size();) {
            const Ref<KorkScript> &script_ref = pending[i];
            if (script_ref.is_null()) {
                pending.erase(pending.begin() + static_cast<int64_t>(i));
                continue;
            }

            if (eval_script_source(script_ref.ptr())) {
                pending.erase(pending.begin() + static_cast<int64_t>(i));
                progress = true;
                continue;
            }

            ++i;
        }

        if (!pending.empty() && !progress) {
            // Force one last failing evaluation to keep the most relevant error in the VM log.
            eval_script_source(pending.front().ptr());
            return false;
        }
    }

    return true;
}

void KorkScriptVMHost::dedupe_known_scripts_for_path(const KorkScript *script) {
    if (script == nullptr) {
        return;
    }

    const String path = script->get_path();
    if (path.is_empty()) {
        return;
    }

    const uint64_t keep_key = reinterpret_cast<uint64_t>(script);
    for (auto it = known_scripts_.begin(); it != known_scripts_.end();) {
        if (it->first == keep_key) {
            ++it;
            continue;
        }

        auto active = active_scripts_.find(it->first);
        if (active != active_scripts_.end() && active->second.ref_count > 0) {
            ++it;
            continue;
        }

        const Ref<KorkScript> &candidate = it->second;
        if (candidate.is_null() || candidate->get_path() != path) {
            ++it;
            continue;
        }

        loaded_scripts_.erase(it->first);
        it = known_scripts_.erase(it);
    }
}

void KorkScriptVMHost::prune_script_caches(const KorkScript *preserve_script) {
    std::unordered_set<uint64_t> keep_keys;
    keep_keys.reserve(active_scripts_.size() + (preserve_script != nullptr ? 1 : 0));
    for (const auto &entry : active_scripts_) {
        if (entry.second.ref_count > 0) {
            keep_keys.insert(entry.first);
        }
    }
    if (preserve_script != nullptr) {
        keep_keys.insert(reinterpret_cast<uint64_t>(preserve_script));
    }

    for (auto it = known_scripts_.begin(); it != known_scripts_.end();) {
        if (keep_keys.find(it->first) == keep_keys.end()) {
            loaded_scripts_.erase(it->first);
            it = known_scripts_.erase(it);
            continue;
        }
        ++it;
    }

    for (auto it = loaded_scripts_.begin(); it != loaded_scripts_.end();) {
        if (keep_keys.find(it->first) == keep_keys.end()) {
            it = loaded_scripts_.erase(it);
            continue;
        }
        ++it;
    }
}

bool KorkScriptVMHost::ensure_script_loaded(const KorkScript *script) {
    if (script == nullptr || vm_ == nullptr) {
        return false;
    }

    const uint64_t key = reinterpret_cast<uint64_t>(script);
    dedupe_known_scripts_for_path(script);
    if (known_scripts_.size() > 4096) {
        prune_script_caches(script);
    }
    known_scripts_[key] = Ref<KorkScript>(const_cast<KorkScript *>(script));

    if (reload_pending_) {
        reset_vm();
        reload_pending_ = false;
        return reload_known_scripts(script);
    }

    ScriptLoadState &state = loaded_scripts_[key];
    if (state.revision == script->get_revision()) {
        return true;
    }

    if (state.revision != 0) {
        reset_vm();
        return reload_known_scripts(script);
    }

    return eval_script_source(script);
}

void KorkScriptVMHost::notify_script_changed(const KorkScript *script) {
    if (script == nullptr || vm_ == nullptr) {
        return;
    }

    dedupe_known_scripts_for_path(script);
    if (known_scripts_.size() > 4096) {
        prune_script_caches(script);
    }
    known_scripts_[reinterpret_cast<uint64_t>(script)] = Ref<KorkScript>(const_cast<KorkScript *>(script));
    reload_pending_ = true;
}

void KorkScriptVMHost::retain_script(const KorkScript *script) {
    if (script == nullptr) {
        return;
    }

    ActiveScriptState &state = active_scripts_[reinterpret_cast<uint64_t>(script)];
    state.script = Ref<KorkScript>(const_cast<KorkScript *>(script));
    ++state.ref_count;
}

void KorkScriptVMHost::release_script(const KorkScript *script) {
    if (script == nullptr) {
        return;
    }

    const uint64_t key = reinterpret_cast<uint64_t>(script);
    auto found = active_scripts_.find(key);
    if (found == active_scripts_.end()) {
        return;
    }

    if (found->second.ref_count > 1) {
        --found->second.ref_count;
        return;
    }

    active_scripts_.erase(found);
    known_scripts_.erase(key);
    loaded_scripts_.erase(key);
}

KorkApi::NamespaceId KorkScriptVMHost::resolve_object_namespace(Object *owner, const KorkScript *script) {
    if (vm_ == nullptr || owner == nullptr) {
        return nullptr;
    }

    KorkApi::NamespaceId ns = nullptr;
    resolve_vm_object_class(owner, script, ns);
    return ns != nullptr ? ns : ensure_namespace_for_class(owner->get_class());
}

const KorkScript *KorkScriptVMHost::get_attached_korkscript(Object *owner) const {
    if (owner == nullptr || !owner->has_method(StringName("get_script"))) {
        return nullptr;
    }

    const Variant script_value = owner->call(StringName("get_script"));
    if (script_value.get_type() != Variant::OBJECT) {
        return nullptr;
    }

    Ref<KorkScript> script = script_value;
    return script.is_valid() ? script.ptr() : nullptr;
}

String KorkScriptVMHost::get_object_name(Object *owner) const {
    if (owner == nullptr) {
        return String();
    }

    Node *node = Object::cast_to<Node>(owner);
    if (node != nullptr) {
        return String(node->get_name());
    }

    return vformat("%s_%d", owner->get_class(), static_cast<int64_t>(ensure_sim_object_id(owner)));
}

Object *KorkScriptVMHost::resolve_object_reference(Object *context_owner, const String &query) const {
    String trimmed = query.strip_edges();
    if (trimmed.is_empty()) {
        return nullptr;
    }

    if (trimmed == "." || trimmed == "self") {
        return context_owner;
    }

    if (is_numeric_lookup(trimmed)) {
        const KorkApi::SimObjectId sim_id = static_cast<KorkApi::SimObjectId>(trimmed.to_int());
        auto found = vm_objects_by_id_.find(sim_id);
        if (found != vm_objects_by_id_.end() && found->second != nullptr) {
            return static_cast<Object *>(found->second->userPtr);
        }

        return UtilityFunctions::instance_from_id(trimmed.to_int());
    }

    Node *context_node = Object::cast_to<Node>(context_owner);
    if (context_node != nullptr) {
        Node *relative = context_node->get_node_or_null(NodePath(trimmed));
        if (relative != nullptr) {
            return relative;
        }

        if (trimmed.find("/") < 0) {
            Node *descendant = context_node->find_child(trimmed, true, false);
            if (descendant != nullptr) {
                return descendant;
            }
        }
    }

    SceneTree *tree = context_node != nullptr ? context_node->get_tree() : nullptr;
    Window *root = tree != nullptr ? tree->get_root() : nullptr;
    if (root == nullptr && !vm_objects_by_owner_id_.empty()) {
        for (const auto &entry : vm_objects_by_owner_id_) {
            Object *candidate_owner = entry.second != nullptr ? static_cast<Object *>(entry.second->userPtr) : nullptr;
            Node *candidate_node = Object::cast_to<Node>(candidate_owner);
            if (candidate_node != nullptr && candidate_node->get_tree() != nullptr) {
                root = candidate_node->get_tree()->get_root();
                break;
            }
        }
    }

    if (root != nullptr) {
        Node *absolute = nullptr;
        if (trimmed.begins_with("/")) {
            absolute = root->get_node_or_null(NodePath(trimmed));
        } else {
            absolute = root->get_node_or_null(NodePath(trimmed));
            if (absolute == nullptr) {
                absolute = root->find_child(trimmed, true, false);
            }
        }

        if (absolute != nullptr) {
            return absolute;
        }
    }

    auto by_name = vm_objects_by_name_.find(intern_utf8(trimmed));
    if (by_name != vm_objects_by_name_.end() && by_name->second != nullptr) {
        return static_cast<Object *>(by_name->second->userPtr);
    }

    return nullptr;
}

KorkApi::VMObject *KorkScriptVMHost::get_or_create_vm_object(Object *owner, const KorkScript *script) {
    if (owner == nullptr || vm_ == nullptr) {
        return nullptr;
    }

    KorkApi::NamespaceId desired_namespace = nullptr;
    const KorkApi::ClassId desired_class_id = resolve_vm_object_class(owner, script != nullptr ? script : get_attached_korkscript(owner), desired_namespace);
    if (desired_class_id < 0 || desired_namespace == nullptr) {
        return nullptr;
    }

    const uint64_t owner_id = owner->get_instance_id();
    auto found = vm_objects_by_owner_id_.find(owner_id);
    if (found != vm_objects_by_owner_id_.end() && found->second != nullptr) {
        const bool class_mismatch = found->second->klass == nullptr || found->second->klass->name != vm_->getClassName(desired_class_id);
        if (!class_mismatch) {
            found->second->flags |= KorkApi::ModDynamicFields;
            vm_->setObjectNamespace(found->second, desired_namespace);
            const bool has_ctor = desired_namespace != nullptr &&
                    vm_->isNamespaceFunction(desired_namespace, vm_->internString("__kork_ctor"));
            if (has_ctor && (found->second->flags & KorkApi::ModScriptCtorInvoked) == 0) {
                vm_->invokeScriptClassConstructor(found->second);
            } else if (!has_ctor) {
                found->second->flags |= KorkApi::ModScriptCtorInvoked;
            }
            return found->second;
        }

        unregister_vm_object(owner, found->second);
        vm_->decVMRef(found->second);
    }

    const KorkApi::SimObjectId sim_id = ensure_sim_object_id(owner);
    KorkApi::VMObject *vm_object = vm_->createVMObject(desired_class_id, owner);
    vm_object->flags |= KorkApi::ModDynamicFields;
    vm_->setObjectNamespace(vm_object, desired_namespace);
    const bool has_ctor = desired_namespace != nullptr &&
            vm_->isNamespaceFunction(desired_namespace, vm_->internString("__kork_ctor"));
    if (has_ctor && !vm_->invokeScriptClassConstructor(vm_object)) {
        UtilityFunctions::push_error(vformat("Failed to invoke Kork script class constructor for '%s'.", String(vm_->getClassName(desired_class_id))));
        vm_->decVMRef(vm_object);
        return nullptr;
    } else if (!has_ctor) {
        vm_object->flags |= KorkApi::ModScriptCtorInvoked;
    }
    register_vm_object(owner, vm_object, sim_id);
    return vm_object;
}

KorkApi::VMObject *KorkScriptVMHost::create_vm_object_for(Object *owner, const KorkScript *script) {
    KorkApi::VMObject *vm_object = get_or_create_vm_object(owner, script);
    seed_script_class_defaults(owner, vm_object, script);
    if (vm_ != nullptr && vm_object != nullptr) {
        vm_->incVMRef(vm_object);
    }
    return vm_object;
}

void KorkScriptVMHost::destroy_vm_object_for(Object *owner, const KorkScript *script, KorkApi::VMObject *vm_object) {
    if (vm_ != nullptr && vm_object != nullptr) {
        vm_->decVMRef(vm_object);
    }
}

void KorkScriptVMHost::release_vm_object_for_generation(KorkApi::VMObject *vm_object, uint64_t generation) {
    if (vm_object == nullptr) {
        return;
    }

    KorkApi::Vm *vm = get_vm_for_generation(generation);
    if (vm != nullptr) {
        vm->decVMRef(vm_object);
    }
}

bool KorkScriptVMHost::call_method(KorkApi::VMObject *vm_object, const StringName &method, const Variant **args, GDExtensionInt arg_count, Variant &ret) const {
    if (vm_ == nullptr || vm_object == nullptr) {
        return false;
    }

    const uint64_t execution_target = get_dynamic_field_owner_key(vm_object);
    push_execution_target(execution_target);

    std::vector<KorkApi::ConsoleValue> argv(static_cast<size_t>(arg_count) + 2);
    const std::string method_utf8 = intern_utf8(method);
    argv[0] = console_value_from_variant_for_call(String(method_utf8.c_str()));
    argv[1] = KorkApi::ConsoleValue::makeUnsigned(0);
    for (GDExtensionInt i = 0; i < arg_count; ++i) {
        argv[static_cast<size_t>(i) + 2] = console_value_from_variant_for_call(*args[i]);
    }

    KorkApi::ConsoleValue result;
    const bool ok = vm_->callObjectFunction(vm_object, vm_->internString(method_utf8.c_str()), static_cast<int>(argv.size()), argv.data(), result, false);
    pop_execution_target();
    if (ok) {
        ret = variant_from_console_value(result);
    }
    return ok;
}

bool KorkScriptVMHost::has_signal(KorkApi::VMObject *vm_object, const StringName &signal) const {
    if (vm_ == nullptr || vm_object == nullptr) {
        return false;
    }

    return vm_->isNamespaceSignal(vm_->getObjectNamespace(vm_object), vm_->internString(intern_utf8(signal).c_str()));
}

void KorkScriptVMHost::trigger_signal(KorkApi::VMObject *vm_object, const StringName &signal, const Variant **args, GDExtensionInt arg_count) const {
    if (vm_ == nullptr || vm_object == nullptr) {
        return;
    }

    std::vector<KorkApi::ConsoleValue> argv(static_cast<size_t>(arg_count));
    for (GDExtensionInt i = 0; i < arg_count; ++i) {
        argv[static_cast<size_t>(i)] = console_value_from_variant_for_call(*args[i]);
    }

    vm_object->klass->iSignals.TriggerSignal(nullptr, vm_object, vm_->internString(intern_utf8(signal).c_str()), static_cast<int>(argv.size()), argv.data());
}

KorkApi::NamespaceId KorkScriptVMHost::get_script_namespace(const KorkScript *script) const {
    if (vm_ == nullptr || script == nullptr) {
        return nullptr;
    }

    const String namespace_name = script->get_effective_namespace_name();
    if (namespace_name.is_empty()) {
        return nullptr;
    }

    return vm_->findNamespace(vm_->internString(intern_utf8(namespace_name).c_str()));
}

bool KorkScriptVMHost::has_script_method(const KorkScript *script, const StringName &method) {
    if (script == nullptr || !ensure_script_loaded(script)) {
        return false;
    }

    KorkApi::NamespaceId script_ns = get_script_namespace(script);
    if (script_ns == nullptr) {
        return false;
    }

    const std::string method_utf8 = intern_utf8(method);
    struct MethodCheckCapture {
        const std::string *target = nullptr;
        bool found = false;
    } capture{ &method_utf8, false };

    vm_->enumerateNamespaceEntries(script_ns, &capture, [](void *user_ptr, const KorkApi::NamespaceEntryInfo *info) {
        MethodCheckCapture *capture = static_cast<MethodCheckCapture *>(user_ptr);
        if (capture->found || info == nullptr || info->kind != KorkApi::NamespaceEntryFunction || info->name == nullptr) {
            return;
        }

        if (*capture->target == info->name) {
            capture->found = true;
        }
    });
    return capture.found;
}

Dictionary KorkScriptVMHost::get_script_method_info(const KorkScript *script, const StringName &method) {
    Dictionary out;
    if (script == nullptr || !ensure_script_loaded(script)) {
        return out;
    }

    KorkApi::NamespaceId script_ns = get_script_namespace(script);
    if (script_ns == nullptr) {
        return out;
    }

    const std::string method_utf8 = intern_utf8(method);
    struct MethodInfoCapture {
        const std::string *target = nullptr;
        Dictionary *result = nullptr;
        bool found = false;
    } capture{ &method_utf8, &out, false };

    vm_->enumerateNamespaceEntries(script_ns, &capture, [](void *user_ptr, const KorkApi::NamespaceEntryInfo *info) {
        MethodInfoCapture *capture = static_cast<MethodInfoCapture *>(user_ptr);
        if (capture->found || info == nullptr || info->kind != KorkApi::NamespaceEntryFunction || info->name == nullptr) {
            return;
        }

        if (*capture->target != info->name) {
            return;
        }

        MethodInfo method_info(StringName(info->name));
        const PackedStringArray arg_names = parse_usage_argument_names(info->usage);
        for (int i = 0; i < arg_names.size(); ++i) {
            method_info.arguments.push_back(PropertyInfo(Variant::NIL, arg_names[i]));
        }
        *capture->result = method_info.operator Dictionary();
        capture->found = true;
    });

    return out;
}

TypedArray<Dictionary> KorkScriptVMHost::get_script_method_list(const KorkScript *script) {
    TypedArray<Dictionary> out;
    if (script == nullptr || !ensure_script_loaded(script)) {
        return out;
    }

    KorkApi::NamespaceId script_ns = get_script_namespace(script);
    if (script_ns == nullptr) {
        return out;
    }

    struct MethodCapture {
        TypedArray<Dictionary> *methods = nullptr;
        std::unordered_set<std::string> seen;
    } capture{ &out, {} };

    vm_->enumerateNamespaceEntries(script_ns, &capture, [](void *user_ptr, const KorkApi::NamespaceEntryInfo *info) {
        if (info == nullptr || info->kind != KorkApi::NamespaceEntryFunction || info->name == nullptr) {
            return;
        }

        MethodCapture *capture = static_cast<MethodCapture *>(user_ptr);
        const std::string key(info->name);
        if (!capture->seen.insert(key).second) {
            return;
        }

        MethodInfo method_info(StringName(info->name));
        const PackedStringArray arg_names = parse_usage_argument_names(info->usage);
        for (int i = 0; i < arg_names.size(); ++i) {
            method_info.arguments.push_back(PropertyInfo(Variant::NIL, arg_names[i]));
        }
        capture->methods->push_back(method_info.operator Dictionary());
    });

    return out;
}

bool KorkScriptVMHost::has_script_signal(const KorkScript *script, const StringName &signal) {
    if (script == nullptr || !ensure_script_loaded(script)) {
        return false;
    }

    KorkApi::NamespaceId script_ns = get_script_namespace(script);
    return script_ns != nullptr && vm_->isNamespaceSignal(script_ns, vm_->internString(intern_utf8(signal).c_str()));
}

TypedArray<Dictionary> KorkScriptVMHost::get_script_signal_list(const KorkScript *script) {
    TypedArray<Dictionary> out;
    if (script == nullptr || !ensure_script_loaded(script)) {
        return out;
    }

    KorkApi::NamespaceId script_ns = get_script_namespace(script);
    if (script_ns == nullptr) {
        return out;
    }

    struct SignalCapture {
        TypedArray<Dictionary> *signals = nullptr;
        std::unordered_set<std::string> seen;
    } capture{ &out, {} };

    vm_->enumerateNamespaceEntries(script_ns, &capture, [](void *user_ptr, const KorkApi::NamespaceEntryInfo *info) {
        if (info == nullptr || info->kind != KorkApi::NamespaceEntrySignal || info->name == nullptr) {
            return;
        }

        SignalCapture *capture = static_cast<SignalCapture *>(user_ptr);
        const std::string key(info->name);
        if (!capture->seen.insert(key).second) {
            return;
        }

        Dictionary signal_info;
        signal_info["name"] = String(info->name);

        Array args;
        const PackedStringArray arg_names = parse_usage_argument_names(info->usage);
        for (int i = 0; i < arg_names.size(); ++i) {
            Dictionary arg_info;
            arg_info["name"] = arg_names[i];
            arg_info["type"] = static_cast<int64_t>(Variant::NIL);
            args.push_back(arg_info);
        }
        signal_info["args"] = args;
        signal_info["default_args"] = Array();
        capture->signals->push_back(signal_info);
    });

    return out;
}

bool KorkScriptVMHost::set_instance_field(KorkApi::VMObject *vm_object, const StringName &field, const Variant &value) {
    if (vm_ == nullptr || vm_object == nullptr || field.is_empty()) {
        return false;
    }

    Object *owner = static_cast<Object *>(vm_object->userPtr);
    const std::string field_utf8 = intern_utf8(field);
    const bool is_script_class_field = has_declared_script_field(owner, field);
    const uint64_t owner_key = get_dynamic_field_owner_key(vm_object);
    const bool is_new_field = find_dynamic_field_entry(owner_key, field_utf8) == nullptr;

    if (is_script_class_field) {
        set_declared_field_value(vm_object, field, value);
        return true;
    }

    const KorkApi::ConsoleValue field_value = console_value_from_variant_for_call(value);
    ++script_instance_field_write_depth_;
    const bool ok = vm_->setObjectField(vm_object, vm_->internString(field_utf8.c_str()), field_value, KorkApi::ConsoleValue::makeUnsigned(0));
    --script_instance_field_write_depth_;
    if (!ok) {
        return false;
    }

    DynamicFieldEntry *entry = upsert_dynamic_field_entry(owner_key, field_utf8);
    entry->value = value;
    entry->type = value.get_type();
    if (is_new_field) {
        notify_owner_property_list_changed(owner);
    }
    return true;
}

bool KorkScriptVMHost::get_instance_field(KorkApi::VMObject *vm_object, const StringName &field, Variant &value) const {
    if (vm_ == nullptr || vm_object == nullptr || field.is_empty()) {
        return false;
    }

    Object *owner = static_cast<Object *>(vm_object->userPtr);
    if (has_declared_script_field(owner, field)) {
        return get_declared_field_value(vm_object, field, value);
    }

    const uint64_t owner_key = get_dynamic_field_owner_key(vm_object);
    const std::string field_utf8 = intern_utf8(field);
    const DynamicFieldEntry *entry = find_dynamic_field_entry(owner_key, field_utf8);
    if (entry == nullptr) {
        return false;
    }

    value = entry->value;
    return true;
}

Variant::Type KorkScriptVMHost::get_instance_field_type(KorkApi::VMObject *vm_object, const StringName &field, bool *r_exists) const {
    Object *owner = vm_object != nullptr ? static_cast<Object *>(vm_object->userPtr) : nullptr;
    Variant::Type declared_type = Variant::NIL;
    if (has_declared_script_field(owner, field, &declared_type)) {
        if (r_exists != nullptr) {
            *r_exists = true;
        }
        return declared_type;
    }

    const uint64_t owner_key = get_dynamic_field_owner_key(vm_object);
    const std::string field_utf8 = intern_utf8(field);
    const DynamicFieldEntry *entry = find_dynamic_field_entry(owner_key, field_utf8);
    if (r_exists != nullptr) {
        *r_exists = entry != nullptr;
    }
    return entry != nullptr ? entry->type : Variant::NIL;
}

void KorkScriptVMHost::refresh_script_class_defaults(const KorkScript *script) {
    (void)script;
}

TypedArray<Dictionary> KorkScriptVMHost::get_instance_field_list(KorkApi::VMObject *vm_object) const {
    TypedArray<Dictionary> out;
    std::unordered_set<std::string> emitted_fields;

    if (vm_ != nullptr && vm_object != nullptr && vm_object->klass != nullptr) {
        const KorkApi::ClassId class_id = vm_->getClassId(vm_object->klass->name);
        if (class_id >= 0) {
            struct Capture {
                const KorkScriptVMHost *self = nullptr;
                TypedArray<Dictionary> *out = nullptr;
                std::unordered_set<std::string> *emitted = nullptr;
            } capture{ this, &out, &emitted_fields };

            vm_->enumerateClassFields(class_id, KorkApi::EnumerateClassFieldsIncludeParents, &capture, [](void *user_ptr, KorkApi::ClassId, const KorkApi::ClassFieldEnumerationInfo *info) {
                Capture *capture = static_cast<Capture *>(user_ptr);
                if (info == nullptr || info->fieldName == nullptr) {
                    return true;
                }

                const std::string field_name(info->fieldName);
                if (!capture->emitted->insert(field_name).second) {
                    return true;
                }

                Dictionary prop;
                prop["name"] = String(field_name.c_str());
                prop["type"] = static_cast<int64_t>(variant_type_from_kork_type_id(info->typeId, capture->self->vector2_type_id_, capture->self->vector3_type_id_, capture->self->vector4_type_id_, capture->self->color_type_id_));
                prop["hint"] = static_cast<int64_t>(PROPERTY_HINT_NONE);
                prop["hint_string"] = String();
                prop["usage"] = static_cast<int64_t>(PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SCRIPT_VARIABLE);
                capture->out->push_back(prop);
                return true;
            });
        }
    }

    Object *owner = vm_object != nullptr ? static_cast<Object *>(vm_object->userPtr) : nullptr;
    const KorkScript *script = get_attached_korkscript(owner);
    if (script != nullptr) {
        const PackedStringArray field_names = script->get_class_field_names();
        for (int i = 0; i < field_names.size(); ++i) {
            const String field_name = field_names[i];
            const std::string key = intern_utf8(field_name);
            if (!emitted_fields.insert(key).second) {
                continue;
            }

            bool exists = false;
            const Variant::Type type = script->get_class_field_type(StringName(field_name), &exists);
            if (!exists) {
                continue;
            }

            Dictionary prop;
            prop["name"] = field_name;
            prop["type"] = static_cast<int64_t>(type);
            prop["hint"] = static_cast<int64_t>(PROPERTY_HINT_NONE);
            prop["hint_string"] = String();
            prop["usage"] = static_cast<int64_t>(PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SCRIPT_VARIABLE);
            out.push_back(prop);
        }
    }

    const DynamicFieldState *state = get_dynamic_field_state_for_key(get_dynamic_field_owner_key(vm_object));
    if (state == nullptr) {
        return out;
    }

    for (const std::string &field_name : state->order) {
        if (emitted_fields.find(field_name) != emitted_fields.end()) {
            continue;
        }
        const DynamicFieldEntry *entry = find_dynamic_field_entry(get_dynamic_field_owner_key(vm_object), field_name);
        if (entry == nullptr) {
            continue;
        }

        Dictionary prop;
        prop["name"] = String(field_name.c_str());
        prop["type"] = static_cast<int64_t>(entry->type);
        prop["hint"] = static_cast<int64_t>(PROPERTY_HINT_NONE);
        prop["hint_string"] = String();
        prop["usage"] = static_cast<int64_t>(PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SCRIPT_VARIABLE);
        out.push_back(prop);
    }

    return out;
}

bool KorkScriptVMHost::is_current_execution_target(KorkApi::VMObject *vm_object) const {
    if (vm_object == nullptr || execution_target_stack_.empty()) {
        return false;
    }

    return execution_target_stack_.back() == get_dynamic_field_owner_key(vm_object);
}

Variant KorkScriptVMHost::value_from_console_assignment_args(U32 argc, KorkApi::ConsoleValue *argv) const {
    if (argc == 0 || argv == nullptr) {
        return Variant();
    }

    if (argc == 1) {
        if (argv[0].isFloat()) {
            return argv[0].getFloat();
        }
        if (argv[0].isUnsigned()) {
            return static_cast<int64_t>(argv[0].getInt());
        }
        return parse_script_argument(argv[0]);
    }

    PackedStringArray parts;
    for (U32 i = 0; i < argc; ++i) {
        parts.push_back(console_value_to_string(argv[i]));
    }
    return String(" ").join(parts);
}

bool KorkScriptVMHost::try_get_object_property(Object *owner, const StringName &property, Variant &value) const {
    if (owner == nullptr) {
        return false;
    }

    const uint64_t owner_id = owner->get_instance_id();
    if (property_probe_owner_ids_.find(owner_id) != property_probe_owner_ids_.end()) {
        return false;
    }

    if (!has_godot_property(owner, property)) {
        return false;
    }

    property_probe_owner_ids_.insert(owner_id);
    value = owner->get(property);
    property_probe_owner_ids_.erase(owner_id);
    return true;
}

bool KorkScriptVMHost::try_set_object_property(Object *owner, const StringName &property, const Variant &value) const {
    if (owner == nullptr) {
        return false;
    }

    const uint64_t owner_id = owner->get_instance_id();
    if (property_probe_owner_ids_.find(owner_id) != property_probe_owner_ids_.end()) {
        return false;
    }

    property_probe_owner_ids_.insert(owner_id);
    owner->set(property, value);

    const Variant readback = owner->get(property);
    property_probe_owner_ids_.erase(owner_id);
    if ((readback.get_type() != Variant::NIL || has_godot_property(owner, property)) && readback == value) {
        notify_owner_property_list_changed(owner);
        return true;
    }

    return false;
}

bool KorkScriptVMHost::get_script_class_field_info(KorkApi::VMObject *vm_object, const StringName &field, KorkApi::ScriptClassFieldInfo *out_info) const {
    if (vm_ == nullptr || vm_object == nullptr || field.is_empty()) {
        return false;
    }

    const std::string field_utf8 = intern_utf8(field);
    KorkApi::ScriptClassFieldInfo info{};
    const bool found = vm_->getScriptClassFieldInfo(vm_object, vm_->internString(field_utf8.c_str()), out_info != nullptr ? &info : nullptr);
    if (found && out_info != nullptr) {
        *out_info = info;
    }
    return found;
}

bool KorkScriptVMHost::get_namespace_parent(KorkApi::NamespaceId ns_id, KorkApi::NamespaceId &r_parent, String *r_parent_name) const {
    r_parent = nullptr;
    if (r_parent_name != nullptr) {
        *r_parent_name = String();
    }
    if (vm_ == nullptr || ns_id == nullptr) {
        return false;
    }

    struct Capture {
        KorkApi::NamespaceId target = nullptr;
        KorkApi::NamespaceId parent = nullptr;
        String parent_name;
        bool found = false;
    } capture{ ns_id, nullptr, String(), false };

    vm_->enumerateNamespaces(&capture, [](void *user_ptr, const KorkApi::NamespaceInfo *info) {
        Capture *capture = static_cast<Capture *>(user_ptr);
        if (capture->found || info == nullptr || info->nsId != capture->target) {
            return;
        }
        capture->parent = info->parentId;
        capture->parent_name = String(info->parentName ? info->parentName : "");
        capture->found = true;
    });

    if (!capture.found) {
        return false;
    }

    r_parent = capture.parent;
    if (r_parent_name != nullptr) {
        *r_parent_name = capture.parent_name;
    }
    return true;
}

bool KorkScriptVMHost::namespace_inherits_from(KorkApi::NamespaceId ns_id, KorkApi::NamespaceId ancestor_id) const {
    if (ns_id == nullptr || ancestor_id == nullptr) {
        return false;
    }

    KorkApi::NamespaceId current = ns_id;
    for (int depth = 0; depth < 256 && current != nullptr; ++depth) {
        if (current == ancestor_id) {
            return true;
        }

        KorkApi::NamespaceId parent = nullptr;
        if (!get_namespace_parent(current, parent, nullptr)) {
            break;
        }
        current = parent;
    }

    return false;
}

KorkApi::ClassId KorkScriptVMHost::resolve_vm_object_class(Object *owner, const KorkScript *script, KorkApi::NamespaceId &r_namespace) {
    r_namespace = nullptr;
    if (vm_ == nullptr || owner == nullptr) {
        return -1;
    }

    const KorkApi::NamespaceId class_ns = ensure_namespace_for_class(owner->get_class());
    r_namespace = class_ns;

    if (script == nullptr) {
        return godot_object_class_id_;
    }

    String script_namespace_name = script->get_effective_namespace_name().strip_edges();
    const String declared_script_class_name = script->get_declared_script_class_name().strip_edges();
    if (script_namespace_name.is_empty() && !declared_script_class_name.is_empty()) {
        script_namespace_name = declared_script_class_name;
    }
    if (script_namespace_name.is_empty()) {
        return godot_object_class_id_;
    }

    auto resolve_namespace = [&](const String &name, KorkApi::NamespaceId &out_ns, KorkApi::ClassId &out_class_id) {
        if (name.is_empty()) {
            out_ns = nullptr;
            out_class_id = -1;
            return;
        }
        const std::string name_utf8 = intern_utf8(name);
        out_ns = vm_->findNamespace(vm_->internString(name_utf8.c_str()));
        out_class_id = vm_->getClassId(name_utf8.c_str());
    };

    KorkApi::NamespaceId script_ns = nullptr;
    KorkApi::ClassId script_class_id = -1;
    resolve_namespace(script_namespace_name, script_ns, script_class_id);
    if (script_class_id < 0 && !declared_script_class_name.is_empty() && declared_script_class_name != script_namespace_name) {
        KorkApi::NamespaceId declared_ns = nullptr;
        KorkApi::ClassId declared_class_id = -1;
        resolve_namespace(declared_script_class_name, declared_ns, declared_class_id);
        if (declared_ns != nullptr) {
            script_namespace_name = declared_script_class_name;
            script_ns = declared_ns;
            script_class_id = declared_class_id;
        }
    }

    if (script_ns == nullptr) {
        UtilityFunctions::push_error(vformat(
                "Kork namespace '%s' was not found while binding script to owner '%s'.",
                script_namespace_name,
                String(owner->get_class())));
        return -1;
    }
    install_object_bridge_methods(script_ns, false);
    if (script_class_id >= 0) {
        bool matches_owner_linkage = false;
        for (StringName current_class = owner->get_class(); !current_class.is_empty(); current_class = ClassDBSingleton::get_singleton()->get_parent_class(current_class)) {
            if (namespace_inherits_from(script_ns, ensure_namespace_for_class(current_class))) {
                matches_owner_linkage = true;
                break;
            }
        }

        if (!matches_owner_linkage) {
            UtilityFunctions::push_error(vformat(
                    "Kork script class '%s' is not linked to owner type '%s' or any of its Godot parent namespaces.",
                    script_namespace_name,
                    String(owner->get_class())));
            return -1;
        }

        r_namespace = script_ns;
        return script_class_id;
    }

    if (script_ns == class_ns) {
        r_namespace = class_ns;
        return godot_object_class_id_;
    }

    KorkApi::NamespaceId parent_ns = nullptr;
    String parent_name;
    get_namespace_parent(script_ns, parent_ns, &parent_name);
    if (parent_ns != nullptr && parent_ns != class_ns) {
        UtilityFunctions::push_error(vformat(
                "Kork namespace '%s' is already linked to '%s' and cannot be relinked to '%s'.",
                script_namespace_name,
                parent_name,
                String(owner->get_class())));
        return -1;
    }

    if (parent_ns == nullptr) {
        vm_->linkNamespaceById(class_ns, script_ns);
    }

    r_namespace = script_ns;
    return godot_object_class_id_;
}

Variant KorkScriptVMHost::get_vm_object_field_value(KorkApi::VMObject *vm_object, const StringName &field) const {
    if (vm_ == nullptr || vm_object == nullptr || field.is_empty()) {
        return Variant();
    }

    const std::string field_utf8 = intern_utf8(field);
    const KorkApi::ConsoleValue value = vm_->getObjectField(vm_object, vm_->internString(field_utf8.c_str()), KorkApi::ConsoleValue::makeUnsigned(0));
    return variant_from_console_value(value);
}

void KorkScriptVMHost::add_instance_property_state(KorkApi::VMObject *vm_object, GDExtensionScriptInstancePropertyStateAdd add_func, void *userdata) const {
    if (vm_object == nullptr || add_func == nullptr) {
        return;
    }

    std::unordered_set<std::string> emitted_fields;
    if (vm_ != nullptr && vm_object->klass != nullptr) {
        const KorkApi::ClassId class_id = vm_->getClassId(vm_object->klass->name);
        if (class_id >= 0) {
            struct Capture {
                const KorkScriptVMHost *self = nullptr;
                KorkApi::VMObject *vm_object = nullptr;
                GDExtensionScriptInstancePropertyStateAdd add_func = nullptr;
                void *userdata = nullptr;
                std::unordered_set<std::string> *emitted = nullptr;
            } capture{ this, vm_object, add_func, userdata, &emitted_fields };

            vm_->enumerateClassFields(class_id, KorkApi::EnumerateClassFieldsIncludeParents, &capture, [](void *user_ptr, KorkApi::ClassId, const KorkApi::ClassFieldEnumerationInfo *info) {
                Capture *capture = static_cast<Capture *>(user_ptr);
                if (info == nullptr || info->fieldName == nullptr) {
                    return true;
                }

                const std::string field_name(info->fieldName);
                if (!capture->emitted->insert(field_name).second) {
                    return true;
                }

                const StringName godot_name(field_name.c_str());
                const Variant value = capture->self->get_vm_object_field_value(capture->vm_object, godot_name);
                capture->add_func(&godot_name, &value, capture->userdata);
                return true;
            });
        }
    }

    const DynamicFieldState *state = get_dynamic_field_state_for_key(get_dynamic_field_owner_key(vm_object));
    if (state == nullptr) {
        return;
    }

    for (const std::string &field_name : state->order) {
        if (emitted_fields.find(field_name) != emitted_fields.end()) {
            continue;
        }
        const DynamicFieldEntry *entry = find_dynamic_field_entry(get_dynamic_field_owner_key(vm_object), field_name);
        if (entry == nullptr) {
            continue;
        }

        const StringName godot_name(field_name.c_str());
        add_func(&godot_name, &entry->value, userdata);
    }
}

void KorkScriptVMHost::trigger_godot_signal(Object *owner, StringTableEntry signal_name, int argc, KorkApi::ConsoleValue *argv) const {
    if (owner == nullptr || signal_name == nullptr) {
        return;
    }

    Array args;
    args.push_back(String(signal_name));
    for (int i = 0; i < argc; ++i) {
        args.push_back(parse_script_argument(argv[i]));
    }

    owner->callv(StringName("emit_signal"), args);
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

KorkApi::ConsoleValue KorkScriptVMHost::global_m_sin_callback(void *, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]) {
    return static_cast<KorkScriptVMHost *>(user_ptr)->bridge_global_trig(argc, argv, static_cast<real_t (*)(real_t)>(std::sin));
}

KorkApi::ConsoleValue KorkScriptVMHost::global_m_cos_callback(void *, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]) {
    return static_cast<KorkScriptVMHost *>(user_ptr)->bridge_global_trig(argc, argv, static_cast<real_t (*)(real_t)>(std::cos));
}

KorkApi::ConsoleValue KorkScriptVMHost::global_m_tan_callback(void *, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]) {
    return static_cast<KorkScriptVMHost *>(user_ptr)->bridge_global_trig(argc, argv, static_cast<real_t (*)(real_t)>(std::tan));
}

KorkApi::ConsoleValue KorkScriptVMHost::global_get_word_callback(void *, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]) {
    return static_cast<KorkScriptVMHost *>(user_ptr)->bridge_global_get_word(argc, argv);
}

KorkApi::ConsoleValue KorkScriptVMHost::global_is_object_callback(void *, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]) {
    return static_cast<KorkScriptVMHost *>(user_ptr)->bridge_global_is_object(argc, argv);
}

bool KorkScriptVMHost::custom_field_iterate_callback(KorkApi::Vm *vm, KorkApi::VMObject *object, KorkApi::VMIterator &state, StringTableEntry *name) {
    if (vm == nullptr || object == nullptr || object->klass == nullptr || object->klass->userPtr == nullptr || name == nullptr) {
        return false;
    }

    KorkScriptVMHost *self = static_cast<KorkScriptVMHost *>(object->klass->userPtr);
    const DynamicFieldState *fields = self->get_dynamic_field_state_for_key(self->get_dynamic_field_owner_key(object));
    if (fields == nullptr) {
        return false;
    }

    const int index = state.count;
    if (index < 0 || static_cast<size_t>(index) >= fields->order.size()) {
        return false;
    }

    const std::string &field_name = fields->order[static_cast<size_t>(index)];
    *name = vm->internString(field_name.c_str());
    state.internalEntry = reinterpret_cast<void *>(static_cast<uintptr_t>(index + 1));
    state.count = index + 1;
    return true;
}

KorkApi::ConsoleValue KorkScriptVMHost::custom_field_get_by_iterator_callback(KorkApi::Vm *, KorkApi::VMObject *object, KorkApi::VMIterator &state) {
    if (object == nullptr || object->klass == nullptr || object->klass->userPtr == nullptr || state.internalEntry == nullptr) {
        return KorkApi::ConsoleValue();
    }

    KorkScriptVMHost *self = static_cast<KorkScriptVMHost *>(object->klass->userPtr);
    const DynamicFieldState *fields = self->get_dynamic_field_state_for_key(self->get_dynamic_field_owner_key(object));
    if (fields == nullptr) {
        return KorkApi::ConsoleValue();
    }

    const uintptr_t entry_index = reinterpret_cast<uintptr_t>(state.internalEntry);
    if (entry_index == 0) {
        return KorkApi::ConsoleValue();
    }

    const size_t index = static_cast<size_t>(entry_index - 1);
    if (index >= fields->order.size()) {
        return KorkApi::ConsoleValue();
    }

    const DynamicFieldEntry *entry = self->find_dynamic_field_entry(self->get_dynamic_field_owner_key(object), fields->order[index]);
    return entry != nullptr ? self->console_value_from_variant(entry->value) : KorkApi::ConsoleValue();
}

KorkApi::ConsoleValue KorkScriptVMHost::custom_field_get_by_name_callback(KorkApi::Vm *, KorkApi::VMObject *object, const char *name, KorkApi::ConsoleValue) {
    if (object == nullptr || object->klass == nullptr || object->klass->userPtr == nullptr || name == nullptr || name[0] == '\0') {
        return KorkApi::ConsoleValue();
    }

    KorkScriptVMHost *self = static_cast<KorkScriptVMHost *>(object->klass->userPtr);
    Object *owner = static_cast<Object *>(object->userPtr);
    const StringName property_name(name);
    const DynamicFieldEntry *entry = self->find_dynamic_field_entry(self->get_dynamic_field_owner_key(object), std::string_view(name));
    const bool is_script_instance_read = self->script_instance_field_read_depth_ > 0;

    if (self->has_declared_script_field(owner, property_name)) {
        Variant value;
        if (self->get_declared_field_value(object, property_name, value)) {
            return self->console_value_from_variant(value);
        }
        return KorkApi::ConsoleValue();
    }

    if (self->is_current_execution_target(object)) {
        if (entry != nullptr) {
            return self->console_value_from_variant(entry->value);
        }
        Variant property_value;
        if (!is_script_instance_read && self->try_get_object_property(owner, property_name, property_value)) {
            return self->console_value_from_variant(property_value);
        }
        return KorkApi::ConsoleValue();
    }

    Variant property_value;
    if (!is_script_instance_read && self->try_get_object_property(owner, property_name, property_value)) {
        return self->console_value_from_variant(property_value);
    }

    return entry != nullptr ? self->console_value_from_variant(entry->value) : KorkApi::ConsoleValue();
}

void KorkScriptVMHost::custom_field_set_by_name_callback(KorkApi::Vm *, KorkApi::VMObject *object, const char *name, KorkApi::ConsoleValue, U32 argc, KorkApi::ConsoleValue *argv) {
    if (object == nullptr || object->klass == nullptr || object->klass->userPtr == nullptr || name == nullptr || name[0] == '\0') {
        return;
    }

    KorkScriptVMHost *self = static_cast<KorkScriptVMHost *>(object->klass->userPtr);
    Object *owner = static_cast<Object *>(object->userPtr);
    const StringName property_name(name);
    const Variant value = self->value_from_console_assignment_args(argc, argv);
    if (self->has_declared_script_field(owner, property_name)) {
        self->set_declared_field_value(object, property_name, value);
        return;
    }

    const bool is_script_instance_write = self->script_instance_field_write_depth_ > 0;
    if (!self->is_current_execution_target(object) && !is_script_instance_write) {
        if (self->try_set_object_property(owner, property_name, value)) {
            return;
        }
    }

    const bool is_new_field = self->find_dynamic_field_entry(self->get_dynamic_field_owner_key(object), std::string_view(name)) == nullptr;
    DynamicFieldEntry *entry = self->upsert_dynamic_field_entry(self->get_dynamic_field_owner_key(object), std::string_view(name));
    entry->value = value;
    entry->type = entry->value.get_type();
    if (is_new_field && self->script_instance_field_write_depth_ == 0) {
        self->notify_owner_property_list_changed(owner);
    }
}

bool KorkScriptVMHost::custom_field_set_type_callback(KorkApi::Vm *, KorkApi::VMObject *object, const char *name, KorkApi::ConsoleValue, U32 type_id) {
    if (object == nullptr || object->klass == nullptr || object->klass->userPtr == nullptr || name == nullptr || name[0] == '\0') {
        return false;
    }

    KorkScriptVMHost *self = static_cast<KorkScriptVMHost *>(object->klass->userPtr);
    if (self->get_script_class_field_info(object, StringName(name), nullptr)) {
        return true;
    }
    DynamicFieldEntry *entry = self->upsert_dynamic_field_entry(self->get_dynamic_field_owner_key(object), std::string_view(name));
    if (entry->type == Variant::NIL) {
        entry->type = variant_type_from_kork_type_id(type_id, self->vector2_type_id_, self->vector3_type_id_, self->vector4_type_id_, self->color_type_id_);
    }
    return true;
}

KorkApi::VMObject *KorkScriptVMHost::find_by_name_callback(void *user_ptr, StringTableEntry name, KorkApi::VMObject *parent) {
    KorkScriptVMHost *self = static_cast<KorkScriptVMHost *>(user_ptr);
    if (self == nullptr || name == nullptr) {
        return nullptr;
    }

    auto found = self->vm_objects_by_name_.find(std::string(name));
    if (found != self->vm_objects_by_name_.end()) {
        return found->second;
    }

    Object *context_owner = parent != nullptr ? static_cast<Object *>(parent->userPtr) : nullptr;
    Object *owner = self->resolve_object_reference(context_owner, String(name));
    return self->get_or_create_vm_object(owner, nullptr);
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

    auto by_path = self->vm_objects_by_path_.find(std::string(path));
    if (by_path != self->vm_objects_by_path_.end()) {
        return by_path->second;
    }

    auto by_name = self->vm_objects_by_name_.find(std::string(path));
    if (by_name != self->vm_objects_by_name_.end()) {
        return by_name->second;
    }

    Object *owner = self->resolve_object_reference(nullptr, String(path));
    return self->get_or_create_vm_object(owner, nullptr);
}

KorkApi::VMObject *KorkScriptVMHost::find_by_id_callback(void *user_ptr, KorkApi::SimObjectId ident) {
    KorkScriptVMHost *self = static_cast<KorkScriptVMHost *>(user_ptr);
    if (self == nullptr) {
        return nullptr;
    }

    auto found = self->vm_objects_by_id_.find(ident);
    if (found != self->vm_objects_by_id_.end()) {
        return found->second;
    }

    Object *owner = self->resolve_object_reference(nullptr, String::num_int64(ident));
    return self->get_or_create_vm_object(owner, nullptr);
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
    const CharString utf8 = self->get_object_name(owner).utf8();
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

KorkApi::ConsoleValue KorkScriptVMHost::object_get_id_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]) {
    return static_cast<KorkScriptVMHost *>(user_ptr)->bridge_object_get_id(static_cast<Object *>(obj), argc, argv);
}

KorkApi::ConsoleValue KorkScriptVMHost::object_get_name_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]) {
    return static_cast<KorkScriptVMHost *>(user_ptr)->bridge_object_get_name(static_cast<Object *>(obj), argc, argv);
}

KorkApi::ConsoleValue KorkScriptVMHost::object_dump_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]) {
    return static_cast<KorkScriptVMHost *>(user_ptr)->bridge_object_dump(static_cast<Object *>(obj), argc, argv);
}

KorkApi::ConsoleValue KorkScriptVMHost::object_find_object_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]) {
    return static_cast<KorkScriptVMHost *>(user_ptr)->bridge_object_find_object(static_cast<Object *>(obj), argc, argv);
}

KorkApi::ConsoleValue KorkScriptVMHost::object_get_parent_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]) {
    return static_cast<KorkScriptVMHost *>(user_ptr)->bridge_object_get_parent(static_cast<Object *>(obj), argc, argv);
}

KorkApi::ConsoleValue KorkScriptVMHost::object_get_object_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]) {
    return static_cast<KorkScriptVMHost *>(user_ptr)->bridge_object_get_object(static_cast<Object *>(obj), argc, argv);
}

KorkApi::ConsoleValue KorkScriptVMHost::object_get_count_callback(void *obj, void *user_ptr, int32_t argc, KorkApi::ConsoleValue argv[]) {
    return static_cast<KorkScriptVMHost *>(user_ptr)->bridge_object_get_count(static_cast<Object *>(obj), argc, argv);
}

KorkApi::ConsoleValue KorkScriptVMHost::object_kork_ctor_callback(void *, void *, int32_t, KorkApi::ConsoleValue[]) {
    return KorkApi::ConsoleValue();
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

KorkApi::ClassId KorkScriptVMHost::ensure_class_for_godot_type(const StringName &class_name) {
    if (vm_ == nullptr || class_name.is_empty()) {
        return -1;
    }

    StringName resolved_class_name = class_name;
    ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
    if (class_db == nullptr) {
        return -1;
    }

    if (!class_db->class_exists(resolved_class_name)) {
        const String requested = String(resolved_class_name);
        const PackedStringArray all_classes = class_db->get_class_list();
        for (int i = 0; i < all_classes.size(); ++i) {
            const String candidate = all_classes[i];
            if (candidate.nocasecmp_to(requested) == 0) {
                resolved_class_name = StringName(candidate);
                break;
            }
        }
    }

    if (!class_db->class_exists(resolved_class_name)) {
        return -1;
    }

    ensure_type_for_godot_type(resolved_class_name);

    const std::string class_utf8 = intern_utf8(resolved_class_name);
    auto found = godot_class_ids_by_name_.find(class_utf8);
    if (found != godot_class_ids_by_name_.end()) {
        return found->second;
    }

    KorkApi::ClassId parent_class_id = -1;
    const StringName parent_name = class_db->get_parent_class(resolved_class_name);
    if (!parent_name.is_empty()) {
        parent_class_id = ensure_class_for_godot_type(parent_name);
    } else if (resolved_class_name == StringName("Object")) {
        parent_class_id = godot_object_class_id_;
    }

    KorkApi::ClassInfo class_info{};
    class_info.name = vm_->internString(class_utf8.c_str());
    class_info.userPtr = this;
    class_info.numFields = 0;
    class_info.fields = &g_empty_field_info;
    class_info.parentKlassId = parent_class_id;
    class_info.nativeRootClassId = parent_class_id >= 0 ? parent_class_id : godot_object_class_id_;
    class_info.iCreate = {
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &KorkScriptVMHost::get_vm_object_id,
        &KorkScriptVMHost::get_vm_object_name
    };
    class_info.iSignals = {
        [](void *, KorkApi::VMObject *object, StringTableEntry signal_name, int argc, KorkApi::ConsoleValue *argv) {
            if (object == nullptr || object->klass == nullptr || object->klass->userPtr == nullptr) {
                return;
            }

            KorkScriptVMHost *self = static_cast<KorkScriptVMHost *>(object->klass->userPtr);
            self->trigger_godot_signal(static_cast<Object *>(object->userPtr), signal_name, argc, argv);
        }
    };
    class_info.iCustomFields = {
        &KorkScriptVMHost::custom_field_iterate_callback,
        &KorkScriptVMHost::custom_field_get_by_iterator_callback,
        &KorkScriptVMHost::custom_field_get_by_name_callback,
        &KorkScriptVMHost::custom_field_set_by_name_callback,
        &KorkScriptVMHost::custom_field_set_type_callback
    };

    const KorkApi::ClassId class_id = vm_->registerClass(class_info);
    godot_class_ids_by_name_.emplace(class_utf8, class_id);
    return class_id;
}

KorkApi::TypeId KorkScriptVMHost::ensure_type_for_godot_type(const StringName &type_name) {
    if (vm_ == nullptr || type_name.is_empty()) {
        return -1;
    }

    const std::string type_utf8 = intern_utf8(type_name);
    auto found = godot_type_ids_by_name_.find(type_utf8);
    if (found != godot_type_ids_by_name_.end()) {
        return found->second;
    }

    KorkApi::TypeInfo type_info{};
    type_info.name = vm_->internString(type_utf8.c_str());
    type_info.inspectorFieldType = nullptr;
    type_info.userPtr = nullptr;
    type_info.fieldSize = sizeof(U64);
    type_info.valueSize = sizeof(U64);
    type_info.iFuncs.CastValueFn = &cast_object_reference_type;
    type_info.iFuncs.PerformOpFn = [](void *, KorkApi::Vm *, U32, KorkApi::ConsoleValue lhs, KorkApi::ConsoleValue) {
        return lhs;
    };

    const KorkApi::TypeId type_id = vm_->registerType(type_info);
    godot_type_ids_by_name_.emplace(type_utf8, type_id);
    return type_id;
}

void KorkScriptVMHost::install_object_bridge_methods(KorkApi::NamespaceId ns_id, bool include_script_ctor) {
    if (vm_ == nullptr || ns_id == nullptr) {
        return;
    }

    if (include_script_ctor) {
        vm_->addNamespaceFunction(ns_id, vm_->internString("__kork_ctor"), &KorkScriptVMHost::object_kork_ctor_callback, this, "()", 2, 2);
    }
    vm_->addNamespaceFunction(ns_id, vm_->internString("call"), &KorkScriptVMHost::object_call_callback, this, "(method, ...args)", 3, KorkApi::Constants::MaxArgs);
    vm_->addNamespaceFunction(ns_id, vm_->internString("get"), &KorkScriptVMHost::object_get_callback, this, "(property)", 3, 3);
    vm_->addNamespaceFunction(ns_id, vm_->internString("set"), &KorkScriptVMHost::object_set_callback, this, "(property, value)", 4, 4);
    vm_->addNamespaceFunction(ns_id, vm_->internString("print"), &KorkScriptVMHost::object_print_callback, this, "(...args)", 2, KorkApi::Constants::MaxArgs);
    vm_->addNamespaceFunction(ns_id, vm_->internString("getId"), &KorkScriptVMHost::object_get_id_callback, this, "()", 2, 2);
    vm_->addNamespaceFunction(ns_id, vm_->internString("getName"), &KorkScriptVMHost::object_get_name_callback, this, "()", 2, 2);
    vm_->addNamespaceFunction(ns_id, vm_->internString("dump"), &KorkScriptVMHost::object_dump_callback, this, "()", 2, 2);
    vm_->addNamespaceFunction(ns_id, vm_->internString("findObject"), &KorkScriptVMHost::object_find_object_callback, this, "(path)", 3, 3);
    vm_->addNamespaceFunction(ns_id, vm_->internString("getParent"), &KorkScriptVMHost::object_get_parent_callback, this, "()", 2, 2);
    vm_->addNamespaceFunction(ns_id, vm_->internString("getObject"), &KorkScriptVMHost::object_get_object_callback, this, "(index)", 3, 3);
    vm_->addNamespaceFunction(ns_id, vm_->internString("getCount"), &KorkScriptVMHost::object_get_count_callback, this, "()", 2, 2);
}

void KorkScriptVMHost::ensure_global_math_namespace() {
    KorkApi::NamespaceId global_ns = vm_->getGlobalNamespace();
    vm_->addNamespaceFunction(global_ns, vm_->internString("mSin"), &KorkScriptVMHost::global_m_sin_callback, this, "(value)", 2, 2);
    vm_->addNamespaceFunction(global_ns, vm_->internString("mCos"), &KorkScriptVMHost::global_m_cos_callback, this, "(value)", 2, 2);
    vm_->addNamespaceFunction(global_ns, vm_->internString("mTan"), &KorkScriptVMHost::global_m_tan_callback, this, "(value)", 2, 2);
    vm_->addNamespaceFunction(global_ns, vm_->internString("getWord"), &KorkScriptVMHost::global_get_word_callback, this, "(text, index)", 3, 3);
    vm_->addNamespaceFunction(global_ns, vm_->internString("isObject"), &KorkScriptVMHost::global_is_object_callback, this, "(handle)", 2, 2);
}

void KorkScriptVMHost::ensure_object_bridge_namespace() {
    KorkApi::NamespaceId object_ns = ensure_namespace_for_class("Object");
    install_object_bridge_methods(object_ns, true);
}

bool KorkScriptVMHost::has_godot_property(Object *owner, const StringName &property) const {
    if (owner == nullptr || property.is_empty()) {
        return false;
    }

    const uint64_t owner_id = owner->get_instance_id();
    if (property_probe_owner_ids_.find(owner_id) != property_probe_owner_ids_.end()) {
        return false;
    }

    property_probe_owner_ids_.insert(owner_id);
    const TypedArray<Dictionary> properties = owner->get_property_list();
    property_probe_owner_ids_.erase(owner_id);
    for (int i = 0; i < properties.size(); ++i) {
        const Dictionary info = properties[i];
        if (StringName(info.get("name", Variant())) == property) {
            return true;
        }
    }
    return false;
}

void KorkScriptVMHost::notify_owner_property_list_changed(Object *owner) const {
    if (owner != nullptr) {
        owner->notify_property_list_changed();
    }
}

void KorkScriptVMHost::seed_script_class_defaults(Object *owner, KorkApi::VMObject *vm_object, const KorkScript *script) {
}

bool KorkScriptVMHost::get_declared_script_default(Object *owner, const StringName &field, Variant &value) const {
    const KorkScript *script = get_attached_korkscript(owner);
    return script != nullptr && script->get_class_field_default_value(field, &value);
}

bool KorkScriptVMHost::has_declared_script_field(Object *owner, const StringName &field, Variant::Type *r_type) const {
    const KorkScript *script = get_attached_korkscript(owner);
    if (script == nullptr) {
        if (r_type != nullptr) {
            *r_type = Variant::NIL;
        }
        return false;
    }

    bool exists = false;
    const Variant::Type type = script->get_class_field_type(field, &exists);
    if (r_type != nullptr) {
        *r_type = type;
    }
    return exists;
}

bool KorkScriptVMHost::get_declared_field_value(const KorkApi::VMObject *vm_object, const StringName &field, Variant &value) const {
    if (vm_object == nullptr || field.is_empty()) {
        return false;
    }

    const uint64_t owner_key = get_dynamic_field_owner_key(vm_object);
    const std::string field_utf8 = intern_utf8(field);
    const DynamicFieldEntry *entry = find_dynamic_field_entry(owner_key, field_utf8);
    if (entry != nullptr) {
        value = entry->value;
        return true;
    }

    Object *owner = static_cast<Object *>(vm_object->userPtr);
    return get_declared_script_default(owner, field, value);
}

void KorkScriptVMHost::set_declared_field_value(KorkApi::VMObject *vm_object, const StringName &field, const Variant &value) {
    if (vm_object == nullptr || field.is_empty()) {
        return;
    }

    Object *owner = static_cast<Object *>(vm_object->userPtr);
    const uint64_t owner_key = get_dynamic_field_owner_key(vm_object);
    const std::string field_utf8 = intern_utf8(field);
    Variant default_value;
    if (get_declared_script_default(owner, field, default_value) && default_value == value) {
        DynamicFieldState *state = get_dynamic_field_state_for_key(owner_key);
        if (state != nullptr) {
            state->by_name.erase(field_utf8);
        }
        return;
    }

    DynamicFieldEntry *entry = upsert_dynamic_field_entry(owner_key, field_utf8);
    entry->value = value;
    entry->type = value.get_type();
}

void KorkScriptVMHost::push_execution_target(uint64_t owner_key) const {
    if (owner_key != 0) {
        execution_target_stack_.push_back(owner_key);
    }
}

void KorkScriptVMHost::pop_execution_target() const {
    if (!execution_target_stack_.empty()) {
        execution_target_stack_.pop_back();
    }
}

KorkApi::SimObjectId KorkScriptVMHost::ensure_sim_object_id(Object *owner) const {
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

    vm_objects_by_owner_id_[owner->get_instance_id()] = vm_object;
    vm_objects_by_id_[sim_id] = vm_object;

    Node *node = Object::cast_to<Node>(owner);
    if (node != nullptr) {
        const CharString name_utf8 = String(node->get_name()).utf8();
        vm_objects_by_name_[std::string(name_utf8.get_data(), static_cast<size_t>(name_utf8.length()))] = vm_object;

        const std::string path_key = make_node_path_key(node);
        if (!path_key.empty()) {
            vm_objects_by_path_[path_key] = vm_object;
        }
    }
}

void KorkScriptVMHost::unregister_vm_object(Object *owner, KorkApi::VMObject *vm_object) {
    if (owner == nullptr) {
        return;
    }

    auto owner_it = vm_objects_by_owner_id_.find(owner->get_instance_id());
    if (owner_it != vm_objects_by_owner_id_.end() && owner_it->second == vm_object) {
        vm_objects_by_owner_id_.erase(owner_it);
    }

    erase_dynamic_field_state(owner->get_instance_id());

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

        const std::string path_key = make_node_path_key(node);
        if (!path_key.empty()) {
            auto path_it = vm_objects_by_path_.find(path_key);
            if (path_it != vm_objects_by_path_.end() && path_it->second == vm_object) {
                vm_objects_by_path_.erase(path_it);
            }
        }
    }
}

uint64_t KorkScriptVMHost::get_dynamic_field_owner_key(const KorkApi::VMObject *vm_object) const {
    if (vm_object == nullptr || vm_object->userPtr == nullptr) {
        return 0;
    }
    return get_dynamic_field_owner_key(static_cast<const Object *>(vm_object->userPtr));
}

uint64_t KorkScriptVMHost::get_dynamic_field_owner_key(const Object *owner) const {
    return owner != nullptr ? owner->get_instance_id() : 0;
}

KorkScriptVMHost::DynamicFieldState *KorkScriptVMHost::get_dynamic_field_state_for_key(uint64_t owner_key) {
    if (owner_key == 0) {
        return nullptr;
    }
    auto found = dynamic_fields_by_owner_id_.find(owner_key);
    return found != dynamic_fields_by_owner_id_.end() ? &found->second : nullptr;
}

const KorkScriptVMHost::DynamicFieldState *KorkScriptVMHost::get_dynamic_field_state_for_key(uint64_t owner_key) const {
    if (owner_key == 0) {
        return nullptr;
    }
    auto found = dynamic_fields_by_owner_id_.find(owner_key);
    return found != dynamic_fields_by_owner_id_.end() ? &found->second : nullptr;
}

KorkScriptVMHost::DynamicFieldEntry *KorkScriptVMHost::upsert_dynamic_field_entry(uint64_t owner_key, std::string_view field_name) {
    DynamicFieldState &state = dynamic_fields_by_owner_id_[owner_key];
    const std::string key(field_name);
    auto found = state.by_name.find(key);
    if (found == state.by_name.end()) {
        state.order.push_back(key);
        found = state.by_name.emplace(key, DynamicFieldEntry()).first;
    }
    return &found->second;
}

const KorkScriptVMHost::DynamicFieldEntry *KorkScriptVMHost::find_dynamic_field_entry(uint64_t owner_key, std::string_view field_name) const {
    const DynamicFieldState *state = get_dynamic_field_state_for_key(owner_key);
    if (state == nullptr) {
        return nullptr;
    }

    auto found = state->by_name.find(std::string(field_name));
    return found != state->by_name.end() ? &found->second : nullptr;
}

void KorkScriptVMHost::erase_dynamic_field_state(uint64_t owner_key) {
    if (owner_key != 0) {
        dynamic_fields_by_owner_id_.erase(owner_key);
    }
}

KorkApi::ConsoleValue KorkScriptVMHost::bridge_object_call(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const {
    if (target == nullptr || argc < 3) {
        return KorkApi::ConsoleValue();
    }

    const StringName method(console_value_to_string(argv[2]));
    if (method == StringName("connect") && argc >= 6) {
        const StringName signal_name(console_value_to_string(argv[3]));
        const String target_handle = console_value_to_string(argv[4]).strip_edges();
        Object *callable_target = resolve_object_reference(target, target_handle);
        if (callable_target == nullptr) {
            return console_value_from_variant_for_call(static_cast<int64_t>(ERR_INVALID_PARAMETER));
        }

        const StringName callable_method(console_value_to_string(argv[5]));
        const Callable callable(callable_target, callable_method);
        const uint32_t flags = argc >= 7 ? static_cast<uint32_t>(static_cast<int64_t>(parse_script_argument(argv[6]))) : 0;
        const Error err = target->connect(signal_name, callable, static_cast<uint32_t>(flags));
        return console_value_from_variant_for_call(static_cast<int64_t>(err));
    }

    if (method == StringName("disconnect") && argc >= 6) {
        const StringName signal_name(console_value_to_string(argv[3]));
        const String target_handle = console_value_to_string(argv[4]).strip_edges();
        Object *callable_target = resolve_object_reference(target, target_handle);
        if (callable_target == nullptr) {
            return console_value_from_variant_for_call(static_cast<int64_t>(ERR_INVALID_PARAMETER));
        }

        const StringName callable_method(console_value_to_string(argv[5]));
        target->disconnect(signal_name, Callable(callable_target, callable_method));
        return console_value_from_variant_for_call(static_cast<int64_t>(OK));
    }

    Array args;
    for (int32_t i = 3; i < argc; ++i) {
        args.push_back(parse_script_argument(argv[i]));
    }

    return console_value_from_variant(target->callv(method, args));
}

KorkApi::ConsoleValue KorkScriptVMHost::bridge_object_get(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const {
    if (target == nullptr || argc < 3) {
        return KorkApi::ConsoleValue();
    }
    const StringName property_name(console_value_to_string(argv[2]));
    Variant value = target->get(property_name);
    KorkApi::ConsoleValue out = console_value_from_variant(value);
    return out;
}

KorkApi::ConsoleValue KorkScriptVMHost::bridge_object_set(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const {
    if (target == nullptr || argc < 4) {
        return KorkApi::ConsoleValue::makeUnsigned(0);
    }
    target->set(StringName(console_value_to_string(argv[2])), parse_script_argument(argv[3]));
    notify_owner_property_list_changed(target);
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

KorkApi::ConsoleValue KorkScriptVMHost::bridge_object_get_id(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const {
    if (target == nullptr || argc != 2) {
        return KorkApi::ConsoleValue::makeUnsigned(0);
    }
    return KorkApi::ConsoleValue::makeUnsigned(ensure_sim_object_id(target));
}

KorkApi::ConsoleValue KorkScriptVMHost::bridge_object_get_name(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const {
    if (target == nullptr || argc != 2) {
        return KorkApi::ConsoleValue::makeString("");
    }

    const CharString utf8 = get_object_name(target).utf8();
    KorkApi::ConsoleValue buffer = vm_->getStringInZone(KorkApi::ConsoleValue::ZoneReturn, static_cast<uint32_t>(utf8.length() + 1));
    char *out = static_cast<char *>(buffer.evaluatePtr(vm_->getAllocBase()));
    if (out != nullptr) {
        std::memcpy(out, utf8.get_data(), static_cast<size_t>(utf8.length() + 1));
    }
    return buffer;
}

KorkApi::ConsoleValue KorkScriptVMHost::bridge_object_dump(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const {
    if (target == nullptr || argc != 2) {
        return KorkApi::ConsoleValue::makeUnsigned(0);
    }

    UtilityFunctions::print("Static Fields:");
    const TypedArray<Dictionary> properties = target->get_property_list();
    for (int i = 0; i < properties.size(); ++i) {
        const Dictionary property = properties[i];
        const String property_name = property.get("name", "");
        if (property_name.is_empty()) {
            continue;
        }

        const Variant value = target->get(StringName(property_name));
        if (value.get_type() == Variant::NIL) {
            continue;
        }

        UtilityFunctions::print(vformat("  %s = \"%s\"", property_name, value.stringify()));
    }

    UtilityFunctions::print("Dynamic Fields:");
    UtilityFunctions::print("Methods:");

    std::unordered_set<std::string> printed_methods;
    const TypedArray<Dictionary> methods = target->get_method_list();
    for (int i = 0; i < methods.size(); ++i) {
        const Dictionary method = methods[i];
        const String method_name = method.get("name", "");
        if (method_name.is_empty()) {
            continue;
        }

        const std::string key = intern_utf8(method_name);
        if (!printed_methods.insert(key).second) {
            continue;
        }

        UtilityFunctions::print(vformat("  %s()", method_name));
    }

    KorkApi::VMObject *vm_object = const_cast<KorkScriptVMHost *>(this)->get_or_create_vm_object(target, nullptr);
    if (vm_object != nullptr) {
        vm_->enumerateNamespaceEntries(vm_->getObjectNamespace(vm_object), &printed_methods, [](void *user_ptr, const KorkApi::NamespaceEntryInfo *info) {
            if (info == nullptr || info->name == nullptr) {
                return;
            }

            if (info->kind != KorkApi::NamespaceEntryFunction && info->kind != KorkApi::NamespaceEntrySignal) {
                return;
            }

            std::unordered_set<std::string> *printed = static_cast<std::unordered_set<std::string> *>(user_ptr);
            const std::string key(info->name);
            if (!printed->insert(key).second) {
                return;
            }

            const char *usage = info->usage != nullptr ? info->usage : "";
            if (usage != nullptr && usage[0] != '\0') {
                UtilityFunctions::print(vformat(info->kind == KorkApi::NamespaceEntrySignal ? "  [signal] %s() - %s" : "  %s() - %s", String(info->name), String(usage)));
            } else {
                UtilityFunctions::print(vformat(info->kind == KorkApi::NamespaceEntrySignal ? "  [signal] %s()" : "  %s()", String(info->name)));
            }
        });
    }

    return KorkApi::ConsoleValue::makeUnsigned(0);
}

KorkApi::ConsoleValue KorkScriptVMHost::bridge_object_find_object(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) {
    if (target == nullptr || argc != 3) {
        return KorkApi::ConsoleValue::makeUnsigned(0);
    }

    Object *resolved = resolve_object_reference(target, console_value_to_string(argv[2]));
    if (resolved == nullptr) {
        return KorkApi::ConsoleValue::makeUnsigned(0);
    }

    get_or_create_vm_object(resolved, nullptr);
    return KorkApi::ConsoleValue::makeUnsigned(ensure_sim_object_id(resolved));
}

KorkApi::ConsoleValue KorkScriptVMHost::bridge_object_get_parent(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) {
    if (target == nullptr || argc != 2) {
        return KorkApi::ConsoleValue::makeUnsigned(0);
    }

    Node *node = Object::cast_to<Node>(target);
    if (node == nullptr) {
        return KorkApi::ConsoleValue::makeUnsigned(0);
    }

    Node *parent = node->get_parent();
    if (parent == nullptr) {
        return KorkApi::ConsoleValue::makeUnsigned(0);
    }

    get_or_create_vm_object(parent, nullptr);
    return KorkApi::ConsoleValue::makeUnsigned(ensure_sim_object_id(parent));
}

KorkApi::ConsoleValue KorkScriptVMHost::bridge_object_get_object(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) {
    if (target == nullptr || argc != 3) {
        return KorkApi::ConsoleValue::makeUnsigned(0);
    }

    Node *node = Object::cast_to<Node>(target);
    if (node == nullptr) {
        return KorkApi::ConsoleValue::makeUnsigned(0);
    }

    const int64_t index = parse_script_argument(argv[2]);
    if (index < 0 || index >= node->get_child_count()) {
        return KorkApi::ConsoleValue::makeUnsigned(0);
    }

    Node *child = node->get_child(static_cast<int32_t>(index));
    if (child == nullptr) {
        return KorkApi::ConsoleValue::makeUnsigned(0);
    }

    get_or_create_vm_object(child, nullptr);
    return KorkApi::ConsoleValue::makeUnsigned(ensure_sim_object_id(child));
}

KorkApi::ConsoleValue KorkScriptVMHost::bridge_object_get_count(Object *target, int32_t argc, KorkApi::ConsoleValue argv[]) const {
    if (target == nullptr || argc != 2) {
        return KorkApi::ConsoleValue::makeUnsigned(0);
    }

    Node *node = Object::cast_to<Node>(target);
    if (node == nullptr) {
        return KorkApi::ConsoleValue::makeUnsigned(0);
    }

    return KorkApi::ConsoleValue::makeUnsigned(static_cast<uint64_t>(node->get_child_count()));
}

KorkApi::ConsoleValue KorkScriptVMHost::bridge_global_trig(int32_t argc, KorkApi::ConsoleValue argv[], real_t (*fn)(real_t)) const {
    if (argc != 2 || fn == nullptr) {
        return KorkApi::ConsoleValue::makeNumber(0.0);
    }

    const real_t value = static_cast<real_t>(parse_script_argument(argv[1]));
    return KorkApi::ConsoleValue::makeNumber(static_cast<double>(fn(value)));
}

KorkApi::ConsoleValue KorkScriptVMHost::bridge_global_get_word(int32_t argc, KorkApi::ConsoleValue argv[]) const {
    if (argc != 3) {
        return KorkApi::ConsoleValue::makeString("");
    }

    const String text = console_value_to_string(argv[1]);
    const int64_t index = parse_script_argument(argv[2]);
    if (index < 0) {
        return KorkApi::ConsoleValue::makeString("");
    }

    PackedStringArray words = text.split(" ", false);
    PackedStringArray filtered_words;
    for (int i = 0; i < words.size(); ++i) {
        const String word = words[i].strip_edges();
        if (!word.is_empty()) {
            filtered_words.push_back(word);
        }
    }

    if (index >= filtered_words.size()) {
        return KorkApi::ConsoleValue::makeString("");
    }

    const CharString utf8 = filtered_words[static_cast<int32_t>(index)].utf8();
    KorkApi::ConsoleValue buffer = vm_->getStringInZone(KorkApi::ConsoleValue::ZoneReturn, static_cast<uint32_t>(utf8.length() + 1));
    char *out = static_cast<char *>(buffer.evaluatePtr(vm_->getAllocBase()));
    if (out != nullptr) {
        std::memcpy(out, utf8.get_data(), static_cast<size_t>(utf8.length() + 1));
    }
    return buffer;
}

KorkApi::ConsoleValue KorkScriptVMHost::bridge_global_is_object(int32_t argc, KorkApi::ConsoleValue argv[]) const {
    if (argc != 2) {
        return KorkApi::ConsoleValue::makeUnsigned(0);
    }

    const String handle = console_value_to_string(argv[1]).strip_edges();
    if (handle.is_empty() || handle == "0") {
        return KorkApi::ConsoleValue::makeUnsigned(0);
    }

    Object *resolved = resolve_object_reference(nullptr, handle);
    return KorkApi::ConsoleValue::makeUnsigned(resolved != nullptr ? 1 : 0);
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
            return KorkApi::ConsoleValue::makeUnsigned(static_cast<bool>(value) ? 1 : 0);
        }
        case Variant::INT:
        {
            return KorkApi::ConsoleValue::makeUnsigned(static_cast<uint64_t>(static_cast<int64_t>(value)));
        }
        case Variant::FLOAT:
        {
            return KorkApi::ConsoleValue::makeNumber(static_cast<double>(value));
        }
        case Variant::VECTOR2: {
            KorkApi::ConsoleValue out = vm_->getTypeInZone(KorkApi::ConsoleValue::ZoneReturn, vector2_type_id_, sizeof(Vector2));
            Vector2 *dst = static_cast<Vector2 *>(out.evaluatePtr(vm_->getAllocBase()));
            if (dst != nullptr) {
                *dst = static_cast<Vector2>(value);
            }
            return out;
        }
        case Variant::VECTOR3: {
            KorkApi::ConsoleValue out = vm_->getTypeInZone(KorkApi::ConsoleValue::ZoneReturn, vector3_type_id_, sizeof(Vector3));
            Vector3 *dst = static_cast<Vector3 *>(out.evaluatePtr(vm_->getAllocBase()));
            if (dst != nullptr) {
                *dst = static_cast<Vector3>(value);
            }
            return out;
        }
        case Variant::VECTOR4: {
            KorkApi::ConsoleValue out = vm_->getTypeInZone(KorkApi::ConsoleValue::ZoneReturn, vector4_type_id_, sizeof(Vector4));
            Vector4 *dst = static_cast<Vector4 *>(out.evaluatePtr(vm_->getAllocBase()));
            if (dst != nullptr) {
                *dst = static_cast<Vector4>(value);
            }
            return out;
        }
        case Variant::COLOR: {
            KorkApi::ConsoleValue out = vm_->getTypeInZone(KorkApi::ConsoleValue::ZoneReturn, color_type_id_, sizeof(Color));
            Color *dst = static_cast<Color *>(out.evaluatePtr(vm_->getAllocBase()));
            if (dst != nullptr) {
                *dst = static_cast<Color>(value);
            }
            return out;
        }
        case Variant::NIL:
        {
            return KorkApi::ConsoleValue();
        }
        default: {
            const CharString utf8 = value.stringify().utf8();
            KorkApi::ConsoleValue buffer = vm_->getStringInZone(KorkApi::ConsoleValue::ZoneReturn, static_cast<uint32_t>(utf8.length() + 1));
            char *out = static_cast<char *>(buffer.evaluatePtr(vm_->getAllocBase()));
            if (out != nullptr) {
                std::memcpy(out, utf8.get_data(), static_cast<size_t>(utf8.length() + 1));
            }
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
            return out;
        }
        case Variant::VECTOR3: {
            KorkApi::ConsoleValue out = vm_->getTypeFunc(vector3_type_id_, sizeof(Vector3));
            Vector3 *dst = static_cast<Vector3 *>(out.evaluatePtr(vm_->getAllocBase()));
            if (dst != nullptr) {
                *dst = static_cast<Vector3>(value);
            }
            return out;
        }
        case Variant::VECTOR4: {
            KorkApi::ConsoleValue out = vm_->getTypeFunc(vector4_type_id_, sizeof(Vector4));
            Vector4 *dst = static_cast<Vector4 *>(out.evaluatePtr(vm_->getAllocBase()));
            if (dst != nullptr) {
                *dst = static_cast<Vector4>(value);
            }
            return out;
        }
        case Variant::COLOR: {
            KorkApi::ConsoleValue out = vm_->getTypeFunc(color_type_id_, sizeof(Color));
            Color *dst = static_cast<Color *>(out.evaluatePtr(vm_->getAllocBase()));
            if (dst != nullptr) {
                *dst = static_cast<Color>(value);
            }
            return out;
        }
        case Variant::OBJECT: {
            Object *object = value.operator Object *();
            if (object != nullptr) {
                return KorkApi::ConsoleValue::makeUnsigned(ensure_sim_object_id(object));
            }
            return KorkApi::ConsoleValue();
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
