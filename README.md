GitHub CI Linux QuickSync (libmfx)
==================================

There are 2 attempts in this branch:

- U18.04
- u20.04

Problems
---------
### UG build with U18
- only possible to have the dispatcher included (linked in, it is static)
- doesn't work

### UG build with U20
- hardcoded location of plugins - Windows allows bundling them as described
[here](https://github.com/Intel-Media-SDK/MediaSDK/blob/master/doc/mediasdkusr-man.md#application-folder-installation) but not for Linux
- add or not libmfxhw64.so.1

#### Possible solution
- use system libmfx.so.1, libmfxhw64.so.1 (`LD_PRELOAD`) - not sure if needed to use also system va

Problem:
- `some encoding parameters are not supported by the QSV runtime. Please double check the input parameters.` (compiled with system mfx U20.04, run on debian 11)
