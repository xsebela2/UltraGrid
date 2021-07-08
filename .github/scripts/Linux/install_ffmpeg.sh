#!/bin/bash -eux

cd /var/tmp/ffmpeg
( cd libvpx && sudo make install )
( cd msdk/build && sudo rm -rf /usr/local/include/mfx && sudo cp -r tmpinst/include /usr/local/include/mfx && sudo cp -nr tmpinst/{lib,plugins} /usr/local || exit 1 )
( cd x264 && sudo make install )
( cd nv-codec-headers && sudo make install )
( cd aom/build && sudo cmake --install . )
( cd SVT-HEVC/Build/linux/Release && sudo make install )
( cd SVT-AV1/Build && sudo make install )
( cd SVT-VP9/Build && sudo make install || exit 1 )
sudo make install
sudo ldconfig
