#pragma once
#include <cstdint>
#include <string_view>

// xg_x.dll (Xbox One) and xg_xs.dll (Xbox Series X|S): how the console lays a texture out in
// memory. Titles use it to size and fill memory they hand to Direct3D themselves. Since both
// sides of that are ours, the layout is the plainest one: every mip of every array slice in
// order, rows top to bottom, no tiling.
namespace xv::runtime {
class Registry;
namespace modules {
// DXGI_FORMAT -> bytes of one pixel, or of one 4x4 block for the block-compressed formats
struct FormatSize {
    std::uint32_t bytes;
    bool blocks;
};
FormatSize format_size(std::uint32_t format);
// bytes of a whole texture in the plain layout, rounded up to a 4 KiB page
std::uint64_t texture_bytes(std::uint32_t width, std::uint32_t height, std::uint32_t mips,
                            std::uint32_t array_size, std::uint32_t format);
// `module` is the name the console gives the DLL
void install_xg(Registry &registry, std::string_view module);

// The Series console's XG takes the Direct3D 12 resource description (D3D12_RESOURCE_DESC,
// whose layout this repeats so as not to need the header here). Without the console's own
// xg_xs.dll the layout is the plain one above; this says where a subresource is in it.
struct SeriesResourceDesc {
    std::uint32_t dimension; // 2 buffer, 3 2D texture, 4 3D texture
    std::uint64_t alignment, width;
    std::uint32_t height;
    std::uint16_t depth_or_array_size, mip_levels;
    std::uint32_t format, sample_count, sample_quality, layout, flags;
};
struct LinearSubresource {
    std::uint64_t offset;    // from the start of the texture's memory
    std::uint32_t row_pitch; // bytes of one row (of blocks, for a compressed format)
    std::uint32_t rows;      // rows of one depth slice
    std::uint32_t depth;     // depth slices (1 but for a 3D texture)
};
// false for a subresource the texture does not have
bool series_linear_subresource(const SeriesResourceDesc &desc, std::uint32_t subresource,
                               LinearSubresource &where);
// bytes of the whole texture in that layout
std::uint64_t series_linear_bytes(const SeriesResourceDesc &desc);
} // namespace modules
} // namespace xv::runtime
