#!/bin/sh -eu

mkdir -p /var/tmp/sdl
cd /var/tmp/sdl

SDL_VER=2.26.2
curl -sSLO https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VER}/SDL2-${SDL_VER}.tar.gz
tar xaf SDL2*tar*
mv SDL2-${SDL_VER} SDL2
cd SDL2
./configure
make -j "$(nproc)"
sudo make install # SDL needs to be installed here because it is a dependency for the below
cd ..

git clone -b SDL2 --depth 1 https://github.com/libsdl-org/SDL_mixer
cd SDL_mixer
./configure
make -j "$(nproc)"
sudo make install
cd ..

# v2.0.18 and further require automake 1.16 but U18.04 has only automake 1.15.1
git clone -b release-2.0.15 --depth 1 https://github.com/libsdl-org/SDL_ttf
cd SDL_ttf
./autogen.sh # to allow automake 1.15.1
./configure
make -j "$(nproc)"
sudo make install
cd ..

