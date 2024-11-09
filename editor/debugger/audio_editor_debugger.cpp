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

#include "editor/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/control.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/text_edit.h"
#include "scene/gui/tree.h"

void AudioEditorDebugger::_bind_methods() {
}

bool AudioEditorDebugger::has_capture(const String &p_capture) const {
	return p_capture == "audio";
}

bool AudioEditorDebugger::capture(const String &p_msg, const Array &p_data, int p_index) {
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

	if (p_msg == "audio:update") {
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

	HBoxContainer *hbox = memnew(HBoxContainer);
	audio->add_child(hbox);

	summary = memnew(Label);
	summary->set_custom_minimum_size(Size2(200, 0));
	hbox->add_child(summary);

	search = memnew(LineEdit);
	search->set_placeholder(TTR("Search"));
	search->connect("text_changed", callable_mp(this, &AudioEditorDebugger::on_search_changed));
	hbox->add_child(search);

	tree = memnew(Tree);
	tree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	tree->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	audio->add_child(tree);

	tree->set_columns(6);
	tree->set_column_titles_visible(true);
	tree->set_column_title(0, TTR("Source"));
	tree->set_column_title(1, TTR("Stream"));
	tree->set_column_title(2, TTR("Bus"));
	tree->set_column_title(3, TTR("Volume"));
	tree->set_column_title(4, TTR("Playback Time"));
	tree->set_column_title(5, TTR("Direction"));
	tree->set_hide_root(true);

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
	int count = player_map.size();
	summary->set_text("Total Playing: " + itos(count));
	tree->clear();

	if (count == 0)
		return;

	auto root = tree->create_item();

	auto type1 = tree->create_item(root);
	type1->set_text(0, "2D");
	auto type2 = tree->create_item(root);
	type2->set_text(0, "3D");
	auto type0 = tree->create_item(root);
	type0->set_text(0, "Other");

	String filter = search->get_text().strip_edges();

	for (auto x : player_map) {
		auto info = x.value;

		if (!filter.is_empty() && !info.instance_path.contains(filter) && !info.stream_path.contains(filter))
			continue;

		auto parent = type0;
		if (info.type == 1) {
			parent = type1;
		} else if (info.type == 2) {
			parent = type2;
		}

		TreeItem *it = tree->create_item(parent);
		it->set_text(0, info.instance_path);
		it->set_text(1, info.stream_path);
		it->set_text(2, info.bus.operator String());
		it->set_text(3, String::num_real(info.volume));
		it->set_text(4, String::num_real(info.playback_position));

		if (info.direction.length_squared() > 0.1) {
			float angle = info.direction.angle();
			if (angle < 0) {
				angle += 2 * Math_PI;
			}
			if (angle < Math_PI * 0.33) {
				it->add_button(5, tree->get_editor_theme_icon(SNAME("ArrowRight")));
			} else if (angle < Math_PI * 0.66) {
				it->add_button(5, tree->get_editor_theme_icon(SNAME("ArrowDown")));
			} else if (angle < Math_PI * 1.33) {
				it->add_button(5, tree->get_editor_theme_icon(SNAME("ArrowLeft")));
			} else if (angle < Math_PI * 1.66) {
				it->add_button(5, tree->get_editor_theme_icon(SNAME("ArrowUp")));
			} else {
				it->add_button(5, tree->get_editor_theme_icon(SNAME("ArrowRight")));
			}
		}
	}
}

void AudioEditorDebugger::_notification(int p_what) {
	switch (p_what) {

		case Node::NOTIFICATION_ENTER_TREE:
			[[fallthrough]];
		case Control::NOTIFICATION_THEME_CHANGED:
			search->set_right_icon(search->get_editor_theme_icon(SNAME("Search")));
			break;
	}
}
