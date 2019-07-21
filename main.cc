/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Samuel Bourasseau wrote this file. You can do whatever you want with this
 * stuff. If we meet some day, and you think this stuff is worth it, you can
 * buy me a beer in return.
 * ----------------------------------------------------------------------------
 */

#include <algorithm>
#include <cmath>
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

    std::uint8_t const* stream_begin = nullptr;
};

using PageContainer = std::vector<PageDesc>;
using OggContents = std::unordered_map<std::uint32_t, PageContainer>;

int debug_PageCount = 0;
std::uint8_t* debug_baseBuff;

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
    // + 1 bit framing flag (0x01 because of byte alignment)
};

struct VorbisCodebook
{
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
    kInvalidStream,
    kEndOfStream,

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
    o_codebook.entry_lengths.reset(new std::uint8_t[o_codebook.entry_count]);

    if (!_remaining_bits)
        return EVorbisError::kIncompleteHeader;
    _remaining_bits -= 1;
    bool ordered = ReadBits(1, _base_address, _bit_offset);

    if (!ordered)
    {
        if (!_remaining_bits)
            return EVorbisError::kIncompleteHeader;
        _remaining_bits -= 1;
        bool sparse = ReadBits(1, _base_address, _bit_offset);

        if (sparse)
        {
            std::cout << "sparse" << std::endl;

            for (std::size_t entry_index = 0u;
                 entry_index < o_codebook.entry_count; ++entry_index)
            {
                if (!_remaining_bits)
                    return EVorbisError::kIncompleteHeader;
                --_remaining_bits;
                bool flag = ReadBits(1, _base_address, _bit_offset);

                o_codebook.entry_lengths.get()[entry_index] = 0u;
                if (flag)
                {
                    if (_remaining_bits < 5)
                        return EVorbisError::kIncompleteHeader;
                    _remaining_bits -= 5;
                    o_codebook.entry_lengths.get()[entry_index] =
                        (std::uint8_t)ReadBits(5, _base_address, _bit_offset) + 1u;
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
                o_codebook.entry_lengths.get()[entry_index] =
                    (std::uint8_t)ReadBits(5, _base_address, _bit_offset) + 1u;
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

            std::fill(o_codebook.entry_lengths.get() + entry_index,
                      o_codebook.entry_lengths.get() + entry_index + entry_range,
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
    std::uint8_t const lookup_type = (std::uint8_t)ReadBits(4, _base_address, _bit_offset);

    std::cout << "Lookup type " << (unsigned)lookup_type << std::endl;

    if (lookup_type > 2u)
        return EVorbisError::kInvalidSetupHeader;

    if (lookup_type)
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
        float min_value = float32_unpack(binary_min_value);

        if (_remaining_bits < 32)
            return EVorbisError::kIncompleteHeader;
        _remaining_bits -= 32;
        std::uint32_t binary_delta_value = ReadBits(32, _base_address, _bit_offset);
        float delta_value = float32_unpack(binary_delta_value);

        std::cout << "Min value " << min_value << std::endl;
        std::cout << "Delta value " << delta_value << std::endl;

        if (_remaining_bits < 4)
            return EVorbisError::kIncompleteHeader;
        _remaining_bits -= 4;
        std::uint8_t value_bit_count = (std::uint8_t)ReadBits(4, _base_address, _bit_offset) + 1u;

        if (!_remaining_bits)
            return EVorbisError::kIncompleteHeader;
        --_remaining_bits;
        bool sequence_p = !!ReadBits(1, _base_address, _bit_offset);

        std::uint32_t value_count = 0u;
        if (lookup_type == 1u)
            value_count = lookup1_values(o_codebook.entry_count, o_codebook.dimensions);
        else
            value_count = o_codebook.entry_count * o_codebook.dimensions;

        std::vector<std::uint16_t> multiplicands;
        multiplicands.reserve(value_count);
        for (std::uint32_t value_index = 0u; value_index < value_count; ++value_index)
        {
            if (_remaining_bits < value_bit_count)
                return EVorbisError::kIncompleteHeader;
            _remaining_bits -= value_bit_count;
            multiplicands.emplace_back((std::uint16_t)ReadBits(value_bit_count, _base_address, _bit_offset));
        }
    }

    return EVorbisError::kNoError;
}

std::uint32_t VorbisHeaders(PageContainer const &_pages, VorbisIDHeader &o_id_header)
{
    EVorbisError error_code = EVorbisError::kNoError;
    std::uint16_t error_flags = 0u;

    std::size_t page_index = 0u;
    std::size_t seg_index = 0u;

    {
        std::size_t packet_size = _pages[page_index].segment_table[seg_index];

        if (packet_size == 255u)
            return PackError(EVorbisError::kMissingHeader, 0u);

        if (std::strncmp((char const*)_pages[page_index].stream_begin, "\x01vorbis", 7u))
            return PackError(EVorbisError::kMissingHeader, 0u);

        if (packet_size < VorbisIDHeader::kSizeOnStream + 7u)
            return PackError(EVorbisError::kIncompleteHeader, 0u);
        if (packet_size > VorbisIDHeader::kSizeOnStream + 7u)
            std::cout << "[WARNING] Unexpected size for Vorbis ID header" << std::endl;

        std::uint8_t const* read_position = _pages[page_index].stream_begin + 7u;
        std::uint16_t error_flags = 0u;

        o_id_header.page_index = page_index;
        o_id_header.segment_index = seg_index;

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

        if (++seg_index == _pages[page_index].segment_count)
        {
            ++page_index;
            seg_index = 0u;
        }
    }

    std::cout << "Page index " << page_index << " segment index " << seg_index << std::endl;

    std::size_t stream_offset = 0u;
    {
        std::size_t packet_size, page_end, seg_end;
        error_code = ComputePacketSize(_pages, page_index, seg_index,
                                       packet_size, page_end, seg_end);
        if (error_code != EVorbisError::kNoError)
            return PackError(error_code, 0u);

        std::cout << std::dec << "Page end " << page_end << " segment end " << seg_end << std::endl;

        if (std::strncmp((char const*)_pages[page_index].stream_begin, "\x03vorbis", 7))
            return PackError(EVorbisError::kMissingHeader, 0u);

        std::cout << std::dec << "Comment header found page " << page_index << " segment " << seg_index << std::endl;
        std::cout << std::dec << "Size is " << packet_size << " bytes" << std::endl;

        page_index = page_end;
        seg_index = seg_end;
        stream_offset = packet_size;
    }

    {
        std::size_t packet_size, page_end, seg_end;
        error_code = ComputePacketSize(_pages, page_index, seg_index,
                                       packet_size, page_end, seg_end);
        if (error_code != EVorbisError::kNoError)
            return PackError(error_code, 0u);

        std::cout << std::dec << "Page end " << page_end << " segment end " << seg_end << std::endl;

        if (std::strncmp((char const*)_pages[page_index].stream_begin + stream_offset, "\x05vorbis", 7))
            return PackError(EVorbisError::kMissingHeader, 0u);

        std::cout << std::dec << "Setup header found page " << page_index << " segment " << seg_index << std::endl;
        std::cout << std::dec << "Size is " << packet_size << " bytes" << std::endl;

        std::uint8_t const* read_position = _pages[page_index].stream_begin + stream_offset + 7u;
        std::size_t const codebook_count = (std::size_t)(*read_position++) + 1u;

        std::cout << std::dec << "Codebook count " << codebook_count << std::endl;
        std::vector<VorbisCodebook> codebooks(codebook_count);
        int bit_offset = 0;
        int remaining_bits = (packet_size - 8) * 8;
        for (std::size_t codebook_index = 0u;
             error_code == EVorbisError::kNoError && codebook_index < codebook_count;
             ++codebook_index)
        {
            error_code = VorbisCodebookDecode(read_position,
                                              bit_offset,
                                              remaining_bits,
                                              codebooks[codebook_index]);

            VorbisCodebook const& codebook = codebooks[codebook_index];

#if 1
            std::cout << "Codebook " << std::dec << codebook_index << std::endl
                      << std::dec << codebook.dimensions << " "
                      << std::dec << codebook.entry_count << std::endl;
            if (codebook.entry_lengths)
                for (std::uint32_t entry_index = 0u;
                     entry_index < codebook.entry_count; ++entry_index)
                    std::cout << std::dec
                              << (unsigned)codebook.entry_lengths.get()[entry_index] << " ";
            std::cout << std::endl;
#endif
        }
        std::cout << "Remaining bits " << remaining_bits << std::endl;

        if (error_code != EVorbisError::kNoError)
            return PackError(error_code, 0u);

        if (remaining_bits < 6)
            return PackError(EVorbisError::kInvalidSetupHeader, 0u);
        remaining_bits -= 6;
        std::uint8_t vorbis_time_count = (std::uint8_t)ReadBits(6, read_position, bit_offset) + 1u;
        for (std::uint8_t vorbis_time_index = 0u;
             vorbis_time_index < vorbis_time_count; ++vorbis_time_index)
        {
            if (remaining_bits < 16)
                return PackError(EVorbisError::kInvalidSetupHeader, 0u);
            remaining_bits -= 16;
            std::uint16_t v = (std::uint16_t)ReadBits(16, read_position, bit_offset);
            if (v)
                return PackError(EVorbisError::kInvalidSetupHeader, 0u);
        }

        if (remaining_bits < 6)
            return PackError(EVorbisError::kInvalidSetupHeader, 0u);
        remaining_bits -= 6;
        std::uint8_t vorbis_floor_count = (std::uint8_t)ReadBits(6, read_position, bit_offset) + 1u;
        std::cout << std::dec << "floor count " << (unsigned)vorbis_floor_count << std::endl;
        for (std::uint8_t floor_index = 0u;
             floor_index < vorbis_floor_count; ++floor_index)
        {
            if (remaining_bits < 16)
                return PackError(EVorbisError::kInvalidSetupHeader, 0u);
            remaining_bits -= 6;
            std::uint16_t vorbis_floor_type = (std::uint16_t)ReadBits(16, read_position, bit_offset);
            std::cout << std::dec << "floor type " << (unsigned)vorbis_floor_type << std::endl;

            if (vorbis_floor_type == 0u)
            {
                std::cout << "[WARNING] floor0 detected, anything may happen" << std::endl;

                if (remaining_bits < 8)
                    return PackError(EVorbisError::kInvalidSetupHeader, 0u);
                remaining_bits -= 8;
                std::uint8_t floor0_order = (std::uint8_t)ReadBits(8, read_position, bit_offset);

                if (remaining_bits < 16)
                    return PackError(EVorbisError::kInvalidSetupHeader, 0u);
                remaining_bits -= 16;
                std::uint16_t floor0_rate = (std::uint16_t)ReadBits(16, read_position, bit_offset);

                if (remaining_bits < 16)
                    return PackError(EVorbisError::kInvalidSetupHeader, 0u);
                remaining_bits -= 16;
                std::uint16_t floor0_bark_map_size = (std::uint16_t)ReadBits(16, read_position, bit_offset);

                if (remaining_bits < 6)
                    return PackError(EVorbisError::kInvalidSetupHeader, 0u);
                remaining_bits -= 6;
                std::uint8_t floor0_amplitude_bits = (std::uint8_t)ReadBits(6, read_position, bit_offset);

                if (remaining_bits < 8)
                    return PackError(EVorbisError::kInvalidSetupHeader, 0u);
                remaining_bits -= 8;
                std::uint8_t floor0_amplitude_offset = (std::uint8_t)ReadBits(8, read_position, bit_offset);

                if (remaining_bits < 4)
                    return PackError(EVorbisError::kInvalidSetupHeader, 0u);
                remaining_bits -= 4;
                std::uint8_t floor0_book_count = (std::uint8_t)ReadBits(4, read_position, bit_offset) + 1u;

                for (std::uint8_t book_index = 0u;
                     book_index < floor0_book_count; ++book_index)
                {
                    if (remaining_bits < 8)
                        return PackError(EVorbisError::kInvalidSetupHeader, 0u);
                    remaining_bits -= 8;
                    std::uint8_t codebook_index = (std::uint8_t)ReadBits(8, read_position, bit_offset);
                    std::cout << std::dec << "codebook index " << (unsigned)codebook_index << std::endl;
                }
            }

            else if (vorbis_floor_type == 1u)
            {
                if (remaining_bits < 5)
                    return PackError(EVorbisError::kInvalidSetupHeader, 0u);
                remaining_bits -= 5;
                std::uint8_t floor1_partition_count = (std::uint8_t)ReadBits(5, read_position, bit_offset);

                int maximum_class = -1;
                std::vector<std::uint8_t> floor1_partition_classes(floor1_partition_count);
                for (std::uint8_t partition_index = 0u;
                     partition_index < floor1_partition_count;
                     ++partition_index)
                {
                    if (remaining_bits < 4)
                        return PackError(EVorbisError::kInvalidSetupHeader, 0u);
                    remaining_bits -= 4;
                    floor1_partition_classes[partition_index] = (std::uint8_t)ReadBits(4, read_position, bit_offset);
                    maximum_class = std::max(maximum_class, (int)floor1_partition_classes[partition_index]);
                }

                std::vector<std::uint8_t> floor1_class_dimensions(maximum_class + 1);
                for (int class_index = 0;
                     class_index <= maximum_class; ++class_index)
                {
                    if (remaining_bits < 3)
                        return PackError(EVorbisError::kInvalidSetupHeader, 0u);
                    remaining_bits -= 3;
                    floor1_class_dimensions[class_index] = (std::uint8_t)ReadBits(3, read_position, bit_offset) + 1u;

                    if (remaining_bits < 2)
                        return PackError(EVorbisError::kInvalidSetupHeader, 0u);
                    remaining_bits -= 2;
                    std::uint8_t floor1_class_subclass_logcount =
                        (std::uint8_t)ReadBits(2, read_position, bit_offset);

                    std::uint8_t floor1_class_masterbook_count = 0u;
                    if (floor1_class_subclass_logcount)
                    {
                        if (remaining_bits < 8)
                            return PackError(EVorbisError::kInvalidSetupHeader, 0u);
                        remaining_bits -= 8;
                        floor1_class_masterbook_count = (std::uint8_t)ReadBits(8, read_position, bit_offset);
                    }

                    std::vector<std::uint8_t> floor1_subclass_books((1u << floor1_class_subclass_logcount));
                    for (std::size_t subclass_index = 0u;
                         subclass_index < floor1_subclass_books.size();
                         ++subclass_index)
                    {
                        if (remaining_bits < 8)
                            return PackError(EVorbisError::kInvalidSetupHeader, 0u);
                        remaining_bits -= 8;
                        floor1_subclass_books[subclass_index] =
                            (std::uint8_t)ReadBits(8, read_position, bit_offset) - 1u;
                    }
                }

                if (remaining_bits < 2)
                    return PackError(EVorbisError::kInvalidSetupHeader, 0u);
                remaining_bits -= 2;
                std::uint8_t floor1_multiplier = (std::uint8_t)ReadBits(2, read_position, bit_offset) + 1u;

                if (remaining_bits < 4)
                    return PackError(EVorbisError::kInvalidSetupHeader, 0u);
                remaining_bits -= 4;
                std::uint8_t range_bits = (std::uint8_t)ReadBits(4, read_position, bit_offset);

                unsigned floor1_value_count = 2u;
                for (std::size_t partition_index = 0u;
                     partition_index < floor1_partition_classes.size();
                     ++partition_index)
                {
                    std::uint8_t class_index = floor1_partition_classes[partition_index];
                    std::uint8_t dimension_count = floor1_class_dimensions[class_index];
                    floor1_value_count += dimension_count;
                }

                std::vector<std::uint16_t> floor1_Xs(floor1_value_count);
                floor1_Xs[0] = 0u;
                floor1_Xs[1] = (1u << range_bits);
                std::size_t floor1_value_index = 2u;
                for (std::size_t partition_index = 0u;
                     partition_index < floor1_partition_count;
                     ++partition_index)
                {
                    std::uint8_t class_index = floor1_partition_classes[partition_index];
                    std::uint8_t dimension_count = floor1_class_dimensions[class_index];

                    for (std::uint8_t dimension_index = 0u;
                         dimension_index < dimension_count;
                         ++dimension_index)
                    {
                        if (remaining_bits < range_bits)
                            return PackError(EVorbisError::kInvalidSetupHeader, 0u);
                        remaining_bits -= range_bits;
                        floor1_Xs[floor1_value_index++] =
                            (std::uint16_t)ReadBits(range_bits, read_position, bit_offset);
                    }
                }
            }

            else
                return PackError(EVorbisError::kInvalidSetupHeader, 0u);
        }

        std::cout << "Remaining bits " << remaining_bits << std::endl;

        page_index = page_end;
        seg_index = seg_end;
    }

    return 0u;
}

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

    VorbisIDHeader id_header;
    std::uint32_t res = VorbisHeaders(ogg_pages.at(vorbis_serials.front()), id_header);
    if (res >> 16u != EVorbisError::kNoError)
    {
        std::cout << "Vorbis error " << (res >> 16u) << std::endl;
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
