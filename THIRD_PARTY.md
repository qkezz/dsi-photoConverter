# Third-party components and references

Project code is distributed under the [MIT license](LICENSE). Third-party
libraries retain their own licenses; the project license does not replace them.

## Required libraries

- [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo) or a compatible
  libjpeg implementation: JPEG decoding and encoding. See the license files of
  the implementation used in your build.
- [libpng](https://www.libpng.org/pub/png/libpng.html): PNG decoding. See its
  accompanying license.
- [OpenSSL](https://www.openssl.org/): AES and random-number generation. License
  terms depend on the OpenSSL version used in the build.

When redistributing a binary, include the applicable copyright and license
notices for all dependencies it distributes. This also applies to statically
linked Windows builds. Linux and macOS builds use shared libraries; install the
runtime dependencies as described in the README.

## Format documentation and prior work

- [GBATEK: DSi Camera files](https://problemkaputt.de/gbatek.htm#dsisdmmccamerafilesoverview)
  documents JPEG metadata and the SD album index, `pit.bin`.
- [DSiBrew: Nintendo DSi Camera](https://dsibrew.org/wiki/Nintendo_DSi_Camera).
- [Signature research](https://4dsdev.kuribo64.net/thread.php?pid=585).
- [SIGMAFLIP](https://github.com/CybernatedChris/SIGMAFLIP): original experiment
  referenced by this C implementation.

Nintendo DSi and Nintendo DSi Camera are Nintendo trademarks. This is an
independent project and is not affiliated with Nintendo.
