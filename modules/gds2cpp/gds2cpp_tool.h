/**************************************************************************/
/*  gds2cpp_tool.h                                                        */
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

#include "core/object/ref_counted.h"

// gds2cpp: dev tool to drive the GDScript->C++ transpiler over a script and
// report per-function transpilability. Not part of a shipped game.
class Gds2cppTool : public RefCounted {
	GDCLASS(Gds2cppTool, RefCounted);

protected:
	static void _bind_methods();

public:
	// Loads the GDScript at p_path, transpiles each function, and returns a
	// human-readable report (transpiled vs interpreted, first blocking opcode).
	String analyze_script(const String &p_path);
	String analyze_dir(const String &p_root);

	// Returns the generated C++ for the whole script (one translation unit).
	String transpile_script(const String &p_path, const String &p_cpp_class);

	// Writes a compilable <file_base>.{h,cpp} (struct <cpp_class>) into p_out_dir.
	String transpile_module_files(const String &p_path, const String &p_out_dir, const String &p_cpp_class, const String &p_file_base);
};
