#pragma once

#include "image_png.hpp"
#include "game/game.hpp"
#include "component/engine/scripting/filesystem.hpp"
#include <d3d9.h>
#include <memory>
#include <fstream>
#include <unordered_map>

// Display-only menu images. No DB globals are borrowed for upload and no stock
// asset is mutated. Reuse the existing, validated QoS 2D white material pipeline.
namespace menu_image
{
    // Accessed GfxImage fields adapted from KisakCOD r_image.h and verified in
    // QoS 103ADC00/10385930. Opaque bytes are not imported from another title.
    struct image
    {
        unsigned map_type = 3;
        IDirect3DTexture9* texture = nullptr;
        unsigned char opaque[16]{};
        unsigned short width{}, height{}, depth = 1;
        unsigned char category = 3, delayed = 0;
        const char* name = nullptr;
    };
    static_assert(sizeof(image) == 36);
    static_assert(offsetof(image, texture) == 4 && offsetof(image, name) == 32);

    struct resource
    {
        game::Material material{};
        game::MaterialTextureDef binding{};
        image picture{};
        Microsoft::WRL::ComPtr<IDirect3DDevice9> device;
        Microsoft::WRL::ComPtr<IDirect3DTexture9> texture;
        std::string name;
    };

    inline game::Material* get(const std::string& filename)
    {
        // Process-lifetime storage also keeps queued draw-command pointers valid
        // across reloadMenus. Managed textures survive a normal D3D9 Reset.
        // A replacement device gets a new resource; old queued generations remain.
        static auto* resources = new std::vector<std::unique_ptr<resource>>;
        static auto* rejected = new std::unordered_map<std::string, bool>;
        if (filename.empty() || filename.find_first_of("/\\:") != std::string::npos
            || !filename.ends_with(".png")) return nullptr;
        auto* donor = game::Material_RegisterHandle("white");
        if (!donor || donor->textureCount != 1 || !donor->textureTable || !donor->techniqueSet) return nullptr;
        auto* donor_image = static_cast<image*>(donor->textureTable[0].image);
        if (!donor_image || donor_image->map_type != 3 || !donor_image->texture) return nullptr;
        Microsoft::WRL::ComPtr<IDirect3DDevice9> device;
        if (FAILED(donor_image->texture->GetDevice(device.GetAddressOf()))) return nullptr;
        for (auto& entry : *resources)
            if (entry->name == filename && entry->device.Get() == device.Get())
            {
                // Resolve zone-owned techniques afresh rather than caching them
                // through DB unload/renderer registration.
                entry->material = *donor;
                entry->material.name = entry->name.c_str();
                entry->material.textureTable = &entry->binding;
                entry->binding = donor->textureTable[0];
                entry->binding.image = &entry->picture;
                return &entry->material;
            }
        if (rejected->contains(filename)) return nullptr;
        try
        {
            std::string path;
            if (!filesystem::find_file("images/" + filename, &path))
            {
                // A map-specific preview is optional: menu_background tries the
                // installed slideshow next. Do not throw for a normal lookup
                // miss, which otherwise stops first-chance exception debugging.
                rejected->emplace(filename, true);
                game::Com_Printf(13, "^3[menu - image] %s: file not found in consolation/images; skipping optional image\n",
                    filename.c_str());
                return nullptr;
            }
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream) throw std::runtime_error("could not open PNG");
            const auto length = static_cast<std::streamoff>(stream.tellg());
            if (length < 8 || length > static_cast<std::streamoff>(image_overrides::maximum_file_size))
                throw std::runtime_error("PNG file size exceeds limits");
            std::string file(static_cast<std::size_t>(length), '\0');
            stream.seekg(0);
            if (!stream.read(file.data(), static_cast<std::streamsize>(length)))
                throw std::runtime_error("could not read complete PNG");
            const auto decoded = image_overrides::decode_png(std::span(
                reinterpret_cast<const unsigned char*>(file.data()), file.size()));
            image_overrides::load_definition header{};
            std::memcpy(&header, decoded.data(), sizeof(header));
            auto entry = std::make_unique<resource>();
            entry->name = filename;
            entry->device = device;
            if (FAILED(device->CreateTexture(header.width, header.height, 1, 0, D3DFMT_A8R8G8B8,
                D3DPOOL_MANAGED, entry->texture.GetAddressOf(), nullptr)))
                throw std::runtime_error("CreateTexture failed");
            D3DLOCKED_RECT locked{};
            if (FAILED(entry->texture->LockRect(0, &locked, nullptr, 0)))
                throw std::runtime_error("LockRect failed");
            for (unsigned row = 0; row < header.height; ++row)
                std::memcpy(static_cast<unsigned char*>(locked.pBits) + row * locked.Pitch,
                    decoded.data() + sizeof(header) + row * header.width * 4, header.width * 4);
            entry->texture->UnlockRect(0);
            entry->picture.texture = entry->texture.Get();
            entry->picture.width = header.width;
            entry->picture.height = header.height;
            entry->picture.name = entry->name.c_str();
            entry->material = *donor;
            entry->material.name = entry->name.c_str();
            entry->material.textureTable = &entry->binding;
            entry->binding = donor->textureTable[0];
            entry->binding.image = &entry->picture;
            auto* result = &entry->material;
            resources->push_back(std::move(entry));
            game::Com_Printf(13, "[menu - image] loaded %s (%ux%u)\n", filename.c_str(), header.width, header.height);
            return result;
        }
        catch (const std::exception& error)
        {
            rejected->emplace(filename, true);
            game::Com_Printf(13, "^3[menu - image] %s: %s\n", filename.c_str(), error.what());
            return nullptr;
        }
    }
}
