/**************************************************************************/
/*  gdscript_transpiler.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "gdscript_function.h"

#include "core/string/ustring.h"
#include "core/templates/hash_set.h"

// Fetch source line p_ln (1-based); empty if out of range.
static String _src_line(const Vector<String> &p_lines, int p_ln) {
	return (p_ln >= 1 && p_ln <= p_lines.size()) ? p_lines[p_ln - 1] : String();
}

// Map a bytecode address to a C++ `Variant *` expression.
// self=s[0], nil=s[2], args/locals=s[n]; constants via gf; members via inst.
static String _addr(int p_addr) {
	const int idx = p_addr & GDScriptFunction::ADDR_MASK;
	switch (p_addr >> GDScriptFunction::ADDR_BITS) {
		case GDScriptFunction::ADDR_TYPE_STACK:
			return "(&s[" + itos(idx) + "])";
		case GDScriptFunction::ADDR_TYPE_CONSTANT:
			return "gf->gds2cpp_constant_ptr(" + itos(idx) + ")";
		case GDScriptFunction::ADDR_TYPE_MEMBER:
			return "inst->gds2cpp_member_ptr(" + itos(idx) + ")";
	}
	return "((Variant *)nullptr)";
}

// Instruction size for the opcodes we support (some are variable-length).
// Returns 0 for unsupported opcodes.
static int _instr_size(const int *p_code, int p_ip) {
	switch (GDScriptFunction::Opcode(p_code[p_ip])) {
		case GDScriptFunction::OPCODE_JUMP_TO_DEF_ARGUMENT:
			return 1; // always jumps; occupies a single slot
		case GDScriptFunction::OPCODE_LINE:
		case GDScriptFunction::OPCODE_ASSIGN_NULL:
		case GDScriptFunction::OPCODE_ASSIGN_TRUE:
		case GDScriptFunction::OPCODE_ASSIGN_FALSE:
		case GDScriptFunction::OPCODE_RETURN:
		case GDScriptFunction::OPCODE_JUMP:
			return 2;
		case GDScriptFunction::OPCODE_ASSIGN:
		case GDScriptFunction::OPCODE_JUMP_IF:
		case GDScriptFunction::OPCODE_JUMP_IF_NOT:
		case GDScriptFunction::OPCODE_SET_MEMBER:
		case GDScriptFunction::OPCODE_STORE_GLOBAL:
			return 3;
		case GDScriptFunction::OPCODE_RETURN_TYPED_BUILTIN:
			return 3;
		case GDScriptFunction::OPCODE_TYPE_TEST_BUILTIN:
		case GDScriptFunction::OPCODE_TYPE_TEST_NATIVE:
		case GDScriptFunction::OPCODE_GET_NAMED_VALIDATED:
		case GDScriptFunction::OPCODE_GET_NAMED:
		case GDScriptFunction::OPCODE_ASSIGN_TYPED_BUILTIN:
		case GDScriptFunction::OPCODE_ASSIGN_TYPED_NATIVE:
			return 4;
		case GDScriptFunction::OPCODE_TYPE_TEST_ARRAY:
			return 6;
		case GDScriptFunction::OPCODE_TYPE_TEST_DICTIONARY:
			return 9;
		case GDScriptFunction::OPCODE_OPERATOR_VALIDATED:
		case GDScriptFunction::OPCODE_GET_KEYED_VALIDATED:
		case GDScriptFunction::OPCODE_SET_KEYED_VALIDATED:
		case GDScriptFunction::OPCODE_ITERATE_BEGIN:
		case GDScriptFunction::OPCODE_ITERATE_BEGIN_ARRAY:
		case GDScriptFunction::OPCODE_ITERATE_BEGIN_DICTIONARY:
		case GDScriptFunction::OPCODE_ITERATE:
		case GDScriptFunction::OPCODE_ITERATE_ARRAY:
		case GDScriptFunction::OPCODE_ITERATE_DICTIONARY:
			return 5;
		case GDScriptFunction::OPCODE_OPERATOR:
			return 7 + (int)(sizeof(Variant::ValidatedOperatorEvaluator) / sizeof(int));
		case GDScriptFunction::OPCODE_CALL:
		case GDScriptFunction::OPCODE_CALL_RETURN:
			return p_code[p_ip + 1] + 4;
		case GDScriptFunction::OPCODE_CALL_BUILTIN_TYPE_VALIDATED:
		case GDScriptFunction::OPCODE_CALL_UTILITY:
		case GDScriptFunction::OPCODE_CALL_METHOD_BIND:
		case GDScriptFunction::OPCODE_CALL_METHOD_BIND_RET:
		case GDScriptFunction::OPCODE_CONSTRUCT_VALIDATED:
			return p_code[p_ip + 1] + 4;
		case GDScriptFunction::OPCODE_CONSTRUCT_DICTIONARY:
		case GDScriptFunction::OPCODE_CONSTRUCT_ARRAY:
			return p_code[p_ip + 1] + 3;
		default:
			return 0; // unsupported -> function stays interpreted
	}
}

String GDScriptFunction::transpile_to_cpp(const String &p_cpp_class, const String &p_cpp_func, const Vector<String> &p_source_lines, bool &r_ok) const {
	r_ok = true;

	// --- Pass 1: verify all opcodes are supported + collect jump targets. ---
	HashSet<int> jump_targets;
	String trace;
	int min_line = 0, max_line = 0; // source span of this function (via OPCODE_LINE).
	for (int ip = 0; ip < _code_size;) {
		Opcode op = Opcode(_code_ptr[ip]);
		if (op == OPCODE_END) {
			break;
		}
		if (op == OPCODE_LINE) {
			int ln = _code_ptr[ip + 1];
			if (min_line == 0 || ln < min_line) {
				min_line = ln;
			}
			if (ln > max_line) {
				max_line = ln;
			}
		}
		int size = _instr_size(_code_ptr, ip);
		if (size == 0) {
			r_ok = false;
			// Report the first unsupported opcode + the walk trace (to catch misalignment).
			return "OPCODE_" + itos((int)op) + " @ip=" + itos(ip) + " trace=[" + trace + "]";
		}
		trace += itos(ip) + ":" + itos((int)op) + " ";
		if (op == OPCODE_JUMP) {
			jump_targets.insert(_code_ptr[ip + 1]);
		} else if (op == OPCODE_JUMP_IF || op == OPCODE_JUMP_IF_NOT) {
			jump_targets.insert(_code_ptr[ip + 2]);
		} else if (op == OPCODE_ITERATE_BEGIN || op == OPCODE_ITERATE_BEGIN_ARRAY || op == OPCODE_ITERATE_BEGIN_DICTIONARY ||
				op == OPCODE_ITERATE || op == OPCODE_ITERATE_ARRAY || op == OPCODE_ITERATE_DICTIONARY) {
			jump_targets.insert(_code_ptr[ip + 4]);
		} else if (op == OPCODE_JUMP_TO_DEF_ARGUMENT) {
			for (int d = 0; d <= _default_arg_count; d++) {
				jump_targets.insert(_default_arg_ptr[d]);
			}
		}
		ip += size;
	}

	// --- Pass 2: emit the C++ body. ---
	String b; // body
	for (int ip = 0; ip < _code_size;) {
		if (jump_targets.has(ip)) {
			b += "L" + itos(ip) + ":;\n";
		}
		Opcode op = Opcode(_code_ptr[ip]);
		switch (op) {
			case OPCODE_END: {
				ip = _code_size;
			} break;
			case OPCODE_LINE: {
				// Interleave the matching GDScript source line as a comment.
				String t = _src_line(p_source_lines, _code_ptr[ip + 1]).strip_edges();
				if (!t.is_empty()) {
					b += "\t// " + t + "\n";
				}
				ip += 2;
			} break;
			case OPCODE_ASSIGN: {
				b += "\t*" + _addr(_code_ptr[ip + 1]) + " = *" + _addr(_code_ptr[ip + 2]) + ";\n";
				ip += 3;
			} break;
			case OPCODE_ASSIGN_NULL: {
				b += "\t*" + _addr(_code_ptr[ip + 1]) + " = Variant();\n";
				ip += 2;
			} break;
			case OPCODE_ASSIGN_TRUE: {
				b += "\t*" + _addr(_code_ptr[ip + 1]) + " = true;\n";
				ip += 2;
			} break;
			case OPCODE_ASSIGN_FALSE: {
				b += "\t*" + _addr(_code_ptr[ip + 1]) + " = false;\n";
				ip += 2;
			} break;
			case OPCODE_RETURN: {
				b += "\treturn *" + _addr(_code_ptr[ip + 1]) + ";\n";
				ip += 2;
			} break;
			case OPCODE_JUMP: {
				b += "\tgoto L" + itos(_code_ptr[ip + 1]) + ";\n";
				ip += 2;
			} break;
			case OPCODE_JUMP_IF: {
				b += "\tif (bool(*" + _addr(_code_ptr[ip + 1]) + ")) goto L" + itos(_code_ptr[ip + 2]) + ";\n";
				ip += 3;
			} break;
			case OPCODE_JUMP_IF_NOT: {
				b += "\tif (!bool(*" + _addr(_code_ptr[ip + 1]) + ")) goto L" + itos(_code_ptr[ip + 2]) + ";\n";
				ip += 3;
			} break;
			case OPCODE_CALL:
			case OPCODE_CALL_RETURN: {
				// Faithful: base->callp(name, args, argc, ret, err) — same as the VM handler.
				const bool ret = (op == OPCODE_CALL_RETURN);
				const int iac = _code_ptr[ip + 1];
				const int argc = _code_ptr[ip + 2 + iac];
				const int methodname_idx = _code_ptr[ip + 3 + iac];
				const int base_addr = _code_ptr[ip + 2 + argc];
				b += "\t{\n";
				if (argc > 0) {
					b += "\t\tconst Variant *ca[] = { ";
					for (int i = 0; i < argc; i++) {
						if (i) {
							b += ", ";
						}
						b += _addr(_code_ptr[ip + 2 + i]);
					}
					b += " };\n";
				}
				b += "\t\tVariant cret; Callable::CallError ce;\n";
				b += "\t\t" + _addr(base_addr) + "->callp(gf->get_global_name(" + itos(methodname_idx) + "), " + (argc > 0 ? "ca" : "nullptr") + ", " + itos(argc) + ", cret, ce);\n";
				if (ret) {
					const int target_addr = _code_ptr[ip + 3 + argc];
					b += "\t\t*" + _addr(target_addr) + " = cret;\n";
				}
				b += "\t}\n";
				ip += iac + 4;
			} break;
			case OPCODE_OPERATOR: {
				const int opr = _code_ptr[ip + 4];
				b += "\t{ bool valid; Variant::evaluate((Variant::Operator)" + itos(opr) + ", *" + _addr(_code_ptr[ip + 1]) + ", *" + _addr(_code_ptr[ip + 2]) + ", *" + _addr(_code_ptr[ip + 3]) + ", valid); }\n";
				ip += 7 + (int)(sizeof(Variant::ValidatedOperatorEvaluator) / sizeof(int));
			} break;
			case OPCODE_OPERATOR_VALIDATED: {
				b += "\tgf->gds2cpp_operator_func(" + itos(_code_ptr[ip + 4]) + ")(" + _addr(_code_ptr[ip + 1]) + ", " + _addr(_code_ptr[ip + 2]) + ", " + _addr(_code_ptr[ip + 3]) + ");\n";
				ip += 5;
			} break;
			case OPCODE_TYPE_TEST_BUILTIN: {
				b += "\t*" + _addr(_code_ptr[ip + 1]) + " = (" + _addr(_code_ptr[ip + 2]) + "->get_type() == (Variant::Type)" + itos(_code_ptr[ip + 3]) + ");\n";
				ip += 4;
			} break;
			case OPCODE_TYPE_TEST_NATIVE: {
				b += "\t{ Object *o = " + _addr(_code_ptr[ip + 2]) + "->operator Object *(); *" + _addr(_code_ptr[ip + 1]) + " = (o && ClassDB::is_parent_class(o->get_class_name(), gf->get_global_name(" + itos(_code_ptr[ip + 3]) + "))); }\n";
				ip += 4;
			} break;
			case OPCODE_TYPE_TEST_ARRAY: {
				b += "\t{ Variant *val = " + _addr(_code_ptr[ip + 2]) + "; bool result = false;\n";
				b += "\t\tif (val->get_type() == Variant::ARRAY) { Array arr = *val; result = arr.get_typed_builtin() == (uint32_t)" + itos(_code_ptr[ip + 4]) + " && arr.get_typed_class_name() == gf->get_global_name(" + itos(_code_ptr[ip + 5]) + ") && arr.get_typed_script() == *" + _addr(_code_ptr[ip + 3]) + "; }\n";
				b += "\t\t*" + _addr(_code_ptr[ip + 1]) + " = result; }\n";
				ip += 6;
			} break;
			case OPCODE_TYPE_TEST_DICTIONARY: {
				b += "\t{ Variant *val = " + _addr(_code_ptr[ip + 2]) + "; bool result = false;\n";
				b += "\t\tif (val->get_type() == Variant::DICTIONARY) { Dictionary d = *val; result = d.get_typed_key_builtin() == (uint32_t)" + itos(_code_ptr[ip + 5]) + " && d.get_typed_key_class_name() == gf->get_global_name(" + itos(_code_ptr[ip + 6]) + ") && d.get_typed_key_script() == *" + _addr(_code_ptr[ip + 3]) + " && d.get_typed_value_builtin() == (uint32_t)" + itos(_code_ptr[ip + 7]) + " && d.get_typed_value_class_name() == gf->get_global_name(" + itos(_code_ptr[ip + 8]) + ") && d.get_typed_value_script() == *" + _addr(_code_ptr[ip + 4]) + "; }\n";
				b += "\t\t*" + _addr(_code_ptr[ip + 1]) + " = result; }\n";
				ip += 9;
			} break;
			case OPCODE_GET_KEYED_VALIDATED: {
				b += "\t{ bool valid; gf->gds2cpp_keyed_getter(" + itos(_code_ptr[ip + 4]) + ")(" + _addr(_code_ptr[ip + 1]) + ", " + _addr(_code_ptr[ip + 2]) + ", " + _addr(_code_ptr[ip + 3]) + ", &valid); }\n";
				ip += 5;
			} break;
			case OPCODE_SET_KEYED_VALIDATED: {
				b += "\t{ bool valid; gf->gds2cpp_keyed_setter(" + itos(_code_ptr[ip + 4]) + ")(" + _addr(_code_ptr[ip + 1]) + ", " + _addr(_code_ptr[ip + 2]) + ", " + _addr(_code_ptr[ip + 3]) + ", &valid); }\n";
				ip += 5;
			} break;
			case OPCODE_GET_NAMED_VALIDATED: {
				b += "\tgf->gds2cpp_getter(" + itos(_code_ptr[ip + 3]) + ")(" + _addr(_code_ptr[ip + 1]) + ", " + _addr(_code_ptr[ip + 2]) + ");\n";
				ip += 4;
			} break;
			case OPCODE_SET_MEMBER: {
				b += "\t{ bool valid; ClassDB::set_property(inst->get_owner(), gf->get_global_name(" + itos(_code_ptr[ip + 2]) + "), *" + _addr(_code_ptr[ip + 1]) + ", &valid); }\n";
				ip += 3;
			} break;
			case OPCODE_ASSIGN_TYPED_BUILTIN: {
				const String dst = _addr(_code_ptr[ip + 1]);
				const String src = _addr(_code_ptr[ip + 2]);
				const int vt = _code_ptr[ip + 3];
				b += "\t{ Variant *td = " + dst + "; const Variant *ts = " + src + "; Variant::Type vt = (Variant::Type)" + itos(vt) + ";\n";
				b += "\t\tif (ts->get_type() != vt && Variant::can_convert_strict(ts->get_type(), vt)) { Callable::CallError ce; Variant::construct(vt, *td, &ts, 1, ce); } else { *td = *ts; } }\n";
				ip += 4;
			} break;
			case OPCODE_STORE_GLOBAL: {
				b += "\t*" + _addr(_code_ptr[ip + 1]) + " = GDScriptLanguage::get_singleton()->get_global_array()[" + itos(_code_ptr[ip + 2]) + "];\n";
				ip += 3;
			} break;
			case OPCODE_CONSTRUCT_DICTIONARY: {
				const int iac = _code_ptr[ip + 1];
				const int argc = _code_ptr[ip + 2 + iac];
				b += "\t{\n\t\tDictionary d;\n";
				for (int i = 0; i < argc; i++) {
					b += "\t\td[*" + _addr(_code_ptr[ip + 2 + i * 2]) + "] = *" + _addr(_code_ptr[ip + 2 + i * 2 + 1]) + ";\n";
				}
				b += "\t\tVariant *dd = " + _addr(_code_ptr[ip + 2 + argc * 2]) + "; *dd = Variant(); *dd = d;\n\t}\n";
				ip += iac + 3;
			} break;
			case OPCODE_CALL_BUILTIN_TYPE_VALIDATED: {
				const int iac = _code_ptr[ip + 1];
				const int argc = _code_ptr[ip + 2 + iac];
				const int method_idx = _code_ptr[ip + 3 + iac];
				b += "\t{\n";
				if (argc > 0) {
					b += "\t\tconst Variant *ca[] = { ";
					for (int i = 0; i < argc; i++) {
						if (i) {
							b += ", ";
						}
						b += _addr(_code_ptr[ip + 2 + i]);
					}
					b += " };\n";
				}
				b += "\t\tgf->gds2cpp_builtin_method(" + itos(method_idx) + ")(" + _addr(_code_ptr[ip + 2 + argc]) + ", " + (argc > 0 ? "ca" : "nullptr") + ", " + itos(argc) + ", " + _addr(_code_ptr[ip + 2 + argc + 1]) + ");\n";
				b += "\t}\n";
				ip += iac + 4;
			} break;
			case OPCODE_GET_NAMED: {
				// VM: src = code[ip+1], dst = code[ip+2]  ->  dst = src[name]
				b += "\t{ bool valid; *" + _addr(_code_ptr[ip + 2]) + " = " + _addr(_code_ptr[ip + 1]) + "->get_named(gf->get_global_name(" + itos(_code_ptr[ip + 3]) + "), valid); }\n";
				ip += 4;
			} break;
			case OPCODE_RETURN_TYPED_BUILTIN: {
				const String r = _addr(_code_ptr[ip + 1]);
				const int rt = _code_ptr[ip + 2];
				b += "\t{ const Variant *rr = " + r + "; Variant::Type rt = (Variant::Type)" + itos(rt) + ";\n";
				b += "\t\tif (rr->get_type() != rt) { Callable::CallError ce; Variant rv; if (Variant::can_convert_strict(rr->get_type(), rt)) { Variant::construct(rt, rv, &rr, 1, ce); } else { Variant::construct(rt, rv, nullptr, 0, ce); } return rv; }\n";
				b += "\t\treturn *rr; }\n";
				ip += 3;
			} break;
			case OPCODE_CONSTRUCT_ARRAY: {
				const int iac = _code_ptr[ip + 1];
				const int argc = _code_ptr[ip + 2 + iac];
				b += "\t{\n\t\tArray a; a.resize(" + itos(argc) + ");\n";
				for (int i = 0; i < argc; i++) {
					b += "\t\ta[" + itos(i) + "] = *" + _addr(_code_ptr[ip + 2 + i]) + ";\n";
				}
				b += "\t\tVariant *dd = " + _addr(_code_ptr[ip + 2 + argc]) + "; *dd = Variant(); *dd = a;\n\t}\n";
				ip += iac + 3;
			} break;
			case OPCODE_CALL_UTILITY: {
				const int iac = _code_ptr[ip + 1];
				const int argc = _code_ptr[ip + 2 + iac];
				const int fn_idx = _code_ptr[ip + 3 + iac];
				b += "\t{\n";
				if (argc > 0) {
					b += "\t\tconst Variant *ca[] = { ";
					for (int i = 0; i < argc; i++) {
						if (i) {
							b += ", ";
						}
						b += _addr(_code_ptr[ip + 2 + i]);
					}
					b += " };\n";
				}
				b += "\t\tCallable::CallError ce; Variant::call_utility_function(gf->get_global_name(" + itos(fn_idx) + "), " + _addr(_code_ptr[ip + 2 + argc]) + ", " + (argc > 0 ? "ca" : "nullptr") + ", " + itos(argc) + ", ce);\n";
				b += "\t}\n";
				ip += iac + 4;
			} break;
			case OPCODE_CALL_METHOD_BIND:
			case OPCODE_CALL_METHOD_BIND_RET: {
				const bool ret = (op == OPCODE_CALL_METHOD_BIND_RET);
				const int iac = _code_ptr[ip + 1];
				const int argc = _code_ptr[ip + 2 + iac];
				const int method_idx = _code_ptr[ip + 3 + iac];
				b += "\t{\n";
				if (argc > 0) {
					b += "\t\tconst Variant *ca[] = { ";
					for (int i = 0; i < argc; i++) {
						if (i) {
							b += ", ";
						}
						b += _addr(_code_ptr[ip + 2 + i]);
					}
					b += " };\n";
				}
				b += "\t\tObject *bo = " + _addr(_code_ptr[ip + 2 + argc]) + "->operator Object *();\n";
				b += "\t\tCallable::CallError ce;\n";
				b += "\t\tVariant tr = gf->gds2cpp_method(" + itos(method_idx) + ")->call(bo, " + (argc > 0 ? "ca" : "nullptr") + ", " + itos(argc) + ", ce);\n";
				if (ret) {
					b += "\t\t*" + _addr(_code_ptr[ip + 2 + argc + 1]) + " = tr;\n";
				}
				b += "\t}\n";
				ip += iac + 4;
			} break;
			case OPCODE_ITERATE_BEGIN:
			case OPCODE_ITERATE_BEGIN_ARRAY:
			case OPCODE_ITERATE_BEGIN_DICTIONARY: {
				// Generic iter protocol (works for array/dict/any).
				b += "\t{ Variant *ctr = " + _addr(_code_ptr[ip + 1]) + "; Variant *cont = " + _addr(_code_ptr[ip + 2]) + "; bool valid;\n";
				b += "\t\t*ctr = Variant();\n";
				b += "\t\tif (!cont->iter_init(*ctr, valid)) goto L" + itos(_code_ptr[ip + 4]) + ";\n";
				b += "\t\t*" + _addr(_code_ptr[ip + 3]) + " = cont->iter_get(*ctr, valid); }\n";
				ip += 5;
			} break;
			case OPCODE_ITERATE:
			case OPCODE_ITERATE_ARRAY:
			case OPCODE_ITERATE_DICTIONARY: {
				b += "\t{ Variant *ctr = " + _addr(_code_ptr[ip + 1]) + "; Variant *cont = " + _addr(_code_ptr[ip + 2]) + "; bool valid;\n";
				b += "\t\tif (!cont->iter_next(*ctr, valid)) goto L" + itos(_code_ptr[ip + 4]) + ";\n";
				b += "\t\t*" + _addr(_code_ptr[ip + 3]) + " = cont->iter_get(*ctr, valid); }\n";
				ip += 5;
			} break;
			case OPCODE_ASSIGN_TYPED_NATIVE: {
				// Release path: type checks are debug-only; the effect is a plain assign.
				b += "\t*" + _addr(_code_ptr[ip + 1]) + " = *" + _addr(_code_ptr[ip + 2]) + ";\n";
				ip += 4;
			} break;
			case OPCODE_CONSTRUCT_VALIDATED: {
				const int iac = _code_ptr[ip + 1];
				const int argc = _code_ptr[ip + 2 + iac];
				const int ctor_idx = _code_ptr[ip + 3 + iac];
				b += "\t{\n";
				if (argc > 0) {
					b += "\t\tconst Variant *ca[] = { ";
					for (int i = 0; i < argc; i++) {
						if (i) {
							b += ", ";
						}
						b += _addr(_code_ptr[ip + 2 + i]);
					}
					b += " };\n";
				}
				b += "\t\tgf->gds2cpp_constructor(" + itos(ctor_idx) + ")(" + _addr(_code_ptr[ip + 2 + argc]) + ", " + (argc > 0 ? "ca" : "nullptr") + ");\n";
				b += "\t}\n";
				ip += iac + 4;
			} break;
			case OPCODE_JUMP_TO_DEF_ARGUMENT: {
				// Route to the default-arg init block for the number of omitted args.
				b += "\t{ int defarg = (p_argc < " + itos(_argument_count) + ") ? (" + itos(_argument_count) + " - p_argc) : 0;\n";
				b += "\t\tif (defarg > " + itos(_default_arg_count) + ") defarg = " + itos(_default_arg_count) + ";\n";
				b += "\t\tswitch (defarg) {\n";
				for (int d = 0; d <= _default_arg_count; d++) {
					b += "\t\t\tcase " + itos(d) + ": goto L" + itos(_default_arg_ptr[d]) + ";\n";
				}
				b += "\t\t} }\n";
				ip += 1;
			} break;
			default: {
				r_ok = false;
				return "OPCODE_" + itos((int)op);
			}
		}
	}

	// --- Assemble the function. ---
	String out;

	// Block comment: the whole matching GDScript function (decl line .. last body line).
	if (!p_source_lines.is_empty() && min_line > 0) {
		int decl_line = min_line;
		for (int l = min_line; l >= 1; l--) {
			String t = _src_line(p_source_lines, l).strip_edges();
			if (t.begins_with("func ") || t.begins_with("static func ")) {
				decl_line = l;
				break;
			}
		}
		out += "// " + String("-").repeat(76) + "\n";
		for (int l = decl_line; l <= max_line; l++) {
			out += "// " + _src_line(p_source_lines, l) + "\n";
		}
		out += "// " + String("-").repeat(76) + "\n";
	}

	out += "Variant " + p_cpp_class + "::" + p_cpp_func + "(GDScriptInstance *inst, GDScriptFunction *gf, const Variant **p_args, int p_argc) {\n";
	out += "\tVariant s[" + itos(_stack_size > 0 ? _stack_size : 1) + "];\n";
	out += "\ts[0] = inst ? Variant(inst->get_owner()) : Variant();\n";
	out += "\tfor (int i = 0; i < " + itos(_argument_count) + " && i < p_argc; i++) { s[3 + i] = *p_args[i]; }\n";
	out += b;
	out += "\treturn Variant();\n";
	out += "}\n";
	return out;
}
