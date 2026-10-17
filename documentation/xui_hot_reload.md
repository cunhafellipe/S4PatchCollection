# XUI hot reload

`src/enhancements/Xui_Hot_Reload.cpp`. Edit a `.xui`, repack `resource.s4hd`, press
**F6**, and the screen rebuilds from the new file without restarting the client.

`StartXuiHotReload()` must be called **first** in `InitThread`. The client preloads
every screen while booting; hooks installed any later miss that sweep and never
capture a single instance, so F6 finds nothing to reload.

## How the client builds a screen

Each screen is a class whose `::Create` reloads its `.xui` and then resolves every
widget by name:

```
Game::CServerListConnector::Create(this):           01768700
    this->vtbl[8]("Resources/XUI/UI/4server.xui")   // load + parse
    this[2] = FindChild(this[1], "Server_Window")   // the root
    this[3] = FindChild(this[2], "Server_GroupBox")
    ...                                             // one per widget
    operator_new(0x28) + button->vtbl[0x50](handler)  // one handler per button
```

Two things follow from this, and both cost a lot of time to learn:

- **Create does not replace the old tree, it appends a second one.** `FindChild`
  binds whichever comes first, which is the stale one, so replaying `Create`
  reloads the file and changes nothing on screen.
- **Widget names are a contract with the exe.** `FindChild` matches them exactly
  and hangs the click handlers off the result. Renaming one in the XML leaves it
  with no handler. Text, position, size, images and fonts are free to edit.

## The sequence that works

1. `Create` — parses the file and appends a fresh tree.
2. Rename **every** child carrying the root name except the **last** one. The
   children vector appends, so the freshest tree is always last. Renaming *the
   first* instead rebinds to leftovers from earlier reloads, which is why it used
   to take three or four presses before an edit showed up.
3. `Create` again — now `FindChild` binds the fresh tree.
4. Hide the leftovers, show the bound one, and reset focus.

Renaming without a replacement in place crashes inside `Create` at `01768AA5`,
dereferencing the null `FindChild` returns.

## The size check

`sub_1B84DA0` compares the size the container index remembers against what the
read produced:

```
01B84FF0  mov edx, [ecx+0x20]     ; size from the index, captured at boot
01B84FF3  cmp edx, [ebp-0x98]     ; against the bytes actually read
01B84FF9  je  0x1B85027           ; 74 2C -- equal, carry on; otherwise free and fail
```

Repacking changes the file but not the in-memory index, so the two only match when
the edit keeps the exact same length. That is why `"ZZZZ"` -> `"9999"` reloaded
fine and `"Now!"` -> `"After!"` did not - it looked random until the sizes in the
log were lined up: 158279, 158280, 158282, 158307, tracking the edit.

`je` becomes `jmp` for the duration of a reload and is restored right after, so
ordinary loading keeps its check. The buffer is allocated with 2048 bytes of slack
over the expected size, which covers text edits; a change of several KB would also
need `this[8]` corrected.

## Offsets

All verified against this client, none guessed:

| what | where |
|---|---|
| widget name | `+0x28`, VS2013 `std::string`: length `+0x10`, capacity `+0x14`, inline while capacity < 16 |
| label text | `+0x21C` |
| children | `+0x208` - a **pointer** to the vector, not the vector |
| visible flag | `+0xAA` |
| set visible | `vtbl[0x1B4](this, 0|1)` |
| focus / hover reset | ui manager `vtbl[0xAC]()` |

`vtbl[0x1B4]` takes an argument even though the decompiler prints none; the
disassembly at `01768F82` shows the `push 0`. Calling it without one unbalances the
stack and kills the next call.

## Which screens are hooked

`xui_hot_reload.ini` next to the exe, one `.xui` per line, created with a single
entry the first time. A line containing only `*` hooks all 114.

Only `4server.xui` is verified end to end. The other 113 addresses were derived
from the string each `Create` pushes - a reasonable inference, not a measurement -
and detouring all of them at once crashes the client a second into booting. It only
takes one row that is not really a `Create`; the log gave that away with
`[Lobby_list.xui] instance 0019EF68`, a **stack** address.

Add names one at a time as they are confirmed.

## Dead ends, and what ruled each one out

Kept here because every one of them looked right at the time.

| tried | why it is wrong |
|---|---|
| `Destroy` (vtable slot 4) as the counterpart of the load | It does not unload the document: its two slots handle focus and hover. Placed before `Create` the open starts failing from the first reload. |
| Releasing the old resource with `FUN_01B79FB0` | Crashes at `01B79FDB` reading a null vtable - that object is already destroyed. |
| Remounting `resource.s4hd` | `FileExists` on the backing file returns 1 during a reload, so the container was never the problem. |
| The document cache | `lookup` reports a miss every time. |
| The manager mode at `DAT_025BD094+0x2C` | Always 1, boot and reload alike. |
| The decompression scratch pool (3 slots at `mgr+0x1F..0x21`) | `0/3 in use` and it still failed. |
| Wrong thread or wrong window | The reload runs from the WndProc, on the client's own thread. |

## Known limits

- The list of servers is filled by `CServerListConnector::AddServerList` when it
  arrives over the network, not by the XML. A reloaded tree starts empty, so
  clicking a row does nothing until the list is repopulated (Join Now still works;
  it does not depend on a row).
- The repaint happens when the screen is re-entered. Hiding the stale tree and
  showing the fresh one is not enough, so the renderer is not walking the children
  vector every frame - something caches a draw list and only rebuilds it on entry.
- Every reload leaves orphaned trees behind: they are hidden and renamed, never
  freed. `Create` also re-registers a handler per button without dropping the
  previous ones, so events accumulate.
