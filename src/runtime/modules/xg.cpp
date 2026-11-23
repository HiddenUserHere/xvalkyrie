#include "runtime/modules/xg.hpp"
#include "runtime/common/runtime.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace xv::runtime::modules {
FormatSize format_size(std::uint32_t format) {
    const auto within = [&](std::uint32_t first, std::uint32_t last) {
        return format >= first && format <= last;
    };
    if (within(1, 4)) // R32G32B32A32
        return {16, false};
    if (within(5, 8)) // R32G32B32
        return {12, false};
    if (within(9, 22)) // R16G16B16A16, R32G32, D32 + S8
        return {8, false};
    if (within(23, 47)) // 10:10:10:2, 11:11:10, R8G8B8A8, R16G16, R32, D24 + S8
        return {4, false};
    if (within(48, 59)) // R8G8, R16
        return {2, false};
    if (within(60, 65)) // R8, A8
        return {1, false};
    if (within(70, 72) || within(79, 81)) // BC1, BC4
        return {8, true};
    if (within(73, 78) || within(82, 84) || within(94, 99)) // BC2, BC3, BC5, BC6H, BC7
        return {16, true};
    if (format == 85 || format == 86 || format == 115) // 5:6:5, 5:5:5:1, 4:4:4:4
        return {2, false};
    return {4, false}; // B8G8R8A8 and friends, and anything unlisted
}

std::uint64_t texture_bytes(std::uint32_t width, std::uint32_t height, std::uint32_t mips,
                            std::uint32_t array_size, std::uint32_t format) {
    const auto size = format_size(format);
    if (!mips) { // a full chain
        for (auto side = std::max(width, height); side; side >>= 1)
            ++mips;
    }
    std::uint64_t total = 0;
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
        const std::uint64_t w = std::max(width >> mip, 1u), h = std::max(height >> mip, 1u);
        total += size.blocks ? ((w + 3) / 4) * ((h + 3) / 4) * size.bytes : w * h * size.bytes;
    }
    total *= std::max(array_size, 1u);
    return (total + 4095) & ~std::uint64_t{4095};
}

namespace {
// XG_TEXTURE2D_DESC as titles fill it in: the Direct3D 11.X texture description followed
// by the tile mode and the pitch.
struct TextureDescription {
    std::uint32_t width, height, mips, array_size, format, sample_count, sample_quality, usage,
        bind_flags, cpu_access, misc_flags, esram_offset, esram_usage, tile_mode, pitch;
};
// Titles read the total size from the start of XG_RESOURCE_LAYOUT. The structure goes on
// with per-plane and per-mip tables, 0xd40 bytes in all, which are left zeroed but for the
// row pitch of the first mip in bytes, at 0x5c: a title multiplies it by the height to
// size a copy of the texture.
constexpr std::size_t layout_bytes = 0xd40, first_pitch = 0x5c;
struct Computer {
    void *const *vtable;
    std::atomic<std::uint32_t> references;
    TextureDescription description;
};
std::uint32_t add_ref(Computer *self) { return ++self->references; }
std::uint32_t release(Computer *self) {
    const auto left = --self->references;
    if (!left)
        delete self;
    return left;
}
std::int32_t texture2d_layout(const TextureDescription *description, void *layout) {
    if (!description || !layout)
        return static_cast<std::int32_t>(0x80070057); // E_INVALIDARG
    std::memset(layout, 0, layout_bytes);
    const auto &d = *description;
    const std::uint64_t header[2] = {
        texture_bytes(d.width, d.height, d.mips, d.array_size, d.format), 4096};
    std::memcpy(layout, header, sizeof header);
    const std::uint32_t counts[2] = {d.mips, 1}; // mip levels, planes
    std::memcpy(static_cast<char *>(layout) + sizeof header, counts, sizeof counts);
    const auto size = format_size(d.format);
    const std::uint32_t pitch = size.blocks ? (std::max(d.width, 1u) + 3) / 4 * size.bytes
                                            : std::max(d.width, 1u) * size.bytes;
    std::memcpy(static_cast<char *>(layout) + first_pitch, &pitch, sizeof pitch);
    return 0;
}
std::int32_t resource_layout(Computer *self, void *layout) {
    return texture2d_layout(&self->description, layout);
}
// XG_TEXTURE3D_DESC, and the part of XG_MIPLEVEL_LAYOUT that says where a mip level is.
struct VolumeDescription {
    std::uint32_t width, height, depth, mips, format, usage, bind_flags, cpu_access, misc_flags,
        esram_offset, esram_usage, tile_mode, pitch;
};
struct MipLayout {
    std::uint64_t size, offset, slice_size;
    std::uint32_t pitch_pixels, pitch_bytes, alignment, padded_width, padded_height, padded_depth,
        width, height, depth, samples;
};
constexpr std::size_t plane_size = 0x20, bytes_per_element = 0x38, first_mip = 0x40,
                      mip_bytes = 0x60;
std::int32_t texture3d_layout(const VolumeDescription *description, void *layout) {
    if (!description || !layout)
        return static_cast<std::int32_t>(0x80070057); // E_INVALIDARG
    std::memset(layout, 0, layout_bytes);
    const auto &d = *description;
    const auto size = format_size(d.format);
    auto mips = d.mips;
    if (!mips) {
        for (auto side = std::max({d.width, d.height, d.depth}); side; side >>= 1)
            ++mips;
    }
    mips = std::min(mips, 15u);
    const auto base = static_cast<char *>(layout);
    std::uint64_t total = 0;
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
        const std::uint32_t w = std::max(d.width >> mip, 1u), h = std::max(d.height >> mip, 1u),
                            depth = std::max(d.depth >> mip, 1u);
        const std::uint32_t across = size.blocks ? (w + 3) / 4 : w,
                            down = size.blocks ? (h + 3) / 4 : h;
        MipLayout level{};
        level.slice_size = std::uint64_t{across} * down * size.bytes;
        level.size = level.slice_size * depth;
        level.offset = total;
        level.pitch_pixels = size.blocks ? across * 4 : across;
        level.pitch_bytes = across * size.bytes;
        level.alignment = 1;
        level.padded_width = level.width = across;
        level.padded_height = level.height = down;
        level.padded_depth = level.depth = depth;
        level.samples = 1;
        std::memcpy(base + first_mip + mip * mip_bytes, &level, sizeof level);
        total += level.size;
    }
    total = (total + 4095) & ~std::uint64_t{4095};
    const std::uint64_t header[2] = {total, 4096};
    std::memcpy(base, header, sizeof header);
    const std::uint32_t counts[2] = {mips, 1}; // mip levels, planes
    std::memcpy(base + sizeof header, counts, sizeof counts);
    const std::uint64_t plane[3] = {total, 0, 4096}; // size, offset, alignment
    std::memcpy(base + plane_size, plane, sizeof plane);
    std::memcpy(base + bytes_per_element, &size.bytes, sizeof(std::uint32_t));
    return 0;
}
template <int Slot> std::int32_t unknown(Computer *) {
    std::fprintf(stderr, "[Runtime] unimplemented xg_x.dll texture computer method %d\n", Slot);
    std::fflush(stderr);
    return static_cast<std::int32_t>(0x80004001); // E_NOTIMPL
}
void *const computer_vtable[] = {
    reinterpret_cast<void *>(&add_ref),         reinterpret_cast<void *>(&release),
    reinterpret_cast<void *>(&resource_layout), reinterpret_cast<void *>(&unknown<3>),
    reinterpret_cast<void *>(&unknown<4>),      reinterpret_cast<void *>(&unknown<5>),
    reinterpret_cast<void *>(&unknown<6>),      reinterpret_cast<void *>(&unknown<7>),
    reinterpret_cast<void *>(&unknown<8>),      reinterpret_cast<void *>(&unknown<9>),
    reinterpret_cast<void *>(&unknown<10>),     reinterpret_cast<void *>(&unknown<11>),
};

std::int32_t create_texture2d_computer(const TextureDescription *description, Computer **computer) {
    if (!description || !computer)
        return static_cast<std::int32_t>(0x80070057); // E_INVALIDARG
    *computer = new Computer{computer_vtable, 1, *description};
    return 0;
}
std::int32_t create_texture_computer(const std::uint32_t *description, Computer **) {
    std::fprintf(stderr, "[Runtime] XG_RESOURCE_DESC:");
    for (int i = 0; i < 16; ++i)
        std::fprintf(stderr, " %x", description[i]);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    return static_cast<std::int32_t>(0x80004001);
}
// There is one layout, so one tile mode: 0.
std::uint32_t optimal_tile_mode(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                std::uint32_t, std::uint32_t, std::uint32_t) {
    return 0;
}
// format, width, height, depth or layers, samples, then whether it is compressed, a tiled
// resource, and read as a texture
void optimal_depth_stencil_tile_modes(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                      std::uint32_t, std::int32_t, std::int32_t, std::int32_t,
                                      std::uint32_t *depth, std::uint32_t *stencil) {
    if (depth)
        *depth = 0;
    if (stencil)
        *stencil = 0;
}
// The Series computer (XGCreateTextureComputer of xg_xs.dll), for the plain layout: AddRef,
// Release, GetResourceLayout (2), GetResourceSizeBytes (3), ..., CopyIntoSubresource (8),
// CopyFromSubresource (9). The copies take (memory of the texture, plane, subresource, linear
// data, row pitch, depth pitch), as titles of kit 0x20004 call them.
struct SeriesComputer {
    void *const *vtable;
    std::atomic<std::uint32_t> references;
    SeriesResourceDesc desc;
};
std::uint32_t series_add_ref(SeriesComputer *self) { return ++self->references; }
std::uint32_t series_release(SeriesComputer *self) {
    const auto left = --self->references;
    if (!left)
        delete self;
    return left;
}
// what titles read of XG_RESOURCE_LAYOUT: the size first, then the alignment
std::int32_t series_layout(SeriesComputer *self, void *layout) {
    if (!layout)
        return static_cast<std::int32_t>(0x80070057); // E_INVALIDARG
    std::memset(layout, 0, layout_bytes);
    const std::uint64_t header[2] = {series_linear_bytes(self->desc), 4096};
    std::memcpy(layout, header, sizeof header);
    return 0;
}
std::uint64_t series_size(SeriesComputer *self) { return series_linear_bytes(self->desc); }
std::int32_t copy_rows(const SeriesComputer *self, std::uint8_t *texture, std::uint32_t subresource,
                       std::uint8_t *linear, std::uint32_t row_pitch, std::uint32_t depth_pitch,
                       bool into) {
    LinearSubresource where{};
    if (!texture || !linear || !series_linear_subresource(self->desc, subresource, where))
        return static_cast<std::int32_t>(0x80070057); // E_INVALIDARG
    for (std::uint32_t z = 0; z < where.depth; ++z)
        for (std::uint32_t y = 0; y < where.rows; ++y) {
            auto *const at =
                texture + where.offset + (std::uint64_t{z} * where.rows + y) * where.row_pitch;
            auto *const data =
                linear + std::uint64_t{z} * depth_pitch + std::uint64_t{y} * row_pitch;
            if (into)
                std::memcpy(at, data, where.row_pitch);
            else
                std::memcpy(data, at, where.row_pitch);
        }
    return 0;
}
std::int32_t series_copy_into(SeriesComputer *self, void *texture, std::uint32_t,
                              std::uint32_t subresource, const void *linear,
                              std::uint32_t row_pitch, std::uint32_t depth_pitch) {
    return copy_rows(self, static_cast<std::uint8_t *>(texture), subresource,
                     const_cast<std::uint8_t *>(static_cast<const std::uint8_t *>(linear)),
                     row_pitch, depth_pitch, true);
}
std::int32_t series_copy_from(SeriesComputer *self, void *linear, std::uint32_t,
                              std::uint32_t subresource, const void *texture,
                              std::uint32_t row_pitch, std::uint32_t depth_pitch) {
    return copy_rows(self, const_cast<std::uint8_t *>(static_cast<const std::uint8_t *>(texture)),
                     subresource, static_cast<std::uint8_t *>(linear), row_pitch, depth_pitch,
                     false);
}
// Method 10: GetResourceTiling as the device has it (tiles of the whole resource, packed mips,
// the tile shape of the others, how many subresource tilings and from which, the tilings), for
// tiles of 64 KiB in the shape the desktop's standard swizzle gives each element size
struct PackedMips {
    std::uint8_t standard, packed;
    std::uint32_t packed_tiles, first_packed_tile;
};
struct TileShape {
    std::uint32_t width, height, depth;
};
// D3D12_SUBRESOURCE_TILING: 12 bytes, which titles keep arrays of on their stack
struct SubresourceTiling {
    std::uint32_t width;
    std::uint16_t height, depth;
    std::uint32_t first_tile;
};
static_assert(sizeof(PackedMips) == 12 && sizeof(TileShape) == 12 &&
              sizeof(SubresourceTiling) == 12);
std::int32_t series_tiling(SeriesComputer *self, std::uint32_t *total, PackedMips *packed,
                           TileShape *shape, std::uint32_t *count, std::uint32_t first,
                           SubresourceTiling *tilings) {
    const auto &desc = self->desc;
    const auto size = format_size(desc.format);
    // the elements of a 64 KiB tile: 256x256 bytes' worth, halved across as elements grow
    const std::uint32_t element = std::max(size.bytes, 1u);
    std::uint32_t across = 256, down = 256;
    for (std::uint32_t bytes = 1; bytes < element; bytes *= 2)
        (across > down ? across : down) /= 2;
    const bool volume = desc.dimension == 4;
    const std::uint32_t tile_w = across * (size.blocks ? 4 : 1),
                        tile_h = down * (size.blocks ? 4 : 1), tile_d = volume ? 1 : 1;
    const std::uint32_t mips = std::max<std::uint32_t>(desc.mip_levels, 1);
    const std::uint32_t slices = volume ? 1 : std::max<std::uint32_t>(desc.depth_or_array_size, 1);
    std::uint32_t standard = 0;
    while (standard < mips &&
           std::max(static_cast<std::uint32_t>(desc.width >> standard), 1u) >= tile_w &&
           std::max(desc.height >> standard, 1u) >= tile_h)
        ++standard;
    // the packed mips of a slice share tiles, as many as their bytes fill
    std::uint64_t packed_bytes = 0;
    for (std::uint32_t mip = standard; mip < mips; ++mip) {
        LinearSubresource where{};
        if (series_linear_subresource(desc, mip, where))
            packed_bytes += std::uint64_t{where.row_pitch} * where.rows * where.depth;
    }
    const std::uint32_t packed_tiles =
        static_cast<std::uint32_t>((packed_bytes + 0xffff) / 0x10000);
    std::uint32_t tile = 0;
    std::vector<SubresourceTiling> all(std::size_t{mips} * slices);
    for (std::uint32_t slice = 0; slice < slices; ++slice) {
        for (std::uint32_t mip = 0; mip < mips; ++mip) {
            auto &one = all[std::size_t{slice} * mips + mip];
            if (mip >= standard) {
                one = {0, 0, 0, 0xffffffff}; // D3D12_PACKED_TILE
                continue;
            }
            const std::uint32_t w = std::max(static_cast<std::uint32_t>(desc.width >> mip), 1u),
                                h = std::max(desc.height >> mip, 1u),
                                d = volume ? std::max<std::uint32_t>(
                                                 desc.depth_or_array_size >> mip, 1)
                                           : 1;
            one = {(w + tile_w - 1) / tile_w, static_cast<std::uint16_t>((h + tile_h - 1) / tile_h),
                   static_cast<std::uint16_t>((d + tile_d - 1) / tile_d), tile};
            tile += one.width * one.height * one.depth;
        }
        if (slice == 0 && packed)
            *packed = {static_cast<std::uint8_t>(standard),
                       static_cast<std::uint8_t>(mips - standard), packed_tiles, tile};
        tile += packed_tiles;
    }
    if (total)
        *total = tile;
    if (shape)
        *shape = {tile_w, tile_h, tile_d};
    if (count) {
        const auto available =
            first < all.size() ? static_cast<std::uint32_t>(all.size() - first) : 0u;
        *count = std::min(*count, available);
        for (std::uint32_t i = 0; tilings && i < *count; ++i)
            tilings[i] = all[first + i];
    }
    return 0;
}
template <int Slot> std::int32_t series_unknown(SeriesComputer *) {
    if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
        std::fprintf(stderr, "[Runtime] unimplemented xg_xs.dll texture computer method %d\n",
                     Slot);
        std::fflush(stderr);
    }
    return static_cast<std::int32_t>(0x80004001); // E_NOTIMPL
}
void *const series_vtable[] = {
    reinterpret_cast<void *>(&series_add_ref),     reinterpret_cast<void *>(&series_release),
    reinterpret_cast<void *>(&series_layout),      reinterpret_cast<void *>(&series_size),
    reinterpret_cast<void *>(&series_unknown<4>),  reinterpret_cast<void *>(&series_unknown<5>),
    reinterpret_cast<void *>(&series_unknown<6>),  reinterpret_cast<void *>(&series_unknown<7>),
    reinterpret_cast<void *>(&series_copy_into),   reinterpret_cast<void *>(&series_copy_from),
    reinterpret_cast<void *>(&series_tiling),      reinterpret_cast<void *>(&series_unknown<11>),
    reinterpret_cast<void *>(&series_unknown<12>), reinterpret_cast<void *>(&series_unknown<13>),
    reinterpret_cast<void *>(&series_unknown<14>), reinterpret_cast<void *>(&series_unknown<15>),
};
std::int32_t create_series_computer(const SeriesResourceDesc *desc, SeriesComputer **computer) {
    if (!desc || !computer)
        return static_cast<std::int32_t>(0x80070057); // E_INVALIDARG
    *computer = new SeriesComputer{series_vtable, 1, *desc};
    return 0;
}
// xg_xs.dll's XGSuggestSwizzleMode(description, flags, layout, options): the swizzle mode for
// a texture, written where the title points, which is the Layout of the Direct3D 12
// description it goes on to create the texture with. Console layouts are 0x100 and the mode;
// mode 0 is linear (SW_LINEAR), which is how textures in the title's memory are read here.
std::int32_t suggest_swizzle_mode(const void *description, std::uint32_t, std::uint32_t *layout,
                                  std::uint32_t) {
    if (!description || !layout)
        return static_cast<std::int32_t>(0x80070057); // E_INVALIDARG
    *layout = 0x100;
    return 0;
}
} // namespace

bool series_linear_subresource(const SeriesResourceDesc &desc, std::uint32_t subresource,
                               LinearSubresource &where) {
    const std::uint32_t mips = std::max<std::uint32_t>(desc.mip_levels, 1);
    const bool volume = desc.dimension == 4;
    const std::uint32_t slices = volume ? 1 : std::max<std::uint32_t>(desc.depth_or_array_size, 1);
    const auto size = format_size(desc.format);
    std::uint64_t offset = 0;
    for (std::uint32_t slice = 0; slice < slices; ++slice)
        for (std::uint32_t mip = 0; mip < mips; ++mip) {
            const std::uint32_t w = std::max(static_cast<std::uint32_t>(desc.width >> mip), 1u),
                                h = std::max(desc.height >> mip, 1u);
            const std::uint32_t pitch = size.blocks ? (w + 3) / 4 * size.bytes : w * size.bytes;
            const std::uint32_t rows = size.blocks ? (h + 3) / 4 : h;
            const std::uint32_t depth =
                volume ? std::max<std::uint32_t>(desc.depth_or_array_size >> mip, 1) : 1;
            if (slice * mips + mip == subresource) {
                where = {offset, pitch, rows, depth};
                return true;
            }
            offset += std::uint64_t{pitch} * rows * depth;
        }
    // past the last subresource: where the next one would start, for the size
    where = {offset, 0, 0, 0};
    return false;
}
std::uint64_t series_linear_bytes(const SeriesResourceDesc &desc) {
    LinearSubresource end{};
    series_linear_subresource(desc, ~0u, end);
    return (end.offset + 4095) & ~std::uint64_t{4095};
}

void install_xg(Registry &registry, std::string_view module) {
    registry.add(module, "XGSuggestSwizzleMode", reinterpret_cast<void *>(&suggest_swizzle_mode));
    registry.add(module, "XGCreateTexture2DComputer",
                 reinterpret_cast<void *>(&create_texture2d_computer));
    // the Series XG takes the Direct3D 12 description; Xbox One's another one
    registry.add(module, "XGCreateTextureComputer",
                 module == "xg_xs.dll" ? reinterpret_cast<void *>(&create_series_computer)
                                       : reinterpret_cast<void *>(&create_texture_computer));
    registry.add(module, "XGComputeTexture2DLayout", reinterpret_cast<void *>(&texture2d_layout));
    registry.add(module, "XGComputeTexture3DLayout", reinterpret_cast<void *>(&texture3d_layout));
    registry.add(module, "XGComputeOptimalTileMode", reinterpret_cast<void *>(&optimal_tile_mode));
    registry.add(module, "XGComputeOptimalDepthStencilTileModes",
                 reinterpret_cast<void *>(&optimal_depth_stencil_tile_modes));
}
} // namespace xv::runtime::modules
