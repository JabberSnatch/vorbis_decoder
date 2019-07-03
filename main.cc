/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Samuel Bourasseau wrote this file. You can do whatever you want with this
 * stuff. If we meet some day, and you think this stuff is worth it, you can
 * buy me a beer in return.
 * ----------------------------------------------------------------------------
 */

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <vector>

enum class EDecodeState
{
    kError = 0,
    kCapturePattern,
    kStreamStructureVersion,
    kHeaderType,
    kGranulePosition,
    kStreamSerialNum,
    kPageSequenceNum,
    kPageChecksum,
    kPageSegments,
    kSegmentTable,
    kPacketData
};

struct PageDesc
{
    enum FHeaderType
    {
        kContinuedPacket = 0x01,
        kFirstPage = 0x02,
        kLastPage = 0x04
    };

    std::uint8_t header_type = 0u;
    std::int64_t granule_position = 0; // -1 means no packets finish on the page
    std::uint32_t stream_serial_num = 0u;
    std::uint32_t page_sequence_num = 0u;
    std::uint32_t page_checksum = 0u;
    std::uint8_t segment_count = 0u;
    std::uint8_t segment_table[256];
    unsigned debug_StreamSize = 0u;

    unsigned char* stream_begin = nullptr;
};

using PageContainer = std::vector<PageDesc>;
using OggContents = std::unordered_map<std::uint32_t, PageContainer>;

int debug_PageCount = 0;

OggContents DecodeOgg(unsigned char* _buff, std::size_t _size)
{
    EDecodeState decode_state = EDecodeState::kCapturePattern;
    std::uint32_t decode_buff = 0u;
    OggContents pages;
    PageDesc current_page;

    std::size_t buff_index = 0u;
    while (buff_index < _size)
    {
        std::size_t bytes_read = 1u;

        switch (decode_state)
        {

        case EDecodeState::kCapturePattern:
        {
            decode_buff = (decode_buff << 8u) | static_cast<std::uint32_t>(_buff[buff_index]);
            if (decode_buff == 0x4f676753u) // OggS
            {
                current_page = PageDesc{};
                decode_state = EDecodeState::kStreamStructureVersion;
                decode_buff = 0u;
            }
        } break;

        case EDecodeState::kStreamStructureVersion:
        {
            if (_buff[buff_index] == '\0')
            {
                decode_state = EDecodeState::kHeaderType;
            }
            else
                decode_state = EDecodeState::kError;
        } break;

        case EDecodeState::kHeaderType:
        {
            if (!(_buff[buff_index] & 0xf0))
            {
                current_page.header_type = static_cast<std::uint8_t>(_buff[buff_index]);
                decode_state = EDecodeState::kGranulePosition;
            }
            else
                decode_state = EDecodeState::kError;
        } break;

        case EDecodeState::kGranulePosition:
        {
            bytes_read = 8u;
            if (buff_index + bytes_read <= _size)
            {
                std::uint64_t u_granule_position = 0u;
                u_granule_position |= static_cast<std::uint64_t>(_buff[buff_index+0]) << 0;
                u_granule_position |= static_cast<std::uint64_t>(_buff[buff_index+1]) << 8;
                u_granule_position |= static_cast<std::uint64_t>(_buff[buff_index+2]) << 16;
                u_granule_position |= static_cast<std::uint64_t>(_buff[buff_index+3]) << 24;
                u_granule_position |= static_cast<std::uint64_t>(_buff[buff_index+4]) << 32;
                u_granule_position |= static_cast<std::uint64_t>(_buff[buff_index+5]) << 40;
                u_granule_position |= static_cast<std::uint64_t>(_buff[buff_index+6]) << 48;
                u_granule_position |= static_cast<std::uint64_t>(_buff[buff_index+7]) << 56;
                std::memcpy(&current_page.granule_position, &u_granule_position, bytes_read);

                decode_state = EDecodeState::kStreamSerialNum;
            }
            else
                decode_state = EDecodeState::kError;
        } break;

        case EDecodeState::kStreamSerialNum:
        {
            bytes_read = 4u;
            if (buff_index + bytes_read <= _size)
            {
                std::uint32_t stream_serial_num = 0u;
                stream_serial_num |= static_cast<std::uint32_t>(_buff[buff_index+0]) << 0*8;
                stream_serial_num |= static_cast<std::uint32_t>(_buff[buff_index+1]) << 1*8;
                stream_serial_num |= static_cast<std::uint32_t>(_buff[buff_index+2]) << 2*8;
                stream_serial_num |= static_cast<std::uint32_t>(_buff[buff_index+3]) << 3*8;
                current_page.stream_serial_num = stream_serial_num;

                decode_state = EDecodeState::kPageSequenceNum;
            }
            else
                decode_state = EDecodeState::kError;
        } break;

        case EDecodeState::kPageSequenceNum:
        {
            bytes_read = 4u;
            if (buff_index + bytes_read <= _size)
            {
                std::uint32_t page_sequence_num = 0u;
                page_sequence_num |= static_cast<std::uint32_t>(_buff[buff_index+0]) << 0*8;
                page_sequence_num |= static_cast<std::uint32_t>(_buff[buff_index+1]) << 1*8;
                page_sequence_num |= static_cast<std::uint32_t>(_buff[buff_index+2]) << 2*8;
                page_sequence_num |= static_cast<std::uint32_t>(_buff[buff_index+3]) << 3*8;
                current_page.page_sequence_num = page_sequence_num;

                decode_state = EDecodeState::kPageChecksum;
            }
            else
                decode_state = EDecodeState::kError;
        } break;

        case EDecodeState::kPageChecksum:
        {
            bytes_read = 4u;
            if (buff_index + bytes_read <= _size)
            {
                std::uint32_t page_checksum = 0u;
                page_checksum |= static_cast<std::uint32_t>(_buff[buff_index+0]) << 0*8;
                page_checksum |= static_cast<std::uint32_t>(_buff[buff_index+1]) << 1*8;
                page_checksum |= static_cast<std::uint32_t>(_buff[buff_index+2]) << 2*8;
                page_checksum |= static_cast<std::uint32_t>(_buff[buff_index+3]) << 3*8;
                current_page.page_checksum = page_checksum;

                decode_state = EDecodeState::kPageSegments;
            }
            else
                decode_state = EDecodeState::kError;
        } break;

        case EDecodeState::kPageSegments:
        {
            current_page.segment_count = static_cast<std::uint8_t>(_buff[buff_index]);
            decode_state = EDecodeState::kSegmentTable;
        } break;

        case EDecodeState::kSegmentTable:
        {
            bytes_read = current_page.segment_count;
            if (buff_index + bytes_read <= _size)
            {
                for (unsigned seg_index = 0u; seg_index < current_page.segment_count; ++seg_index)
                {
                    current_page.segment_table[seg_index] = _buff[buff_index + seg_index];
                    current_page.debug_StreamSize += _buff[buff_index + seg_index];
                }
                decode_state = EDecodeState::kPacketData;
            }
            else
                decode_state = EDecodeState::kError;
        } break;

        case EDecodeState::kPacketData:
        {
            current_page.stream_begin = _buff + buff_index;

            auto const emplace_info = pages.emplace(current_page.stream_serial_num, PageContainer{ current_page });
            if (!emplace_info.second)
                emplace_info.first->second.emplace_back(current_page);

            bytes_read = 0u;
            decode_state = EDecodeState::kCapturePattern;
        } break;

        default: break;
        }

        buff_index += bytes_read;
    }

    return pages;
}

std::uint32_t FindVorbisSerialNumber(OggContents const& _ogg_contents)
{
    std::uint32_t result = 0u;
    for (std::pair<std::uint32_t const, PageContainer> const& pages_pair : _ogg_contents)
    {
        PageContainer const& pages = pages_pair.second;
        // PRE-CONDITION :
        // (pages.front().segment_count == 1)
        // (pages.front().header_type & PageDesc::FHeaderType::kFirstPage)
        if (!std::strncmp((char const*)pages.front().stream_begin + 1, "vorbis", 6))
            result = pages_pair.first;
    }
    return result;
}

void PrintPages(PageContainer const &_pages)
{
    std::for_each(std::cbegin(_pages), std::cend(_pages), [](PageDesc const& _desc) {
        if (_desc.header_type & PageDesc::FHeaderType::kFirstPage)
            for (unsigned seg_index = 0; seg_index < _desc.segment_count; ++seg_index)
            {
                for (unsigned byte_index = 0; byte_index < _desc.segment_table[seg_index]; ++byte_index)
                    std::cout << _desc.stream_begin[byte_index] << " ";
                std::cout << std::endl << std::endl;
            }

        std::cout << "PAGE DESC" << std::endl;
        std::cout << std::dec << (uint32_t)_desc.header_type << " "
                  << std::dec << _desc.granule_position << " "
                  << std::hex << _desc.stream_serial_num << " "
                  << std::dec << _desc.page_sequence_num << " "
                  << std::hex << _desc.page_checksum << " "
                  << std::dec << (uint32_t)_desc.segment_count << " " << std::endl;;
        for (unsigned i = 0; i < _desc.segment_count; ++i)
            std::cout << std::dec << (uint32_t)_desc.segment_table[i] << " ";
        std::cout << std::endl << std::endl;
    });
}

struct VorbisIDHeader
{
    static constexpr std::streamsize kSizeOnStream = 23u;
    // std::uint32_t vorbis_version; Should be '0' to be compatible
    std::uint8_t audio_channels;
    std::uint32_t audio_sample_rate;
    std::int32_t bitrate_max;
    std::int32_t bitrate_nominal;
    std::int32_t bitrate_min;
    std::uint8_t blocksize_0; // read 4 bits, 2 exponent
    std::uint8_t blocksize_1; // read 4 bits, 2 exponent
    // + 1 bit framing flag (0x01 because bits are supposedly encoded one after the other)
};

void VorbisHeaders(PageContainer const &_pages, unsigned char const* _buff_base)
{
    for (PageDesc const& page : _pages)
    {
        std::ptrdiff_t page_offset = (page.stream_begin - _buff_base);
        std::cout << "Page offset " << std::hex << page_offset << std::endl;
        std::cout << std::dec << (std::uint32_t)page.segment_count << " segments" << std::endl;
        std::ptrdiff_t segment_offset = 0;
        for (int i = 0; i < page.segment_count; ++i)
        {
            if (page.segment_table[i] >= 7)
            {
                if (!std::strncmp((char const*)page.stream_begin + segment_offset, "\x01vorbis", 7))
                    std::cout << "Vorbis ID header offset " << std::hex << page_offset + segment_offset << std::endl;
                if (!std::strncmp((char const*)page.stream_begin + segment_offset, "\x03vorbis", 7))
                    std::cout << "Vorbis comment header offset " << std::hex << page_offset + segment_offset << std::endl;
                if (!std::strncmp((char const*)page.stream_begin + segment_offset, "\x05vorbis", 7))
                    std::cout << "Vorbis setup header offset " << std::hex << page_offset + segment_offset << std::endl;
            }

            segment_offset += page.segment_table[i];
        }
        std::cout << std::endl;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cout << "No file specified" << std::endl;
        return 1;
    }

    std::unique_ptr<unsigned char> buff{};
    std::streamsize file_size = 0ull;
    {
        std::ifstream file(argv[1], std::ios_base::binary);
        file.seekg(0, std::ios_base::end);
        file_size = static_cast<std::streamsize>(file.tellg());
        file.seekg(0, std::ios_base::beg);

        if (file_size < (1024 * 1024 * 1024))
        {
            buff.reset(new unsigned char [file_size]);
            file.read(reinterpret_cast<char*>(buff.get()), file_size);
        }
        else
        {
            std::cout << "File is too large" << std::endl;
            return 1;
        }

        std::cout << file_size << std::endl;
    }

#ifdef SHOW_FIRST_KB
    for (int i = 0; i < 1024 && i < file_size; ++i)
    {
        int v = (int)buff.get()[i];
        if (v < 0x10) std::cout << 0;
        std::cout << std::hex << v;
        if ((i & 0x3) == 0x3) std::cout << " ";
        if ((i & 0xf) == 0xf) std::cout << std::endl;
    }
#endif

    OggContents const ogg_pages = DecodeOgg(buff.get(), static_cast<std::size_t>(file_size));
    std::uint32_t const vorbis_serial = FindVorbisSerialNumber(ogg_pages);
    if (!vorbis_serial)
    {
        std::cout << "No Vorbis frame found in file" << std::endl;
        return 1;
    }

    std::cout << std::hex << vorbis_serial << std::endl;
    VorbisHeaders(ogg_pages.at(vorbis_serial), buff.get());
    //PrintPages(ogg_pages.at(vorbis_serial));

    return 0;
}
