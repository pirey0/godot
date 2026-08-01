/**************************************************************************/
/*  batch_load_token.h                                                    */
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
#include "core/os/mutex.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/dictionary.h"

class BatchLoadToken : public RefCounted {
	GDCLASS(BatchLoadToken, RefCounted);

public:
	enum Status {
		STATUS_QUEUED,
		STATUS_SCANNING,
		STATUS_LOADING,
		STATUS_DONE,
		STATUS_FAILED,
	};

private:
	SafeNumeric<int> status;
	SafeNumeric<int> completed_count;
	SafeNumeric<int> total_count;

	mutable Mutex results_mutex;
	Dictionary results;

protected:
	static void _bind_methods();

public:
	void set_status(Status p_status) { status.set((int)p_status); }
	Status get_status() const { return (Status)status.get(); }

	void set_total(int p_total) { total_count.set(p_total); }
	void mark_items_completed(int p_count) { completed_count.add(p_count); }
	float get_progress() const;

	void set_results(const Dictionary &p_results);
	Dictionary get_results() const;

	BatchLoadToken() {
		status.set(STATUS_QUEUED);
		completed_count.set(0);
		total_count.set(0);
	}
};

VARIANT_ENUM_CAST(BatchLoadToken::Status);
