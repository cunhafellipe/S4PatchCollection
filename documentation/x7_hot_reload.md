# Reloading .x7 data files while the client runs

`X7_Hot_Reload.cpp`. Edit a `.x7` with ItemManager, save, press **F7**, and the
client re-reads it. **F8** lists every `.x7` loaded so far, with the parser that
handled each one. `x7_hot_reload.ini` decides what gets hooked, so it is read at
startup, not at reload time.

Verified end to end on `Language/message_box.x7` and `xml/item.x7`.

## The chain

    sub_1B83840(path)                    -> the index record for that file
    sub_1B84DA0(reader, record, name,    -> opens it; the size comes from
                record[0x108], 0)           record+0x108
    sub_1B8E590(&count, buffer, size)    -> decompresses, reports the real size
    <per-file parser>                    -> parses and hands the result to a manager

The reload just calls the parser again with the arguments it was first called
with, which the hooks record. Everything else is about making that call succeed.

## The index record

The record the client keeps in memory is the container entry, decrypted, with the
same layout. `readEntry()` in `C:\ItemManager\s4zip\s4zip.cjs` and the loader
agree field for field:

| Field | In the container | In memory |
|---|---|---|
| name | `[0..256]` | `+0` |
| checksum | `@256`, 8 bytes | `+0x100` |
| length | `@264`, int32 | `+0x108` |

The checksum names the extracted blob: `_resources/<checksum as 16 hex digits>`.

In `sub_1B84DA0` the record is the **second** argument. The first is a reader
built moments earlier, and dumping that one shows the size field as zero.

## Why a reload gets refused after an edit

`sub_1B84DA0` ends with

    if (v15[8] != v12) { sub_1B7DF00(v15); return 0; }     // the je at 01B84FF9

comparing the size asked for against the bytes actually produced. A mismatch frees
the buffer and returns 0. **Failing here is clean.** What corrupts the heap is
patching that `je`, which lets a short read through as though it were complete;
the heap then dies by fail-fast, which never reaches `SetUnhandledExceptionFilter`,
and that is the "crash with no log".

ItemManager rewrites the entry when it saves. The client's record still holds the
size from boot, so it no longer describes the entry, and the check catches it.

## The 1,024,000 byte threshold

    bool sub_1B8E590(rsize_t *a1, void *Destination, unsigned a3)
    {
      if (a3 && a3 < 0xFA000)                  // at or above: read raw, no LZO
      {
        DestinationSize = *a1;
        v6 = sub_1B7C760(Destination, DestinationSize, Source, &DestinationSize) == 0;
        *a1 = DestinationSize;                 // the real size, handed back
        ...

This is why two files behaved differently:

| File | Size | Path | In-place edit |
|---|---|---|---|
| `Language/message_box.x7` | 1,155,004 | read raw | works, if the length does not change |
| `xml/item.x7` | 869,868 | LZO | refused, the recorded size no longer fits |

## The fix: tell the client the truth about the index

Saving from ItemManager rewrites an entry's **length and its checksum**, and the checksum is what
names the extracted blob (`_resources/<checksum>`). A client that kept its startup record opens
the old file at the old size, so both fields have to be refreshed, not just the length.

Neither can be recomputed inside the client without reimplementing four layers of container
encryption (`swapBlocks` -> `extractKeys` -> `decryptVersion2` -> `seedCtr`), so s4zip is asked
instead. `x7_index_dump.cjs`, next to the client, writes one line per entry:

    xml/item.x7<TAB>869924<TAB>7a855cc57be46d96

F7 runs it, reads the file back, and hooks `sub_1B83840` - `__thiscall(this, path)` returning the
index record, called by the loader **before** it builds the blob filename and reads the length out
of the record. Patching there means everything downstream picks the new values up on its own:

    index refresh xml/item.x7: 869852 7A855CC580122E10 -> 869924 7A855CC57BE46D96

`x7_index_dump.cjs` has to sit in the same folder as the client executable, and needs node on the
path. Without it the reload still runs, it just cannot refresh anything.

### The wrong version of this fix, and how it looked

The first attempt took the size from the decompressor instead - `sub_1B8E590` writes the real
count back through `*a1` on its way out, which seemed authoritative. It is not: that count is the
output of a read that may itself have stopped early, so feeding it back into `record+0x108`
drifted the value downward on every reload,

    869852 -> 869792 -> 869788        (the true value was 869924)

until the entry was short of the real length and the client rejected the data with a Korean
message box, `... 잘못되었습니다` - "item loading went wrong". It looked like it was converging.
It was not; it was falling.

What made this hard to catch is that it half worked: the icon changed, because it is read from
early in the file and re-fetched per draw, while the description did not, because it sits later
and the tail was already truncated. The offline check is what settled it - decoding the entry with
s4zip gave the complete XML, 5639 items, ending in `</itemlist>`, which proved the container was
fine and the client's copy of the number was not.

## Calling the parsers back

They do not share a convention, so both registers are set and `ECX` only when the
captured value is plausible:

| Parser | File | Convention |
|---|---|---|
| `sub_1BD8AC0` | `xml/item.x7` | `__thiscall(this, path)`, this in `ECX` |
| `sub_17BC300` | `Language/message_box.x7` | `__userpurge`, first argument in `ESI` |

Forcing stack garbage into `ECX` breaks the `ESI` kind. Prologue lengths are
measured with a disassembler and carried per row in the table: assuming five bytes
split `sub esp,0x30` in `sub_1BD8AC0` and left a stray byte running as code.

## Consumers that only insert

Parsing correctly is not enough when the manager refuses to replace what it has.
`Language/message_box.x7` is one of these:

    sub_17B9DE0(this, name, text):        __thiscall, ret 8
        id = sub_17BABD0(name)
        if (map.find(id) != end) return 0        // nothing is written

so the entry is overwritten in place instead: `sub_17BA0A0(this, id)` returns the
`CMessageData` (`+0` vtable, `+4` refcounted pointer, `+8` flag, `+12` a 0x40
block), the new text is AddRef'd and written to `+4`. The old reference is
deliberately leaked, because the release slot for that class is unverified and
getting it wrong corrupts the heap.

`xml/item.x7` needs it too, and gets it. `sub_1BD9100` walks `<itemlist>` calling `sub_1BD9450`
per `<item>`, which ends in a `std::map` insert whose tail is

    if ( v13 >= v8 )        // the key is already in the map
    {
      sub_1BDC710(a5);      // the freshly built item is thrown away
      *(BYTE *)(a2 + 4) = 0;
    }

The layout, measured rather than assumed: `sub_1BDB860` builds a `_Tree` node with the pair at
`node+16`, `sub_1BDBFB0` does `a1[3] = *a3` so the value is one pointer at `pair+12`, and
`sub_F4E670` shows the key is 12 guarded bytes (`[2] ^ [0]`, with `[0] == ~[1]`). So the item
pointer lives at `node+28`. Discarding is safe to work around because `sub_1BDC8D0` releases
nothing - it pushes the node onto a free list - so the pointer can be saved before the call and
planted afterwards, no AddRef needed.

## Known limits

- The first refused open already decompresses into a buffer sized from the old
  value (`a4` plus slack). A file that grew a lot can overflow it before the size
  can be corrected. This is not introduced by the reload; it is what the client
  already does.
- An entry that grows past 1,024,000 bytes stops being decompressed, and the
  correction above does not cover that crossing.
- Each in-place overwrite of a message leaks one reference.
