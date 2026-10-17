# S4Fixes

Client-side hardening for the S10 client, S4Client.exe,
Each fix is a Detours hook, added one at a time as it gets confirmed.

## Layout

- `s4fixes.sln` / `src/s4fixes.vcxproj` - the project, Win32 only.
- `src/dllmain.cpp` - loads every patch on `DLL_PROCESS_ATTACH`.
- `src/patches/` - one file per bug fix. Everything in here is a patch,
  nothing else goes in this folder.
- `third_party/` - Detours, the hooking library.

## Build

Open `s4fixes.sln`, build `Release|Win32`. `OutDir` is `C:\S4Plain\` -
change it in the vcxproj if your client lives elsewhere.

## Fixes

- `patches/fix_c03_arcade_overflow.cpp` - stack buffer overflow in the
  arcade P2P entry reader. A crafted entry from another player can overflow
  a char[256] stack buffer. Confirmed firing in Warfare, primitive isn't
  arcade-only despite the name.
- `patches/heap_fixes.cpp` - C-01: unbounded reliable-frame count handed to
  the frame-map resize (0x1B0DAF0), 8*count wraps 32-bit to a tiny alloc ->
  heap overflow. Rejects counts over 0x100000. Also the OOM fail-soft hooks.
- `patches/fix_c02_enchant_oob.cpp` - C-02: renewal-enchant .x7 loaders
  (0x1C207C0 / 0x1C25600) write each row at a file-controlled INDEX with no
  bound. Guards both memcpy sites against the table's real allocated range.
- `patches/fix_m01_httpcache_cap.cpp` - M-01: CHTTPImageCache (0x142C650)
  never evicts -> unbounded texture map feeds the long-session freeze. Repoints
  the insert call site inside LoadTexture (0x142C776) to cap the map at 1024.
- `patches/fix_l01_structvec_cap.cpp` - L-01: struct-vector reader (0x1918580)
  trusts a ~1M element count -> iteration/alloc DoS. Repoints that reader's cap
  compare (0x19185DA) to a smaller per-reader cap, leaving the shared cap alone.
- `patches/fix_dos_arcade_alloc.cpp` - arcade P2P stream alloc (0x1155B90) does
  operator new[] on an attacker 4-byte size -> ~2GB OOM. Clamps to 4MB.
- `patches/fix_perf_g1_tipreload.cpp` - perf: loading-tip table re-parsed every
  loading screen (0x13DA240). Repoints its reparse call (0x13DA462) to parse once.
- `patches/fix_s2_x7decrypt.cpp` - S2: encrypted .x7 decrypt (0x1B42C50) underflows
  (Size-8)>>2 on a <8-byte file (OOB read) and allocs the first-4-bytes size (~4GB
  OOM). Gates Size>=8 and SEH-wraps the alloc throw.

## Enhancements

- `enhancements/Fps.cpp` - unlocks the frame rate (`max_framerate` in
  `conf.json`, up to 1000) and keeps the physics identical to 60 fps. The
  client integrates movement with whole-millisecond frame times and constants
  tuned for 60 fps, so above that the collision skin, gravity, fly drag and the
  one-frame air slam drift. Each one is corrected where it is computed, and the
  limiter waits on the game clock at the start of the frame to keep input lag
  low. Also sets the normal, center and sprint field of view.
- `enhancements/hd_text.cpp` - redraws the client's text with D3DX fonts at
  screen resolution and draws chat emoji (`:)`, `:D`, `<3`...) from
  `<game folder>\emoji\` through `emoji.cpp`. `activarTextoHd` and
  `activarEmoji` turn each one off. `text_languages.cpp` shapes Arabic and
  right-to-left text and picks a font that covers CJK and Indic scripts.
- `enhancements/resource_log_test.cpp` - resource optimizer. During boot it
  skips the client's mass scene preload except for the scenes listed in
  `whitelist_code.inc`, which still preload; everything else loads on demand
  the first time the game asks for it. Shop, save and collection-book files are
  never cut.
