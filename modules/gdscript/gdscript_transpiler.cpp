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
			return 3;
		case GDScriptFunction::OPCODE_CALL:
		case GDScriptFunction::OPCODE_CALL_RETURN: {
			int iac = p_code[p_ip + 1]; // instruction arg count
			return iac + 4;
		}
		default:
			return 0; // unsupported -> function stays interpreted
	}
}

String GDScriptFunction::transpile_to_cpp(const String &p_cpp_class, const String &p_cpp_func, bool &r_ok) const {
	r_ok = true;

	// --- Pass 1: verify all opcodes are supported + collect jump targets. ---
	HashSet<int> jump_targets;
	for (int ip = 0; ip < _code_size;) {
		Opcode op = Opcode(_code_ptr[ip]);
		if (op == OPCODE_END) {
			break;
		}
		int size = _instr_size(_code_ptr, ip);
		if (size == 0) {
			r_ok = false;
			// Report the first unsupported opcode so callers can prioritize.
			return "OPCODE_" + itos((int)op);
		}
		if (op == OPCODE_JUMP) {
			jump_targets.insert(_code_ptr[ip + 1]);
		} else if (op == OPCODE_JUMP_IF || op == OPCODE_JUMP_IF_NOT) {
			jump_targets.insert(_code_ptr[ip + 2]);
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
			default: {
				r_ok = false;
				return "OPCODE_" + itos((int)op);
			}
		}
	}

	// --- Assemble the function. ---
	String out;
	out += "Variant " + p_cpp_class + "::" + p_cpp_func + "(GDScriptInstance *inst, GDScriptFunction *gf, const Variant **p_args, int p_argc) {\n";
	out += "\tVariant s[" + itos(_stack_size > 0 ? _stack_size : 1) + "];\n";
	out += "\ts[0] = inst ? Variant(inst->get_owner()) : Variant();\n";
	out += "\tfor (int i = 0; i < " + itos(_argument_count) + " && i < p_argc; i++) { s[3 + i] = *p_args[i]; }\n";
	out += b;
	out += "\treturn Variant();\n";
	out += "}\n";
	return out;
}
