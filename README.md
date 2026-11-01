# Image Face — transfer spike (emery + gabbro)

Throwaway face to answer one question: **can we stream a full-screen image from the phone
into RAM on gabbro/emery, with how much heap left, and how fast?**

## Layout
```
package.json          (new SDK layout, same as Radium2: src/c + src/pkjs)
wscript
src/c/main.c          watch side
src/pkjs/index.js     phone side (generates a synthetic test image — no photo needed)
```

## What you see
Concentric blue bands, a white 52px grid, a yellow rim at the edge of the screen, the time,
and a 3-line report at the top:

```
4bpp 33KB 2.31s          <- format, size, transfer time (request -> last chunk)
free 61K>27K>27K         <- heap free: before request > after bitmap alloc > after transfer
inbox 8200 nib hi ok     <- AppMessage inbox size; 4-bit nibble order result
```

**Shake your wrist** to switch 4bpp <-> 8bpp and re-fetch (the old image is freed first).

## What to send back
1. The report line for 4bpp and for 8bpp, on gabbro (and emery if you have one).
2. Shake 5+ times: does it ever show `ERR`, `alloc FAILED` or `timeout`? Does free heap after
   transfer drift down each time (fragmentation / leak)?
3. Does the picture look right? Grid lines straight (stride OK), yellow rim all the way round,
   bands smooth. Skewed or striped = stride/packing bug.
4. Bluetooth distance/phone type: Android or iOS, which Pebble app.

## Known unknowns (not verified — no SDK access when this was written)
- **Not compiled.** The cloud workspace can't reach sdk.repebble.com. Expect a typo or two.
- 4-bit nibble order isn't documented. The watch reads the centre pixel back from the
  frame buffer; if red/blue are swapped it swaps every nibble and says `lo (fixed)`.
- `app_message_inbox_size_maximum()` on gabbro is unknown; the face falls back to 4096.
