#include <gtest/gtest.h>
#include "pattern_scanner.h"

#include <cstdint>
#include <span>
#include <vector>

using ck3accel::parse_signature;
using ck3accel::scan_span;
using ck3accel::ScanStatus;
using ck3accel::Signature;

namespace {
    // span over a vector for scan tests.
    std::span<const std::uint8_t> view(const std::vector<std::uint8_t>& v) {
        return std::span<const std::uint8_t>(v.data(), v.size());
    }
}

TEST(ParseSignatureTest, ValidBytesAllRequired) {
    Signature sig = parse_signature("48 8B C4 90");
    ASSERT_TRUE(sig.valid());
    ASSERT_EQ(sig.bytes.size(), 4u);
    ASSERT_EQ(sig.mask.size(), 4u);
    EXPECT_EQ(sig.bytes[0], 0x48u);
    EXPECT_EQ(sig.bytes[1], 0x8Bu);
    EXPECT_EQ(sig.bytes[2], 0xC4u);
    EXPECT_EQ(sig.bytes[3], 0x90u);
    EXPECT_TRUE(sig.mask[0]);
    EXPECT_TRUE(sig.mask[1]);
    EXPECT_TRUE(sig.mask[2]);
    EXPECT_TRUE(sig.mask[3]);
}

TEST(ParseSignatureTest, SingleQuestionIsWildcard) {
    Signature sig = parse_signature("48 ? C4");
    ASSERT_TRUE(sig.valid());
    ASSERT_EQ(sig.bytes.size(), 3u);
    EXPECT_TRUE(sig.mask[0]);
    EXPECT_FALSE(sig.mask[1]);   // wildcard
    EXPECT_TRUE(sig.mask[2]);
    EXPECT_EQ(sig.bytes[1], 0x00u);  // wildcard byte is zero-filled
}

TEST(ParseSignatureTest, DoubleQuestionIsWildcard) {
    Signature sig = parse_signature("48 ?? C4");
    ASSERT_TRUE(sig.valid());
    ASSERT_EQ(sig.bytes.size(), 3u);
    EXPECT_TRUE(sig.mask[0]);
    EXPECT_FALSE(sig.mask[1]);
    EXPECT_TRUE(sig.mask[2]);
}

TEST(ParseSignatureTest, MixedCaseHexAndExtraWhitespace) {
    Signature sig = parse_signature("  4a   8b\tff  ");
    ASSERT_TRUE(sig.valid());
    ASSERT_EQ(sig.bytes.size(), 3u);
    EXPECT_EQ(sig.bytes[0], 0x4Au);
    EXPECT_EQ(sig.bytes[1], 0x8Bu);
    EXPECT_EQ(sig.bytes[2], 0xFFu);
}

TEST(ParseSignatureTest, RejectsOddHexDigitCount) {
    Signature sig = parse_signature("48 8 C4");   // "8" is a single hex digit
    EXPECT_FALSE(sig.valid());
    EXPECT_TRUE(sig.bytes.empty());
}

TEST(ParseSignatureTest, RejectsNonHexToken) {
    Signature sig = parse_signature("48 ZZ C4");
    EXPECT_FALSE(sig.valid());
    EXPECT_TRUE(sig.bytes.empty());
}

TEST(ParseSignatureTest, RejectsEmptyPattern) {
    Signature sig = parse_signature("   ");
    EXPECT_FALSE(sig.valid());
    EXPECT_TRUE(sig.bytes.empty());
}

TEST(ScanSpanTest, NotFoundWhenAbsent) {
    std::vector<std::uint8_t> hay{0x00, 0x01, 0x02, 0x03};
    Signature sig = parse_signature("AA BB");
    ASSERT_TRUE(sig.valid());
    ck3accel::ScanResult r = scan_span(view(hay), sig);
    EXPECT_EQ(r.status, ScanStatus::NotFound);
    EXPECT_EQ(r.match_count, 0u);
    EXPECT_EQ(r.address, nullptr);
}

TEST(ScanSpanTest, FoundSingleMatchReportsAddress) {
    std::vector<std::uint8_t> hay{0x00, 0x48, 0x8B, 0xC4, 0x00};
    Signature sig = parse_signature("48 8B C4");
    ASSERT_TRUE(sig.valid());
    ck3accel::ScanResult r = scan_span(view(hay), sig);
    EXPECT_EQ(r.status, ScanStatus::Found);
    EXPECT_EQ(r.match_count, 1u);
    EXPECT_EQ(r.address, hay.data() + 1);
}

TEST(ScanSpanTest, AmbiguousWhenManyMatches) {
    std::vector<std::uint8_t> hay{0xAA, 0xBB, 0x00, 0xAA, 0xBB};
    Signature sig = parse_signature("AA BB");
    ASSERT_TRUE(sig.valid());
    ck3accel::ScanResult r = scan_span(view(hay), sig);
    EXPECT_EQ(r.status, ScanStatus::Ambiguous);
    EXPECT_EQ(r.match_count, 2u);
    EXPECT_EQ(r.address, nullptr);   // address invalid unless exactly one match
}

TEST(ScanSpanTest, OverlappingMatchesAreCountedSeparately) {
    // "AA AA" overlaps at offsets 0 and 1 in {AA AA AA}.
    std::vector<std::uint8_t> hay{0xAA, 0xAA, 0xAA};
    Signature sig = parse_signature("AA AA");
    ASSERT_TRUE(sig.valid());
    ck3accel::ScanResult r = scan_span(view(hay), sig);
    EXPECT_EQ(r.status, ScanStatus::Ambiguous);
    EXPECT_EQ(r.match_count, 2u);
}

TEST(ScanSpanTest, WildcardMatchesAnyByte) {
    std::vector<std::uint8_t> hay{0x48, 0x13, 0xC4, 0x48, 0x99, 0xC4};
    Signature sig = parse_signature("48 ?? C4");
    ASSERT_TRUE(sig.valid());
    ck3accel::ScanResult r = scan_span(view(hay), sig);
    EXPECT_EQ(r.status, ScanStatus::Ambiguous);
    EXPECT_EQ(r.match_count, 2u);
}

TEST(ScanSpanTest, SignatureLongerThanHaystackIsNotFound) {
    std::vector<std::uint8_t> hay{0x48, 0x8B};
    Signature sig = parse_signature("48 8B C4 90");
    ASSERT_TRUE(sig.valid());
    ck3accel::ScanResult r = scan_span(view(hay), sig);
    EXPECT_EQ(r.status, ScanStatus::NotFound);
    EXPECT_EQ(r.match_count, 0u);
}

TEST(ScanSpanTest, InvalidSignatureIsNotFound) {
    std::vector<std::uint8_t> hay{0x48, 0x8B, 0xC4};
    Signature sig = parse_signature("not a signature");
    ASSERT_FALSE(sig.valid());
    ck3accel::ScanResult r = scan_span(view(hay), sig);
    EXPECT_EQ(r.status, ScanStatus::NotFound);
    EXPECT_EQ(r.match_count, 0u);
}
