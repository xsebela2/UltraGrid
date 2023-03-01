#!/bin/sh -eu

cd /var/tmp/sdl/SDL2
sudo make install
cd ../SDL_mixer
sudo make install
cd ../SDL_ttf
sudo make install

