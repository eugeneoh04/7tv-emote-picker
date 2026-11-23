// The search box's text, caret and selection, and the usual ways to edit them.
// Drawing and mouse hit testing are done by the App with DirectWrite.
#pragma once

#include <string>
#include <string_view>
#include <vector>

class TextBox {
public:
    const std::wstring& text() const { return text_; }
    size_t caret() const { return caret_; }
    size_t sel_start() const { return caret_ < anchor_ ? caret_ : anchor_; }
    size_t sel_end() const { return caret_ < anchor_ ? anchor_ : caret_; }
    bool has_selection() const { return caret_ != anchor_; }
    std::wstring selected() const { return text_.substr(sel_start(), sel_end() - sel_start()); }

    // Edits return true if the text changed.
    bool insert(std::wstring_view s);  // replaces the selection; line breaks become spaces
    bool erase_back(bool word);        // Backspace (Ctrl: the word before)
    bool erase_forward(bool word);     // Delete (Ctrl: the word after)
    bool undo();
    bool redo();

    void move(int dir, bool word, bool extend);  // arrow keys
    void home(bool extend);
    void end(bool extend);
    void select_all();
    void place(size_t pos, bool extend);  // caret to pos (a click), extend: Shift held / drag
    void select_word(size_t pos);         // double-click

private:
    struct State {
        std::wstring text;
        size_t caret, anchor;
    };
    enum Kind { Other, Typing, Deleting };
    std::wstring text_;
    size_t caret_ = 0, anchor_ = 0;
    std::vector<State> undo_, redo_;
    Kind last_ = Other;

    void before_edit(Kind kind);
    bool replace(size_t from, size_t to, std::wstring_view with, Kind kind);
    size_t step(size_t pos, int dir) const;       // next character boundary
    size_t word_left(size_t pos) const;
    size_t word_right(size_t pos) const;
};
