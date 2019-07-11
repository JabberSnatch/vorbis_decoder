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

enum class EOggDecodeState
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

constexpr unsigned ilog(std::uint32_t _v)
{
    unsigned res = 0u;
    for (; _v; _v = _v >> 1u, ++res);
    return res;
}

OggContents DecodeOgg(unsigned char* _buff, std::size_t _size)
{
    EOggDecodeState decode_state = EOggDecodeState::kCapturePattern;
    std::uint32_t decode_buff = 0u;
    OggContents pages;
    PageDesc current_page;

    std::size_t buff_index = 0u;
    while (buff_index < _size)
    {
        std::size_t bytes_read = 1u;

        switch (decode_state)
        {

        case EOggDecodeState::kCapturePattern:
        {
            decode_buff = (decode_buff << 8u) | static_cast<std::uint32_t>(_buff[buff_index]);
            if (decode_buff == 0x4f676753u) // OggS
            {
                current_page = PageDesc{};
                decode_state = EOggDecodeState::kStreamStructureVersion;
                decode_buff = 0u;
            }
        } break;

        case EOggDecodeState::kStreamStructureVersion:
        {
            if (_buff[buff_index] == '\0')
            {
                decode_state = EOggDecodeState::kHeaderType;
            }
            else
                decode_state = EOggDecodeState::kError;
        } break;

        case EOggDecodeState::kHeaderType:
        {
            if (!(_buff[buff_index] & 0xf0))
            {
                current_page.header_type = *(std::uint8_t const*)(_buff + buff_index);
                decode_state = EOggDecodeState::kGranulePosition;
            }
            else
                decode_state = EOggDecodeState::kError;
        } break;

        case EOggDecodeState::kGranulePosition:
        {
            bytes_read = 8u;
            if (buff_index + bytes_read <= _size)
            {
                current_page.granule_position = *(std::int64_t const*)(_buff + buff_index);
                decode_state = EOggDecodeState::kStreamSerialNum;
            }
            else
                decode_state = EOggDecodeState::kError;
        } break;

        case EOggDecodeState::kStreamSerialNum:
        {
            bytes_read = 4u;
            if (buff_index + bytes_read <= _size)
            {
                current_page.stream_serial_num = *(std::uint32_t const*)(_buff + buff_index);
                decode_state = EOggDecodeState::kPageSequenceNum;
            }
            else
                decode_state = EOggDecodeState::kError;
        } break;

        case EOggDecodeState::kPageSequenceNum:
        {
            bytes_read = 4u;
            if (buff_index + bytes_read <= _size)
            {
                current_page.page_sequence_num = *(std::uint32_t const*)(_buff + buff_index);
                decode_state = EOggDecodeState::kPageChecksum;
            }
            else
                decode_state = EOggDecodeState::kError;
        } break;

        case EOggDecodeState::kPageChecksum:
        {
            bytes_read = 4u;
            if (buff_index + bytes_read <= _size)
            {
                current_page.page_checksum = *(std::uint32_t const*)(_buff + buff_index);
                decode_state = EOggDecodeState::kPageSegments;
            }
            else
                decode_state = EOggDecodeState::kError;
        } break;

        case EOggDecodeState::kPageSegments:
        {
            current_page.segment_count = static_cast<std::uint8_t>(_buff[buff_index]);
            decode_state = EOggDecodeState::kSegmentTable;
        } break;

        case EOggDecodeState::kSegmentTable:
        {
            bytes_read = current_page.segment_count;
            if (buff_index + bytes_read <= _size)
            {
                for (unsigned seg_index = 0u; seg_index < current_page.segment_count; ++seg_index)
                {
                    current_page.segment_table[seg_index] = _buff[buff_index + seg_index];
                    current_page.debug_StreamSize += _buff[buff_index + seg_index];
                }
                decode_state = EOggDecodeState::kPacketData;
            }
            else
                decode_state = EOggDecodeState::kError;
        } break;

        case EOggDecodeState::kPacketData:
        {
            current_page.stream_begin = _buff + buff_index;

            auto const emplace_info = pages.emplace(current_page.stream_serial_num, PageContainer{ current_page });
            if (!emplace_info.second)
                emplace_info.first->second.emplace_back(current_page);

            bytes_read = 0u;
            decode_state = EOggDecodeState::kCapturePattern;
        } break;

        default: break;
        }

        buff_index += bytes_read;
    }

    return pages;
}

std::vector<std::uint32_t> GetVorbisSerials(OggContents const& _ogg_contents)
{
    std::vector<std::uint32_t> result{};
    for (std::pair<std::uint32_t const, PageContainer> const& pages_pair : _ogg_contents)
    {
        PageContainer const& pages = pages_pair.second;
        // PRE-CONDITION :
        // (pages.front().segment_count == 1)
        // (pages.front().header_type & PageDesc::FHeaderType::kFirstPage)
        if (!std::strncmp((char const*)pages.front().stream_begin, "\01vorbis", 7))
            result.push_back(pages_pair.first);
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
    std::size_t page_index;
    std::size_t segment_index;

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

struct VorbisCodebook
{
    enum class EDecodeState
    {
        kSyncPattern = 0,
        kDimensions,
        kEntryCount,
        kCodewordLengths,
    };

    std::uint16_t dimensions;
    std::uint32_t entry_count;
    std::unique_ptr<std::uint8_t> entry_lengths;
};

enum class EVorbisDecodeState
{
    kIDHeader = 0,
    kCommentHeader,
    kSetupHeader,
    kAudioDecode,
};

enum EVorbisError
{
    kNoError = 0,
    kMissingHeader,
    kIncompleteHeader,
    kInvalidIDHeader,
    kInvalidSetupHeader,
};

enum FInvalidIDHeader
{
    kVorbisVersion = 0x1,
    kAudioChannels = 0x2,
    kSampleRate = 0x4,
    kBlocksize = 0x8,
    kFramingBit = 0x10,
};

EVorbisError VorbisCodebookSegmentDecode(unsigned char const* _segment_begin,
                                         std::size_t _segment_size,
                                         VorbisCodebook::EDecodeState &io_state,
                                         VorbisCodebook &o_codebook)
{
    std::size_t seg_offset = 0u;
    std::size_t bit_offset = 0u;
    while (seg_offset < _segment_size)
    {
        std::size_t bytes_read = 1u;
        switch (io_state)
        {

        case VorbisCodebook::EDecodeState::kSyncPattern:
        {
            if (seg_offset)
            {
                std::cout << "Debug abort (incomplete codebook decode)" << std::endl;
                return EVorbisError::kInvalidSetupHeader;
            }

            bytes_read = 3u;
            if (seg_offset + bytes_read < _segment_size)
            {
                if (((*(std::uint32_t const*)(_segment_begin + seg_offset)) & 0xffffffu) == 0x564342u)
                    io_state = VorbisCodebook::EDecodeState::kDimensions;
            }
            else
                return EVorbisError::kIncompleteHeader;
        } break;

        case VorbisCodebook::EDecodeState::kDimensions:
        {
            bytes_read = 2u;
            if (seg_offset + bytes_read < _segment_size)
            {
                o_codebook.dimensions = *(std::uint16_t const*)(_segment_begin + seg_offset);
                io_state = VorbisCodebook::EDecodeState::kEntryCount;
            }
            else
                return EVorbisError::kIncompleteHeader;
        } break;

        case VorbisCodebook::EDecodeState::kEntryCount:
        {
            bytes_read = 3u;
            if (seg_offset + bytes_read < _segment_size)
            {
                o_codebook.entry_count = *(std::uint32_t const*)(_segment_begin + seg_offset) & 0xffffffu;
                o_codebook.entry_lengths.reset(new std::uint8_t[o_codebook.entry_count]);
                io_state = VorbisCodebook::EDecodeState::kCodewordLengths;
            }
            else
                return EVorbisError::kIncompleteHeader;
        } break;

        case VorbisCodebook::EDecodeState::kCodewordLengths:
        {
            std::size_t bit_offset = 0u;
            bool ordered = (_segment_begin[seg_offset]) & 0x1u;
            bit_offset = 1u;
            if (!ordered)
            {
                std::cout << "unordered" << std::endl;
                bool sparse = (_segment_begin[seg_offset]) & 0x2u;
                bit_offset = 2u;

                if (sparse)
                {
                    std::cout << "sparse" << std::endl;
                    std::size_t debug_bitsRem = (2u + 6u * o_codebook.entry_count) & 0x7u;
                    std::size_t debug_byteIndex = 0u;

                    bytes_read = (2u + 6u * o_codebook.entry_count) >> 3u;
                    if (seg_offset + bytes_read >= _segment_size)
                        return EVorbisError::kIncompleteHeader;

                    for (std::size_t entry_index = 0u; entry_index < o_codebook.entry_count; ++entry_index)
                    {
                        bit_offset = 2u + 6u * entry_index;
                        std::size_t byte_index = seg_offset + (bit_offset >> 3u);
                        bit_offset &= 0x7u;

                        bool flag = (_segment_begin[byte_index]) & (1u << bit_offset);

                        bit_offset += 1u;
                        byte_index += bit_offset >> 3u;
                        bit_offset &= 0x7u;

                        std::uint8_t entry_length = 0u;
                        if (flag)
                        {
                            entry_length = static_cast<std::uint8_t>(
                                (*(std::uint16_t const*)(_segment_begin + byte_index)
                                 >> bit_offset) & 0x1fu
                            ) + 1u;

                            bit_offset += 5u;
                            byte_index += bit_offset >> 3u;
                            bit_offset &= 0x7u;
                        }

                        o_codebook.entry_lengths.get()[entry_index] = entry_length;

                        debug_byteIndex = byte_index;
                    }

                    std::cout << "debug_byteIndex " << (debug_byteIndex - seg_offset) << " " << bytes_read << std::endl;
                    std::cout << "debug_bitsRem " << debug_bitsRem << " " << bit_offset << std::endl;
                }
                else
                {
                    bytes_read = (2u + 5u * o_codebook.entry_count) >> 3u;
                    if (seg_offset + bytes_read >= _segment_size)
                        return EVorbisError::kIncompleteHeader;

                    for (std::size_t entry_index = 0u; entry_index < o_codebook.entry_count; ++entry_index)
                    {
                        bit_offset = 2u + 5u * entry_index;
                        std::size_t byte_index = seg_offset + (bit_offset >> 3u);
                        bit_offset &= 0x7u;

                        std::uint8_t entry_length = static_cast<std::uint8_t>(
                            (*(std::uint16_t const*)(_segment_begin + byte_index)
                             >> bit_offset) & 0x1fu
                        ) + 1u;
                        o_codebook.entry_lengths.get()[entry_index] = entry_length;
                    }
                }
            }
            else
            {
                std::size_t byte_index = seg_offset;
                std::uint32_t entry_index = 0u;

                std::uint8_t current_length = static_cast<std::uint8_t>(
                    (*(std::uint16_t const*)(_segment_begin + byte_index)
                     >> bit_offset) & 0x1fu
                ) + 1u;
                bit_offset += 5u;
                byte_index += bit_offset >> 3u;
                bit_offset &= 0x7u;

                while (entry_index < o_codebook.entry_count)
                {
                    std::size_t bits_read = ilog(o_codebook.entry_count - entry_index); // entry_count is 24 bits
                    std::uint32_t entry_range =
                        (*(std::uint32_t const*)(_segment_begin + byte_index)
                         >> bit_offset) & ((1u << bits_read) - 1u);
                    bit_offset += bits_read;
                    byte_index += bit_offset >> 3u;
                    bit_offset &= 0x7u;

                    std::fill(o_codebook.entry_lengths.get() + entry_index,
                              o_codebook.entry_lengths.get() + entry_index + entry_range,
                              current_length);

                    entry_index += entry_range;
                    if (entry_index > o_codebook.entry_count)
                        return EVorbisError::kInvalidSetupHeader;

                    ++current_length;
                }

                bytes_read = byte_index - seg_offset;
            }

            io_state = VorbisCodebook::EDecodeState::kSyncPattern;
        } break;

        default: break;
        }
        seg_offset += bytes_read;
    }

    return EVorbisError::kNoError;
}

std::uint32_t VorbisHeaders(PageContainer const &_pages, VorbisIDHeader &o_id_header)
{
    EVorbisError error_code = EVorbisError::kNoError;
    std::uint16_t error_flags = 0u;

    EVorbisDecodeState decode_state = EVorbisDecodeState::kIDHeader;

    for (std::size_t page_index = 0u; page_index < _pages.size(); ++page_index)
    {
        PageDesc const& page = _pages[page_index];

        std::ptrdiff_t segment_offset = 0;
        for (std::size_t seg_index = 0;
             seg_index < page.segment_count && error_code == EVorbisError::kNoError;
             ++seg_index)
        {
            switch (decode_state)
            {
            case EVorbisDecodeState::kIDHeader:
            {
                if (page.segment_table[seg_index] >= 7u &&
                    !std::strncmp((char const*)page.stream_begin + segment_offset, "\x01vorbis", 7u))
                {
                    if (page.segment_table[seg_index] < VorbisIDHeader::kSizeOnStream + 7u)
                    {
                        error_code = EVorbisError::kIncompleteHeader;
                        break;
                    }

                    unsigned char const* read_position = page.stream_begin + segment_offset + 7u;

                    if (*(std::uint32_t const*)read_position != 0u)
                    {
                        error_code = EVorbisError::kInvalidIDHeader;
                        error_flags |= FInvalidIDHeader::kVorbisVersion;
                    }
                    read_position += 4u;

                    o_id_header.page_index = page_index;
                    o_id_header.segment_index = seg_index;

                    o_id_header.audio_channels = *read_position;
                    read_position += 1u;

                    o_id_header.audio_sample_rate = *(std::uint32_t const*)read_position;
                    read_position += 4u;

                    o_id_header.bitrate_max = *(std::int32_t const*)read_position;
                    read_position += 4u;

                    o_id_header.bitrate_nominal = *(std::int32_t const*)read_position;
                    read_position += 4u;

                    o_id_header.bitrate_min = *(std::int32_t const*)read_position;
                    read_position += 4u;

                    o_id_header.blocksize_0 = (*read_position) & 0xf;
                    o_id_header.blocksize_1 = (*read_position) >> 4u;
                    read_position += 1u;

                    if (*read_position != 1u)
                    {
                        error_code = EVorbisError::kInvalidIDHeader;
                        error_flags |= FInvalidIDHeader::kFramingBit;
                    }

                    if (!o_id_header.audio_channels)
                    {
                        error_code = EVorbisError::kInvalidIDHeader;
                        error_flags |= FInvalidIDHeader::kAudioChannels;
                    }
                    if (!o_id_header.audio_sample_rate)
                    {
                        error_code = EVorbisError::kInvalidIDHeader;
                        error_flags |= FInvalidIDHeader::kSampleRate;
                    }
                    if (o_id_header.blocksize_0 > o_id_header.blocksize_1)
                    {
                        error_code = EVorbisError::kInvalidIDHeader;
                        error_flags |= FInvalidIDHeader::kBlocksize;
                    }

                    decode_state = EVorbisDecodeState::kCommentHeader;
                }
            } break;

            case EVorbisDecodeState::kCommentHeader:
            {
                if (page.segment_table[seg_index] >= 7
                    && !std::strncmp((char const*)page.stream_begin + segment_offset, "\x03vorbis", 7))
                {
                    std::cout << "Comment header found page " << page_index << " segment " << seg_index << std::endl;
                    decode_state = EVorbisDecodeState::kSetupHeader;
                }
            } break;

            case EVorbisDecodeState::kSetupHeader:
            {
                if (page.segment_table[seg_index] >= 7
                    && !std::strncmp((char const*)page.stream_begin + segment_offset, "\x05vorbis", 7))
                {
                    unsigned char const* read_position = page.stream_begin + segment_offset + 7u;
                    std::size_t const codebook_count = (std::size_t)(*read_position++) + 1u;
                    std::cout << "Setup header found page " << page_index << " segment " << seg_index << std::endl;

                    VorbisCodebook::EDecodeState codebook_state = VorbisCodebook::EDecodeState::kSyncPattern;
                    VorbisCodebook test_codebook{};
                    error_code = VorbisCodebookSegmentDecode(read_position,
                                                             page.segment_table[seg_index] - 8u,
                                                             codebook_state,
                                                             test_codebook);

                    std::cout << "Codebook : " << std::endl
                              << std::dec << test_codebook.dimensions << " "
                              << std::dec << test_codebook.entry_count << std::endl;
                    if (test_codebook.entry_lengths)
                    {
                        for (std::uint32_t entry_index = 0u; entry_index < test_codebook.entry_count; ++entry_index)
                            std::cout << std::dec << (unsigned)test_codebook.entry_lengths.get()[entry_index] << " ";
                        std::cout << std::endl;
                    }
                    decode_state = EVorbisDecodeState::kAudioDecode;
                }
            } break;

            default: break;
            }

            segment_offset += page.segment_table[seg_index];
        }

        if (error_code != EVorbisError::kNoError)
            break;
    }

    if (error_code == EVorbisError::kNoError && decode_state != EVorbisDecodeState::kAudioDecode)
        error_code = EVorbisError::kMissingHeader;

    return error_code << 16u || error_flags;
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
    std::vector<std::uint32_t> const vorbis_serials = GetVorbisSerials(ogg_pages);
    if (vorbis_serials.empty())
    {
        std::cout << "No Vorbis frame found in file" << std::endl;
        return 1;
    }

#if 0
    PrintPages(ogg_pages.at(vorbis_serials.front()));
    return 0;
#endif

    std::cout << std::hex << vorbis_serials.front() << std::endl;

    VorbisIDHeader id_header;
    std::uint32_t res = VorbisHeaders(ogg_pages.at(vorbis_serials.front()), id_header);
    if (res >> 16u != EVorbisError::kNoError)
    {
        std::cout << "Vorbis error" << std::endl;
        // return 1;
    }

    std::cout << "ID header : " << std::endl
              << std::dec
              << id_header.page_index << " " << id_header.segment_index << std::endl
              << (unsigned)id_header.audio_channels << " " << id_header.audio_sample_rate << std::endl
              << id_header.bitrate_max << " " << id_header.bitrate_nominal << " " << id_header.bitrate_min << std::endl
              << (unsigned)id_header.blocksize_0 << " " << (unsigned)id_header.blocksize_1 << std::endl;

    return 0;
}
