/**************************************************************************/
/*  audio_editor_plugin.cpp												  */
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

#include "audio_editor_debugger.h"

#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/text_edit.h"
#include "scene/gui/control.h"
#include "scene/gui/tree.h"
#include "editor/editor_scale.h"

void AudioEditorDebugger::_bind_methods() {
}

bool AudioEditorDebugger::has_capture(const String& p_capture) const {
	return p_capture == "audio";
}

bool AudioEditorDebugger::capture(const String& p_msg, const Array& p_data, int p_index) {

	if (p_msg == "audio:play") {

		uint64_t id = p_data[0];
		AudioInfo info;
		info.instance_id = id;
		info.instance_path = p_data[1];
		info.stream_path = p_data[2];
		info.type = p_data[3].operator unsigned char();

		player_map[id] = info;

		dirty = true;
		return true;
	}

	if (p_msg == "audio:stop") {
		uint64_t id = p_data[0];
		if (player_map.erase(id)) {
			dirty = true;
		}
		return true;
	}

	if (p_msg == "audio:update"){
		uint64_t id = p_data[0];
		auto info = player_map.getptr(id);
		if (info) {
			info->volume = p_data[1];
			info->bus = p_data[2];
			info->playback_position = p_data[3];
			dirty = true;
		}
		return true;
	}

	if (p_msg == "audio:relative_position") {
		uint64_t id = p_data[0];
		auto info = player_map.getptr(id);
		if (info) {
			info->direction = p_data[1];
			dirty = true;
		}
		return true;
	}

	return false;
}

void AudioEditorDebugger::setup_session(int p_session_id) {

	Ref<EditorDebuggerSession> session = get_session(p_session_id);
	ERR_FAIL_COND(session.is_null());

	VBoxContainer *audio = memnew(VBoxContainer);
	audio->set_name(TTR("Audio"));
	audio->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	audio->set_v_size_flags(Control::SIZE_EXPAND_FILL);

	tree = memnew(Tree);
	tree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	tree->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	audio->add_child(tree);


	tree->set_columns(7);
	tree->set_column_titles_visible(true);
	tree->set_column_title(0, TTR("Type"));
	tree->set_column_title(1, TTR("Source"));
	tree->set_column_title(2, TTR("Stream"));
	tree->set_column_title(3, TTR("Bus"));
	tree->set_column_title(4, TTR("Volume"));
	tree->set_column_title(5, TTR("Playback Time"));
	tree->set_column_title(6, TTR("Direction"));
	tree->set_hide_root(true);

	//tree->set_column_custom_minimum_width(3, 100 * EDSCALE);
	//tree->set_hide_root(true);
	
	refresh_timer = memnew(Timer);
	refresh_timer->set_wait_time(0.5);
	refresh_timer->connect("timeout", callable_mp(this, &AudioEditorDebugger::refresh_display));
	refresh_timer->set_autostart(true);
	audio->add_child(refresh_timer);

	session->add_session_tab(audio);

}

void AudioEditorDebugger::refresh_display() {
	if (!dirty) {
		return;
	}

	dirty = false;
	String out = "Total Playing: " + itos(player_map.size()) + "\n";

	tree->clear();
	auto root = tree->create_item();

	auto type1 = tree->create_item(root);
	type1->set_text(0, "2D");
	auto type2 = tree->create_item(root);
	type2->set_text(0, "3D");
	auto type0 = tree->create_item(root);
	type0->set_text(0, "Other");

	for (auto x : player_map) {
		auto info = x.value;
		auto parent = type0;
		if (info.type == 1) {
			parent = type1;
		} else if (info.type == 2) {
			parent = type2;
		}

		TreeItem *it = tree->create_item(parent);
		it->set_text(0, itos(info.type));
		it->set_text(1, info.instance_path);
		it->set_text(2, info.stream_path);
		it->set_text(3, info.bus.operator String());
		it->set_text(4, String::num_real(info.volume));
		it->set_text(5, String::num_real(info.playback_position));
		it->set_text(6, info.direction.operator String());
	}

}
