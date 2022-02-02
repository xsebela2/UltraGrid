#!/bin/bash -eux

TEMP_INST=/tmp/install

CPATH=/usr/local/include:/usr/local/opt/qt/include
DYLIBBUNDLER_FLAGS="${DYLIBBUNDLER_FLAGS:+$DYLIBBUNDLER_FLAGS }-s /usr/local/lib"
LIBRARY_PATH=/usr/local/lib:/usr/local/opt/qt/lib
echo "UG_SKIP_NET_TESTS=1" >> $GITHUB_ENV
echo "CPATH=$CPATH" >> $GITHUB_ENV
echo "LIBRARY_PATH=$LIBRARY_PATH" >> $GITHUB_ENV
# libcrypto.pc (and other libcrypto files) is not linked to /usr/local/{lib,include} because conflicting with system libcrypto
echo "PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/local/opt/qt/lib/pkgconfig:/usr/local/opt/openssl/lib/pkgconfig" >> $GITHUB_ENV
echo "/usr/local/opt/qt/bin" >> $GITHUB_PATH
echo "DYLIBBUNDLER_FLAGS=$DYLIBBUNDLER_FLAGS" >> $GITHUB_ENV

brew install autoconf automake cppunit libtool pkg-config
brew install speexdsp
brew install ffmpeg portaudio sdl2 sdl2_mixer sdl2_ttf
brew install imagemagick jack libnatpmp opencv openssl
brew install ossp-uuid # for cineform
brew install qt@5

sudo ln -s /usr/local/opt/qt@5 /usr/local/opt/qt

.github/scripts/macOS/install_dylibbundler_v2.sh

mkdir $TEMP_INST
cd $TEMP_INST

build_cineform() {(
        cd $GITHUB_WORKSPACE/cineform-sdk
        cmake -DBUILD_TOOLS=OFF .
        make -j $(sysctl -n hw.ncpu) CFHDCodecStatic
)}

# Install XIMEA (see <dmg>/install.app/Contents/MacOS/install.sh)
install_ximea() {
        hdiutil mount /private/var/tmp/XIMEA_OSX_SP.dmg
        sudo cp -a /Volumes/XIMEA/m3api.framework $(xcrun --show-sdk-path)/System/Library/Frameworks
        sudo xattr -dr com.apple.quarantine $(xcrun --show-sdk-path)/System/Library/Frameworks
        umount /Volumes/XIMEA
}

install_aja() {
        AJA_DIRECTORY=$SDK_NONFREE_PATH/ntv2sdk
        if [ ! -d $AJA_DIRECTORY ]; then
                return 0
        fi
        echo "AJA_DIRECTORY=$AJA_DIRECTORY" >> $GITHUB_ENV
        FEATURES="$FEATURES --enable-aja"
        echo "FEATURES=$FEATURES" >> $GITHUB_ENV
        sudo rm -f /usr/local/lib/libajantv2.dylib
        sudo cp $AJA_DIRECTORY/bin/ajantv2.dylib /usr/local/lib/libajantv2.dylib
        sudo ln -fs /usr/local/lib/libajantv2.dylib /usr/local/lib/ajantv2.dylib
        cd $TEMP_INST
}

install_deltacast() {
        DELTA_CACHE_INST=$SDK_NONFREE_PATH/VideoMasterHD_inst
        if [ ! -d $DELTA_CACHE_INST ]; then
                return 0
        fi
        FEATURES="$FEATURES --enable-deltacast"
        echo "FEATURES=$FEATURES" >> $GITHUB_ENV
        sudo cp -a $DELTA_CACHE_INST/* $(xcrun --show-sdk-path)/System/Library/Frameworks
}

# Install NDI
install_ndi() {
        sudo installer -pkg /private/var/tmp/Install_NDI_SDK_Apple.pkg -target /
        sudo mv /Library/NDI\ SDK\ for\ * /Library/NDI
        cat /Library/NDI/Version.txt | sed 's/\(.*\)/\#define NDI_VERSION \"\1\"/' | sudo tee /usr/local/include/ndi_version.h
        if [ -d /Library/NDI/lib/x64 ]; then # NDI 4
                cd /Library/NDI/lib/x64
                sudo ln -s libndi.?.dylib libndi.dylib
                NDI_LIB=/Library/NDI/lib/x64
        else # NDI 5
                NDI_LIB=/Library/NDI/lib/macOS
                # TODO: remove - workaround for not using struct keyword causing C code fail
                sudo sed -i -e 's/typedef \([^ ]*_instance_type\)/typedef struct \1/' /Library/NDI/include/*
        fi
        export CPATH=${CPATH:+"$CPATH:"}/Library/NDI/include
        export DYLIBBUNDLER_FLAGS="${DYLIBBUNDLER_FLAGS:+$DYLIBBUNDLER_FLAGS }-s $NDI_LIB"
        export LIBRARY_PATH=${LIBRARY_PATH:+"$LIBRARY_PATH:"}$NDI_LIB
        export MY_DYLD_LIBRARY_PATH="${MY_DYLD_LIBRARY_PATH:+$MY_DYLD_LIBRARY_PATH:}$NDI_LIB"
        echo "CPATH=$CPATH" >> $GITHUB_ENV
        echo "DYLIBBUNDLER_FLAGS=$DYLIBBUNDLER_FLAGS" >> $GITHUB_ENV
        echo "LIBRARY_PATH=$LIBRARY_PATH" >> $GITHUB_ENV
        echo "MY_DYLD_LIBRARY_PATH=$MY_DYLD_LIBRARY_PATH" >> $GITHUB_ENV
        cd $TEMP_INST
}

install_live555() {
        git clone https://github.com/xanview/live555/
        cd live555
        git checkout 35c375
        ./genMakefiles macosx
        make -j $(sysctl -n hw.ncpu) install
        cd ..
}

install_syphon() {
        wget --no-verbose https://github.com/Syphon/Syphon-Framework/releases/download/5/Syphon.SDK.5.zip
        unzip Syphon.SDK.5.zip
        sudo cp -R 'Syphon SDK 5/Syphon.framework' /Library/Frameworks
}

# Install cross-platform deps
$GITHUB_WORKSPACE/.github/scripts/install-common-deps.sh

build_cineform
install_aja
install_deltacast
install_live555
install_ndi
install_syphon
install_ximea

# Remove installation files
cd
rm -rf $TEMP_INST

