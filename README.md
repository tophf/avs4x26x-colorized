# avs4x26x
avs4x264 modified by 06_taro (astrataro@gmail.com) & colorized by tophf

![screenshot-0 10](https://cloud.githubusercontent.com/assets/1310400/4079094/e1072552-2ed1-11e4-9679-5b063f64e7ba.png)

The code is based on AVS-input from [x264](http://www.videolan.org/developers/x264.html).

Use this software to encode videos using a 32-bit Avisynth with a 64-bit version of x264 or x265 on Windows. The x26x executable needs to be named x264_64.exe or x265 and placed in the same folder as this program, otherwise you have to specify --x26x-binary "x26x_path" or -L "x26x_path" to define the path of x26x binary.

Example: `avs4x26x.exe --x26x-binary "C:\x264-abc.exe" -o out.264 in.avs`

#### avs4x26x v0.10 features:

* When x26x parameter **--input-depth** is set to a value higher than the default 8, avs4x26x divides the video width by 2 thus allowing a fake 16-bit avs output with MSB/LSB interleaved horizontally be treated correctly by x26x.

* Full command line of the invoked x26x.exe is printed with a "avs4x26x [info]:" prefix.

* x264 path is customizable via **--x26x-binary** "x26x_path" or **-L** "x26x_path" command line switch. By default "x264_64.exe" will be used (or "x265.exe" in case the output file extension is .265/.h265/.hevc).

* Direct output of i422/i444 (YV16/YV24) colorspace with AviSynth 2.6 is supported.

* Help is shown when no command line switches are provided.

* Alternative parameter syntax using the equal sign is allowed:<br/> `--tcfile-in="timecode.txt" --input-depth=16 --x26x-binary="x264" -L=x264` (or `-Lx265`)

* No superfluous *--input-res/--fps/--frames/--input-csp* are added to the x26x command line if those were present in the original command line.

* The number of frames specified with --frames is checked and corrected if needed.

* **--seek-mode** switch added, default is *fast*:
   * *fast* mode is similar to x26x's internal method of avs demuxer and simply skips frames until the specified frame. However an alternative method is used with --qpfile/--tcfile-in: `FreezeFrame(0, seek, seek)` because x26x doesn't modify qpfile or tcfile-in contents accordingly.
   * *safe* mode is safer but slower: it sends all frames to x26x as is, so it might take a very very long time to process the preceding frames depending on the source complexity and the seek frame value, but the result is safer for scripts like TDecimate(mode=3) which may be processed only in a linear way.

* **--timebase** switch added, used with *--tcfile-in*.

* The framerate is corrected to a proper NTSC fraction if applicable.

* Direct .d2v/.dga/.dgi input is supported, plus .vpy input with either VapourSource(VSImport) or VFW interface (AVISource/HBVFWSource), as well as .avi/.mkv/.mp4/.m2ts etc.

* AviSynth+ is supported.

* x262 and x265 are supported.<br/>
However the simple `x265 <infile> <outfile>` style is not supported and probably will never be. Use `--output  outfile` instead.

#### Building from source:

* gcc 4.6.0+: `gcc avs4x26x.c -s -Ofast -oavs4x26x -Wl,--large-address-aware`
* older versions: `gcc avs4x26x.c -s -O3 -ffast-math -oavs4x26x -Wl,--large-address-aware`