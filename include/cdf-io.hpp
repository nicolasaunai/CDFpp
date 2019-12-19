#pragma once
/*------------------------------------------------------------------------------
-- This file is a part of the CDFpp library
-- Copyright (C) 2019, Plasma Physics Laboratory - CNRS
--
-- This program is free software; you can redistribute it and/or modify
-- it under the terms of the GNU General Public License as published by
-- the Free Software Foundation; either version 2 of the License, or
-- (at your option) any later version.
--
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
-- GNU General Public License for more details.
--
-- You should have received a copy of the GNU General Public License
-- along with this program; if not, write to the Free Software
-- Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
-------------------------------------------------------------------------------*/
/*-- Author : Alexis Jeandet
-- Mail : alexis.jeandet@member.fsf.org
----------------------------------------------------------------------------*/
#include "cdf-endianness.hpp"
#include "cdf-enums.hpp"
#include "cdf-file.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <optional>
#include <utility>

namespace cdf::io
{
namespace
{
    struct data_chunk_t
    {
        data_chunk_t(std::size_t position, std::size_t size) : position { position }, size { size }
        {
        }
        std::size_t position;
        std::size_t size;
    };


    bool is_big_endian(cdf_encoding encoding)
    {
        return encoding == cdf_encoding::network || encoding == cdf_encoding::SUN
            || encoding == cdf_encoding::NeXT || encoding == cdf_encoding::PPC
            || encoding == cdf_encoding::SGi || encoding == cdf_encoding::IBMRS
            || encoding == cdf_encoding::ARM_BIG;
    }
    bool is_little_endian(cdf_encoding encoding) { return !is_big_endian(encoding); }

    using magic_numbers_t = std::pair<uint32_t, uint32_t>;

    template <std::size_t offset_param, typename T>
    struct field_t
    {
        using type = T;
        inline static constexpr std::size_t len = sizeof(T);
        inline static constexpr std::size_t offset = offset_param;
        T value;
        operator T() { return value; }
        field_t& operator=(const T& v)
        {
            this->value = v;
            return *this;
        }
    };

    template <std::size_t offset_param, std::size_t len_param>
    struct str_field_t
    {
        using type = std::string;
        inline static constexpr std::size_t offset = offset_param;
        inline static constexpr std::size_t len = len_param;
        std::string value;
        operator std::string() { return value; }
        str_field_t& operator=(const std::string& v)
        {
            this->value = v;
            return *this;
        }
    };

    template <std::size_t offset_param, typename T>
    struct unused_field_t
    {
        using type = T;
        inline static constexpr std::size_t len = sizeof(T);
        inline static constexpr std::size_t offset = offset_param;
        inline static constexpr T value = 0;
        operator T() { return value; }
    };

    template <typename buffer_t, typename T>
    void extract_field(buffer_t buffer, std::size_t offset, T& field)
    {
        if constexpr (std::is_same_v<std::string, typename T::type>)
        {
            std::size_t size = 0;
            for (; size < T::len; size++)
            {
                if (buffer[T::offset - offset + size] == '\0')
                    break;
            }
            field = std::string { buffer + T::offset - offset, size };
        }
        else
            field = endianness::decode<typename T::type>(buffer + T::offset - offset);
    }

    template <typename buffer_t, typename... Ts>
    void extract_fields(buffer_t buffer, std::size_t offset, Ts&&... fields)
    {
        (extract_field(buffer, offset, std::forward<Ts>(fields)), ...);
    }

    template <typename field_type>
    inline constexpr std::size_t after = field_type::offset + sizeof(typename field_type::type);

    struct v3x_tag
    {
    };

    struct v2x_tag
    {
    };

    template <typename version_t>
    inline constexpr bool is_v3_v = std::is_same_v<version_t, v3x_tag>;

    template <typename version_t, typename previous_member_t>
    using cdf_offset_field_t = std::conditional_t<is_v3_v<version_t>,
        field_t<after<previous_member_t>, uint64_t>, field_t<after<previous_member_t>, uint32_t>>;

    template <typename version_t, typename previous_member_t, std::size_t v3size,
        std::size_t v2size>
    using cdf_string_field_t
        = std::conditional_t<is_v3_v<version_t>, str_field_t<after<previous_member_t>, v3size>,
            str_field_t<after<previous_member_t>, v2size>>;

    template <typename version_t, cdf_record_type record_t>
    struct cdf_DR_header
    {
        inline static constexpr bool v3 = is_v3_v<version_t>;
        inline static constexpr std::size_t offset = 0;
        inline static constexpr cdf_record_type rec_type = record_t;
        using type = uint64_t;
        std::conditional_t<v3, field_t<0, uint64_t>, field_t<0, uint32_t>> record_size;
        field_t<after<decltype(record_size)>, cdf_record_type> record_type;
        template <typename buffert_t>
        bool load(buffert_t&& buffer)
        {
            extract_fields(std::forward<buffert_t>(buffer), 0, record_size, record_type);
            return record_type == record_type;
        }
    };

    template <typename T>
    constexpr auto last_member(T&& field)
    {
        return field;
    }

    template <typename T1, typename T2>
    constexpr auto last_member(T1&& field1, T2&& field2)
    {
        if constexpr (field1.offset > field2.offset)
            return field1;
        else
            return field2;
    }

    template <typename T1, typename T2, typename... Ts>
    constexpr auto last_member(T1&& field1, T2&& field2, Ts&&... fields)
    {
        if constexpr (field1.offset > field2.offset)
            return last_member(field1, fields...);
        else
            return last_member(field2, fields...);
    }

    template <typename T>
    constexpr auto first_member(T&& field)
    {
        return field;
    }

    template <typename T1, typename T2>
    constexpr auto first_member(T1&& field1, T2&& field2)
    {
        if constexpr (field1.offset < field2.offset)
            return field1;
        else
            return field2;
    }

    template <typename T1, typename T2, typename... Ts>
    constexpr auto first_member(T1&& field1, T2&& field2, Ts&&... fields)
    {
        if constexpr (field1.offset < field2.offset)
            return last_member(field1, fields...);
        else
            return last_member(field2, fields...);
    }


    template <typename streamT, typename cdf_DR_t, typename... Ts>
    constexpr bool load_desc_record(
        streamT&& stream, std::size_t offset, cdf_DR_t&& cdf_desc_record, Ts&&... fields)
    {
        using last_member_t = decltype(last_member(fields...));
        constexpr std::size_t buffer_len = last_member_t::offset + last_member_t::len;
        char buffer[buffer_len];
        stream.seekg(offset);
        stream.read(buffer, buffer_len);
        if (cdf_desc_record.header.load(buffer))
        {
            extract_fields(buffer, 0, fields...);
            return true;
        }
        return false;
    }

    template <typename streamT, typename... Ts>
    constexpr bool load_fields(streamT&& stream, std::size_t offset, Ts&&... fields)
    {
        using last_member_t = decltype(last_member(fields...));
        using first_member_t = decltype(first_member(fields...));
        constexpr std::size_t buffer_len
            = last_member_t::offset + last_member_t::len - first_member_t::offset;
        char buffer[buffer_len];
        std::size_t pos = offset + first_member_t::offset;
        stream.seekg(pos);
        stream.read(buffer, buffer_len);
        extract_fields(buffer, first_member_t::offset, fields...);
        return true;
    }

    template <typename streamT>
    std::vector<char> load_chunks(streamT&& stream, const std::vector<data_chunk_t>& chunks)
    {
        std::size_t total_size = std::accumulate(std::cbegin(chunks), std::cend(chunks), 0UL,
            [](auto& sz, auto& chunk) { return sz + chunk.size; });
        std::vector<char> buffer(total_size);
        std::size_t pos = 0;
        std::for_each(
            std::cbegin(chunks), std::cend(chunks), [&buffer, &stream, &pos](auto& chunk) {
                stream.seekg(chunk.position);
                stream.read(buffer.data() + pos, chunk.size);
                pos += chunk.size;
            });
        return buffer;
    }

    template <typename version_t>
    struct cdf_CDR_t
    {
        inline static constexpr bool v3 = is_v3_v<version_t>;
        cdf_DR_header<version_t, cdf_record_type::CDR> header;
        cdf_offset_field_t<version_t, decltype(header)> GDRoffset;
        field_t<after<decltype(GDRoffset)>, uint32_t> Version;
        field_t<after<decltype(Version)>, uint32_t> Release;
        field_t<after<decltype(Release)>, cdf_encoding> Encoding;
        field_t<after<decltype(Encoding)>, uint32_t> Flags;
        unused_field_t<after<decltype(Flags)>, uint32_t> rfuA;
        unused_field_t<after<decltype(rfuA)>, uint32_t> rfuB;
        field_t<after<decltype(rfuB)>, uint32_t> Increment;
        field_t<after<decltype(Increment)>, uint32_t> Identifier;
        unused_field_t<after<decltype(Identifier)>, uint32_t> rfuE;
        str_field_t<after<decltype(rfuE)>, 256> copyright; // ignore format < 2.6

        template <typename streamT>
        bool load(streamT&& stream)
        {
            return load_desc_record(stream, 8, *this, GDRoffset, Version, Release, Encoding, Flags,
                Increment, Identifier, copyright);
        }
    };

    template <typename version_t>
    struct cdf_GDR_t
    {
        inline static constexpr bool v3 = is_v3_v<version_t>;
        cdf_DR_header<version_t, cdf_record_type::GDR> header;
        cdf_offset_field_t<version_t, decltype(header)> rVDRhead;
        cdf_offset_field_t<version_t, decltype(rVDRhead)> zVDRhead;
        cdf_offset_field_t<version_t, decltype(zVDRhead)> ADRhead;
        cdf_offset_field_t<version_t, decltype(ADRhead)> eof;
        field_t<after<decltype(eof)>, uint32_t> NrVars;
        field_t<after<decltype(NrVars)>, uint32_t> NumAttr;
        field_t<after<decltype(NumAttr)>, uint32_t> rMaxRec;
        field_t<after<decltype(rMaxRec)>, uint32_t> rNumDims;
        field_t<after<decltype(rNumDims)>, uint32_t> NzVars;
        field_t<after<decltype(NzVars)>, uint32_t> UIRhead;
        unused_field_t<after<decltype(UIRhead)>, uint32_t> rfuC;
        field_t<after<decltype(rfuC)>, uint32_t> LeapSecondLastUpdated;
        unused_field_t<after<decltype(LeapSecondLastUpdated)>, uint32_t> rfuE;
        template <typename streamT>
        bool load(streamT&& stream, std::size_t GDRoffset)
        {
            return load_desc_record(stream, GDRoffset, *this, rVDRhead, zVDRhead, ADRhead, eof,
                NrVars, NumAttr, rMaxRec, rNumDims, NzVars, UIRhead, LeapSecondLastUpdated);
        }
    };

    template <typename version_t>
    struct cdf_ADR_t
    {
        inline static constexpr bool v3 = is_v3_v<version_t>;
        cdf_DR_header<version_t, cdf_record_type::ADR> header;
        cdf_offset_field_t<version_t, decltype(header)> ADRnext;
        cdf_offset_field_t<version_t, decltype(ADRnext)> AgrEDRhead;
        field_t<after<decltype(AgrEDRhead)>, uint32_t> scope;
        field_t<after<decltype(scope)>, uint32_t> num;
        field_t<after<decltype(num)>, uint32_t> NgrEntries;
        field_t<after<decltype(NgrEntries)>, uint32_t> MAXgrEntries;
        unused_field_t<after<decltype(MAXgrEntries)>, uint32_t> rfuA;
        cdf_offset_field_t<version_t, decltype(rfuA)> AzEDRhead;
        field_t<after<decltype(AzEDRhead)>, uint32_t> NzEntries;
        field_t<after<decltype(NzEntries)>, uint32_t> MAXzEntries;
        unused_field_t<after<decltype(MAXzEntries)>, uint32_t> rfuE;
        cdf_string_field_t<version_t, decltype(rfuE), 256, 64> Name;

        template <typename streamT>
        bool load(streamT&& stream, std::size_t ADRoffset)
        {
            return load_desc_record(stream, ADRoffset, *this, ADRnext, AgrEDRhead, scope, num,
                NgrEntries, MAXgrEntries, AzEDRhead, NzEntries, MAXzEntries, Name);
        }
    };

    template <typename version_t>
    struct cdf_AEDR_t
    {
        inline static constexpr bool v3 = is_v3_v<version_t>;
        cdf_DR_header<version_t, cdf_record_type::ADR> header;
        cdf_offset_field_t<version_t, decltype(header)> AEDRnext;
        field_t<after<decltype(AEDRnext)>, uint32_t> AttrNum;
        field_t<after<decltype(AttrNum)>, CDF_Types> DataType;
        field_t<after<decltype(DataType)>, uint32_t> Num;
        field_t<after<decltype(Num)>, uint32_t> NumElements;
        field_t<after<decltype(NumElements)>, uint32_t> NumStrings;
        unused_field_t<after<decltype(NumStrings)>, uint32_t> rfB;
        unused_field_t<after<decltype(rfB)>, uint32_t> rfC;
        unused_field_t<after<decltype(rfC)>, uint32_t> rfD;
        unused_field_t<after<decltype(rfD)>, uint32_t> rfE;
        field_t<after<decltype(rfE)>, uint32_t> Values;

        template <typename streamT>
        bool load(streamT&& stream, std::size_t AEDRoffset)
        {
            return load_desc_record(stream, AEDRoffset, *this, AEDRnext, AttrNum, DataType, Num,
                NumElements, NumStrings);
        }
    };

    template <typename version_t>
    struct cdf_parsing_t
    {
        inline static constexpr bool v3 = is_v3_v<version_t>;
        magic_numbers_t magic;
        bool is_compressed;
        cdf_CDR_t<version_t> cdr;
        cdf_GDR_t<version_t> gdr;
    };

    bool is_cdf(const magic_numbers_t& magic_numbers) noexcept
    {
        return (magic_numbers.first & 0xfff00000) == 0xCDF00000
            && (magic_numbers.second == 0xCCCC0001 || magic_numbers.second == 0x0000FFFF);
    }

    template <typename context_t>
    bool is_cdf(const context_t& context) noexcept
    {
        return is_cdf(context.magic);
    }

    template <typename streamT>
    magic_numbers_t get_magic(streamT& stream)
    {
        stream.seekg(std::ios::beg);
        char buffer[8];
        stream.read(buffer, 8);
        uint32_t magic1 = cdf::endianness::decode<uint32_t>(buffer);
        uint32_t magic2 = cdf::endianness::decode<uint32_t>(buffer + 4);
        return { magic1, magic2 };
    }

    template <typename streamT>
    std::size_t get_file_len(streamT& stream)
    {
        auto pos = stream.tellg();
        stream.seekg(std::ios::end);
        auto length = stream.tellg();
        stream.seekg(pos);
        return length;
    }

    bool is_compressed(const magic_numbers_t& magic_numbers) noexcept
    {
        return magic_numbers.second == 0xCCCC0001;
    }

    bool is_v3x(const magic_numbers_t& magic) { return ((magic.first >> 12) & 0xff) >= 0x30; }

    template <typename cdf_version_tag_t, typename buffer_t>
    cdf_record_type record_type(buffer_t&& buffer)
    {
        if constexpr (std::is_same_v<cdf_version_tag_t, v3x_tag>)
            return endianness::decode<cdf_record_type>(buffer + 8);
        else
            return endianness::decode<cdf_record_type>(buffer + 4);
    }

    template <typename cdf_version_tag_t, typename streamT, typename context_t>
    bool parse_CDR(streamT& stream, context_t& context)
    {
        if (!context.cdr.load(stream))
            return false;
        return true;
    }

    template <typename cdf_version_tag_t, typename streamT, typename context_t>
    bool parse_GDR(streamT& stream, context_t& context)
    {
        if (!context.gdr.load(stream, context.cdr.GDRoffset.value))
            return false;
        return true;
    }

    template <typename cdf_version_tag_t, typename streamT>
    data_t load_attribute_data(std::size_t offset, streamT& stream, std::size_t AEDRCount)
    {
        cdf_AEDR_t<cdf_version_tag_t> AEDR;
        if (AEDR.load(stream, offset))
        {
            std::vector<data_chunk_t> chunks;
            std::size_t element_size = cdf_type_size(CDF_Types { AEDR.DataType.value });
            chunks.emplace_back(offset + AEDR.Values.offset, AEDR.NumElements * element_size);
            if (AEDRCount > 1)
            {
                decltype(AEDR.AEDRnext) next;
                decltype(AEDR.AEDRnext) current = AEDR.AEDRnext;
                decltype(AEDR.NumElements) NumElements;
                for (; AEDRCount > 1; --AEDRCount)
                {
                    load_fields(stream, next.value, next, NumElements);
                    chunks.emplace_back(
                        current.value + AEDR.Values.offset, NumElements.value * element_size);
                    current = next;
                };
            }
            auto raw_data = load_chunks(stream, chunks);
            return load_values(
                raw_data.data(), std::size(raw_data), AEDR.DataType.value, cdf_encoding::IBMPC);
        }
        return {};
    }

    template <typename cdf_version_tag_t, typename streamT>
    std::size_t load_attribute(std::size_t offset, streamT& stream, CDF& cdf)
    {
        cdf_ADR_t<cdf_version_tag_t> ADR;
        if (ADR.load(stream, offset))
        {
            data_t data;
            if (ADR.AzEDRhead != 0)
                data = load_attribute_data<cdf_version_tag_t>(ADR.AzEDRhead, stream, ADR.NzEntries);
            else if (ADR.AgrEDRhead != 0)
                data = load_attribute_data<cdf_version_tag_t>(
                    ADR.AgrEDRhead, stream, ADR.NgrEntries);
            cdf.attrinutes.emplace(std::piecewise_construct, std::forward_as_tuple(ADR.Name.value),
                std::forward_as_tuple(ADR.Name.value, std::move(data)));
            return ADR.ADRnext;
        }
        return 0;
    }

    template <typename cdf_version_tag_t, typename streamT, typename context_t>
    bool load_attributes(streamT& stream, context_t& context, CDF& cdf)
    {
        auto attr_count = context.gdr.NumAttr.value;
        auto next_attr = context.gdr.ADRhead.value;
        while (attr_count > 0)
        {
            next_attr = load_attribute<cdf_version_tag_t>(next_attr, stream, cdf);
            --attr_count;
        }
        return true;
    }

    template <typename cdf_version_tag_t, typename streamT, typename context_t>
    bool load_zVars(streamT& stream, context_t& context, CDF& cdf)
    {
        return true;
    }

    template <typename cdf_version_tag_t, typename streamT, typename context_t>
    bool load_rVars(streamT& stream, context_t& context, CDF& cdf)
    {
        return true;
    }

    template <typename cdf_version_tag_t, typename stream_t>
    std::optional<CDF> parse_cdf(stream_t& cdf_file)
    {
        cdf_parsing_t<cdf_version_tag_t> context;
        context.magic = get_magic(cdf_file);
        if (!is_cdf(context))
            return std::nullopt;
        if (!parse_CDR<cdf_version_tag_t>(cdf_file, context))
            return std::nullopt;
        if (!parse_GDR<cdf_version_tag_t>(cdf_file, context))
            return std::nullopt;
        CDF cdf;
        if (!load_attributes<cdf_version_tag_t>(cdf_file, context, cdf))
            return std::nullopt;
        return CDF {};
    }
} // namespace

std::optional<CDF> load(const std::string& path)
{
    if (std::filesystem::exists(path))
    {
        std::fstream cdf_file(path, std::ios::in | std::ios::binary);
        auto length = get_file_len(cdf_file);
        auto magic = get_magic(cdf_file);
        if (is_v3x(magic))
        {
            return parse_cdf<v3x_tag>(cdf_file);
        }
        else
        {
            return parse_cdf<v2x_tag>(cdf_file);
        }
    }
    return std::nullopt;
}
} // namespace cdf::io
