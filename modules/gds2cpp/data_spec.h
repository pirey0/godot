/**************************************************************************/
/*  data_spec.h                                                           */
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

#pragma once
// Type-SPECIALIZED transpile target (hand-written to represent what an automated
// type-inference pass would emit): native Dictionary/String ops, no Variant
// dispatch, minimal boxing. Same signature as the faithful Data_gen for the harness.
#include "core/string/string_name.h"
#include "core/variant/variant.h"

class GDScriptInstance;
class GDScriptFunction;

struct Data_spec {
	typedef Variant (*Fn)(GDScriptInstance *, GDScriptFunction *, const Variant **, int);
	static Fn lookup(const StringName &p_name);
	static Variant has(GDScriptInstance *, GDScriptFunction *, const Variant **, int);
	static Variant ofOr(GDScriptInstance *, GDScriptFunction *, const Variant **, int);
	static Variant startCaptialized(GDScriptInstance *, GDScriptFunction *, const Variant **, int);
};
