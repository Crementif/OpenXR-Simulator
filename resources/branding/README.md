# OpenXR Simulator icon

`openxr-simulator.svg` is the source for the application, installer and desktop icon.
Regenerate the derivatives with ImageMagick:

```sh
magick -background none openxr-simulator.svg -resize 256x256 openxr-simulator.png
magick openxr-simulator.png -resize 64x64 BMP:openxr-simulator.bmp
magick openxr-simulator.png -define icon:auto-resize=256,128,64,48,32,16 openxr-simulator.ico
```
