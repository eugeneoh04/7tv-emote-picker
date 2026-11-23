// Previews: downloaded and decoded by a pool of threads, put on the graphics card here.
// Previews download for the rows on screen, then the GIFs of the animated ones there, then
// the next screen down; the rest wait until you scroll to them. Only PREVIEW_DOWNLOADS run
// at once, and whenever one finishes this runs again, so whatever is in view by then goes next.
#include <algorithm>

#include "app.h"

namespace {

std::wstring thumb_url(const wchar_t* pattern, const std::string& id, int s) {
    std::wstring url = pattern;
    url.replace(url.find(L"{id}"), 4, widen(id));
    url.replace(url.find(L"{s}"), 3, std::to_wstring(s));
    return url;
}

D2D1_BITMAP_PROPERTIES1 picture_props() {
    return D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
}

}  // namespace

void App::start_workers() {
    for (int i = 0; i < PREVIEW_DOWNLOADS; i++) std::thread([this] { worker(); }).detach();
}

// A download thread: fetches and decodes one preview at a time. Always reports back, even
// on failure, so the slot frees up.
void App::worker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);  // drawing comes first
    for (;;) {
        Job job;
        {
            std::unique_lock lock(job_mutex);
            job_ready.wait(lock, [&] { return !jobs.empty(); });
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        JobResult r;
        r.bytes = job.bytes;
        if (!r.bytes) {
            const wchar_t* pattern = job.kind == Job::Gif ? THUMB_GIF
                                     : job.animated       ? THUMB_ANIMATED
                                                          : THUMB_STATIC;
            Bytes data;
            std::wstring err;
            if (fetch(thumb_url(pattern, job.id, job.s), "", data, err))
                r.bytes = std::make_shared<const Bytes>(std::move(data));
        }
        if (r.bytes)
            r.ok = job.kind == Job::Gif
                       ? decode_gif(*r.bytes, job.height, job.room, job.max_texture, r.anim)
                       : decode_still(*r.bytes, job.height, job.room, r.still);
        r.job = std::move(job);
        {
            std::lock_guard lock(result_mutex);
            results.push_back(std::move(r));
        }
        PostMessageW(hwnd, WM_APP_RESULTS, 0, 0);
    }
}

void App::submit(Job job) {
    fetching.insert(job.key + (job.kind == Job::Gif ? "g" : "s"));
    {
        std::lock_guard lock(job_mutex);
        jobs.push_back(std::move(job));
    }
    job_ready.notify_one();
}

// The threads put results in a list and wake the window; they're handled here.
void App::drain_results() {
    std::vector<JobResult> got;
    std::vector<SearchResult> found;
    std::vector<FileDone> files;
    {
        std::lock_guard lock(result_mutex);
        got.swap(results);
        found.swap(search_results);
        files.swap(files_done);
    }
    for (auto& f : files) file_done(f);
    for (auto& s : found) {
        if (s.gen != search_gen) continue;  // an older search
        if (s.ok) show_results(std::move(s.emotes));
        else say(L"Search failed: " + s.error);
    }
    for (auto& r : got) {
        if (r.job.kind == Job::Gif) got_gif(r);
        else got_still(r);
    }
    if (!got.empty() || !files.empty()) load_previews();  // download slots are free
}

void App::got_still(JobResult& r) {
    const Job& j = r.job;
    fetching.erase(j.key + "s");
    if (!r.bytes) return;  // that cell stays blank
    raw.try_emplace(j.key, r.bytes);
    if (!r.ok || j.s != thumb_s || !index.count(j.id) || !dc) return;
    Still s;
    s.w = r.still.w;
    s.h = r.still.h;
    s.height = j.height;
    s.room = j.room;
    s.by_room = r.still.by_room;
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(s.w, s.h), r.still.px.data(), s.w * 4, picture_props(),
                                &s.bitmap)))
        return;
    stills[j.key] = std::move(s);
    request_frame();
}

void App::got_gif(JobResult& r) {
    const Job& j = r.job;
    fetching.erase(j.key + "g");
    if (!index.count(j.id) || !dc) return;  // no longer in the results
    auto& slot = anims[j.key];
    const Image& atlas = r.anim.atlas;
    if (r.ok) {
        auto a = std::make_unique<Anim>();
        a->gif = r.bytes;
        a->w = r.anim.w;
        a->h = r.anim.h;
        a->count = r.anim.count;
        a->cols = r.anim.cols;
        a->height = j.height;
        a->room = j.room;
        a->by_room = r.anim.by_room;
        a->ends = std::move(r.anim.ends);
        if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(atlas.w, atlas.h), atlas.px.data(), atlas.w * 4,
                                       picture_props(), &a->atlas)))
            slot = std::move(a);
        else
            slot.reset();
    } else {
        slot.reset();  // can't play it: it stays still
    }
    // GIFs are big: past MAX_GIF_MB, forget the ones off screen (they download again if you
    // scroll back to them).
    size_t total = 0;
    for (auto& [key, a] : anims)
        if (a && a->gif) total += a->gif->size();
    if (total > (size_t)MAX_GIF_MB << 20) {
        std::unordered_set<std::string> keep;
        auto [from, to] = on_screen();
        for (int i = from; i < to; i++) keep.insert(key_of(i));
        std::erase_if(anims, [&](const auto& e) { return !keep.count(e.first); });
    }
    request_frame();
}

void App::load_previews() {
    if (emotes.empty() || !dc) return;
    // While the size slider is dragged, previews show scaled from what's there; they're made
    // at the new size once it's let go.
    if (drag == Drag::Slider) return;
    auto [from, to] = on_screen();
    load_stills(from, to);
    if (settings.paste == L"file") fetch_files_in_view(from, to);
    for (int i = from; ANIMATE && i < to && (int)fetching.size() < PREVIEW_DOWNLOADS; i++) {
        const Emote& e = emotes[i];
        std::string key = key_of(i);
        if (!e.animated || fetching.count(key + "g")) continue;
        Job job{Job::Gif, key, e.id, thumb_s, true, nullptr, emote_h, room, max_texture};
        auto it = anims.find(key);
        if (it == anims.end()) {
            submit(std::move(job));
        } else if (Anim* a = it->second.get();
                   a && a->gif && (!a->atlas || !fits(a->w, a->h, a->by_room, a->height, a->room))) {
            job.bytes = a->gif;  // downloaded already: just decode its frames again
            submit(std::move(job));
        }
    }
    int bottom = scroll_px + 2 * (int)grid.h();
    load_stills(to, std::min((int)emotes.size(), (bottom / cell + 1) * cols));
}

void App::load_stills(int from, int to) {
    for (int i = from; i < to; i++) {
        const Emote& e = emotes[i];
        if (started.count(e.id)) continue;
        std::string key = key_of(i);
        auto have = stills.find(key);
        if (have == stills.end() ||
            !fits(have->second.w, have->second.h, have->second.by_room, have->second.height, have->second.room)) {
            if (!fetching.count(key + "s")) {
                if ((int)fetching.size() >= PREVIEW_DOWNLOADS) continue;  // its turn comes later
                auto saved = raw.find(key);
                submit({Job::Still, key, e.id, thumb_s, e.animated,
                        saved != raw.end() ? saved->second : nullptr, emote_h, room, max_texture});
            }
        }
        started.insert(e.id);
    }
}

// Indexes of the emotes in view: [first, last).
std::pair<int, int> App::on_screen() const {
    int n = (int)emotes.size(), top = scroll_px, bottom = top + (int)grid.h();
    int from = std::min(n, top / cell * cols);
    return {from, std::max(from, std::min(n, ((bottom - 1) / cell + 1) * cols))};
}

std::string App::key_of(int i) const { return emotes[i].id + "/" + char('0' + thumb_s); }

// Emote i's animated preview, if it's ready to move. With exact given, also one made for
// another size (*exact false: it's shown scaled until it's made again at this size).
App::Anim* App::anim_at(int i, bool* exact) {
    if (i < 0 || i >= (int)emotes.size()) return nullptr;
    auto it = anims.find(key_of(i));
    if (it == anims.end() || !it->second || !it->second->atlas) return nullptr;
    Anim* a = it->second.get();
    bool as_is = fits(a->w, a->h, a->by_room, a->height, a->room);
    if (exact) *exact = as_is;
    return as_is || exact ? a : nullptr;
}

// Emote i's still preview: the one for the current size if there is one (exact: it shows as
// is), else one of another 7TV size, to show scaled meanwhile.
const App::Still* App::still_for(int i, bool& exact) const {
    auto it = stills.find(key_of(i));
    if (it != stills.end()) {
        const Still& s = it->second;
        exact = fits(s.w, s.h, s.by_room, s.height, s.room);
        return &s;
    }
    exact = false;
    for (int s = 4; s >= 1; s--) {
        if (s == thumb_s) continue;
        auto other = stills.find(emotes[i].id + "/" + char('0' + s));
        if (other != stills.end()) return &other->second;
    }
    return nullptr;
}

// Frees the frames of animated emotes more than a screen away (they're decoded again from
// the GIF kept in memory if you scroll back).
void App::forget_far_animations() {
    int top = scroll_px, h = (int)grid.h();
    int near_from = (top - h) / cell * cols, near_to = ((top + 2 * h) / cell + 1) * cols;
    for (auto& [key, a] : anims) {
        if (!a || !a->atlas) continue;
        auto it = index.find(key.substr(0, key.size() - 2));
        if (key.back() - '0' != thumb_s || it == index.end() || it->second < near_from || it->second >= near_to) {
            a->atlas.Reset();
            a->start = -1;
        }
    }
}

// Whether a preview made at made_height for made_room shows as is at the current size.
bool App::fits(int w, int h, bool by_room, int made_height, int made_room) const {
    return made_height == emote_h && (by_room ? made_room == room : w <= room && h <= room);
}
