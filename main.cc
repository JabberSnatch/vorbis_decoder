/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Samuel Bourasseau wrote this file. You can do whatever you want with this
 * stuff. If we meet some day, and you think this stuff is worth it, you can
 * buy me a beer in return.
 * ----------------------------------------------------------------------------
 */

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <variant>
#include <vector>

// =============================================================================
// HUFFMAN CODING
// =============================================================================

struct BinaryNode
{
    std::uint32_t v = -1u;
    std::uint8_t length = 0u;
    std::size_t left = 0u, right = 0u;
};

std::vector<BinaryNode> BuildHuffmanTree(std::vector<std::uint8_t> const& _lengths);

struct HuffmanLUT
{
    std::vector<std::uint32_t> entries;
    std::vector<std::uint32_t> lengths;
    std::vector<std::uint32_t> indices;
};

HuffmanLUT Huffman_BuildLookupTable(std::vector<BinaryNode> const& _tree);
std::uint32_t Huffman_ReadEntry(HuffmanLUT const& _lut,
                                std::uint8_t const* &_base_address,
                                int &_bit_offset,
                                int &o_bits_read);

// =============================================================================
// OGG FILE FORMAT
// =============================================================================

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

    std::uint8_t const* stream_begin = nullptr;
};

int debug_PageCount = 0;
std::uint8_t* debug_baseBuff;

std::ptrdiff_t debug_ComputeOffset(PageDesc const& _page, std::size_t _seg_index)
{
    std::uint32_t byte_offset = 0u;
    for (int i = 0; i < (int)_seg_index-1; ++i)
        byte_offset += _page.segment_table[i];

    return (_page.stream_begin + byte_offset) - debug_baseBuff;
}

using PageContainer = std::vector<PageDesc>;
using OggContents = std::unordered_map<std::uint32_t, PageContainer>;

OggContents DecodeOgg(std::uint8_t const* _buff, std::size_t _size)
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

void PrintPage(PageDesc const& _desc)
{
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
}

void PrintPages(PageContainer const &_pages)
{
    std::for_each(std::cbegin(_pages), std::cend(_pages), PrintPage);
}

// =============================================================================
// VORBIS DECODER
// =============================================================================

constexpr unsigned ilog(std::uint32_t _v)
{
    unsigned res = 0u;
    for (; _v; _v = _v >> 1u, ++res);
    return res;
}

constexpr std::uint32_t lookup1_values(std::uint32_t _entry_count, std::uint16_t _dimensions)
{
    if (_dimensions == 0u)
        return 0u;
    if (_dimensions == 1u)
        return _entry_count;
    if (_dimensions == 2u)
        return static_cast<std::uint32_t>(std::floor(std::sqrt(_entry_count)));

    float const f_entry_count = (float)_entry_count;
    float const f_dimensions = (float)_dimensions;
    float f_result = 1.f;
    while (std::pow(f_result, f_dimensions) <= f_entry_count)
        f_result += 1.f; // NOTE: _entry_count should be 24 bits
    return static_cast<std::uint32_t>(f_result) - 1u;
}

inline std::size_t low_neighbour(std::vector<std::uint32_t> const& _values, std::size_t _index)
{
    std::size_t n = -1u;
    for (std::size_t i = 0u; i < _index; ++i)
        if (_values[i] < _values[_index] &&
            (n == -1u || _values[i] > _values[n]))
            n = i;
    return n;
}

inline std::size_t high_neighbour(std::vector<std::uint32_t> const& _values, std::size_t _index)
{
    std::size_t n = -1u;
    for (std::size_t i = 0u; i < _index; ++i)
        if (_values[i] > _values[_index] &&
            (n == -1u || _values[i] < _values[n]))
            n = i;
    return n;
}

std::uint32_t render_point(std::uint32_t x0, std::uint32_t y0,
                           std::uint32_t x1, std::uint32_t y1,
                           std::uint32_t x)
{
    std::int32_t dy = y1 - y0;
    std::int32_t adx = x1 - x0;
    std::int32_t ady = std::abs(dy);
    std::int32_t err = ady * (x - x0);
    std::int32_t off = err / adx;
    if (dy < 0)
        return (std::uint32_t)std::max(0u, y0 - off);
    else
        return (std::uint32_t)std::max(0u, y0 + off);
}

float WindowEval(std::uint32_t _n,
                 std::uint32_t _lws, std::uint32_t _lwe,
                 std::uint32_t _rws, std::uint32_t _rwe)
{
    constexpr float piOver2 = 3.1415926536f * .5f;

    if (_n >= _rwe)
        return 0.f;

    if (_n >= _rws)
    {
        float t0 = std::sin(((float)(_n - _rws) + .5f) / (float)(_rwe-_rws) * piOver2 + piOver2);
        return std::sin(piOver2 * t0*t0);
    }

    if (_n >= _lwe)
        return 1.f;

    if (_n >= _lws)
    {
        float t0 = std::sin(((float)(_n - _lws) + .5f) / (float)(_lwe-_lws) * piOver2);
        return std::sin(piOver2 * t0*t0);
    }

    return 0.f;
}

struct VorbisCodebook
{
    std::uint16_t dimensions;
    std::uint32_t entry_count;
    std::vector<std::uint8_t> entry_lengths;

    bool ordered;
    bool sparse;

    std::uint8_t lookup_type;
    float min_value;
    float delta_value;
    std::uint8_t multiplicand_bit_size;
    bool sequence_p;
    std::vector<std::uint16_t> multiplicands;
};

struct VorbisFloor
{
    struct Floor0
    {
        std::uint8_t order;
        std::uint16_t rate;
        std::uint16_t bark_map_size;
        std::uint8_t amplitude_bits;
        std::uint8_t amplitude_offset;
        std::uint8_t book_count;
        std::vector<std::uint8_t> codebooks;
    };

    struct Floor1
    {
        struct Class
        {
            std::uint8_t dimensions;
            std::uint8_t subclass_logcount;
            std::uint8_t masterbook;
            std::vector<std::uint8_t> subclass_codebooks;
        };

        std::uint8_t partition_count;
        std::vector<std::uint8_t> partition_classes;
        std::vector<Class> classes;
        std::uint8_t multiplier;
        std::uint32_t value_count;
        std::vector<std::uint32_t> values;
    };

    std::uint16_t type;
    std::variant<Floor0, Floor1> data;
};

struct VorbisResidue
{
    static constexpr std::uint16_t kUnusedBook = 0x100u;

    std::uint16_t type;
    std::uint32_t begin;
    std::uint32_t end;
    std::uint32_t partition_size;
    std::uint8_t classif_count;
    std::uint8_t classbook;
    std::vector<std::uint8_t> cascade;
    std::vector<std::uint16_t> books;
};

struct VorbisMapping
{
    std::uint16_t type;
    bool submap_flag;
    std::uint8_t submap_count;
    bool coupling_flag;
    std::uint8_t coupling_step_count;
    std::vector<std::uint32_t> magnitudes;
    std::vector<std::uint32_t> angles;
    std::uint8_t reserved_field;
    std::vector<std::uint8_t> muxes;
    std::vector<std::uint8_t> submap_floors;
    std::vector<std::uint8_t> submap_residues;
};

struct VorbisMode
{
    bool blockflag;
    std::uint16_t windowtype;
    std::uint16_t transformtype;
    std::uint8_t mapping;
};

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
    // + 1 bit framing flag (0x01 because of byte alignment)
};

struct VorbisSetupHeader
{
    std::size_t page_index;
    std::size_t segment_index;

    std::vector<VorbisCodebook> codebooks;
    std::vector<VorbisFloor> floors;
    std::vector<VorbisResidue> residues;
    std::vector<VorbisMapping> mappings;
    std::vector<VorbisMode> modes;
};

enum EVorbisError
{
    kNoError = 0,
    kInvalidStream,
    kEndOfStream,

    kMissingHeader,
    kIncompleteHeader,
    kInvalidIDHeader,
    kInvalidSetupHeader,
};

enum FInvalidStream
{
    kEndOfPacket = 0x1,
    kUnexpectedNonAudioPacket = 0x2,
    kUndecodablePacket = 0x4,
    kUnknownCodeword = 0x8,
};

enum FInvalidIDHeader
{
    kVorbisVersion = 0x1,
    kAudioChannels = 0x2,
    kSampleRate = 0x4,
    kBlocksize = 0x8,
    kFramingBit = 0x10,
};


std::uint32_t PackError(EVorbisError _code, std::uint16_t _flags)
{
    return _code << 16u | _flags;
}

EVorbisError ComputePacketSize(PageContainer const& _pages,
                               std::size_t _page_index,
                               std::size_t _seg_index,
                               std::size_t &o_packet_size,
                               std::size_t &o_page_end,
                               std::size_t &o_seg_end)
{
    o_page_end = _page_index;
    o_seg_end = _seg_index;
    o_packet_size = 0u;

    while (_pages[_page_index].segment_table[_seg_index] == 255u)
    {
        std::cout << std::dec << (unsigned)_pages[_page_index].segment_table[_seg_index] << " ";
        o_packet_size += _pages[_page_index].segment_table[_seg_index++];
        if (_seg_index >= _pages[_page_index].segment_count)
            if (++_page_index >= _pages.size())
                return EVorbisError::kInvalidStream;
    }

    std::cout << std::dec << (unsigned)_pages[_page_index].segment_table[_seg_index] << std::endl;
    o_packet_size += _pages[_page_index].segment_table[_seg_index];
    o_page_end = _page_index;
    o_seg_end = _seg_index + 1;

    if (o_page_end < _pages.size() && o_seg_end == _pages[o_page_end].segment_count)
    {
        o_page_end++;
        o_seg_end = 0u;
    }

    return EVorbisError::kNoError;
}

std::uint32_t ReadBits(int _count,
                       std::uint8_t const* &_base_address,
                       int &_bit_offset)
{
    std::uint32_t result = 0u;

    if (_count > 32) _count = 32;
    if (_bit_offset < 0) _bit_offset = 0;
    if (_bit_offset > 7) _bit_offset &= 7u;

    int const t0 = 8 - _bit_offset;
    int const t1 = (_count + _bit_offset) / 8u;
    int const t2 = (_count + _bit_offset) & 7u;

    if (_count > 0)
        result |= (_base_address[0] >> _bit_offset) & (1u << _count) - 1u;

    if (t1 == 1)
    {
        result |= (std::uint32_t)(_base_address[1] & ((1u << t2) - 1u)) << t0;
    }
    else if (t1 == 2)
    {
        result |= (std::uint32_t)_base_address[1] << t0;
        result |= (std::uint32_t)(_base_address[2] & ((1u << t2) - 1u)) << (t0 + 8);
    }
    else if (t1 == 3)
    {
        result |= (std::uint32_t)_base_address[1] << t0;
        result |= (std::uint32_t)_base_address[2] << (t0 + 8);
        result |= (std::uint32_t)(_base_address[3] & ((1u << t2) - 1u)) << (t0 + 16);
    }
    else if (t1 >= 4)
    {
        result |= (std::uint32_t)_base_address[1] << t0;
        result |= (std::uint32_t)_base_address[2] << (t0 + 8);
        result |= (std::uint32_t)_base_address[3] << (t0 + 16);
        result |= (std::uint32_t)(_base_address[4] & ((1u << t2) - 1u)) << (t0 + 24);
    }

    _base_address += t1;
    _bit_offset = t2;

    return result;
}

#if 0
// Single field, N fields
// biased (+1, -1), unbiased
// constant size, variable size

// Single, unbiased, constant size fields
// Single, unbiased, variable size fields
// Single, biased, constant size fields
// N, unbiased, constant size fields
// N, unbiased, variable size fields
// N, biased, constant size fields
template <int ... kSizes, typename ... Fields>
EVorbisError ReadFields(std::uint8_t const* &_base_address,
                        int &_bit_offset,
                        int &_remaining_bits,
                        Fields& ... _fields)
{
    static auto const _ReadBits = [](int _count, std::uint8_t const* &_base, int &_offset,
                                     auto &_field) {
        _field = static_cast<std::remove_reference_t<decltype(_field)>>(ReadBits(_count, _base, _offset));
    };
    constexpr int kTotalSize = (0 + ... + kSizes);

    if (kTotalSize <= _remaining_bits)
    {
        _remaining_bits -= kTotalSize;
        (_ReadBits(kSizes, _base_address, _bit_offset, _fields), ...);

        return EVorbisError::kNoError;
    }
    else
        return EVorbisError::kEndOfStream;
}
#endif

EVorbisError VorbisCodebookDecode(std::uint8_t const* &_base_address,
                                  int &_bit_offset,
                                  int &_remaining_bits,
                                  VorbisCodebook &o_codebook)
{
    std::cout << "Remaining bits " << _remaining_bits << std::endl;

    if (_remaining_bits < 24)
        return EVorbisError::kIncompleteHeader;
    _remaining_bits -= 24;
    std::uint32_t sync_pattern = ReadBits(24, _base_address, _bit_offset);

    if (sync_pattern != 0x564342u)
        return EVorbisError::kInvalidSetupHeader;

    if (_remaining_bits < 16)
        return EVorbisError::kIncompleteHeader;
    _remaining_bits -= 16;
    o_codebook.dimensions = (std::uint16_t)ReadBits(16, _base_address, _bit_offset);

    if (_remaining_bits < 24)
        return EVorbisError::kIncompleteHeader;
    _remaining_bits -= 24;
    o_codebook.entry_count = ReadBits(24, _base_address, _bit_offset);
    o_codebook.entry_lengths.resize(o_codebook.entry_count);

    if (!_remaining_bits)
        return EVorbisError::kIncompleteHeader;
    _remaining_bits -= 1;
    o_codebook.ordered = ReadBits(1, _base_address, _bit_offset);

    if (!o_codebook.ordered)
    {
        if (!_remaining_bits)
            return EVorbisError::kIncompleteHeader;
        _remaining_bits -= 1;
        o_codebook.sparse = ReadBits(1, _base_address, _bit_offset);

        if (o_codebook.sparse)
        {
            std::cout << "sparse" << std::endl;

            for (std::size_t entry_index = 0u;
                 entry_index < o_codebook.entry_count; ++entry_index)
            {
                if (!_remaining_bits)
                    return EVorbisError::kIncompleteHeader;
                --_remaining_bits;
                bool flag = ReadBits(1, _base_address, _bit_offset);

                o_codebook.entry_lengths[entry_index] = 0u;
                if (flag)
                {
                    if (_remaining_bits < 5)
                        return EVorbisError::kIncompleteHeader;
                    _remaining_bits -= 5;
                    o_codebook.entry_lengths[entry_index] = 1u +
                        (std::uint8_t)ReadBits(5, _base_address, _bit_offset);
                }
            }
        }

        else
        {
            if (_remaining_bits < 5 * o_codebook.entry_count)
                return EVorbisError::kIncompleteHeader;
            _remaining_bits -= 5 * o_codebook.entry_count;

            for (std::size_t entry_index = 0u;
                 entry_index < o_codebook.entry_count; ++entry_index)
            {
                o_codebook.entry_lengths[entry_index] = 1u +
                    (std::uint8_t)ReadBits(5, _base_address, _bit_offset);
            }
        }
    }

    else
    {
        if (_remaining_bits < 5)
            return EVorbisError::kIncompleteHeader;
        _remaining_bits -= 5;
        std::uint8_t current_length = (std::uint8_t)ReadBits(5, _base_address, _bit_offset);

        std::uint32_t entry_index = 0u;
        while (entry_index < o_codebook.entry_count)
        {
            int const bits_read = static_cast<int>(ilog(o_codebook.entry_count - entry_index));
            if (_remaining_bits < bits_read)
                return EVorbisError::kIncompleteHeader;
            _remaining_bits -= bits_read;
            std::uint32_t const entry_range = ReadBits(bits_read, _base_address, _bit_offset);

            std::fill(std::next(std::begin(o_codebook.entry_lengths), entry_index),
                      std::next(std::begin(o_codebook.entry_lengths), entry_index + entry_range),
                      current_length);

            entry_index += entry_range;
            if (entry_index > o_codebook.entry_count)
                return EVorbisError::kInvalidSetupHeader;

            ++current_length;
        }
    }

    if (_remaining_bits < 4)
        return EVorbisError::kIncompleteHeader;
    _remaining_bits -= 4;
    o_codebook.lookup_type = (std::uint8_t)ReadBits(4, _base_address, _bit_offset);

    std::cout << "Lookup type " << (unsigned)o_codebook.lookup_type << std::endl;

    if (o_codebook.lookup_type > 2u)
        return EVorbisError::kInvalidSetupHeader;

    if (o_codebook.lookup_type)
    {
        // N.0 * 2^(E - 788) <=> 1.m * 2^(e - 127) * 2^X
        // 1m.0 * 2^-23 * 2^(e-127) * 2^X
        // 1m.0 * 2^(e - 150) * 2^X
        // N.0 * 2^(E - 788) <=> 1m.0 * 2^(e - 150) * 2^X

        // E - 788 = e - 150
        // e = E - 638

        // N = 1m.0 * 2^X
        // log(N) = log(1m.0) + log(2^X)
        // log(N) = 24 + X (log(1m.0) is known to be 24 in IEEE754 binary32)
        // log(N) - 24 = X
        // 1m.0 = N / 2^X

        auto float32_unpack = [](std::uint32_t _v) -> float
        {
            std::uint32_t const mantissa = _v & 0x1fffffu;
            std::uint32_t const sign = _v & 0x80000000u;
            std::uint32_t const exponent = (_v & 0x7fe00000u) >> 21;

            float ref = (float)mantissa * std::pow(2.f, (float)exponent - 788.f) * (sign ? -1.f : 1.f);

            int X = (int)ilog(mantissa) - 24;
            std::uint32_t const ieee_exp = exponent - 638 + X;
            std::uint32_t const ieee_sig = ((X < 0) ? (mantissa << -X) : (mantissa >> X))
                & ((1u << 23) - 1);

            std::uint32_t const ieee_bin = sign | (ieee_exp << 23u) | ieee_sig;
            float res; memcpy(&res, &ieee_bin, 4u);

            if (ref != res)
                std::cout << "[WARNING] incorrect float32_unpack" << std::endl;
            return res;
        };

        if (_remaining_bits < 32)
            return EVorbisError::kIncompleteHeader;
        _remaining_bits -= 32;
        std::uint32_t binary_min_value = ReadBits(32, _base_address, _bit_offset);
        o_codebook.min_value = float32_unpack(binary_min_value);

        if (_remaining_bits < 32)
            return EVorbisError::kIncompleteHeader;
        _remaining_bits -= 32;
        std::uint32_t binary_delta_value = ReadBits(32, _base_address, _bit_offset);
        o_codebook.delta_value = float32_unpack(binary_delta_value);

        std::cout << "Min value " << o_codebook.min_value << std::endl;
        std::cout << "Delta value " << o_codebook.delta_value << std::endl;

        if (_remaining_bits < 4)
            return EVorbisError::kIncompleteHeader;
        _remaining_bits -= 4;
        o_codebook.multiplicand_bit_size = 1u + (std::uint8_t)ReadBits(4, _base_address, _bit_offset);

        if (!_remaining_bits)
            return EVorbisError::kIncompleteHeader;
        --_remaining_bits;
        o_codebook.sequence_p = ReadBits(1, _base_address, _bit_offset);

        std::uint32_t value_count = 0u;
        if (o_codebook.lookup_type == 1u)
            value_count = lookup1_values(o_codebook.entry_count, o_codebook.dimensions);
        else
            value_count = o_codebook.entry_count * o_codebook.dimensions;

        o_codebook.multiplicands.resize(value_count);
        for (std::uint32_t value_index = 0u; value_index < value_count; ++value_index)
        {
            if (_remaining_bits < o_codebook.multiplicand_bit_size)
                return EVorbisError::kIncompleteHeader;
            _remaining_bits -= o_codebook.multiplicand_bit_size;
            o_codebook.multiplicands[value_index] =
                (std::uint16_t)ReadBits(o_codebook.multiplicand_bit_size, _base_address, _bit_offset);
        }
    }

    return EVorbisError::kNoError;
}

std::uint32_t VorbisHeaders(PageContainer const &_pages,
                            std::size_t &_page_index,
                            std::size_t &_seg_index,
                            VorbisIDHeader &o_id_header,
                            VorbisSetupHeader &o_setup_header)
{
    EVorbisError error_code = EVorbisError::kNoError;
    std::uint16_t error_flags = 0u;

    {
        std::size_t packet_size = _pages[_page_index].segment_table[_seg_index];

        if (packet_size == 255u)
            return PackError(EVorbisError::kMissingHeader, 0u);

        if (std::strncmp((char const*)_pages[_page_index].stream_begin, "\x01vorbis", 7u))
            return PackError(EVorbisError::kMissingHeader, 0u);

        if (packet_size < VorbisIDHeader::kSizeOnStream + 7u)
            return PackError(EVorbisError::kIncompleteHeader, 0u);
        if (packet_size > VorbisIDHeader::kSizeOnStream + 7u)
            std::cout << "[WARNING] Unexpected size for Vorbis ID header" << std::endl;

        std::uint8_t const* read_position = _pages[_page_index].stream_begin + 7u;
        std::uint16_t error_flags = 0u;

        o_id_header.page_index = _page_index;
        o_id_header.segment_index = _seg_index;

        if (*(std::uint32_t const*)read_position != 0u)
            error_flags |= FInvalidIDHeader::kVorbisVersion;
        read_position += 4u;

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
            error_flags |= FInvalidIDHeader::kFramingBit;
        if (!o_id_header.audio_channels)
            error_flags |= FInvalidIDHeader::kAudioChannels;
        if (!o_id_header.audio_sample_rate)
            error_flags |= FInvalidIDHeader::kSampleRate;
        if (o_id_header.blocksize_0 > o_id_header.blocksize_1)
            error_flags |= FInvalidIDHeader::kBlocksize;

        if (error_flags)
            return PackError(EVorbisError::kInvalidIDHeader, error_flags);

        if (++_seg_index == _pages[_page_index].segment_count)
        {
            ++_page_index;
            _seg_index = 0u;
        }
    }

    std::cout << "Page index " << _page_index << " segment index " << _seg_index << std::endl;

    std::size_t stream_offset = 0u;
    {
        std::size_t packet_size, page_end, seg_end;
        error_code = ComputePacketSize(_pages, _page_index, _seg_index,
                                       packet_size, page_end, seg_end);
        if (error_code != EVorbisError::kNoError)
            return PackError(error_code, 0u);

        std::cout << std::dec << "Page end " << page_end << " segment end " << seg_end << std::endl;

        if (std::strncmp((char const*)_pages[_page_index].stream_begin, "\x03vorbis", 7))
            return PackError(EVorbisError::kMissingHeader, 0u);

        std::cout << std::dec << "Comment header found page " << _page_index << " segment " << _seg_index << std::endl;
        std::cout << std::dec << "Size is " << packet_size << " bytes" << std::endl;

        _page_index = page_end;
        _seg_index = seg_end;
        stream_offset = packet_size;
    }

    {
        std::size_t packet_size, page_end, seg_end;
        error_code = ComputePacketSize(_pages, _page_index, _seg_index,
                                       packet_size, page_end, seg_end);
        if (error_code != EVorbisError::kNoError)
            return PackError(error_code, 0u);

        std::cout << std::dec << "Page end " << page_end << " segment end " << seg_end << std::endl;

        if (std::strncmp((char const*)_pages[_page_index].stream_begin + stream_offset, "\x05vorbis", 7))
            return PackError(EVorbisError::kMissingHeader, 0u);

        std::cout << std::dec << "Setup header found page " << _page_index << " segment " << _seg_index << std::endl;
        std::cout << std::dec << "Size is " << packet_size << " bytes" << std::endl;

        std::uint8_t const* read_position = _pages[_page_index].stream_begin + stream_offset + 7u;
        int bit_offset = 0;
        int remaining_bits = (packet_size - 7) * 8;

        o_setup_header.page_index = _page_index;
        o_setup_header.segment_index = _seg_index;

        // =====================================================================
        // CODEBOOKS
        // =====================================================================

        std::cout << "CODEBOOKS BEGIN "
                  << std::hex << (read_position - debug_baseBuff)
                  << " offset " << bit_offset << std::endl;
        std::cout << "Remaining bits " << std::dec << remaining_bits << std::endl;

        if (remaining_bits < 8)
            return PackError(EVorbisError::kIncompleteHeader, 0u);
        remaining_bits -= 8;
        std::size_t const codebook_count = 1u + (std::size_t)ReadBits(8, read_position, bit_offset);

        std::cout << std::dec << "Codebook count " << codebook_count << std::endl;
        o_setup_header.codebooks.resize(codebook_count);
        for (std::size_t codebook_index = 0u;
             error_code == EVorbisError::kNoError && codebook_index < codebook_count;
             ++codebook_index)
        {
            error_code = VorbisCodebookDecode(read_position,
                                              bit_offset,
                                              remaining_bits,
                                              o_setup_header.codebooks[codebook_index]);

            VorbisCodebook const& codebook = o_setup_header.codebooks[codebook_index];

#if 1
            std::cout << "Codebook " << std::dec << codebook_index << std::endl
                      << std::dec << codebook.dimensions << " "
                      << std::dec << codebook.entry_count << std::endl;
            for (std::uint32_t entry_index = 0u;
                 entry_index < codebook.entry_count; ++entry_index)
                std::cout << std::dec
                          << (unsigned)codebook.entry_lengths[entry_index] << " ";
            std::cout << std::endl;
#endif
        }

        if (error_code != EVorbisError::kNoError)
            return PackError(error_code, 0u);

        if (remaining_bits < 6)
            return PackError(EVorbisError::kIncompleteHeader, 0u);
        remaining_bits -= 6;
        std::uint8_t vorbis_time_count = 1u + (std::uint8_t)ReadBits(6, read_position, bit_offset);

        for (std::uint8_t vorbis_time_index = 0u;
             vorbis_time_index < vorbis_time_count; ++vorbis_time_index)
        {
            if (remaining_bits < 16)
                return PackError(EVorbisError::kIncompleteHeader, 0u);
            remaining_bits -= 16;
            std::uint16_t v = (std::uint16_t)ReadBits(16, read_position, bit_offset);
            if (v)
                return PackError(EVorbisError::kInvalidSetupHeader, 0u);
        }

        // =====================================================================
        // FLOORS
        // =====================================================================

        std::cout << "FLOORS BEGIN "
                  << std::hex << (read_position - debug_baseBuff)
                  << " offset " << bit_offset << std::endl;
        std::cout << "Remaining bits " << std::dec << remaining_bits << std::endl;

        if (remaining_bits < 6)
            return PackError(EVorbisError::kIncompleteHeader, 0u);
        remaining_bits -= 6;
        std::uint8_t vorbis_floor_count = (std::uint8_t)ReadBits(6, read_position, bit_offset) + 1u;

        std::cout << std::dec << "floor count " << (unsigned)vorbis_floor_count << std::endl;
        o_setup_header.floors.resize(vorbis_floor_count);

        for (std::uint8_t floor_index = 0u;
             floor_index < vorbis_floor_count; ++floor_index)
        {
            VorbisFloor &floor = o_setup_header.floors[floor_index];

            if (remaining_bits < 16)
                return PackError(EVorbisError::kIncompleteHeader, 0u);
            remaining_bits -= 16;
            floor.type = (std::uint16_t)ReadBits(16, read_position, bit_offset);
            std::cout << std::dec << "floor type " << (unsigned)floor.type << std::endl;

            if (floor.type == 0u)
            {
                floor.data = VorbisFloor::Floor0{};
                VorbisFloor::Floor0 &floor0 = std::get<0>(floor.data);

                std::cout << "[WARNING] floor0 detected, anything may happen" << std::endl;

                if (remaining_bits < 8)
                    return PackError(EVorbisError::kIncompleteHeader, 0u);
                remaining_bits -= 8;
                floor0.order = (std::uint8_t)ReadBits(8, read_position, bit_offset);

                if (remaining_bits < 16)
                    return PackError(EVorbisError::kIncompleteHeader, 0u);
                remaining_bits -= 16;
                floor0.rate = (std::uint16_t)ReadBits(16, read_position, bit_offset);

                if (remaining_bits < 16)
                    return PackError(EVorbisError::kIncompleteHeader, 0u);
                remaining_bits -= 16;
                floor0.bark_map_size = (std::uint16_t)ReadBits(16, read_position, bit_offset);

                if (remaining_bits < 6)
                    return PackError(EVorbisError::kIncompleteHeader, 0u);
                remaining_bits -= 6;
                floor0.amplitude_bits = (std::uint8_t)ReadBits(6, read_position, bit_offset);

                if (remaining_bits < 8)
                    return PackError(EVorbisError::kIncompleteHeader, 0u);
                remaining_bits -= 8;
                floor0.amplitude_offset = (std::uint8_t)ReadBits(8, read_position, bit_offset);

                if (remaining_bits < 4)
                    return PackError(EVorbisError::kIncompleteHeader, 0u);
                remaining_bits -= 4;
                floor0.book_count = 1u + (std::uint8_t)ReadBits(4, read_position, bit_offset);

                floor0.codebooks.resize(floor0.book_count);
                for (std::uint8_t book_index = 0u;
                     book_index < floor0.book_count; ++book_index)
                {
                    if (remaining_bits < 8)
                        return PackError(EVorbisError::kIncompleteHeader, 0u);
                    remaining_bits -= 8;
                    floor0.codebooks[book_index] = (std::uint8_t)ReadBits(8, read_position, bit_offset);
                }
            }

            else if (floor.type == 1u)
            {
                floor.data = VorbisFloor::Floor1{};
                VorbisFloor::Floor1 &floor1 = std::get<1>(floor.data);

                if (remaining_bits < 5)
                    return PackError(EVorbisError::kIncompleteHeader, 0u);
                remaining_bits -= 5;
                floor1.partition_count = (std::uint8_t)ReadBits(5, read_position, bit_offset);

                int maximum_class = -1;
                floor1.partition_classes.resize(floor1.partition_count);
                for (std::uint8_t partition_index = 0u;
                     partition_index < floor1.partition_count;
                     ++partition_index)
                {
                    if (remaining_bits < 4)
                        return PackError(EVorbisError::kIncompleteHeader, 0u);
                    remaining_bits -= 4;
                    std::uint8_t partition_class = (std::uint8_t)ReadBits(4, read_position, bit_offset);

                    floor1.partition_classes[partition_index] = partition_class;
                    maximum_class = std::max(maximum_class, (int)partition_class);
                }

                floor1.classes.resize(maximum_class + 1);
                for (int class_index = 0;
                     class_index <= maximum_class; ++class_index)
                {
                    VorbisFloor::Floor1::Class &floor_class = floor1.classes[class_index];

                    if (remaining_bits < 3)
                        return PackError(EVorbisError::kIncompleteHeader, 0u);
                    remaining_bits -= 3;
                    floor_class.dimensions = 1u + (std::uint8_t)ReadBits(3, read_position, bit_offset);

                    if (remaining_bits < 2)
                        return PackError(EVorbisError::kIncompleteHeader, 0u);
                    remaining_bits -= 2;
                    floor_class.subclass_logcount = (std::uint8_t)ReadBits(2, read_position, bit_offset);

                    floor_class.masterbook = 0u;
                    if (floor_class.subclass_logcount)
                    {
                        if (remaining_bits < 8)
                            return PackError(EVorbisError::kIncompleteHeader, 0u);
                        remaining_bits -= 8;
                        floor_class.masterbook = (std::uint8_t)ReadBits(8, read_position, bit_offset);
                    }

                    floor_class.subclass_codebooks.resize(1u << floor_class.subclass_logcount);
                    for (std::size_t subclass_index = 0u;
                         subclass_index < floor_class.subclass_codebooks.size();
                         ++subclass_index)
                    {
                        if (remaining_bits < 8)
                            return PackError(EVorbisError::kIncompleteHeader, 0u);
                        remaining_bits -= 8;
                        floor_class.subclass_codebooks[subclass_index] =
                            (std::uint8_t)ReadBits(8, read_position, bit_offset) - 1u;
                    }
                }

                if (remaining_bits < 2)
                    return PackError(EVorbisError::kIncompleteHeader, 0u);
                remaining_bits -= 2;
                floor1.multiplier = 1u + (std::uint8_t)ReadBits(2, read_position, bit_offset);

                if (remaining_bits < 4)
                    return PackError(EVorbisError::kIncompleteHeader, 0u);
                remaining_bits -= 4;
                std::uint8_t range_bits = (std::uint8_t)ReadBits(4, read_position, bit_offset);

                floor1.value_count = 2u;
                for (std::size_t partition_index = 0u;
                     partition_index < floor1.partition_count;
                     ++partition_index)
                {
                    std::uint8_t class_index = floor1.partition_classes[partition_index];
                    std::uint8_t dimension_count = floor1.classes[class_index].dimensions;
                    floor1.value_count += dimension_count;
                }

                floor1.values.resize(floor1.value_count);
                floor1.values[0] = 0u;
                floor1.values[1] = (1u << range_bits);
                std::size_t floor1_value_index = 2u;
                for (std::size_t partition_index = 0u;
                     partition_index < floor1.partition_count;
                     ++partition_index)
                {
                    std::uint8_t class_index = floor1.partition_classes[partition_index];
                    std::uint8_t dimension_count = floor1.classes[class_index].dimensions;

                    for (std::uint8_t dimension_index = 0u;
                         dimension_index < dimension_count;
                         ++dimension_index)
                    {
                        if (remaining_bits < range_bits)
                            return PackError(EVorbisError::kIncompleteHeader, 0u);
                        remaining_bits -= range_bits;
                        floor1.values[floor1_value_index++] =
                            (std::uint32_t)ReadBits(range_bits, read_position, bit_offset);
                    }
                }

                if (floor1_value_index > 65)
                    return PackError(EVorbisError::kInvalidSetupHeader, 0u);

                for (int i = 0; i < floor1_value_index-1; ++i)
                    for (int j = i+1; j < floor1_value_index; ++j)
                        if (floor1.values[i] == floor1.values[j])
                            return PackError(EVorbisError::kInvalidSetupHeader, 0u);
            }

            else
                return PackError(EVorbisError::kInvalidSetupHeader, 0u);
        }

        // =====================================================================
        // RESIDUES
        // =====================================================================

        std::cout << "RESIDUES BEGIN "
                  << std::hex << (read_position - debug_baseBuff)
                  << " offset " << bit_offset << std::endl;
        std::cout << "Remaining bits " << std::dec << remaining_bits << std::endl;

        if (remaining_bits < 6)
            return PackError(EVorbisError::kIncompleteHeader, 0u);
        remaining_bits -= 6;
        std::uint8_t residue_count = 1u + (std::uint8_t)ReadBits(6, read_position, bit_offset);

        std::cout << "Residue count " << std::dec << (unsigned)residue_count << std::endl;
        o_setup_header.residues.resize(residue_count);

        for (std::uint8_t residue_index = 0u;
             residue_index < residue_count; ++residue_index)
        {
            VorbisResidue &residue = o_setup_header.residues[residue_index];

            if (remaining_bits < 16)
                return PackError(EVorbisError::kIncompleteHeader, 0u);
            remaining_bits -= 16;
            residue.type = (std::uint16_t)ReadBits(16, read_position, bit_offset);

            if (residue.type > 2)
                return PackError(EVorbisError::kInvalidSetupHeader, 0u);

            if (remaining_bits < 24)
                return PackError(EVorbisError::kIncompleteHeader, 0u);
            remaining_bits -= 24;
            residue.begin = ReadBits(24, read_position, bit_offset);

            if (remaining_bits < 24)
                return PackError(EVorbisError::kIncompleteHeader, 0u);
            remaining_bits -= 24;
            residue.end = ReadBits(24, read_position, bit_offset);

            if (remaining_bits < 24)
                return PackError(EVorbisError::kIncompleteHeader, 0u);
            remaining_bits -= 24;
            residue.partition_size = 1u + ReadBits(24, read_position, bit_offset);

            if (remaining_bits < 6)
                return PackError(EVorbisError::kIncompleteHeader, 0u);
            remaining_bits -= 6;
            residue.classif_count = 1u + (std::uint8_t)ReadBits(6, read_position, bit_offset);

            if (remaining_bits < 8)
                return PackError(EVorbisError::kIncompleteHeader, 0u);
            remaining_bits -= 8;
            residue.classbook = (std::uint8_t)ReadBits(8, read_position, bit_offset);

            if (residue.classbook >= o_setup_header.codebooks.size())
                return PackError(EVorbisError::kInvalidSetupHeader, 0u);

            {
                VorbisCodebook const& classbook = o_setup_header.codebooks[residue.classbook];
                if (std::pow((float)residue.classif_count, (float)classbook.dimensions)
                    > (float)classbook.entry_count)
                    return PackError(EVorbisError::kInvalidSetupHeader, 0u);
            }

            std::cout << "Residue " << std::endl
                      << std::dec << residue.type << " "
                      << std::dec << residue.begin << " "
                      << std::dec << residue.end << " "
                      << std::dec << residue.partition_size << " "
                      << std::dec << (unsigned)residue.classif_count << " "
                      << std::dec << (unsigned)residue.classbook << std::endl;

            residue.cascade.resize(residue.classif_count);
            for (std::uint8_t classif_index = 0u;
                 classif_index < residue.classif_count; ++classif_index)
            {
                if (remaining_bits < 3)
                    return PackError(EVorbisError::kIncompleteHeader, 0u);
                remaining_bits -= 3;
                std::uint8_t low_bits = (std::uint8_t)ReadBits(3, read_position, bit_offset);

                if (!remaining_bits)
                    return PackError(EVorbisError::kIncompleteHeader, 0u);
                --remaining_bits;
                bool bitflag = ReadBits(1, read_position, bit_offset);

                std::uint8_t high_bits = 0u;
                if (bitflag)
                {
                    if (remaining_bits < 5)
                        return PackError(EVorbisError::kIncompleteHeader, 0u);
                    remaining_bits -= 5;
                    high_bits = (std::uint8_t)ReadBits(5, read_position, bit_offset);
                }

                residue.cascade[classif_index] = (high_bits << 3) | low_bits;
            }

            std::cout << "Residue cascades " << std::endl;
            std::for_each(residue.cascade.begin(), residue.cascade.end(), [](std::uint8_t const& _v)
            {
                std::cout << std::hex << (unsigned)_v << " ";
            });
            std::cout << std::endl;

            residue.books.resize(residue.classif_count * 8u);
            for (std::uint8_t classif_index = 0u;
                 classif_index < residue.classif_count; ++classif_index)
                for (std::uint8_t stage_index = 0u;
                     stage_index < 8u; ++stage_index)
                    if (residue.cascade[classif_index] & (1u << stage_index))
                    {
                        if (remaining_bits < 8)
                            return PackError(EVorbisError::kIncompleteHeader, 0u);
                        remaining_bits -= 8;
                        std::uint8_t residue_book_index = (std::uint8_t)ReadBits(8, read_position, bit_offset);

                        if (residue_book_index >= codebook_count)
                            return PackError(EVorbisError::kInvalidSetupHeader, 0u);
                        if (!o_setup_header.codebooks[residue_book_index].entry_count)
                            return PackError(EVorbisError::kInvalidSetupHeader, 0u);

                        residue.books[classif_index * 8u + stage_index] = residue_book_index;
                    }
                    else
                        residue.books[classif_index * 8u + stage_index] = VorbisResidue::kUnusedBook;

            std::cout << "Residue books " << std::endl;
            std::for_each(residue.books.begin(), residue.books.end(), [](std::uint16_t const& _v)
            {
                std::cout << std::dec << _v << " ";
            });
            std::cout << std::endl;
        }

        // =====================================================================
        // MAPPINGS
        // =====================================================================

        std::cout << "MAPPINGS BEGIN "
                  << std::hex << (read_position - debug_baseBuff)
                  << " offset " << bit_offset << std::endl;
        std::cout << "Remaining bits " << std::dec << remaining_bits << std::endl;

        if (remaining_bits < 6)
            return PackError(EVorbisError::kIncompleteHeader, 0u);
        remaining_bits -= 6;
        std::uint8_t mapping_count = 1u + (std::uint8_t)ReadBits(6, read_position, bit_offset);

        o_setup_header.mappings.resize(mapping_count);

        for (std::uint8_t mapping_index = 0u;
             mapping_index < mapping_count; ++mapping_index)
        {
            VorbisMapping &mapping = o_setup_header.mappings[mapping_index];

            if (remaining_bits < 16)
                return PackError(EVorbisError::kIncompleteHeader, 0u);
            remaining_bits -= 16;
            mapping.type = (std::uint16_t)ReadBits(16, read_position, bit_offset);

            if (mapping.type)
                return PackError(EVorbisError::kInvalidSetupHeader, 0u);

            if (!remaining_bits)
                return PackError(EVorbisError::kIncompleteHeader, 0u);
            --remaining_bits;
            mapping.submap_flag = ReadBits(1, read_position, bit_offset);

            mapping.submap_count = 1u;
            if (mapping.submap_flag)
            {
                if (remaining_bits < 4)
                    return PackError(EVorbisError::kIncompleteHeader, 0u);
                remaining_bits -= 4;
                mapping.submap_count = 1u + (std::uint8_t)ReadBits(4, read_position, bit_offset);
            }

            if (!remaining_bits)
                return PackError(EVorbisError::kIncompleteHeader, 0u);
            --remaining_bits;
            mapping.coupling_flag = ReadBits(1, read_position, bit_offset);

            mapping.coupling_step_count = 0u;
            if (mapping.coupling_flag)
            {
                std::cout << "coupled" << std::endl;

                if (remaining_bits < 8)
                    return PackError(EVorbisError::kIncompleteHeader, 0u);
                remaining_bits -= 8;
                mapping.coupling_step_count = 1u + (std::uint8_t)ReadBits(8, read_position, bit_offset);

                mapping.magnitudes.resize(mapping.coupling_step_count);
                mapping.angles.resize(mapping.coupling_step_count);
                unsigned bit_size = ilog(o_id_header.audio_channels - 1);

                for (std::uint8_t step_index = 0u;
                     step_index < mapping.coupling_step_count; ++step_index)
                {
                    if (remaining_bits < bit_size)
                        return PackError(EVorbisError::kIncompleteHeader, 0u);
                    remaining_bits -= bit_size;
                    mapping.magnitudes[step_index] = ReadBits(bit_size, read_position, bit_offset);

                    if (remaining_bits < bit_size)
                        return PackError(EVorbisError::kIncompleteHeader, 0u);
                    remaining_bits -= bit_size;
                    mapping.angles[step_index] = ReadBits(bit_size, read_position, bit_offset);

                    if (mapping.magnitudes[step_index] >= o_id_header.audio_channels)
                        return PackError(EVorbisError::kInvalidSetupHeader, 0u);
                    if (mapping.angles[step_index] >= o_id_header.audio_channels)
                        return PackError(EVorbisError::kInvalidSetupHeader, 0u);
                    if (mapping.magnitudes[step_index] == mapping.angles[step_index])
                        return PackError(EVorbisError::kInvalidSetupHeader, 0u);
                }
            }

            if (remaining_bits < 2)
                return PackError(EVorbisError::kIncompleteHeader, 0u);
            remaining_bits -= 2;
            mapping.reserved_field = (std::uint8_t)ReadBits(2, read_position, bit_offset);
            if (mapping.reserved_field)
                return PackError(EVorbisError::kInvalidSetupHeader, 0u);

            std::cout << "Mapping submap count " << (unsigned)mapping.submap_count << std::endl;

            mapping.muxes.resize(o_id_header.audio_channels);
            if (mapping.submap_count > 1)
            {
                for (std::uint32_t channel_index = 0u;
                     channel_index < o_id_header.audio_channels; ++channel_index)
                {
                    if (remaining_bits < 4)
                        return PackError(EVorbisError::kIncompleteHeader, 0u);
                    remaining_bits -= 4;
                    std::uint8_t mapping_mux = (std::uint8_t)ReadBits(4, read_position, bit_offset);

                    if (mapping_mux >= mapping.submap_count)
                        return PackError(EVorbisError::kInvalidSetupHeader, 0u);

                    mapping.muxes[channel_index] = mapping_mux;
                }
            }
            else
                std::fill(mapping.muxes.begin(), mapping.muxes.end(), '\0');

            mapping.submap_floors.resize(mapping.submap_count);
            mapping.submap_residues.resize(mapping.submap_count);
            for (std::uint8_t submap_index = 0u;
                 submap_index < mapping.submap_count; ++submap_index)
            {
                if (remaining_bits < 8)
                    return PackError(EVorbisError::kIncompleteHeader, 0u);
                remaining_bits -= 8;
                ReadBits(8, read_position, bit_offset); // discarded bits

                if (remaining_bits < 8)
                    return PackError(EVorbisError::kIncompleteHeader, 0u);
                remaining_bits -= 8;
                std::uint8_t floor_index = (std::uint8_t)ReadBits(8, read_position, bit_offset);

                std::cout << "Floor index " << (unsigned)floor_index << std::endl;
                if (floor_index >= vorbis_floor_count)
                    return PackError(EVorbisError::kInvalidSetupHeader, 0u);

                mapping.submap_floors[submap_index] = floor_index;

                if (remaining_bits < 8)
                    return PackError(EVorbisError::kIncompleteHeader, 0u);
                remaining_bits -= 8;
                std::uint8_t residue_index = (std::uint8_t)ReadBits(8, read_position, bit_offset);

                std::cout << "Residue index " << (unsigned)residue_index << std::endl;
                if (residue_index >= residue_count)
                    return PackError(EVorbisError::kInvalidSetupHeader, 0u);

                mapping.submap_residues[submap_index] = residue_index;
            }

            std::cout << "Mapping submap floors " << std::endl;
            std::for_each(mapping.submap_floors.begin(), mapping.submap_floors.end(), [](std::uint16_t const& _v)
            {
                std::cout << std::dec << _v << " ";
            });
            std::cout << std::endl;


            std::cout << "Mapping submap residues " << std::endl;
            std::for_each(mapping.submap_residues.begin(), mapping.submap_residues.end(), [](std::uint16_t const& _v)
            {
                std::cout << std::dec << _v << " ";
            });
            std::cout << std::endl;
        }

        // =====================================================================
        // MODES
        // =====================================================================

        std::cout << "MODES BEGIN "
                  << std::hex << (read_position - debug_baseBuff)
                  << " offset " << bit_offset << std::endl;
        std::cout << "Remaining bits " << std::dec << remaining_bits << std::endl;

        if (remaining_bits < 6)
            return PackError(EVorbisError::kIncompleteHeader, 0u);
        remaining_bits -= 6;
        std::uint8_t mode_count = 1u + (std::uint8_t)ReadBits(6, read_position, bit_offset);

        std::cout << "Mode count " << (unsigned)mode_count << std::endl;
        o_setup_header.modes.resize(mode_count);

        for (std::uint8_t mode_index = 0u;
             mode_index < mode_count; ++mode_index)
        {
            VorbisMode &mode = o_setup_header.modes[mode_index];

            if (!remaining_bits)
                return PackError(EVorbisError::kIncompleteHeader, 0u);
            --remaining_bits;
            mode.blockflag = ReadBits(1, read_position, bit_offset);

            if (remaining_bits < 16)
                return PackError(EVorbisError::kIncompleteHeader, 0u);
            remaining_bits -= 16;
            mode.windowtype = (std::uint16_t)ReadBits(16, read_position, bit_offset);

            if (mode.windowtype)
                return PackError(EVorbisError::kInvalidSetupHeader, 0u);

            if (remaining_bits < 16)
                return PackError(EVorbisError::kIncompleteHeader, 0u);
            remaining_bits -= 16;
            mode.transformtype = (std::uint16_t)ReadBits(16, read_position, bit_offset);

            if (mode.transformtype)
                return PackError(EVorbisError::kInvalidSetupHeader, 0u);

            if (remaining_bits < 8)
                return PackError(EVorbisError::kIncompleteHeader, 0u);
            remaining_bits -= 8;
            mode.mapping = (std::uint8_t)ReadBits(8, read_position, bit_offset);

            if (mode.mapping >= mapping_count)
                return PackError(EVorbisError::kInvalidSetupHeader, 0u);
        }

        if (!remaining_bits)
            return PackError(EVorbisError::kIncompleteHeader, 0u);
        --remaining_bits;
        if (!ReadBits(1, read_position, bit_offset))
            return PackError(EVorbisError::kInvalidSetupHeader, 0u);

        ReadBits(remaining_bits, read_position, bit_offset);
        std::cout << "Final bit offset " << bit_offset << std::endl;

        _page_index = page_end;
        _seg_index = seg_end;
    }

    return 0u;
}

std::uint32_t VorbisAudioDecode(PageContainer const &_pages,
                                VorbisIDHeader const &_id,
                                VorbisSetupHeader const &_setup,
                                std::size_t &_page_index,
                                std::size_t &_seg_index)
{
    EVorbisError error_code = EVorbisError::kNoError;

    PageDesc const& page = _pages[_page_index];

    PrintPage(page);
    std::cout << "Offset " << std::hex << debug_ComputeOffset(page, _seg_index) << std::endl;

    if (_seg_index) { int* i = nullptr; *i = 0; }
    std::uint8_t const* read_position = page.stream_begin;
    int bit_offset = 0;

    std::size_t packet_size = 1;
    while (packet_size == 1)
    {
        std::size_t page_end, seg_end;
        error_code = ComputePacketSize(_pages, _page_index, _seg_index,
                                       packet_size, page_end, seg_end);
        if (error_code != EVorbisError::kNoError)
            return PackError(error_code, 0u);

        if (packet_size == 1)
        {
            _page_index = page_end;
            _seg_index = seg_end;
        }
    }

    std::cout << "Packet size " << packet_size << std::endl;

    int remaining_bits = packet_size * 8;

    if (!remaining_bits)
        return PackError(EVorbisError::kInvalidStream, FInvalidStream::kEndOfPacket);
    --remaining_bits;
    std::uint32_t packet_type = ReadBits(1, read_position, bit_offset);
    if (packet_type)
        return PackError(EVorbisError::kInvalidStream, FInvalidStream::kUnexpectedNonAudioPacket);

    unsigned bits_read = ilog(_setup.modes.size() - 1u);
    if (remaining_bits < bits_read)
        return PackError(EVorbisError::kInvalidStream, FInvalidStream::kEndOfPacket);
    remaining_bits -= bits_read;
    std::uint32_t mode_index = ReadBits(bits_read, read_position, bit_offset);
    std::cout << "Mode index " << mode_index << std::endl;

    VorbisMode const& mode = _setup.modes[mode_index];

    std::uint32_t blocksize = !mode.blockflag ?
        1u << _id.blocksize_0 :
        1u << _id.blocksize_1;
    std::cout << "Blocksize " << std::dec << (unsigned)blocksize << std::endl;

    // =========================================================================
    // WINDOW PARAMETERS
    // =========================================================================

    bool vorbis_mode_blockflag = mode.blockflag;
    bool previous_window_flag = false;
    bool next_window_flag = false;

    if (!mode.blockflag)
    {
        if (remaining_bits < 2)
            return PackError(EVorbisError::kInvalidStream, FInvalidStream::kEndOfPacket);
        remaining_bits -= 2;

        previous_window_flag = ReadBits(1, read_position, bit_offset);
        next_window_flag = ReadBits(1, read_position, bit_offset);
        std::cout << "Previous window " << (int)previous_window_flag << std::endl;
        std::cout << "Next window " << (int)next_window_flag << std::endl;
    }

    std::uint32_t window_center = blocksize / 2;

    std::uint32_t left_window_start = 0u;
    std::uint32_t left_window_end = window_center;
    if (vorbis_mode_blockflag && !previous_window_flag)
    {
        left_window_start = blocksize / 4 - _id.blocksize_0 / 4;
        left_window_end = blocksize / 4 + _id.blocksize_0 / 4;
    }

    std::uint32_t right_window_start = window_center;
    std::uint32_t right_window_end = blocksize;
    if (vorbis_mode_blockflag && !next_window_flag)
    {
        right_window_start = blocksize*3 / 4 - _id.blocksize_0 / 4;
        right_window_end = blocksize*3 / 4 + _id.blocksize_0 / 4;
    }

    std::cout << "Window " << std::endl;
    std::cout << blocksize << std::endl;
    std::cout << "[";
    for (std::uint32_t i = 0u; i < blocksize; ++i)
    {
        std::cout << WindowEval(i, left_window_start, left_window_end, right_window_start, right_window_end)
                  << ", ";
    }
    std::cout << std::endl;

    std::cout << "Remaining bits " << remaining_bits << std::endl;

    // =========================================================================
    // FLOOR CURVE
    // =========================================================================

    VorbisMapping const& mapping = _setup.mappings[mode.mapping];

    while (remaining_bits) {

    for (unsigned i = 0; i < _id.audio_channels; ++i)
    {
        std::uint8_t const submap_index = mapping.muxes[i];
        std::uint8_t const floor_index = mapping.submap_floors[submap_index];
        VorbisFloor const& floor_container = _setup.floors[floor_index];

        bool unused = false;
        // according to floor.type, call the appropriate floor decode function
        switch (floor_container.type)
        {
        case 0:
        {
            VorbisFloor::Floor0 const& floor = std::get<0>(floor_container.data);

            if (remaining_bits < floor.amplitude_bits)
                return PackError(EVorbisError::kInvalidStream, FInvalidStream::kEndOfPacket);
            std::uint32_t amplitude = ReadBits(floor.amplitude_bits, read_position, remaining_bits);
            remaining_bits -= floor.amplitude_bits;

            if (amplitude)
            {
                //std::vector<> coefficients;
                unsigned bit_count = ilog(floor.book_count);
                if (remaining_bits < bit_count)
                    return PackError(EVorbisError::kInvalidStream, FInvalidStream::kEndOfPacket);
                std::uint32_t book_index = ReadBits(bit_count, read_position, remaining_bits);
                remaining_bits -= bit_count;

                if (book_index >= _setup.codebooks.size())
                    return PackError(EVorbisError::kInvalidStream, FInvalidStream::kUndecodablePacket);

                std::cout << "Offset " << std::hex << debug_ComputeOffset(page, _seg_index) + (read_position - &page.segment_table[_seg_index]) << std::endl;
            }
        } break;

        case 1:
        {
            VorbisFloor::Floor1 const& floor = std::get<1>(floor_container.data);

            bool nonzero = false;
            if (remaining_bits)
            {
                nonzero = ReadBits(1, read_position, bit_offset);
                --remaining_bits;
            }

            if (nonzero)
            {
                static const std::uint32_t kRanges[] = { 256, 128, 86, 64 };

                std::uint32_t range = kRanges[floor.multiplier-1];
                std::uint32_t bit_count = ilog(range-1);
                std::vector<std::uint32_t> yvalues(2);

                if (remaining_bits < bit_count) {
                    nonzero = false; break;
                }
                yvalues[0] = ReadBits(bit_count, read_position, bit_offset);
                remaining_bits -= bit_count;

                if (remaining_bits < bit_count) {
                    nonzero = false; break;
                }
                yvalues[1] = ReadBits(bit_count, read_position, bit_offset);
                remaining_bits -= bit_count;

                std::size_t yindex = 2;
                for (std::uint8_t i = 0u; i < floor.partition_count; ++i)
                {
                    VorbisFloor::Floor1::Class partition_class = floor.classes[floor.partition_classes[i]];
                    std::uint8_t cdim = partition_class.dimensions;
                    std::uint8_t cbits = partition_class.subclass_logcount;
                    std::uint32_t csub = (1u << cbits) - 1u;
                    std::uint32_t cval = 0u;
                    if (cbits > 0)
                    {
                        VorbisCodebook const& codebook = _setup.codebooks[partition_class.masterbook];
                        HuffmanLUT codebook_lut = Huffman_BuildLookupTable(BuildHuffmanTree(codebook.entry_lengths));

                        int bits_read = 0;
                        cval = Huffman_ReadEntry(codebook_lut, read_position, bit_offset, bits_read);
                        if (remaining_bits < bits_read) {
                            nonzero = false; break;
                        }
                        if (bits_read < 0) return cval;
                        remaining_bits -= bits_read;
                    }

                    yvalues.resize(yindex + cdim);
                    for (std::uint8_t j = 0u; j < cdim; ++j)
                    {
                        std::uint32_t subbook_index = cval & csub;
                        std::uint8_t codebook_index = partition_class.subclass_codebooks[subbook_index];
                        cval = cval >> cbits;
                        if (codebook_index != 0xff)
                        {
                            VorbisCodebook const& codebook = _setup.codebooks[codebook_index];
                            HuffmanLUT codebook_lut = Huffman_BuildLookupTable(BuildHuffmanTree(codebook.entry_lengths));

                            int bits_read = 0;
                            yvalues[yindex + j] = Huffman_ReadEntry(codebook_lut, read_position, bit_offset, bits_read);
                            if (remaining_bits < bits_read) {
                                nonzero = false; break;
                            }
                            if (bits_read < 0) return yvalues[yindex + j];
                            remaining_bits -= bits_read;
                        }
                        else
                        {
                            yvalues[yindex + j] = 0u;
                        }
                    }
                    yindex += cdim;
                }

                assert(nonzero);
                // Amplitude value synthesis
                std::vector<bool> step2_flag(yvalues.size());
                step2_flag[0] = true; step2_flag[1] = true;
                std::vector<std::uint32_t> final_yvalues(yvalues.size());
                final_yvalues[0] = yvalues[0]; final_yvalues[1] = yvalues[1];
                for (std::size_t i = 2; i < yvalues.size(); ++i)
                {
                    std::size_t ln_offset = low_neighbour(floor.values, i);
                    std::size_t hn_offset = high_neighbour(floor.values, i);

                    std::int32_t predicted = (std::int32_t)render_point(floor.values[ln_offset],
                                                                        final_yvalues[ln_offset],
                                                                        floor.values[hn_offset],
                                                                        final_yvalues[hn_offset],
                                                                        floor.values[i]);

                    std::int32_t val = (std::int32_t)yvalues[i];

                    std::int32_t highroom = (std::int32_t)range - predicted;
                    std::int32_t lowroom = (std::int32_t)predicted;
                    std::int32_t room = std::min(highroom, lowroom) * 2;

                    if (val)
                    {
                        step2_flag[ln_offset] = true;
                        step2_flag[hn_offset] = true;
                        step2_flag[i] = true;
                        if (val >= room)
                        {
                            if (highroom > lowroom)
                                final_yvalues[i] = std::min(range, (std::uint32_t)std::max(0, val));
                            else
                                final_yvalues[i] = std::min(range, (std::uint32_t)std::max(0, predicted - (val - highroom) - 1));
                        }
                        else
                        {
                            if (val & 0x1)
                                final_yvalues[i] = std::min(range, (std::uint32_t)std::max(0, predicted - ((val + 1) / 2)));
                            else
                                final_yvalues[i] = std::min(range, (std::uint32_t)std::max(0, predicted + (val / 2)));
                        }
                    }
                    else
                    {
                        step2_flag[i] = false;
                        final_yvalues[i] = std::min(range, (std::uint32_t)std::max(0, predicted));
                    }
                }

                // Curve synthesis
                std::vector<int32_t> floor_vector(blocksize);
            }
        } break;
        default: break;
        }
    }
    }

    return 0u;
}

std::vector<BinaryNode> BuildHuffmanTree(std::vector<std::uint8_t> const& _lengths)
{
    std::vector<BinaryNode> tree(1);
    tree.reserve(_lengths.size() * 2u);

    std::uint32_t entry_count = 0u;
    for (std::uint8_t length : _lengths)
    {
        if (length == 0u) continue;
        ++entry_count;

        std::vector<std::size_t> path{ 0u };
        while (path.back() < -2u)
        {
            std::size_t nindex = path.back();
            if ((path.size() > 1u && !tree[nindex].left && !tree[nindex].right) ||
                (path.size() == length+1))
            {
                path.pop_back();
                while (tree[path.back()].right == nindex)
                {
                    nindex = path.back();
                    path.pop_back();
                }
                if (path.empty())
                    return std::vector<BinaryNode>{};

                nindex = path.back();

                if (tree[nindex].right)
                    path.push_back(tree[nindex].right);
                else
                    path.push_back(-2u);
            }
            else
            {
                if (tree[nindex].left)
                    path.push_back(tree[nindex].left);
                else
                    path.push_back(-1u);
            }

            assert(path.back() <= -1u);
            assert(path.size() <= length+1);
        }

        std::uint32_t codeword = 0u;
        for (int i = 1; i < path.size()-1; ++i)
        {
            std::uint32_t bit = (path[i] == tree[path[i-1]].left) ? 0u : 1u;
            codeword |= bit << (length-i);
        }

        std::size_t nindex = *std::next(path.rbegin(), 1);
        tree.emplace_back();
        if (path.back() == -2u)
        {
            codeword |= 1u << ((length+1)-path.size());
            assert(!tree[nindex].right);
            tree[nindex].right = tree.size()-1u;
            nindex = tree[nindex].right;
        }
        else
        {
            assert(!tree[nindex].left);
            tree[nindex].left = tree.size()-1u;
            nindex = tree[nindex].left;
        }

        for (int i = 0; i < (length - (path.size()-1)); ++i)
        {
            tree.emplace_back();
            tree[nindex].left = tree.size()-1u;
            nindex = tree[nindex].left;
        }

        tree[nindex].v = codeword << (32 - length);
        tree[nindex].length = length;
        assert(!tree[nindex].left && !tree[nindex].right);
    }

    for (BinaryNode const &node : tree)
    {
        assert((!node.left && !node.right) ||
               (node.left && node.right));
    }
    assert(tree.size() == (entry_count * 2u)-1u);

    return tree;
}

HuffmanLUT Huffman_BuildLookupTable(std::vector<BinaryNode> const& _tree)
{
    HuffmanLUT result;
    result.entries.reserve((_tree.size() + 1)/2);
    result.lengths.reserve((_tree.size() + 1)/2);
    result.indices.reserve((_tree.size() + 1)/2);

    std::uint32_t node_index = 0u;
    for (BinaryNode const& node : _tree)
    {
        if (node.v == -1u) continue;

        auto entry_it = std::lower_bound(result.entries.begin(), result.entries.end(), node.v);
        auto entry_index = std::distance(result.entries.begin(), entry_it);
        auto lengths_it = std::next(result.lengths.begin(), entry_index);
        auto indices_it = std::next(result.indices.begin(), entry_index);
        result.entries.insert(entry_it, node.v);
        result.lengths.insert(lengths_it, node.length);
        result.indices.insert(indices_it, node_index++);
    }

    return result;
}

std::uint32_t Huffman_ReadEntry(HuffmanLUT const& _lut,
                                std::uint8_t const* &_base_address,
                                int &_bit_offset,
                                int &o_bits_read)
{
    std::uint32_t buffer = 0u;
    int bits_read = 0;
    auto entry_it = std::prev(_lut.entries.end());
    auto length_it = _lut.lengths.begin();

    while (!(*length_it == bits_read && *entry_it == buffer))
    {
        buffer |= (ReadBits(1, _base_address, _bit_offset) << (31-bits_read++));
        entry_it = std::lower_bound(_lut.entries.begin(), _lut.entries.end(), buffer);
        length_it = std::next(_lut.lengths.begin(), std::distance(_lut.entries.begin(), entry_it));

        if (bits_read > 31)
        {
            o_bits_read = -1;
            return PackError(EVorbisError::kInvalidStream, FInvalidStream::kUnknownCodeword);
        }
    }

    o_bits_read = bits_read-1;
    return *std::next(_lut.indices.begin(), std::distance(_lut.entries.begin(), entry_it));
}

void Huffman_FunctionalTest()
{
    auto test_tree = BuildHuffmanTree({2, 2, 2, 2, 2});
    assert(test_tree.empty());

    test_tree = BuildHuffmanTree({2, 4, 4, 4, 4, 2, 3, 3});

    std::cout << "huffman begin" << std::endl;
    for (BinaryNode const&node : test_tree)
    {
        if (node.v != -1u)
        {
            std::cout << std::dec << (unsigned)node.length << " "
                      << std::hex << (unsigned)node.v << std::endl;
        }
    }
    auto test_lut = Huffman_BuildLookupTable(test_tree);
    std::uint32_t test_value = 0x00000001;
    std::uint8_t const* dummy_buff = (std::uint8_t const*)&test_value;
    int bit_offset = 0;
    int bits_read = 0;
    std::uint32_t entry = Huffman_ReadEntry(test_lut,
                                            dummy_buff,
                                            bit_offset,
                                            bits_read);
    assert(entry == 5);
};

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cout << "No file specified" << std::endl;
        return 1;
    }

    std::unique_ptr<std::uint8_t> buff{};
    std::streamsize file_size = 0ull;
    {
        std::ifstream file(argv[1], std::ios_base::binary);
        file.seekg(0, std::ios_base::end);
        file_size = static_cast<std::streamsize>(file.tellg());
        file.seekg(0, std::ios_base::beg);

        if (file_size < (1024 * 1024 * 1024))
        {
            buff.reset(new std::uint8_t [file_size]);
            file.read(reinterpret_cast<char*>(buff.get()), file_size);
        }
        else
        {
            std::cout << "File is too large" << std::endl;
            return 1;
        }

        std::cout << file_size << std::endl;
    }
    debug_baseBuff = buff.get();

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

    std::size_t page_index = 0u;
    std::size_t seg_index = 0u;
    VorbisIDHeader id_header;
    VorbisSetupHeader setup_header;
    std::uint32_t res = VorbisHeaders(ogg_pages.at(vorbis_serials.front()),
                                      page_index, seg_index,
                                      id_header, setup_header);
    if (res >> 16u != EVorbisError::kNoError)
    {
        std::cout << "Vorbis error " << (res >> 16u) << std::endl;
        return 1;
    }

    std::cout << "Page " << page_index << " segment " << seg_index << std::endl;

#if 0
    for (VorbisCodebook const& codebook : setup_header.codebooks)
    {
        using StdClock_t = std::chrono::high_resolution_clock;
        StdClock_t::time_point begin = StdClock_t::now();
        auto data_tree = BuildHuffmanTree(codebook.entry_lengths);
        auto data_lut = Huffman_BuildLookupTable(data_tree);
        StdClock_t::time_point end = StdClock_t::now();
        std::cout << "BuildHuffmanTree(), size=" << data_tree.size()
                  << "; time=" << std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count() << std::endl;

        for (BinaryNode const&node : data_tree)
        {
            if (node.v != -1u)
            {
                std::cout << std::dec << (unsigned)node.length << " "
                          << std::hex << (unsigned)node.v << std::endl;
            }
        }
        for (int i = 0; i < data_lut.entries.size(); ++i)
        {
            std::uint32_t mask = ((1u << data_lut.lengths[i]) - 1u) << (32-data_lut.lengths[i]);
            std::cout << std::dec << i << " "
                      << std::hex << data_lut.entries[i] << " " << mask << std::endl;
        }
    }
#endif

    res = VorbisAudioDecode(ogg_pages.at(vorbis_serials.front()),
                            id_header,
                            setup_header,
                            page_index, seg_index);
    std::cout << "AudioDecode output " << res << std::endl;

    if (!res)
    {
        seg_index = 0;
        res = VorbisAudioDecode(ogg_pages.at(vorbis_serials.front()),
                                id_header,
                                setup_header,
                                ++page_index, seg_index);
        std::cout << "AudioDecode output " << res << std::endl;
    }

    std::cout << "ID header : " << std::endl
              << std::dec
              << id_header.page_index << " " << id_header.segment_index << std::endl
              << (unsigned)id_header.audio_channels << " " << id_header.audio_sample_rate << std::endl
              << id_header.bitrate_max << " " << id_header.bitrate_nominal << " " << id_header.bitrate_min << std::endl
              << (unsigned)id_header.blocksize_0 << " " << (unsigned)id_header.blocksize_1 << std::endl;

    return 0;
}
