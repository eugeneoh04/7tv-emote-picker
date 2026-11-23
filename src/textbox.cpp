#include "textbox.h"

#include <cwctype>

namespace {

int char_class(wchar_t c) {
    if (std::iswspace(c)) return 0;
    if (std::iswalnum(c) || c == L'_' || (c >= 0xD800 && c < 0xE000)) return 1;
    return 2;  // punctuation
}

}  // namespace

void TextBox::before_edit(Kind kind) {
    // Typing (or deleting) runs undo as one step.
    if (kind == Other || kind != last_ || undo_.empty()) {
        undo_.push_back({text_, caret_, anchor_});
        if (undo_.size() > 100) undo_.erase(undo_.begin());
    }
    redo_.clear();
    last_ = kind;
}

bool TextBox::replace(size_t from, size_t to, std::wstring_view with, Kind kind) {
    if (from == to && with.empty()) return false;
    before_edit(kind);
    text_.replace(from, to - from, with);
    caret_ = anchor_ = from + with.size();
    return true;
}

bool TextBox::insert(std::wstring_view s) {
    std::wstring clean;
    for (wchar_t c : s) {
        if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
        if (c >= 0x20 && c != 0x7F) clean += c;
    }
    if (text_.size() + clean.size() > 200) clean.resize(text_.size() < 200 ? 200 - text_.size() : 0);
    bool typing = clean.size() == 1 && !has_selection();
    return replace(sel_start(), sel_end(), clean, typing ? Typing : Other);
}

bool TextBox::erase_back(bool word) {
    if (has_selection()) return replace(sel_start(), sel_end(), {}, Other);
    size_t from = word ? word_left(caret_) : step(caret_, -1);
    return replace(from, caret_, {}, word ? Other : Deleting);
}

bool TextBox::erase_forward(bool word) {
    if (has_selection()) return replace(sel_start(), sel_end(), {}, Other);
    size_t to = word ? word_right(caret_) : step(caret_, 1);
    return replace(caret_, to, {}, word ? Other : Deleting);
}

bool TextBox::undo() {
    if (undo_.empty()) return false;
    redo_.push_back({text_, caret_, anchor_});
    auto s = undo_.back();
    undo_.pop_back();
    text_ = s.text, caret_ = s.caret, anchor_ = s.anchor;
    last_ = Other;
    return true;
}

bool TextBox::redo() {
    if (redo_.empty()) return false;
    undo_.push_back({text_, caret_, anchor_});
    auto s = redo_.back();
    redo_.pop_back();
    text_ = s.text, caret_ = s.caret, anchor_ = s.anchor;
    last_ = Other;
    return true;
}

size_t TextBox::step(size_t pos, int dir) const {
    if (dir < 0) {
        if (pos == 0) return 0;
        pos--;
        if (pos > 0 && text_[pos] >= 0xDC00 && text_[pos] < 0xE000 && text_[pos - 1] >= 0xD800 &&
            text_[pos - 1] < 0xDC00)
            pos--;  // both halves of a surrogate pair
        return pos;
    }
    if (pos >= text_.size()) return text_.size();
    pos++;
    if (pos < text_.size() && text_[pos] >= 0xDC00 && text_[pos] < 0xE000 &&
        text_[pos - 1] >= 0xD800 && text_[pos - 1] < 0xDC00)
        pos++;
    return pos;
}

size_t TextBox::word_left(size_t pos) const {
    while (pos > 0 && char_class(text_[pos - 1]) == 0) pos--;
    if (pos == 0) return 0;
    int cls = char_class(text_[pos - 1]);
    while (pos > 0 && char_class(text_[pos - 1]) == cls) pos--;
    return pos;
}

size_t TextBox::word_right(size_t pos) const {
    size_t n = text_.size();
    if (pos < n) {
        int cls = char_class(text_[pos]);
        if (cls != 0)
            while (pos < n && char_class(text_[pos]) == cls) pos++;
    }
    while (pos < n && char_class(text_[pos]) == 0) pos++;
    return pos;
}

void TextBox::move(int dir, bool word, bool extend) {
    last_ = Other;
    if (has_selection() && !extend) {  // an arrow collapses the selection to that side
        caret_ = anchor_ = dir < 0 ? sel_start() : sel_end();
        if (!word) return;
    }
    caret_ = word ? (dir < 0 ? word_left(caret_) : word_right(caret_)) : step(caret_, dir);
    if (!extend) anchor_ = caret_;
}

void TextBox::home(bool extend) {
    last_ = Other;
    caret_ = 0;
    if (!extend) anchor_ = caret_;
}

void TextBox::end(bool extend) {
    last_ = Other;
    caret_ = text_.size();
    if (!extend) anchor_ = caret_;
}

void TextBox::select_all() {
    last_ = Other;
    anchor_ = 0;
    caret_ = text_.size();
}

void TextBox::place(size_t pos, bool extend) {
    last_ = Other;
    caret_ = pos > text_.size() ? text_.size() : pos;
    if (!extend) anchor_ = caret_;
}

void TextBox::select_word(size_t pos) {
    last_ = Other;
    size_t n = text_.size();
    if (pos > n) pos = n;
    size_t probe = pos < n ? pos : (pos ? pos - 1 : 0);
    if (n == 0) return;
    int cls = char_class(text_[probe]);
    size_t a = probe, b = probe;
    while (a > 0 && char_class(text_[a - 1]) == cls) a--;
    while (b < n && char_class(text_[b]) == cls) b++;
    anchor_ = a;
    caret_ = b;
}
