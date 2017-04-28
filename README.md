# rpi-mediaplayer
Mediaplayer for Raspberry Pi

## Bugs
* Closing video after rendering to texture is not being done correctly. After a couple
of runs we can not allocate a new EGL Image as destination buffer for rendering any longer.
Resources seem not to be freed correctly to be re-used later.

## TODO
* Subtitles
