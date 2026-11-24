# Search speed

How long a search takes to show its emotes, before and after the changes of 2026-11-24
(commit bbfda50, "Load emotes faster after a search"), and what was measured along the way.

## Results

Measured with `render_test --search-speed 4` on one Windows 11 PC about 310 ms (round trip)
from 7TV's servers. The panel as it's used there: 577x569 at 175% display scaling, size slider at
240% (154px emotes from 7TV's 4x previews, 25 in view, about 70% of them animated), pasting
"auto" files (so the files of the emotes in view download too).

Each of 4 rounds: a 60 s pause (7TV closes idle connections meanwhile), then a search; a second
search typed 1.5 s after another one's results came in, while that one's previews still load;
then the first search again. Times are from the last letter typed, so they include the 250 ms
wait after typing. Medians of the 4 rounds; "before" is two runs of the old code (ddbd742, with
this test added), "now" the code as committed.

| Search | | Before | Now |
|---|---|---|---|
| After a pause | results | 1.6 / 1.6 s | 1.0 s |
| | first emote | 2.8 / 2.8 s | 1.3 s |
| | every emote in view | 3.9 / 4.5 s | 2.1 s |
| | every animation in view moving | 6.6 / 11.4 s | 4.3 s |
| | downloaded | 3.8 / 3.8 MB | 1.8 MB |
| Typed while another loads | results | 1.6 / 1.3 s | 1.0 s |
| | first emote | 1.7 / 1.4 s | 1.2 s |
| | every emote in view | 7.2 / 7.7 s | 3.4 s |
| | every animation in view moving | 20.4 / over 30 s | 7.1 s |
| | downloaded | 12.0 / 16.2 MB | 7.7 MB |
| The same search again | every animation in view moving | 4.8 / 7.3 s | 0.3 s |
| | downloaded | 2.2 / 2.5 MB | 0.3 MB |

The network varies a lot: in one "now" round, a 7 KB preview took 8.6 s while only about
115 KB/s was moving in total, so single rounds can be far off the median.

## What made the difference

- **A new search stops the downloads of emotes it doesn't have.** They used to keep the 32
  download slots and the bandwidth until they finished (4x GIFs average 620 KB), and finished GIFs
  were then thrown away. Closing a request part-way makes HTTP/2 tell the server to stop sending
  (checked against a local test server: it stopped after 64 KB of 4 MB).
- **Connections are opened while you type.** 7TV closes idle connections, its CDN after 30 s and
  its search server after 50 s, and opening one costs about two round trips (a first small
  download on a new connection took ~1.25 s). Showing the panel, a click in the search box and
  typing now reopen them if they're idle. A search takes 0.66-0.95 s on an open connection and
  1.3-2.6 s on a new one; searches now have a connection of their own, so each one finds it open
  (they used to take turns over the previews' four).
- **Previews are 7TV's WebP files** instead of PNG and GIF. For the 15 most popular animated
  emotes at 4x (medians): stills 3.9 KB instead of 19 KB; animations 191 KB instead of 324 KB
  (means 305 KB and 620 KB). At 2x, WebP animations are no smaller than GIF (105 KB and 98 KB),
  but they have smooth transparent edges instead of GIF's dithered ones. Windows can't read WebP
  everywhere, so libwebp's decoder is included (`third_party/libwebp`). Checked against ffmpeg's
  own WebP decoder: frame counts, timing and transparency identical, colors within 2 levels on
  more than 99.96% of pixels.
- **Downloaded animations are kept** (up to `MAX_MOVING_MB`), so going back to a search or an
  emote shown before doesn't download them again.
- **The stills in view load first**; animations, the next screen and file downloads start once
  they're in. In a run without this, "every emote in view" after a pause took 2.5 s and every
  animation 3.4 s; with it, 2.1 s and 4.3 s: what there is to click shows sooner, and the
  animations a little later.

## Tried and left out

- **HTTP/3.** 7TV's CDN has it, and a first download on a new connection took 0.62-0.69 s
  instead of 1.23-1.33 s. But through WinHTTP about 1 request in 4 failed (5 of 6 when a new
  connection got several at once, as the previews do), and in a timed run WinHTTP never used it.
- **Fewer results per search.** 30 or 60 instead of 250 isn't faster: 7TV's search itself takes
  most of the time.
- **Keeping connections open with HTTP/2 pings.** WinHTTP's keep-alive didn't stop the search
  server from closing the connection after 50 s, and the CDN closes after 30 s anyway.
- **Showing matches from the previous results at once.** 7TV's search is fuzzy ("catjam" returns
  emotes named xdding and SaguiPls), so the previous results rarely predict the new ones.
- **7TV's older v3 search.** About 140 ms faster, but at most 100 results.
- **A bigger HTTP/2 receive window.** WinHTTP already allows 10 MB per download and 32 MB per
  connection.

## Running it

`build.cmd test`, then `build\render_test.exe <out dir> --search-speed [rounds] [--pause seconds]`.
It downloads several MB a round. With the environment variable `PICKER_NET_LOG` set to a file's
path, every request is written there (start, duration, size, connection, result, URL).
