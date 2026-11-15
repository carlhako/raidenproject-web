// Emscripten JS library: bridges the C runtime to the worker's `self.psx` host object.
addToLibrary({
  js_init: function () { self.psx.onInit(); },
  js_frame: function (px, w, h, stride) { self.psx.frame(HEAPU8, px, w, h, stride); },
  js_audio: function (ptr, frames) { self.psx.audio(HEAP16, ptr >> 1, frames); },
  js_pad: function (port) { return self.psx.pad(port); },
  js_disc_sectors: function () { return self.psx.discSectors(); },
  js_disc_read: function (lba, dst) { return self.psx.discRead(HEAPU8, lba, dst); },
  js_fatal: function (msg) { self.psx.fatal(UTF8ToString(msg)); },
});
