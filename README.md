# SSTV for Linux


This is a SSTV encoder/decoder for Linux.

Original created by Oona Räisänen (OH2EIQ [at] sral.fi). I've added TX.

http://windytan.github.io/slowrx/

## Features

- Support for a multitude of modes (see `src/modespec.c` for a list)
- Detect frequency-shifted signals – no need to fine-tune the radio
- Automatic slant correction, also manual adjustments are simple
- Adaptive noise reduction
- Decode digital FSK ID
- Save received pictures as PNG
- Written in C99

## Requirements

- Linux
- Alsa (`libasound2-dev`)
- Gtk+ 4 (`libgtk-4-dev`)
- FFTW 3 (`libfftw3-dev`)

And, obviously:

- shortwave radio with SSB
- computer with sound card
- means of getting sound from radio to sound card

## Compiling

```sh
mkdir build
cd build
cmake ..
make
```

## Running

`./sstv`

## Screenshots

![clipboard_20261023114108.bmp](vx_images/clipboard_20261023114108.bmp)
![clipboard_20261023114308.bmp](vx_images/clipboard_20261023114308.bmp)
