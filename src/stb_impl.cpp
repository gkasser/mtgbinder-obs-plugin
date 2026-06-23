// Unité de compilation dédiée à l'implémentation de stb_image_write (single-header).
// Permet d'encoder les frames en JPEG sans dépendance système (libjpeg), ce qui
// simplifie fortement les builds Windows/macOS via obs-deps.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
