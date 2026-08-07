/**************************************************************************/
/*  batch_load_token.cpp                                                  */
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

#include "batch_load_token.h"

void BatchLoadToken::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_status"), &BatchLoadToken::get_status);
	ClassDB::bind_method(D_METHOD("get_progress"), &BatchLoadToken::get_progress);
	ClassDB::bind_method(D_METHOD("get_results"), &BatchLoadToken::get_results);

	ADD_SIGNAL(MethodInfo("completed"));

	BIND_ENUM_CONSTANT(STATUS_QUEUED);
	BIND_ENUM_CONSTANT(STATUS_SCANNING);
	BIND_ENUM_CONSTANT(STATUS_LOADING);
	BIND_ENUM_CONSTANT(STATUS_DONE);
	BIND_ENUM_CONSTANT(STATUS_FAILED);
}

float BatchLoadToken::get_progress() const {
	Status s = get_status();
	if (s == STATUS_QUEUED || s == STATUS_SCANNING) {
		return 0.0f;
	}

	int total = total_count.get();
	if (total == 0) {
		return 100.0f;
	}
	return (float)completed_count.get() / (float)total * 100.0f;
}

void BatchLoadToken::set_results(const Dictionary &p_results) {
	MutexLock lock(results_mutex);
	results = p_results;
}

Dictionary BatchLoadToken::get_results() const {
	MutexLock lock(results_mutex);
	return results;
}
