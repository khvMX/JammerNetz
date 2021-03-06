/*
   Copyright (c) 2019 Christof Ruch. All rights reserved.

   Dual licensed: Distributed under Affero GPL license by default, an MIT license is available for purchase
*/

#pragma once

#include "JuceHeader.h"

class ServerSelector : public Component,
	private Button::Listener, private TextEditor::Listener
{
public:
	ServerSelector(std::function<void()> notify);

	virtual void resized() override;

	// Store to and load from settings
	void fromData();
	void toData() const;

private:
	virtual void buttonClicked(Button*) override;
	virtual void textEditorReturnKeyPressed(TextEditor&) override;
	virtual void textEditorFocusLost(TextEditor&) override;

	ToggleButton useLocalhost_;
	Label serverLabel_;
	TextEditor ipAddress_;

	bool localhostSelected_;
	String lastServer_;

	std::function<void()> notify_;
};
