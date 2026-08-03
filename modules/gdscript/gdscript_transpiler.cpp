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

#include "gdscript.h"

#include "core/object/script_language.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/pair.h"

// Fetch source line p_ln (1-based); empty if out of range.
static String _src_line(const Vector<String> &p_lines, int p_ln) {
	return (p_ln >= 1 && p_ln <= p_lines.size()) ? p_lines[p_ln - 1] : String();
}

// --- Naming context (single-threaded dev tool: set per transpile_to_cpp call). ---
// _addr() and _gname() consult these to emit readable names instead of raw indices.
static const HashMap<int, String> *s_member_names = nullptr; // member idx -> "M_values"
static const HashMap<int, String> *s_slot_names = nullptr; // stack idx -> local/arg name
static const HashMap<int, String> *s_gname_ident = nullptr; // global-name idx -> "GN_has"
static HashSet<int> *s_used_slots = nullptr; // stack idxs (>=3) referenced, for alias decls
static HashSet<int> *s_used_gnames = nullptr; // global-name idxs referenced, for the enum

// Turn an arbitrary string into a valid C++ identifier fragment.
static String _ident(const String &p_s) {
	String r;
	for (int i = 0; i < p_s.length(); i++) {
		char32_t c = p_s[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
		r += ok ? String::chr(c) : String("_");
	}
	if (r.is_empty()) {
		r = "_";
	}
	if (r[0] >= '0' && r[0] <= '9') {
		r = "_" + r;
	}
	return r;
}

// Reserved names the emitter itself uses as locals; a source var that collides gets a '_'.
static bool _is_reserved_local(const String &p_n) {
	static const char *kw[] = { "s", "inst", "gf", "cret", "ce", "ca", "valid", "a", "d", "dd",
		"o", "val", "td", "ts", "vt", "rr", "rt", "rv", "bo", "tr", "ctr", "cont", "defarg", "i", "l",
		// C++ keywords / literals a GDScript identifier might match:
		"default", "new", "delete", "class", "struct", "this", "operator", "template", "typename",
		"namespace", "public", "private", "protected", "virtual", "register", "const", "static",
		"return", "if", "else", "for", "while", "do", "switch", "case", "break", "continue", "goto",
		"true", "false", "nullptr", "int", "float", "double", "char", "bool", "void", "and", "or",
		"not", "xor", "auto", "using", "friend", "union", "enum", "typedef", nullptr };
	for (int i = 0; kw[i]; i++) {
		if (p_n == kw[i]) {
			return true;
		}
	}
	return false;
}

// Map a bytecode address to a C++ `Variant *` expression.
// self=s[0], nil=s[2]; args/locals get readable names (else t<n>); members named; constants shown.
static String _addr(int p_addr) {
	const int idx = p_addr & GDScriptFunction::ADDR_MASK;
	switch (p_addr >> GDScriptFunction::ADDR_BITS) {
		case GDScriptFunction::ADDR_TYPE_STACK:
			if (idx >= GDScriptFunction::FIXED_ADDRESSES_MAX) {
				if (s_used_slots) {
					s_used_slots->insert(idx);
				}
				if (s_slot_names && s_slot_names->has(idx)) {
					return "(&" + (*s_slot_names)[idx] + ")";
				}
				return "(&t" + itos(idx) + ")";
			}
			return "(&s[" + itos(idx) + "])";
		case GDScriptFunction::ADDR_TYPE_CONSTANT:
			return "gf->gds2cpp_constant_ptr(" + itos(idx) + ")";
		case GDScriptFunction::ADDR_TYPE_MEMBER:
			if (s_member_names && s_member_names->has(idx)) {
				return "inst->gds2cpp_member_ptr(" + (*s_member_names)[idx] + ")";
			}
			return "inst->gds2cpp_member_ptr(" + itos(idx) + ")";
	}
	return "((Variant *)nullptr)";
}

// --- Type context (set per transpile_to_cpp call) for native specialization. ---
static const GDScriptFunction *s_fn = nullptr; // for get_constant()
static const HashMap<int, Variant::Type> *s_slot_types = nullptr; // stack idx -> known builtin type
static const HashMap<int, Variant::Type> *s_member_types = nullptr; // member idx -> declared builtin type
static const HashMap<StringName, Pair<int, String>> *s_self_methods = nullptr; // devirt-eligible self methods
static String s_cpp_class; // owning class C++ name (for direct calls to g_gf/fn_)
static Gds2cppStats *s_stats = nullptr; // optional per-call-site outcome tally
static HashMap<int, const GDScript *> s_slot_classes; // stack idx -> GDScript class (from arg types)
static const HashMap<int, const GDScript *> *s_member_classes = nullptr; // member idx -> class
static const HashMap<const GDScript *, HashMap<StringName, Gds2cppTarget>> *s_resolver = nullptr;

// GDScript class held by an operand (STACK arg or MEMBER), or nullptr if unknown.
static const GDScript *_operand_class(int p_addr) {
	const int idx = p_addr & GDScriptFunction::ADDR_MASK;
	switch (p_addr >> GDScriptFunction::ADDR_BITS) {
		case GDScriptFunction::ADDR_TYPE_STACK:
			return s_slot_classes.has(idx) ? s_slot_classes[idx] : nullptr;
		case GDScriptFunction::ADDR_TYPE_MEMBER:
			return (s_member_classes && s_member_classes->has(idx)) ? (*s_member_classes)[idx] : nullptr;
	}
	return nullptr;
}

// Statically-known builtin Variant::Type of an operand, or NIL if unknown.
static Variant::Type _operand_type(int p_addr) {
	const int idx = p_addr & GDScriptFunction::ADDR_MASK;
	switch (p_addr >> GDScriptFunction::ADDR_BITS) {
		case GDScriptFunction::ADDR_TYPE_STACK:
			return (s_slot_types && s_slot_types->has(idx)) ? (*s_slot_types)[idx] : Variant::NIL;
		case GDScriptFunction::ADDR_TYPE_CONSTANT:
			return s_fn ? s_fn->get_constant(idx).get_type() : Variant::NIL;
		case GDScriptFunction::ADDR_TYPE_MEMBER:
			return (s_member_types && s_member_types->has(idx)) ? (*s_member_types)[idx] : Variant::NIL;
	}
	return Variant::NIL;
}

// Native C++ expression for a value-returning builtin method on a known-type base,
// or "" if not in the curated set (caller then falls back to dynamic dispatch).
// p_base / p_args are `Variant *` expressions; the result is assignable to a Variant.
static String _native_builtin_expr(Variant::Type p_type, const String &p_method, int p_argc, const String &p_base, const Vector<String> &p_args) {
	String d;
	switch (p_type) {
		case Variant::DICTIONARY: {
			const String b = "VariantInternal::get_dictionary(" + p_base + ")";
			if (p_method == "has" && p_argc == 1) {
				return b + "->has(*" + p_args[0] + ")";
			}
			if (p_method == "get" && p_argc == 2) {
				return b + "->get(*" + p_args[0] + ", *" + p_args[1] + ")";
			}
			if (p_method == "size" && p_argc == 0) {
				return b + "->size()";
			}
			if (p_method == "is_empty" && p_argc == 0) {
				return b + "->is_empty()";
			}
			if (p_method == "keys" && p_argc == 0) {
				return b + "->keys()";
			}
			if (p_method == "values" && p_argc == 0) {
				return b + "->values()";
			}
		} break;
		case Variant::ARRAY: {
			const String b = "VariantInternal::get_array(" + p_base + ")";
			if (p_method == "size" && p_argc == 0) {
				return b + "->size()";
			}
			if (p_method == "is_empty" && p_argc == 0) {
				return b + "->is_empty()";
			}
			if (p_method == "has" && p_argc == 1) {
				return b + "->has(*" + p_args[0] + ")";
			}
		} break;
		case Variant::STRING: {
			const String b = "VariantInternal::get_string(" + p_base + ")";
			if (p_method == "to_lower" && p_argc == 0) {
				return b + "->to_lower()";
			}
			if (p_method == "to_upper" && p_argc == 0) {
				return b + "->to_upper()";
			}
			if (p_method == "length" && p_argc == 0) {
				return b + "->length()";
			}
			if (p_method == "strip_edges" && p_argc == 0) {
				return b + "->strip_edges()";
			}
		} break;
		default:
			break;
	}
	return d;
}

// A named reference to a global name (method/property), recording the use for the enum.
static String _gname(int p_idx) {
	if (s_used_gnames) {
		s_used_gnames->insert(p_idx);
	}
	if (s_gname_ident && s_gname_ident->has(p_idx)) {
		return "gf->get_global_name(" + (*s_gname_ident)[p_idx] + ")";
	}
	return "gf->get_global_name(" + itos(p_idx) + ")";
}

// Instruction size for the opcodes we support (some are variable-length).
// Returns 0 for unsupported opcodes.
static int _instr_size(const int *p_code, int p_ip) {
	const int op = p_code[p_ip];
	// TYPE_ADJUST_* family: opcode + 1 slot operand.
	if (op >= GDScriptFunction::OPCODE_TYPE_ADJUST_BOOL && op <= GDScriptFunction::OPCODE_TYPE_ADJUST_PACKED_VECTOR4_ARRAY) {
		return 2;
	}
	// Typed ITERATE_BEGIN_* family (RANGE is longer: counter/from/to/step/iter + jump).
	if (op >= GDScriptFunction::OPCODE_ITERATE_BEGIN && op <= GDScriptFunction::OPCODE_ITERATE_BEGIN_RANGE) {
		return op == GDScriptFunction::OPCODE_ITERATE_BEGIN_RANGE ? 7 : 5;
	}
	// Typed ITERATE_* family (RANGE: counter/to/step/iter + jump).
	if (op >= GDScriptFunction::OPCODE_ITERATE && op <= GDScriptFunction::OPCODE_ITERATE_RANGE) {
		return op == GDScriptFunction::OPCODE_ITERATE_RANGE ? 6 : 5;
	}
	switch (GDScriptFunction::Opcode(op)) {
		case GDScriptFunction::OPCODE_JUMP_TO_DEF_ARGUMENT:
			return 1; // always jumps; occupies a single slot
		case GDScriptFunction::OPCODE_BREAKPOINT:
			return 1;
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
		case GDScriptFunction::OPCODE_JUMP_IF_SHARED:
		case GDScriptFunction::OPCODE_SET_MEMBER:
		case GDScriptFunction::OPCODE_GET_MEMBER:
		case GDScriptFunction::OPCODE_STORE_GLOBAL:
		case GDScriptFunction::OPCODE_STORE_NAMED_GLOBAL:
		case GDScriptFunction::OPCODE_ASSERT:
			return 3;
		case GDScriptFunction::OPCODE_RETURN_TYPED_BUILTIN:
		case GDScriptFunction::OPCODE_RETURN_TYPED_NATIVE:
		case GDScriptFunction::OPCODE_RETURN_TYPED_SCRIPT:
			return 3;
		case GDScriptFunction::OPCODE_TYPE_TEST_BUILTIN:
		case GDScriptFunction::OPCODE_TYPE_TEST_NATIVE:
		case GDScriptFunction::OPCODE_TYPE_TEST_SCRIPT:
		case GDScriptFunction::OPCODE_GET_NAMED_VALIDATED:
		case GDScriptFunction::OPCODE_GET_NAMED:
		case GDScriptFunction::OPCODE_SET_NAMED:
		case GDScriptFunction::OPCODE_SET_NAMED_VALIDATED:
		case GDScriptFunction::OPCODE_GET_KEYED:
		case GDScriptFunction::OPCODE_SET_KEYED:
		case GDScriptFunction::OPCODE_CAST_TO_BUILTIN:
		case GDScriptFunction::OPCODE_CAST_TO_NATIVE:
		case GDScriptFunction::OPCODE_CAST_TO_SCRIPT:
		case GDScriptFunction::OPCODE_ASSIGN_TYPED_BUILTIN:
		case GDScriptFunction::OPCODE_ASSIGN_TYPED_NATIVE:
		case GDScriptFunction::OPCODE_ASSIGN_TYPED_SCRIPT:
			return 4;
		case GDScriptFunction::OPCODE_GET_STATIC_VARIABLE:
		case GDScriptFunction::OPCODE_SET_STATIC_VARIABLE:
			return 4;
		case GDScriptFunction::OPCODE_GET_INDEXED_VALIDATED:
		case GDScriptFunction::OPCODE_SET_INDEXED_VALIDATED:
		case GDScriptFunction::OPCODE_RETURN_TYPED_ARRAY:
			return 5;
		case GDScriptFunction::OPCODE_TYPE_TEST_ARRAY:
		case GDScriptFunction::OPCODE_ASSIGN_TYPED_ARRAY:
			return 6;
		case GDScriptFunction::OPCODE_RETURN_TYPED_DICTIONARY:
			return 8;
		case GDScriptFunction::OPCODE_TYPE_TEST_DICTIONARY:
		case GDScriptFunction::OPCODE_ASSIGN_TYPED_DICTIONARY:
			return 9;
		case GDScriptFunction::OPCODE_CONSTRUCT:
			return p_code[p_ip + 1] + 4;
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
		case GDScriptFunction::OPCODE_CALL_UTILITY_VALIDATED:
		case GDScriptFunction::OPCODE_CALL_GDSCRIPT_UTILITY:
		case GDScriptFunction::OPCODE_CALL_METHOD_BIND:
		case GDScriptFunction::OPCODE_CALL_METHOD_BIND_RET:
		case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN:
		case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_NO_RETURN:
		case GDScriptFunction::OPCODE_CALL_NATIVE_STATIC:
		case GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN:
		case GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_NO_RETURN:
		case GDScriptFunction::OPCODE_CONSTRUCT_VALIDATED:
			return p_code[p_ip + 1] + 4;
		case GDScriptFunction::OPCODE_CALL_BUILTIN_STATIC:
		case GDScriptFunction::OPCODE_CONSTRUCT_TYPED_ARRAY:
			return p_code[p_ip + 1] + 5;
		case GDScriptFunction::OPCODE_CONSTRUCT_TYPED_DICTIONARY:
			return p_code[p_ip + 1] + 7;
		case GDScriptFunction::OPCODE_CONSTRUCT_DICTIONARY:
		case GDScriptFunction::OPCODE_CONSTRUCT_ARRAY:
			return p_code[p_ip + 1] + 3;
		default:
			return 0; // unsupported -> function stays interpreted
	}
}

void GDScriptFunction::gds2cpp_slot_names(HashMap<int, String> &r_names) const {
	// Walk the debug scope records: a slot with a single owning identifier across
	// the whole function gets that name; a slot shared by differently-named locals
	// is left unnamed (emitted as a temporary) to avoid a misleading alias.
	HashMap<int, HashSet<String>> owners;
	for (const StackDebug &sd : stack_debug) {
		if (sd.added && sd.pos >= FIXED_ADDRESSES_MAX) {
			owners[sd.pos].insert(String(sd.identifier));
		}
	}
	HashSet<String> taken;
	for (const KeyValue<int, HashSet<String>> &E : owners) {
		if (E.value.size() != 1) {
			continue; // reused slot -> keep as temporary
		}
		String name = _ident(*E.value.begin());
		if (_is_reserved_local(name)) {
			name += "_";
		}
		while (taken.has(name)) {
			name += "_";
		}
		taken.insert(name);
		r_names[E.key] = name;
	}
}

String GDScriptFunction::transpile_to_cpp(const String &p_cpp_class, const String &p_cpp_func, const Vector<String> &p_source_lines, const HashMap<int, String> &p_member_names, const HashMap<int, Variant::Type> &p_member_types, const HashMap<StringName, Pair<int, String>> &p_self_methods, const HashMap<int, const GDScript *> &p_member_classes, const HashMap<const GDScript *, HashMap<StringName, Gds2cppTarget>> &p_resolver, bool &r_ok, Gds2cppStats *r_stats) const {
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
		if (size <= 0 || ip + size > _code_size) {
			r_ok = false;
			// Report the first unsupported / misaligned opcode + the walk trace.
			return "OPCODE_" + itos((int)op) + " @ip=" + itos(ip) + " size=" + itos(size) + " trace=[" + trace + "]";
		}
		trace += itos(ip) + ":" + itos((int)op) + " ";
		if (op == OPCODE_JUMP) {
			jump_targets.insert(_code_ptr[ip + 1]);
		} else if (op == OPCODE_JUMP_IF || op == OPCODE_JUMP_IF_NOT || op == OPCODE_JUMP_IF_SHARED) {
			jump_targets.insert(_code_ptr[ip + 2]);
		} else if (op == OPCODE_ITERATE_BEGIN_RANGE) {
			jump_targets.insert(_code_ptr[ip + 6]);
		} else if (op == OPCODE_ITERATE_RANGE) {
			jump_targets.insert(_code_ptr[ip + 5]);
		} else if ((op >= OPCODE_ITERATE_BEGIN && op <= OPCODE_ITERATE_BEGIN_RANGE) || (op >= OPCODE_ITERATE && op <= OPCODE_ITERATE_RANGE)) {
			jump_targets.insert(_code_ptr[ip + 4]);
		} else if (op == OPCODE_JUMP_TO_DEF_ARGUMENT) {
			for (int d = 0; d <= _default_arg_count; d++) {
				jump_targets.insert(_default_arg_ptr[d]);
			}
		}
		ip += size;
	}

	// --- Build naming context, then set it for _addr()/_gname() during pass 2. ---
	HashMap<int, String> gname_ident; // global-name idx -> "GN_<name>"
	{
		HashSet<String> taken;
		for (int i = 0; i < _global_names_count; i++) {
			String nm = "GN_" + _ident(String(get_global_name(i)));
			while (taken.has(nm)) {
				nm += "_";
			}
			taken.insert(nm);
			gname_ident[i] = nm;
		}
	}
	HashMap<int, String> slot_names; // stack idx -> arg/local name
	gds2cpp_slot_names(slot_names);

	HashSet<int> used_slots, used_gnames;
	s_member_names = &p_member_names;
	s_slot_names = &slot_names;
	s_gname_ident = &gname_ident;
	s_used_slots = &used_slots;
	s_used_gnames = &used_gnames;

	// Type map: typed temporaries (VM temporary_slots) + declared argument types.
	HashMap<int, Variant::Type> slot_types = gds2cpp_temporary_slots();
	for (int i = 0; i < _argument_count; i++) {
		Variant::Type t = gds2cpp_arg_builtin_type(i);
		if (t != Variant::NIL) {
			slot_types[FIXED_ADDRESSES_MAX + i] = t;
		}
	}
	s_fn = this;
	s_slot_types = &slot_types;
	s_member_types = &p_member_types;
	s_self_methods = &p_self_methods;
	s_cpp_class = p_cpp_class;
	s_stats = r_stats;
	s_member_classes = &p_member_classes;
	s_resolver = &p_resolver;
	// Receiver classes for typed GDScript arguments (slot 3+i).
	s_slot_classes.clear();
	for (int i = 0; i < _argument_count && i < argument_types.size(); i++) {
		const GDScriptDataType &dt = argument_types[i];
		if (dt.kind == GDScriptDataType::GDSCRIPT && dt.script_type) {
			s_slot_classes[FIXED_ADDRESSES_MAX + i] = Object::cast_to<GDScript>(dt.script_type);
		}
	}

	// --- Pass 2: emit the C++ body. ---
	String b; // body
	for (int ip = 0; ip < _code_size;) {
		if (jump_targets.has(ip)) {
			b += "L" + itos(ip) + ":;\n";
		}
		Opcode op = Opcode(_code_ptr[ip]);

		// --- Families handled by range (pre-switch), all faithful. ---
		if (op >= OPCODE_TYPE_ADJUST_BOOL && op <= OPCODE_TYPE_ADJUST_PACKED_VECTOR4_ARRAY) {
			int t = (int)op - (int)OPCODE_TYPE_ADJUST_BOOL + (int)Variant::BOOL;
			const String a = _addr(_code_ptr[ip + 1]);
			b += "\t{ Variant *_a = " + a + "; if (_a->get_type() != (Variant::Type)" + itos(t) + ") { const Variant *_ai = _a; Callable::CallError _ce; Variant _tmp; Variant::construct((Variant::Type)" + itos(t) + ", _tmp, &_ai, 1, _ce); *_a = _tmp; } }\n";
			ip += 2;
			continue;
		}
		if (op == OPCODE_ITERATE_BEGIN_RANGE) {
			b += "\t{ Variant *ctr = " + _addr(_code_ptr[ip + 1]) + "; int64_t _from = (int64_t)*" + _addr(_code_ptr[ip + 2]) + "; int64_t _to = (int64_t)*" + _addr(_code_ptr[ip + 3]) + "; int64_t _step = (int64_t)*" + _addr(_code_ptr[ip + 4]) + ";\n";
			b += "\t\t*ctr = _from;\n";
			b += "\t\tif (!(_from == _to ? false : (_from < _to ? _step > 0 : _step < 0))) goto L" + itos(_code_ptr[ip + 6]) + ";\n";
			b += "\t\t*" + _addr(_code_ptr[ip + 5]) + " = _from; }\n";
			ip += 7;
			continue;
		}
		if (op == OPCODE_ITERATE_RANGE) {
			b += "\t{ Variant *ctr = " + _addr(_code_ptr[ip + 1]) + "; int64_t _to = (int64_t)*" + _addr(_code_ptr[ip + 2]) + "; int64_t _step = (int64_t)*" + _addr(_code_ptr[ip + 3]) + ";\n";
			b += "\t\tint64_t _c = (int64_t)*ctr + _step; *ctr = _c;\n";
			b += "\t\tif ((_step < 0 && _c <= _to) || (_step > 0 && _c >= _to)) goto L" + itos(_code_ptr[ip + 5]) + ";\n";
			b += "\t\t*" + _addr(_code_ptr[ip + 4]) + " = _c; }\n";
			ip += 6;
			continue;
		}
		if (op >= OPCODE_ITERATE_BEGIN && op <= OPCODE_ITERATE_BEGIN_RANGE) { // typed BEGIN -> generic iter protocol
			b += "\t{ Variant *ctr = " + _addr(_code_ptr[ip + 1]) + "; Variant *cont = " + _addr(_code_ptr[ip + 2]) + "; bool valid;\n";
			b += "\t\t*ctr = Variant();\n";
			b += "\t\tif (!cont->iter_init(*ctr, valid)) goto L" + itos(_code_ptr[ip + 4]) + ";\n";
			b += "\t\t*" + _addr(_code_ptr[ip + 3]) + " = cont->iter_get(*ctr, valid); }\n";
			ip += 5;
			continue;
		}
		if (op >= OPCODE_ITERATE && op <= OPCODE_ITERATE_RANGE) { // typed ITERATE -> generic iter protocol
			b += "\t{ Variant *ctr = " + _addr(_code_ptr[ip + 1]) + "; Variant *cont = " + _addr(_code_ptr[ip + 2]) + "; bool valid;\n";
			b += "\t\tif (!cont->iter_next(*ctr, valid)) goto L" + itos(_code_ptr[ip + 4]) + ";\n";
			b += "\t\t*" + _addr(_code_ptr[ip + 3]) + " = cont->iter_get(*ctr, valid); }\n";
			ip += 5;
			continue;
		}

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

				// SPECIALIZATION: if the base is a known builtin type and the method is in the
				// native table, emit a direct C++ call instead of dynamic by-name callp.
				Variant::Type bt = _operand_type(base_addr);
				String native;
				if (bt != Variant::NIL) {
					Vector<String> aexpr;
					for (int i = 0; i < argc; i++) {
						aexpr.push_back(_addr(_code_ptr[ip + 2 + i]));
					}
					native = _native_builtin_expr(bt, String(get_global_name(methodname_idx)), argc, _addr(base_addr), aexpr);
				}
				if (!native.is_empty()) {
					if (s_stats) {
						s_stats->native_calls++;
					}
					if (ret) {
						b += "\t*" + _addr(_code_ptr[ip + 3 + argc]) + " = " + native + "; // native " + String(get_global_name(methodname_idx)) + "\n";
					} else {
						b += "\t" + native + "; // native " + String(get_global_name(methodname_idx)) + "\n";
					}
					ip += iac + 4;
					break;
				}

				// DEVIRT: self-call to a devirt-eligible own method -> direct C++ call.
				const bool is_self = ((base_addr >> ADDR_BITS) == ADDR_TYPE_STACK) && ((base_addr & ADDR_MASK) == ADDR_STACK_SELF);
				const StringName mname = get_global_name(methodname_idx);
				if (is_self && s_self_methods && s_self_methods->has(mname)) {
					if (s_stats) {
						s_stats->devirt_calls++;
					}
					const Pair<int, String> &tgt = (*s_self_methods)[mname];
					String ca;
					if (argc > 0) {
						ca = "\t\tconst Variant *ca[] = { ";
						for (int i = 0; i < argc; i++) {
							ca += (i ? ", " : "") + _addr(_code_ptr[ip + 2 + i]);
						}
						ca += " };\n";
					}
					// g_gf slot referenced by the GF(<method>) macro rather than a magic index.
					const String call = s_cpp_class + "::" + tgt.second + "(inst, GF(" + tgt.second.substr(3) + "), " + (argc > 0 ? "ca" : "nullptr") + ", " + itos(argc) + ")";
					b += "\t{\n" + ca;
					if (ret) {
						b += "\t\t*" + _addr(_code_ptr[ip + 3 + argc]) + " = " + call + "; // devirt " + String(mname) + "\n";
					} else {
						b += "\t\t" + call + "; // devirt " + String(mname) + "\n";
					}
					b += "\t}\n";
					ip += iac + 4;
					break;
				}

				// CROSS-CLASS: typed-receiver call to a devirt-eligible method of a known class.
				if (!is_self && s_resolver) {
					const GDScript *rc = _operand_class(base_addr);
					const HashMap<StringName, Gds2cppTarget> *tm = (rc && s_resolver->has(rc)) ? &(*s_resolver)[rc] : nullptr;
					if (tm && tm->has(mname)) {
						const Gds2cppTarget &tgt = (*tm)[mname];
						if (s_stats) {
							s_stats->cross_calls++;
						}
						String ca;
						if (argc > 0) {
							ca = "\t\tconst Variant *ca[] = { ";
							for (int i = 0; i < argc; i++) {
								ca += (i ? ", " : "") + _addr(_code_ptr[ip + 2 + i]);
							}
							ca += " };\n";
						}
						const String call = tgt.class_cpp + "::" + tgt.fn_cpp + "(_si, " + tgt.class_cpp + "::g_gf[" + itos(tgt.gf_index) + "], " + (argc > 0 ? "ca" : "nullptr") + ", " + itos(argc) + ")";
						b += "\t{\n" + ca;
						b += "\t\tObject *_o = " + _addr(base_addr) + "->operator Object *(); GDScriptInstance *_si = _o ? static_cast<GDScriptInstance *>(_o->get_script_instance()) : nullptr;\n";
						if (ret) {
							b += "\t\t*" + _addr(_code_ptr[ip + 3 + argc]) + " = _si ? " + call + " : Variant(); // cross-class " + String(mname) + "\n";
						} else {
							b += "\t\tif (_si) " + call + "; // cross-class " + String(mname) + "\n";
						}
						b += "\t}\n";
						ip += iac + 4;
						break;
					}
				}

				if (s_stats) {
					s_stats->dynamic_calls++;
				}
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
				b += "\t\t" + _addr(base_addr) + "->callp(" + _gname(methodname_idx) + ", " + (argc > 0 ? "ca" : "nullptr") + ", " + itos(argc) + ", cret, ce);\n";
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
				b += "\t{ Object *o = " + _addr(_code_ptr[ip + 2]) + "->operator Object *(); *" + _addr(_code_ptr[ip + 1]) + " = (o && ClassDB::is_parent_class(o->get_class_name(), " + _gname(_code_ptr[ip + 3]) + ")); }\n";
				ip += 4;
			} break;
			case OPCODE_TYPE_TEST_ARRAY: {
				b += "\t{ Variant *val = " + _addr(_code_ptr[ip + 2]) + "; bool result = false;\n";
				b += "\t\tif (val->get_type() == Variant::ARRAY) { Array arr = *val; result = arr.get_typed_builtin() == (uint32_t)" + itos(_code_ptr[ip + 4]) + " && arr.get_typed_class_name() == " + _gname(_code_ptr[ip + 5]) + " && arr.get_typed_script() == *" + _addr(_code_ptr[ip + 3]) + "; }\n";
				b += "\t\t*" + _addr(_code_ptr[ip + 1]) + " = result; }\n";
				ip += 6;
			} break;
			case OPCODE_TYPE_TEST_DICTIONARY: {
				b += "\t{ Variant *val = " + _addr(_code_ptr[ip + 2]) + "; bool result = false;\n";
				b += "\t\tif (val->get_type() == Variant::DICTIONARY) { Dictionary d = *val; result = d.get_typed_key_builtin() == (uint32_t)" + itos(_code_ptr[ip + 5]) + " && d.get_typed_key_class_name() == " + _gname(_code_ptr[ip + 6]) + " && d.get_typed_key_script() == *" + _addr(_code_ptr[ip + 3]) + " && d.get_typed_value_builtin() == (uint32_t)" + itos(_code_ptr[ip + 7]) + " && d.get_typed_value_class_name() == " + _gname(_code_ptr[ip + 8]) + " && d.get_typed_value_script() == *" + _addr(_code_ptr[ip + 4]) + "; }\n";
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
				b += "\t{ bool valid; ClassDB::set_property(inst->get_owner(), " + _gname(_code_ptr[ip + 2]) + ", *" + _addr(_code_ptr[ip + 1]) + ", &valid); }\n";
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
				if (s_stats) {
					s_stats->validated_calls++;
				}
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
				b += "\t{ bool valid; *" + _addr(_code_ptr[ip + 2]) + " = " + _addr(_code_ptr[ip + 1]) + "->get_named(" + _gname(_code_ptr[ip + 3]) + ", valid); }\n";
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
				b += "\t\tCallable::CallError ce; Variant::call_utility_function(" + _gname(fn_idx) + ", " + _addr(_code_ptr[ip + 2 + argc]) + ", " + (argc > 0 ? "ca" : "nullptr") + ", " + itos(argc) + ", ce);\n";
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
			case OPCODE_GET_MEMBER: {
				b += "\t{ ClassDB::get_property(inst->get_owner(), " + _gname(_code_ptr[ip + 2]) + ", *" + _addr(_code_ptr[ip + 1]) + "); }\n";
				ip += 3;
			} break;
			case OPCODE_GET_KEYED: {
				b += "\t{ bool valid; *" + _addr(_code_ptr[ip + 3]) + " = " + _addr(_code_ptr[ip + 1]) + "->get(*" + _addr(_code_ptr[ip + 2]) + ", &valid); }\n";
				ip += 4;
			} break;
			case OPCODE_SET_KEYED: {
				b += "\t{ bool valid; " + _addr(_code_ptr[ip + 1]) + "->set(*" + _addr(_code_ptr[ip + 2]) + ", *" + _addr(_code_ptr[ip + 3]) + ", &valid); }\n";
				ip += 4;
			} break;
			case OPCODE_GET_INDEXED_VALIDATED: {
				b += "\t{ bool valid; *" + _addr(_code_ptr[ip + 3]) + " = " + _addr(_code_ptr[ip + 1]) + "->get(*" + _addr(_code_ptr[ip + 2]) + ", &valid); }\n";
				ip += 5;
			} break;
			case OPCODE_SET_INDEXED_VALIDATED: {
				b += "\t{ bool valid; " + _addr(_code_ptr[ip + 1]) + "->set(*" + _addr(_code_ptr[ip + 2]) + ", *" + _addr(_code_ptr[ip + 3]) + ", &valid); }\n";
				ip += 5;
			} break;
			case OPCODE_SET_NAMED: {
				b += "\t{ bool valid; " + _addr(_code_ptr[ip + 1]) + "->set_named(" + _gname(_code_ptr[ip + 3]) + ", *" + _addr(_code_ptr[ip + 2]) + ", valid); }\n";
				ip += 4;
			} break;
			case OPCODE_SET_NAMED_VALIDATED: {
				b += "\tgf->gds2cpp_setter(" + itos(_code_ptr[ip + 3]) + ")(" + _addr(_code_ptr[ip + 1]) + ", " + _addr(_code_ptr[ip + 2]) + ");\n";
				ip += 4;
			} break;
			case OPCODE_CAST_TO_BUILTIN: {
				const int t = _code_ptr[ip + 3];
				b += "\t{ const Variant *_s = " + _addr(_code_ptr[ip + 1]) + "; Callable::CallError _ce; Variant::construct((Variant::Type)" + itos(t) + ", *" + _addr(_code_ptr[ip + 2]) + ", &_s, 1, _ce); }\n";
				ip += 4;
			} break;
			case OPCODE_CAST_TO_NATIVE: {
				// x as NativeClass: keep the object if it derives from the class, else null.
				b += "\t{ Object *_o = " + _addr(_code_ptr[ip + 1]) + "->operator Object *(); StringName _cn = " + _addr(_code_ptr[ip + 2]) + "->operator StringName(); *" + _addr(_code_ptr[ip + 3]) + " = (_o && ClassDB::is_parent_class(_o->get_class_name(), _cn)) ? Variant(_o) : Variant(); }\n";
				ip += 4;
			} break;
			case OPCODE_CAST_TO_SCRIPT: {
				// Faithful-simplified: pass the object through (script identity check omitted).
				b += "\t*" + _addr(_code_ptr[ip + 3]) + " = *" + _addr(_code_ptr[ip + 1]) + ";\n";
				ip += 4;
			} break;
			case OPCODE_TYPE_TEST_SCRIPT: {
				b += "\t{ Object *_o = " + _addr(_code_ptr[ip + 2]) + "->operator Object *(); ScriptInstance *_si = _o ? _o->get_script_instance() : nullptr; Object *_exp = " + _addr(_code_ptr[ip + 3]) + "->operator Object *(); *" + _addr(_code_ptr[ip + 1]) + " = (_si && _si->get_script().ptr() == _exp); }\n";
				ip += 4;
			} break;
			case OPCODE_JUMP_IF_SHARED: {
				b += "\tif (" + _addr(_code_ptr[ip + 1]) + "->is_shared()) goto L" + itos(_code_ptr[ip + 2]) + ";\n";
				ip += 3;
			} break;
			case OPCODE_STORE_NAMED_GLOBAL: {
				b += "\t*" + _addr(_code_ptr[ip + 1]) + " = GDScriptLanguage::get_singleton()->get_named_globals_map()[" + _gname(_code_ptr[ip + 2]) + "];\n";
				ip += 3;
			} break;
			case OPCODE_ASSERT: {
				// Release: assertions are stripped; keep as a no-op for faithful behavior.
				ip += 3;
			} break;
			case OPCODE_CALL_UTILITY_VALIDATED: {
				const int iac = _code_ptr[ip + 1];
				const int argc = _code_ptr[ip + 2 + iac];
				const int fn_idx = _code_ptr[ip + 3 + iac];
				b += "\t{\n";
				if (argc > 0) {
					b += "\t\tconst Variant *ca[] = { ";
					for (int i = 0; i < argc; i++) {
						b += (i ? ", " : "") + _addr(_code_ptr[ip + 2 + i]);
					}
					b += " };\n";
				}
				b += "\t\tgf->gds2cpp_utility(" + itos(fn_idx) + ")(" + _addr(_code_ptr[ip + 2 + argc]) + ", " + (argc > 0 ? "ca" : "nullptr") + ", " + itos(argc) + ");\n";
				b += "\t}\n";
				ip += iac + 4;
			} break;
			case OPCODE_CALL_GDSCRIPT_UTILITY: {
				const int iac = _code_ptr[ip + 1];
				const int argc = _code_ptr[ip + 2 + iac];
				const int fn_idx = _code_ptr[ip + 3 + iac];
				b += "\t{\n";
				if (argc > 0) {
					b += "\t\tconst Variant *ca[] = { ";
					for (int i = 0; i < argc; i++) {
						b += (i ? ", " : "") + _addr(_code_ptr[ip + 2 + i]);
					}
					b += " };\n";
				}
				b += "\t\tCallable::CallError _ce; gf->gds2cpp_gds_utility(" + itos(fn_idx) + ")(" + _addr(_code_ptr[ip + 2 + argc]) + ", " + (argc > 0 ? "ca" : "nullptr") + ", " + itos(argc) + ", _ce);\n";
				b += "\t}\n";
				ip += iac + 4;
			} break;
			case OPCODE_CALL_BUILTIN_STATIC: {
				const int iac = _code_ptr[ip + 1];
				const int type = _code_ptr[ip + 2 + iac];
				const int methodname_idx = _code_ptr[ip + 3 + iac];
				const int argc = _code_ptr[ip + 4 + iac];
				b += "\t{\n";
				if (argc > 0) {
					b += "\t\tconst Variant *ca[] = { ";
					for (int i = 0; i < argc; i++) {
						b += (i ? ", " : "") + _addr(_code_ptr[ip + 2 + i]);
					}
					b += " };\n";
				}
				b += "\t\tCallable::CallError _ce; Variant::call_static((Variant::Type)" + itos(type) + ", " + _gname(methodname_idx) + ", " + (argc > 0 ? "ca" : "nullptr") + ", " + itos(argc) + ", *" + _addr(_code_ptr[ip + 2 + argc]) + ", _ce);\n";
				b += "\t}\n";
				ip += iac + 5;
			} break;
			case OPCODE_CONSTRUCT_TYPED_ARRAY: {
				const int iac = _code_ptr[ip + 1];
				const int argc = _code_ptr[ip + 2 + iac];
				const int builtin_type = _code_ptr[ip + 3 + iac];
				const int native_idx = _code_ptr[ip + 4 + iac];
				b += "\t{\n";
				b += "\t\tArray _arr; _arr.set_typed((uint32_t)" + itos(builtin_type) + ", " + _gname(native_idx) + ", *" + _addr(_code_ptr[ip + 2 + argc + 1]) + ");\n";
				b += "\t\t_arr.resize(" + itos(argc) + ");\n";
				for (int i = 0; i < argc; i++) {
					b += "\t\t_arr[" + itos(i) + "] = *" + _addr(_code_ptr[ip + 2 + i]) + ";\n";
				}
				b += "\t\t*" + _addr(_code_ptr[ip + 2 + argc]) + " = _arr;\n";
				b += "\t}\n";
				ip += iac + 5;
			} break;
			case OPCODE_CONSTRUCT: {
				const int iac = _code_ptr[ip + 1];
				const int argc = _code_ptr[ip + 2 + iac];
				const int t = _code_ptr[ip + 3 + iac];
				b += "\t{\n";
				if (argc > 0) {
					b += "\t\tconst Variant *ca[] = { ";
					for (int i = 0; i < argc; i++) {
						b += (i ? ", " : "") + _addr(_code_ptr[ip + 2 + i]);
					}
					b += " };\n";
				}
				b += "\t\tCallable::CallError _ce; Variant::construct((Variant::Type)" + itos(t) + ", *" + _addr(_code_ptr[ip + 2 + argc]) + ", " + (argc > 0 ? "ca" : "nullptr") + ", " + itos(argc) + ", _ce);\n";
				b += "\t}\n";
				ip += iac + 4;
			} break;
			case OPCODE_RETURN_TYPED_NATIVE:
			case OPCODE_RETURN_TYPED_SCRIPT: {
				b += "\treturn *" + _addr(_code_ptr[ip + 1]) + ";\n";
				ip += 3;
			} break;
			case OPCODE_RETURN_TYPED_ARRAY: {
				b += "\treturn *" + _addr(_code_ptr[ip + 1]) + ";\n";
				ip += 5;
			} break;
			case OPCODE_RETURN_TYPED_DICTIONARY: {
				b += "\treturn *" + _addr(_code_ptr[ip + 1]) + ";\n";
				ip += 8;
			} break;
			case OPCODE_ASSIGN_TYPED_SCRIPT: {
				b += "\t*" + _addr(_code_ptr[ip + 1]) + " = *" + _addr(_code_ptr[ip + 2]) + ";\n";
				ip += 4;
			} break;
			case OPCODE_ASSIGN_TYPED_ARRAY: {
				b += "\t*" + _addr(_code_ptr[ip + 1]) + " = *" + _addr(_code_ptr[ip + 2]) + ";\n";
				ip += 6;
			} break;
			case OPCODE_ASSIGN_TYPED_DICTIONARY: {
				b += "\t*" + _addr(_code_ptr[ip + 1]) + " = *" + _addr(_code_ptr[ip + 2]) + ";\n";
				ip += 9;
			} break;
			case OPCODE_GET_STATIC_VARIABLE: {
				b += "\t*" + _addr(_code_ptr[ip + 1]) + " = Object::cast_to<GDScript>(" + _addr(_code_ptr[ip + 2]) + "->operator Object *())->gds2cpp_static_get(" + itos(_code_ptr[ip + 3]) + ");\n";
				ip += 4;
			} break;
			case OPCODE_SET_STATIC_VARIABLE: {
				b += "\tObject::cast_to<GDScript>(" + _addr(_code_ptr[ip + 2]) + "->operator Object *())->gds2cpp_static_set(" + itos(_code_ptr[ip + 3]) + ", *" + _addr(_code_ptr[ip + 1]) + ");\n";
				ip += 4;
			} break;
			case OPCODE_CALL_NATIVE_STATIC: {
				const int iac = _code_ptr[ip + 1];
				const int method_idx = _code_ptr[ip + 2 + iac];
				const int argc = _code_ptr[ip + 3 + iac];
				b += "\t{\n";
				if (argc > 0) {
					b += "\t\tconst Variant *ca[] = { ";
					for (int i = 0; i < argc; i++) {
						b += (i ? ", " : "") + _addr(_code_ptr[ip + 2 + i]);
					}
					b += " };\n";
				}
				b += "\t\tCallable::CallError _ce; *" + _addr(_code_ptr[ip + 2 + argc]) + " = gf->gds2cpp_method(" + itos(method_idx) + ")->call(nullptr, " + (argc > 0 ? "ca" : "nullptr") + ", " + itos(argc) + ", _ce);\n";
				b += "\t}\n";
				ip += iac + 4;
			} break;
			case OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN:
			case OPCODE_CALL_NATIVE_STATIC_VALIDATED_NO_RETURN: {
				const bool ret = (op == OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN);
				const int iac = _code_ptr[ip + 1];
				const int argc = _code_ptr[ip + 2 + iac];
				const int method_idx = _code_ptr[ip + 3 + iac];
				b += "\t{\n";
				if (argc > 0) {
					b += "\t\tconst Variant *ca[] = { ";
					for (int i = 0; i < argc; i++) {
						b += (i ? ", " : "") + _addr(_code_ptr[ip + 2 + i]);
					}
					b += " };\n";
				}
				b += "\t\tCallable::CallError _ce; Variant _r = gf->gds2cpp_method(" + itos(method_idx) + ")->call(nullptr, " + (argc > 0 ? "ca" : "nullptr") + ", " + itos(argc) + ", _ce);\n";
				if (ret) {
					b += "\t\t*" + _addr(_code_ptr[ip + 2 + argc]) + " = _r;\n";
				}
				b += "\t}\n";
				ip += iac + 4;
			} break;
			case OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN:
			case OPCODE_CALL_METHOD_BIND_VALIDATED_NO_RETURN: {
				const bool ret = (op == OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN);
				const int iac = _code_ptr[ip + 1];
				const int argc = _code_ptr[ip + 2 + iac];
				const int method_idx = _code_ptr[ip + 3 + iac];
				b += "\t{\n";
				if (argc > 0) {
					b += "\t\tconst Variant *ca[] = { ";
					for (int i = 0; i < argc; i++) {
						b += (i ? ", " : "") + _addr(_code_ptr[ip + 2 + i]);
					}
					b += " };\n";
				}
				b += "\t\tObject *_o = " + _addr(_code_ptr[ip + 2 + argc]) + "->operator Object *();\n";
				b += "\t\tCallable::CallError _ce; Variant _r = gf->gds2cpp_method(" + itos(method_idx) + ")->call(_o, " + (argc > 0 ? "ca" : "nullptr") + ", " + itos(argc) + ", _ce);\n";
				if (ret) {
					b += "\t\t*" + _addr(_code_ptr[ip + 3 + argc]) + " = _r;\n";
				}
				b += "\t}\n";
				ip += iac + 4;
			} break;
			case OPCODE_CONSTRUCT_TYPED_DICTIONARY: {
				const int iac = _code_ptr[ip + 1];
				const int argc = _code_ptr[ip + 2 + iac];
				const int kt = _code_ptr[ip + 3 + iac];
				const int kn = _code_ptr[ip + 4 + iac];
				const int vt = _code_ptr[ip + 5 + iac];
				const int vn = _code_ptr[ip + 6 + iac];
				b += "\t{\n";
				b += "\t\tDictionary _d; _d.set_typed((uint32_t)" + itos(kt) + ", " + _gname(kn) + ", *" + _addr(_code_ptr[ip + 2 + argc * 2 + 1]) + ", (uint32_t)" + itos(vt) + ", " + _gname(vn) + ", *" + _addr(_code_ptr[ip + 2 + argc * 2 + 2]) + ");\n";
				for (int i = 0; i < argc; i++) {
					b += "\t\t_d[*" + _addr(_code_ptr[ip + 2 + i * 2]) + "] = *" + _addr(_code_ptr[ip + 2 + i * 2 + 1]) + ";\n";
				}
				b += "\t\t*" + _addr(_code_ptr[ip + 2 + argc * 2]) + " = _d;\n";
				b += "\t}\n";
				ip += iac + 7;
			} break;
			default: {
				r_ok = false;
				return "OPCODE_" + itos((int)op);
			}
		}
	}
	// Naming/type context is only consulted during pass 2; drop it now.
	s_member_names = nullptr;
	s_slot_names = nullptr;
	s_gname_ident = nullptr;
	s_used_slots = nullptr;
	s_used_gnames = nullptr;
	s_fn = nullptr;
	s_slot_types = nullptr;
	s_member_types = nullptr;
	s_self_methods = nullptr;
	s_stats = nullptr;
	s_member_classes = nullptr;
	s_resolver = nullptr;
	s_slot_classes.clear();

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

	// Pre-type temporary slots exactly as the VM does at function entry, so validated
	// builtin-method / operator calls write into correctly-typed destinations.
	{
		Vector<int> slots;
		for (const KeyValue<int, Variant::Type> &E : gds2cpp_temporary_slots()) {
			slots.push_back(E.key);
		}
		slots.sort();
		for (int i = 0; i < slots.size(); i++) {
			int slot = slots[i];
			int t = (int)gds2cpp_temporary_slots()[slot];
			out += "\t{ Callable::CallError _ce; Variant::construct((Variant::Type)" + itos(t) + ", s[" + itos(slot) + "], nullptr, 0, _ce); }\n";
		}
	}

	// Named constants for the global names this function references.
	if (!used_gnames.is_empty()) {
		Vector<int> gi;
		for (const int &x : used_gnames) {
			gi.push_back(x);
		}
		gi.sort();
		out += "\tenum { ";
		for (int k = 0; k < gi.size(); k++) {
			out += (k ? ", " : "") + gname_ident[gi[k]] + " = " + itos(gi[k]);
		}
		out += " }; // \"";
		for (int k = 0; k < gi.size(); k++) {
			out += (k ? "\", \"" : "") + String(get_global_name(gi[k]));
		}
		out += "\"\n";
	}
	// Readable aliases for the stack slots this function uses.
	if (!used_slots.is_empty()) {
		Vector<int> si;
		for (const int &x : used_slots) {
			si.push_back(x);
		}
		si.sort();
		for (int k = 0; k < si.size(); k++) {
			int idx = si[k];
			bool named = slot_names.has(idx);
			String nm = named ? slot_names[idx] : ("t" + itos(idx));
			String tag = (idx < FIXED_ADDRESSES_MAX + _argument_count) ? "arg" : (named ? "local" : "temp");
			out += "\tVariant &" + nm + " = s[" + itos(idx) + "]; // " + tag + "\n";
		}
	}

	out += b;
	out += "\treturn Variant();\n";
	out += "}\n";
	return out;
}
