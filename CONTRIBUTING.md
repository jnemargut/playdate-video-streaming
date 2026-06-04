# Contributing

Thanks for being here. This project only exists so that streaming video on the
Playdate stops being something everyone has to solve from scratch — so improvements
that go back upstream are the whole idea. **Rough PRs and half-formed issues are
welcome**; a sample `.rwlpv` and a description of what went wrong is already a gift.

## Ways to help

- **Report bugs** — open an issue with your hardware/Simulator, the `.rwlpv` (or how
  you encoded it), and what you saw vs. expected. Console logs help (`StreamVideo.debug = true`).
- **Send fixes or features** — see the "Ideas" list in the README. Seeking, an index
  for mid-file start, alternate transports, and better dithering would all be big wins.
- **Improve the docs** — if something was confusing to integrate, say so or fix it.

## Working on it

- The native engine is `src/streamvideo.c` (+ vendored `minimp3`). The Lua front-end is
  `lua/StreamVideo.lua`. The encoder is `tools/encode-stream-video.py`.
- Keep it **C99 + Lua 5.4**, no extra dependencies beyond the Playdate C API and the
  vendored minimp3.
- Build a host project with the SDK's `common.mk` (`make device` for hardware,
  `make simulator` for the Simulator). **Test both** — a Simulator-only build and a
  device-only build can pass independently while the packaged `.pdx` is broken.
- Match the surrounding style; keep the comments explaining *why*, not *what*.

## Conduct

Be kind and assume good faith. That's the whole policy.

## License

By contributing you agree your work is released under the project's [MIT license](LICENSE).
