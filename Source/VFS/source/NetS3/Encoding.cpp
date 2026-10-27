// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#include "Encoding.h"
#include <array>
#include <cstdio>
#include <cstring>

namespace nc::vfs::s3 {

static bool IsUnreserved(unsigned char _c) noexcept
{
    return (_c >= 'A' && _c <= 'Z') || (_c >= 'a' && _c <= 'z') || (_c >= '0' && _c <= '9') || _c == '-' ||
           _c == '_' || _c == '.' || _c == '~';
}

static int HexValue(char _c) noexcept
{
    if( _c >= '0' && _c <= '9' )
        return _c - '0';
    if( _c >= 'a' && _c <= 'f' )
        return _c - 'a' + 10;
    if( _c >= 'A' && _c <= 'F' )
        return _c - 'A' + 10;
    return -1;
}

std::string UriEncode(std::string_view _input, bool _keep_slash)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(_input.size() * 3);
    for( const char ch : _input ) {
        const auto c = static_cast<unsigned char>(ch);
        if( IsUnreserved(c) || (_keep_slash && c == '/') ) {
            out.push_back(ch);
        }
        else {
            out.push_back('%');
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 0xF]);
        }
    }
    return out;
}

std::string FormUrlDecode(std::string_view _input)
{
    std::string out;
    out.reserve(_input.size());
    for( size_t i = 0; i < _input.size(); ++i ) {
        const char c = _input[i];
        if( c == '+' ) {
            out.push_back(' ');
        }
        else if( c == '%' && i + 2 < _input.size() ) {
            const int hi = HexValue(_input[i + 1]);
            const int lo = HexValue(_input[i + 2]);
            if( hi >= 0 && lo >= 0 ) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            }
            else {
                out.push_back(c);
            }
        }
        else {
            out.push_back(c);
        }
    }
    return out;
}

std::string AmzDateTime(std::time_t _time)
{
    std::tm tm{};
    gmtime_r(&_time, &tm);
    std::array<char, 32> buf{};
    std::strftime(buf.data(), buf.size(), "%Y%m%dT%H%M%SZ", &tm);
    return buf.data();
}

std::optional<std::time_t> ParseIso8601(std::string_view _input) noexcept
{
    // Expected layout: YYYY-MM-DDTHH:MM:SS[.fff]Z
    if( _input.size() < 19 )
        return std::nullopt;
    std::array<char, 32> buf{};
    const size_t n = std::min(_input.size(), buf.size() - 1);
    std::memcpy(buf.data(), _input.data(), n);
    std::tm tm{};
    int year = 0;
    int mon = 0;
    int day = 0;
    int hour = 0;
    int min = 0;
    int sec = 0;
    if( std::sscanf(buf.data(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &mon, &day, &hour, &min, &sec) != 6 )
        return std::nullopt;
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    return timegm(&tm);
}

std::optional<std::time_t> ParseHttpDate(std::string_view _input) noexcept
{
    static constexpr std::array<std::string_view, 12> months = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    std::array<char, 64> buf{};
    const size_t n = std::min(_input.size(), buf.size() - 1);
    std::memcpy(buf.data(), _input.data(), n);
    std::array<char, 4> wday{};
    std::array<char, 4> mon{};
    int day = 0;
    int year = 0;
    int hour = 0;
    int min = 0;
    int sec = 0;
    if( std::sscanf(buf.data(), "%3s, %d %3s %d %d:%d:%d", wday.data(), &day, mon.data(), &year, &hour, &min, &sec) !=
        7 )
        return std::nullopt;
    int month = -1;
    for( size_t i = 0; i < months.size(); ++i )
        if( months[i] == std::string_view(mon.data()) )
            month = static_cast<int>(i);
    if( month < 0 )
        return std::nullopt;
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    return timegm(&tm);
}

std::string_view TrimWhitespace(std::string_view _input) noexcept
{
    const auto first = _input.find_first_not_of(" \t\r\n");
    if( first == std::string_view::npos )
        return {};
    const auto last = _input.find_last_not_of(" \t\r\n");
    return _input.substr(first, last - first + 1);
}

} // namespace nc::vfs::s3
