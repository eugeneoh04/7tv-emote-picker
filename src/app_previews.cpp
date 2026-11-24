// Previews: downloaded and decoded by a pool of threads, put on the graphics card here.
// Stills download for the rows on screen first; once those are in, the animations of the
// animated ones there, then the next screen's stills; the rest wait until you scroll to them.
// Only PREVIEW_DOWNLOADS run at once, and whenever one finishes this runs again, so whatever is
// in view by then goes next. A new search stops the downloads of emotes it doesn't have
// (cancel_unshown).
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
    // Twice as many threads as downloads at once: downloads a new search stopped take a moment
    // to notice (until their next piece arrives), and the new search's shouldn't wait for them.
    for (int i = 0; i < 2 * PREVIEW_DOWNLOADS; i++) std::thread([this] { worker(); }).detach();
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
        if (!r.bytes && !job.cancel->load()) {
            // The WebP (7TV's own format, the smallest), or its PNG or GIF if that can't be had.
            const wchar_t* const* patterns = job.kind == Job::Moving ? THUMB_MOVING
                                             : job.animated          ? THUMB_ANIMATED
                                                                     : THUMB_STATIC;
            Bytes data;
            std::wstring err;
            bool got = fetch(thumb_url(patterns[0], job.id, job.s), "", data, err, job.cancel.get());
            if ((!got && err.starts_with(L"HTTP ")) || (got && !image_readable(data)))
                got = fetch(thumb_url(patterns[1], job.id, job.s), "", data, err, job.cancel.get());
            if (got) r.bytes = std::make_shared<const Bytes>(std::move(data));
        }
        if (r.bytes && !job.cancel->load())  // (stopped: the download is kept, not decoded)
            r.ok = job.kind == Job::Moving
                       ? decode_animation(*r.bytes, job.height, job.room, job.max_texture, r.anim)
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
    job.cancel = std::make_shared<std::atomic<bool>>(false);
    fetching[job.key + (job.kind == Job::Moving ? "g" : "s")] = job.cancel;
    {
        std::lock_guard lock(job_mutex);
        jobs.push_back(std::move(job));
    }
    job_ready.notify_one();
}

// A job is done: its slot frees up (unless it was stopped and a newer job has the slot).
void App::done_fetching(const std::string& what, const CancelFlag& cancel) {
    auto it = fetching.find(what);
    if (it != fetching.end() && it->second == cancel) fetching.erase(it);
}

// New results: the downloads for emotes they don't have stop, and their slots free up at once.
// Left running, they held up the new results' previews for seconds: they kept the slots, and
// the bandwidth (4x animations are often several hundred KB). Ones the new results share carry on.
void App::cancel_unshown(const std::unordered_set<std::string>& shown) {
    for (auto it = fetching.begin(); it != fetching.end();) {
        if (shown.count(it->first.substr(0, it->first.find('/')))) {
            ++it;
            continue;
        }
        it->second->store(true);
        it = fetching.erase(it);
    }
    {
        std::lock_guard lock(job_mutex);
        std::erase_if(jobs, [](const Job& j) { return j.cancel->load(); });  // not started yet
    }
    for (auto& [path, f] : ahead_files)
        if (!shown.count(f.first) && !(pending && pending->path == path)) f.second->store(true);
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
        if (r.job.kind == Job::Moving) got_moving(r);
        else got_still(r);
    }
    if (!got.empty() || !files.empty()) load_previews();  // download slots are free
}

void App::got_still(JobResult& r) {
    const Job& j = r.job;
    done_fetching(j.key + "s", j.cancel);
    if (!r.bytes) return;  // that cell stays blank (or the download was stopped)
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

void App::got_moving(JobResult& r) {
    const Job& j = r.job;
    done_fetching(j.key + "g", j.cancel);
    if (r.bytes) keep_moving_file(j.key, r.bytes);  // even if it's left the results: it may come back
    if (j.cancel->load() || !index.count(j.id) || !dc) return;  // stopped, or no longer in the results
    auto& slot = anims[j.key];
    const Image& atlas = r.anim.atlas;
    if (r.ok) {
        auto a = std::make_unique<Anim>();
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
    request_frame();
}

// Downloaded animations are kept when the results change, so going back to a search (or an
// emote that's in the next one too) doesn't download them again. Past MAX_MOVING_MB, the ones
// unused the longest are forgotten, never ones in view.
void App::keep_moving_file(const std::string& key, SharedBytes bytes) {
    moving_files[key] = {std::move(bytes), ++use_tick};
    size_t total = 0, cap = (size_t)MAX_MOVING_MB << 20;
    for (auto& [name, f] : moving_files) total += f.bytes->size();
    if (total <= cap) return;
    std::unordered_set<std::string> in_view;
    auto [from, to] = on_screen();
    for (int i = from; i < to; i++) in_view.insert(key_of(i));
    std::vector<std::pair<uint64_t, std::string>> oldest;
    for (auto& [name, f] : moving_files)
        if (!in_view.count(name)) oldest.push_back({f.used, name});
    std::sort(oldest.begin(), oldest.end());
    for (auto& [used, name] : oldest) {
        if (total <= cap) break;
        total -= moving_files[name].bytes->size();
        moving_files.erase(name);
    }
}

void App::load_previews() {
    if (emotes.empty() || !dc) return;
    // While the size slider is dragged, previews show scaled from what's there; they're made
    // at the new size once it's let go.
    if (drag == Drag::Slider) return;
    auto [from, to] = on_screen();
    load_stills(from, to);
    // Other downloads wait until the stills in view are in (or failed): those show what there is
    // to click, and downloads alongside would take bandwidth from them (an animation is often 50
    // times the size of its still).
    bool stills_in = true;
    for (int i = from; i < to && stills_in; i++)
        stills_in = started.count(emotes[i].id) && !fetching.count(key_of(i) + "s");
    if (settings.paste == L"file" && stills_in) fetch_files_in_view(from, to);
    for (int i = from; ANIMATE && i < to && (int)fetching.size() < PREVIEW_DOWNLOADS; i++) {
        const Emote& e = emotes[i];
        std::string key = key_of(i);
        if (!e.animated || fetching.count(key + "g")) continue;
        if (auto it = anims.find(key); it != anims.end()) {
            const Anim* a = it->second.get();
            if (!a || (a->atlas && fits(a->w, a->h, a->by_room, a->height, a->room))) continue;  // can't play, or playing
        }
        auto kept = moving_files.find(key);
        if (kept == moving_files.end() && !stills_in) continue;  // a download: after the stills
        Job job{Job::Moving, key, e.id, thumb_s, true, nullptr, emote_h, room, max_texture};
        if (kept != moving_files.end()) {
            kept->second.used = ++use_tick;
            job.bytes = kept->second.bytes;  // downloaded already: just decode its frames
        }
        submit(std::move(job));
    }
    if (!stills_in) return;
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
// the download kept in memory if you scroll back).
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
