HdrHistogram_c: 'C' port of High Dynamic Range (HDR) Histogram

HdrHistogram
----------------------------------------------

[![Gitter chat](https://badges.gitter.im/HdrHistogram/HdrHistogram.png)](https://gitter.im/HdrHistogram/HdrHistogram)

This port contains a subset of the functionality supported by the Java
implementation.  The current supported features are:

* Standard histogram with 64 bit counts (32/16 bit counts not supported)
* All iterator types (all values, recorded, percentiles, linear, logarithmic)
* Histogram serialisation (encoding version 1.2, decoding 1.0-1.2)
* Reader/writer phaser and interval recorder

Features not supported, but planned

* Auto-resizing of histograms

Features unlikely to be implemented

* Double histograms
* Atomic/Concurrent histograms
* 16/32 bit histograms

# Simple Tutorial

## Recording values



## More examples

For more detailed examples of recording and logging results look at the
[hdr_decoder](examples/hdr_decoder.c)
and [hiccup](examples/hiccup.c)
examples.  You can run hiccup and decoder
and pipe the results of one into the other.

```
$ ./examples/hiccup | ./examples/hdr_decoder
```
