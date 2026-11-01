// Image Face spike — phone side.
// Generates a synthetic test image (no canvas needed in PebbleKit JS) and streams it
// to the watch over AppMessage, one message in flight at a time.
// 4bpp: 16-colour palette + packed nibbles (left pixel in HIGH nibble; the watch
//       auto-detects and fixes this if the firmware disagrees).
// 8bpp: one GColor8 (argb2222) byte per pixel.

// GColor8 = 0b AA RR GG BB, alpha 11 = opaque
var PAL = [
  0xC0, // 0 black
  0xF0, // 1 red   (nibble-order probe, left pixel)
  0xC3, // 2 blue  (nibble-order probe, right pixel)
  0xCC, // 3 green
  0xFC, // 4 yellow (edge ring)
  0xCF, // 5 cyan
  0xF3, // 6 magenta
  0xFF, // 7 white (grid)
  0xC1, 0xC2, 0xC3, 0xC7, 0xCB, 0xCF, 0xDF, 0xEF // 8..15 blue->cyan->light ramp
];

function indexAt(x, y, w, h) {
  var cx = (w >> 1) & ~1, cy = h >> 1;
  if (y === cy && x === cx) return 1;      // probe pixel pair at screen centre
  if (y === cy && x === cx + 1) return 2;
  var dx = x - w / 2, dy = y - h / 2;
  var d = Math.sqrt(dx * dx + dy * dy);
  if (d >= w / 2 - 3) return 4;            // yellow rim: shows clipping / stride errors
  if (x % 52 === 0 || y % 52 === 0) return 7; // grid: skew shows up as bent lines
  return 8 + (Math.floor(d / 12) % 8);     // concentric bands through the ramp
}

function build(w, h, bpp) {
  var tight = bpp === 4 ? (w + 1) >> 1 : w;
  var total = tight * h;
  var buf = new Array(total);
  var i;
  for (i = 0; i < total; i++) buf[i] = 0;
  for (var y = 0; y < h; y++) {
    for (var x = 0; x < w; x++) {
      var idx = indexAt(x, y, w, h);
      if (bpp === 4) {
        buf[y * tight + (x >> 1)] |= (x & 1) ? idx : (idx << 4);
      } else {
        buf[y * tight + x] = PAL[idx];
      }
    }
  }
  return { tight: tight, total: total, buf: buf };
}

function sendQueue(msgs, i, tries) {
  if (i >= msgs.length) { console.log('transfer complete'); return; }
  Pebble.sendAppMessage(msgs[i], function () {
    sendQueue(msgs, i + 1, 0);
  }, function () {
    if (tries < 5) {
      console.log('nack on message ' + i + ', retry ' + (tries + 1));
      setTimeout(function () { sendQueue(msgs, i, tries + 1); }, 150);
    } else {
      console.log('giving up on message ' + i);
    }
  });
}

function sendImage(w, h, bpp, chunk) {
  chunk = Math.max(256, Math.min(chunk || 1000, 8000));
  var img = build(w, h, bpp);
  var msgs = [];
  var hdr = { ImgW: w, ImgH: h, ImgBpp: bpp, ImgTotal: img.total };
  if (bpp === 4) hdr.ImgPalette = PAL.slice(0, 16);
  msgs.push(hdr);
  for (var off = 0; off < img.total; off += chunk) {
    msgs.push({ ChunkOffset: off, ChunkData: img.buf.slice(off, Math.min(off + chunk, img.total)) });
  }
  console.log('sending ' + w + 'x' + h + ' ' + bpp + 'bpp, ' + img.total + ' bytes in ' + (msgs.length - 1) + ' chunks of ' + chunk);
  sendQueue(msgs, 0, 0);
}

Pebble.addEventListener('ready', function () {
  Pebble.sendAppMessage({ JsReady: 1 });
});

Pebble.addEventListener('appmessage', function (e) {
  var p = e.payload;
  if (p.ReqBpp === undefined) return;
  sendImage(p.ReqW, p.ReqH, p.ReqBpp, p.ReqChunk);
});
