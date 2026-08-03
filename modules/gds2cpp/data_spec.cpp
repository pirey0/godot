/**************************************************************************/
/*  data_spec.cpp                                                         */
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

#include "data_spec.h"

#include "core/variant/dictionary.h"
#include "core/variant/variant_internal.h"
#include "modules/gdscript/gdscript.h"

// values is member #15 (Dictionary). property/s args are typed String in the source.
static constexpr int M_values = 15;

// func has(property:String) -> bool: return values.has(property)
Variant Data_spec::has(GDScriptInstance *inst, GDScriptFunction *, const Variant **args, int) {
	const Dictionary *values = VariantInternal::get_dictionary(inst->gds2cpp_member_ptr(M_values));
	return values->has(*args[0]);
}

// func ofOr(property:String, default):
//   if values.has(property): return values[property]
//   return values.get(property.to_lower(), default)
Variant Data_spec::ofOr(GDScriptInstance *inst, GDScriptFunction *, const Variant **args, int) {
	Dictionary *values = VariantInternal::get_dictionary(inst->gds2cpp_member_ptr(M_values));
	const Variant *property = args[0];
	if (values->has(*property)) {
		return (*values)[*property];
	}
	const String plow = VariantInternal::get_string(property)->to_lower();
	return values->get(plow, *args[1]);
}

// func startCaptialized(s:String): return s.substr(0,1).to_upper() + s.substr(1, s.length()-1)
Variant Data_spec::startCaptialized(GDScriptInstance *, GDScriptFunction *, const Variant **args, int) {
	const String *s = VariantInternal::get_string(args[0]);
	return s->substr(0, 1).to_upper() + s->substr(1, s->length() - 1);
}

Data_spec::Fn Data_spec::lookup(const StringName &p_name) {
	if (p_name == StringName("has")) {
		return &Data_spec::has;
	}
	if (p_name == StringName("ofOr")) {
		return &Data_spec::ofOr;
	}
	if (p_name == StringName("startCaptialized")) {
		return &Data_spec::startCaptialized;
	}
	return nullptr;
}
