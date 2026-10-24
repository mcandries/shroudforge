#include "scene.h"

#include <windows.h>

#include <fstream>
#include <iterator>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include <stb_image.h>

namespace overlay {

namespace {
constexpr int kMaxImageSide = 4096;

std::wstring widen(const std::string& text) {
    std::wstring wide(MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), static_cast<int>(wide.size()));
    if (!wide.empty()) wide.pop_back();
    return wide;
}
}  // namespace

std::shared_ptr<Image> load_image(const std::string& path, std::string& error) {
    std::ifstream file(widen(path), std::ios::binary);
    if (!file) {
        error = "cannot open image " + path;
        return nullptr;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    int width = 0, height = 0, channels = 0;
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels)) {
        error = "not a PNG or JPEG image: " + path;
        return nullptr;
    }
    if (width > kMaxImageSide || height > kMaxImageSide) {
        error = "image larger than 4096 pixels: " + path;
        return nullptr;
    }
    unsigned char* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (!pixels) {
        error = std::string("cannot decode image ") + path + ": " + stbi_failure_reason();
        return nullptr;
    }
    auto image = std::make_shared<Image>();
    image->path = path;
    image->width = width;
    image->height = height;
    image->rgba.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return image;
}

}  // namespace overlay
