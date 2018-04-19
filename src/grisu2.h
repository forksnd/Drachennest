// Copyright 2017 Alexander Bolz
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// Implements the Grisu2 algorithm for binary to decimal floating-point
// conversion.
//
// This implementation is a slightly modified version of the reference
// implementation by Florian Loitsch which can be obtained from
// http://florian.loitsch.com/publications (bench.tar.gz)
//
// The original license can be found at the end of this file.
//
// References:
//
// [1]  Loitsch, "Printing Floating-Point Numbers Quickly and Accurately with Integers",
//      Proceedings of the ACM SIGPLAN 2010 Conference on Programming Language Design and Implementation, PLDI 2010
// [2]  Burger, Dybvig, "Printing Floating-Point Numbers Quickly and Accurately",
//      Proceedings of the ACM SIGPLAN 1996 Conference on Programming Language Design and Implementation, PLDI 1996

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace grisu {

//--------------------------------------------------------------------------------------------------
// Grisu2
//--------------------------------------------------------------------------------------------------

template <typename Dest, typename Source>
inline Dest ReinterpretBits(Source source)
{
    static_assert(sizeof(Dest) == sizeof(Source), "size mismatch");

    Dest dest;
    std::memcpy(&dest, &source, sizeof(Source));
    return dest;
}

inline char* Utoa100(char* buf, uint32_t digits)
{
    static constexpr char const* kDigits100 =
        "00010203040506070809"
        "10111213141516171819"
        "20212223242526272829"
        "30313233343536373839"
        "40414243444546474849"
        "50515253545556575859"
        "60616263646566676869"
        "70717273747576777879"
        "80818283848586878889"
        "90919293949596979899";

    assert(digits < 100);
    std::memcpy(buf, kDigits100 + 2*digits, 2);
    return buf + 2;
}

struct DiyFp // f * 2^e
{
    static constexpr int kPrecision = 64; // = q

    uint64_t f;
    int e;

    constexpr DiyFp() : f(0), e(0) {}
    constexpr DiyFp(uint64_t f_, int e_) : f(f_), e(e_) {}
};

// Returns x - y.
// PRE: x.e == y.e and x.f >= y.f
inline DiyFp Subtract(DiyFp x, DiyFp y)
{
    assert(x.e == y.e);
    assert(x.f >= y.f);

    return DiyFp(x.f - y.f, x.e);
}

// Returns x * y.
// The result is rounded (ties up). (Only the upper q bits are returned.)
inline DiyFp Multiply(DiyFp x, DiyFp y)
{
    // Computes:
    //  f = round((x.f * y.f) / 2^q)
    //  e = x.e + y.e + q

#if defined(_MSC_VER) && defined(_M_X64)

    uint64_t h = 0;
    uint64_t l = _umul128(x.f, y.f, &h);
    h += l >> 63; // round, ties up: [h, l] += 2^q / 2

    return DiyFp(h, x.e + y.e + 64);

#elif defined(__GNUC__) && defined(__SIZEOF_INT128__)

    __extension__ using Uint128 = unsigned __int128;

    Uint128 const p = Uint128{x.f} * Uint128{y.f};

    uint64_t h = static_cast<uint64_t>(p >> 64);
    uint64_t l = static_cast<uint64_t>(p);
    h += l >> 63; // round, ties up: [h, l] += 2^q / 2

    return DiyFp(h, x.e + y.e + 64);

#else

    // Emulate the 64-bit * 64-bit multiplication:
    //
    // p = u * v
    //   = (u_lo + 2^32 u_hi) (v_lo + 2^32 v_hi)
    //   = (u_lo v_lo         ) + 2^32 ((u_lo v_hi         ) + (u_hi v_lo         )) + 2^64 (u_hi v_hi         )
    //   = (p0                ) + 2^32 ((p1                ) + (p2                )) + 2^64 (p3                )
    //   = (p0_lo + 2^32 p0_hi) + 2^32 ((p1_lo + 2^32 p1_hi) + (p2_lo + 2^32 p2_hi)) + 2^64 (p3                )
    //   = (p0_lo             ) + 2^32 (p0_hi + p1_lo + p2_lo                      ) + 2^64 (p1_hi + p2_hi + p3)
    //   = (p0_lo             ) + 2^32 (Q                                          ) + 2^64 (H                 )
    //   = (p0_lo             ) + 2^32 (Q_lo + 2^32 Q_hi                           ) + 2^64 (H                 )
    //
    // (Since Q might be larger than 2^32 - 1)
    //
    //   = (p0_lo + 2^32 Q_lo) + 2^64 (Q_hi + H)
    //
    // (Q_hi + H does not overflow a 64-bit int)
    //
    //   = p_lo + 2^64 p_hi

    // Note:
    // The 32/64-bit casts here help MSVC to avoid calls to the _allmul
    // library function.

    uint32_t const u_lo = static_cast<uint32_t>(x.f /*& 0xFFFFFFFF*/);
    uint32_t const u_hi = static_cast<uint32_t>(x.f >> 32);
    uint32_t const v_lo = static_cast<uint32_t>(y.f /*& 0xFFFFFFFF*/);
    uint32_t const v_hi = static_cast<uint32_t>(y.f >> 32);

    uint64_t const p0 = uint64_t{u_lo} * v_lo;
    uint64_t const p1 = uint64_t{u_lo} * v_hi;
    uint64_t const p2 = uint64_t{u_hi} * v_lo;
    uint64_t const p3 = uint64_t{u_hi} * v_hi;

    uint64_t const p0_hi = p0 >> 32;
    uint64_t const p1_lo = p1 & 0xFFFFFFFF;
    uint64_t const p1_hi = p1 >> 32;
    uint64_t const p2_lo = p2 & 0xFFFFFFFF;
    uint64_t const p2_hi = p2 >> 32;

    uint64_t Q = p0_hi + p1_lo + p2_lo;

    // The full product might now be computed as
    //
    // p_hi = p3 + p2_hi + p1_hi + (Q >> 32)
    // p_lo = p0_lo + (Q << 32)
    //
    // But in this particular case here, the full p_lo is not required.
    // Effectively we only need to add the highest bit in p_lo to p_hi (and
    // Q_hi + 1 does not overflow).

    Q += uint64_t{1} << (63 - 32); // round, ties up

    uint64_t const h = p3 + p2_hi + p1_hi + (Q >> 32);

    return DiyFp(h, x.e + y.e + 64);

#endif
}

// Normalize x such that the significand is >= 2^(q-1).
// PRE: x.f != 0
inline DiyFp Normalize(DiyFp x)
{
    assert(x.f != 0);

#if defined(_MSC_VER) && defined(_M_X64)

    int const lz = static_cast<int>(__lzcnt64(x.f));
    return DiyFp(x.f << lz, x.e - lz);

#elif defined(_MSC_VER) && defined(_M_IX86)

    int lz = static_cast<int>( __lzcnt(static_cast<uint32_t>(x.f >> 32)) );
    if (lz == 32) {
        lz += static_cast<int>( __lzcnt(static_cast<uint32_t>(x.f)) );
    }
    return DiyFp(x.f << lz, x.e - lz);

#elif defined(__GNUC__)

    int const lz = __builtin_clzll(x.f);
    return DiyFp(x.f << lz, x.e - lz);

#else

    while ((x.f >> 63) == 0) {
        x.f <<= 1;
        x.e--;
    }
    return x;

#endif
}

// Normalize x such that the result has the exponent E.
// PRE: e >= x.e and the upper e - x.e bits of x.f must be zero.
inline DiyFp NormalizeTo(DiyFp x, int e)
{
    int const delta = x.e - e;

    assert(delta >= 0);
    assert(((x.f << delta) >> delta) == x.f);

    return DiyFp(x.f << delta, e);
}

struct Boundaries {
    DiyFp v;
    DiyFp m_minus;
    DiyFp m_plus;
};

// Compute the (normalized) DiyFp representing the input number 'value' and its
// boundaries.
// PRE: 'value' must be finite and positive
template <typename Fp>
inline Boundaries ComputeBoundaries(Fp value)
{
    static_assert(std::numeric_limits<Fp>::is_iec559
                  && ((std::numeric_limits<Fp>::digits == 24 && std::numeric_limits<Fp>::max_exponent == 128) ||
                      (std::numeric_limits<Fp>::digits == 53 && std::numeric_limits<Fp>::max_exponent == 1024)),
        "ComputeBoundaries requires an IEEE-754 single- or double-precision implementation");

    assert(std::isfinite(value));
    assert(value > 0);

    // Convert the IEEE representation into a DiyFp.
    //
    // If v is denormal:
    //      value = 0.F * 2^(1 - bias) = (          F) * 2^(1 - bias - (p-1))
    // If v is normalized:
    //      value = 1.F * 2^(E - bias) = (2^(p-1) + F) * 2^(E - bias - (p-1))

    constexpr int      kPrecision = std::numeric_limits<Fp>::digits; // = p (includes the hidden bit)
    constexpr int      kBias      = std::numeric_limits<Fp>::max_exponent - 1 + (kPrecision - 1);
    constexpr uint64_t kHiddenBit = uint64_t{1} << (kPrecision - 1); // = 2^(p-1)

    using bits_type = typename std::conditional<kPrecision == 24, uint32_t, uint64_t>::type;

    auto const bits = grisu::ReinterpretBits<bits_type>(value);
    auto const E = bits >> (kPrecision - 1);
    auto const F = bits & (kHiddenBit - 1);

    bool const is_denormal = (E == 0);

    DiyFp const v = is_denormal
                        ? DiyFp(F, 1 - kBias)
                        : DiyFp(F + kHiddenBit, static_cast<int>(E) - kBias);

    // Compute the boundaries m- and m+ of the floating-point value
    // v = f * 2^e.
    //
    // Determine v- and v+, the floating-point predecessor and successor if v,
    // respectively.
    //
    //      v- = v - 2^e        if f != 2^(p-1) or e == e_min                (A)
    //         = v - 2^(e-1)    if f == 2^(p-1) and e > e_min                (B)
    //
    //      v+ = v + 2^e
    //
    // Let m- = (v- + v) / 2 and m+ = (v + v+) / 2. All real numbers _strictly_
    // between m- and m+ round to v, regardless of how the input rounding
    // algorithm breaks ties.
    //
    //      ---+-------------+-------------+-------------+-------------+---  (A)
    //         v-            m-            v             m+            v+
    //
    //      -----------------+------+------+-------------+-------------+---  (B)
    //                       v-     m-     v             m+            v+

//  bool const lower_boundary_is_closer = (v.f == kHiddenBit && v.e > kMinExp);
    bool const lower_boundary_is_closer = (F == 0 && E > 1);

    DiyFp const m_plus(2*v.f + 1, v.e - 1);
    DiyFp const m_minus = lower_boundary_is_closer
                                ? DiyFp(4*v.f - 1, v.e - 2)  // (B)
                                : DiyFp(2*v.f - 1, v.e - 1); // (A)

    // Determine the normalized w = v.
    DiyFp const w = Normalize(v);

    // Determine the normalized w+ = m+.
    // Since e_(w+) == e_(w), one can use NormalizeTo instead of Normalize.
    DiyFp const w_plus = NormalizeTo(m_plus, w.e);

    // Determine w- = m- such that e_(w-) = e_(w+).
    DiyFp const w_minus = NormalizeTo(m_minus, w_plus.e);

    return {w, w_minus, w_plus};
}

// Given normalized DiyFp w, Grisu needs to find a (normalized) cached
// power-of-ten c, such that the exponent of the product c * w = f * 2^e lies
// within a certain range [alpha, gamma] (Definition 3.2 from [1])
//
//      alpha <= e = e_c + e_w + q <= gamma
//
// or
//
//      f_c * f_w * 2^alpha <= f_c 2^(e_c) * f_w 2^(e_w) * 2^q
//                          <= f_c * f_w * 2^gamma
//
// Since c and w are normalized, i.e. 2^(q-1) <= f < 2^q, this implies
//
//      2^(q-1) * 2^(q-1) * 2^alpha <= c * w * 2^q < 2^q * 2^q * 2^gamma
//
// or
//
//      2^(q - 2 + alpha) <= c * w < 2^(q + gamma)
//
// The choice of (alpha,gamma) determines the size of the table and the form of
// the digit generation procedure. Using (alpha,gamma)=(-60,-32) works out well
// in practice:
//
// The idea is to cut the number c * w = f * 2^e into two parts, which can be
// processed independently: An integral part p1, and a fractional part p2:
//
//      f * 2^e = ( (f div 2^-e) * 2^-e + (f mod 2^-e) ) * 2^e
//              = (f div 2^-e) + (f mod 2^-e) * 2^e
//              = p1 + p2 * 2^e
//
// The conversion of p1 into decimal form requires a series of divisions and
// modulos by (a power of) 10. These operations are faster for 32-bit than for
// 64-bit integers, so p1 should ideally fit into a 32-bit integer. This can be
// achieved by choosing
//
//      -e >= 32   or   e <= -32 := gamma
//
// In order to convert the fractional part
//
//      p2 * 2^e = p2 / 2^-e = d[-1] / 10^1 + d[-2] / 10^2 + ...
//
// into decimal form, the fraction is repeatedly multiplied by 10 and the digits
// d[-i] are extracted in order:
//
//      (10 * p2) div 2^-e = d[-1]
//      (10 * p2) mod 2^-e = d[-2] / 10^1 + ...
//
// The multiplication by 10 must not overflow. It is sufficient to choose
//
//      10 * p2 < 16 * p2 = 2^4 * p2 <= 2^64.
//
// Since p2 = f mod 2^-e < 2^-e,
//
//      -e <= 60   or   e >= -60 := alpha

constexpr int kAlpha = -60;
constexpr int kGamma = -32;

// Now
//
//      alpha <= e_c + e + q <= gamma                                        (1)
//      ==> f_c * 2^alpha <= c * 2^e * 2^q
//
// and since the c's are normalized, 2^(q-1) <= f_c,
//
//      ==> 2^(q - 1 + alpha) <= c * 2^(e + q)
//      ==> 2^(alpha - e - 1) <= c
//
// If c were an exakt power of ten, i.e. c = 10^k, one may determine k as
//
//      k = ceil( log_10( 2^(alpha - e - 1) ) )
//        = ceil( (alpha - e - 1) * log_10(2) )
//
// From the paper:
// "In theory the result of the procedure could be wrong since c is rounded, and
//  the computation itself is approximated [...]. In practice, however, this
//  simple function is sufficient."
//
// For IEEE double precision floating-point numbers converted into normalized
// DiyFp's w = f * 2^e, with q = 64,
//
//      e >= -1022      (min IEEE exponent)
//           -52        (p - 1)
//           -52        (p - 1, possibly normalize denormal IEEE numbers)
//           -11        (normalize the DiyFp)
//         = -1137
//
// and
//
//      e <= +1023      (max IEEE exponent)
//           -52        (p - 1)
//           -11        (normalize the DiyFp)
//         = 960
//
// This binary exponent range [-1137,960] results in a decimal exponent range
// [-307,324]. One does not need to store a cached power for each k in this
// range. For each such k it suffices to find a cached power such that the
// exponent of the product lies in [alpha,gamma].
// This implies that the difference of the decimal exponents of adjacent table
// entries must be less than or equal to
//
//      floor( (gamma - alpha) * log_10(2) ) = 8.
//
// (A smaller distance gamma-alpha would require a larger table.)

struct CachedPower { // c = f * 2^e ~= 10^k
    uint64_t f;
    int e; // binary exponent
    int k; // decimal exponent
};

constexpr int kCachedPowersSize         =   79;
constexpr int kCachedPowersMinDecExp    = -300;
constexpr int kCachedPowersMaxDecExp    =  324;
constexpr int kCachedPowersDecExpStep   =    8;

inline CachedPower GetCachedPower(int index)
{
    static constexpr CachedPower kCachedPowers[] = {
//      { 0xFA8FD5A0081C0288, -1220, -348 },
//      { 0xBAAEE17FA23EBF76, -1193, -340 },
//      { 0x8B16FB203055AC76, -1166, -332 },
//      { 0xCF42894A5DCE35EA, -1140, -324 },
//      { 0x9A6BB0AA55653B2D, -1113, -316 },
//      { 0xE61ACF033D1A45DF, -1087, -308 },
        { 0xAB70FE17C79AC6CA, -1060, -300 }, // >>> double-precision (-1060 + 960 + 64 = -36)
        { 0xFF77B1FCBEBCDC4F, -1034, -292 },
        { 0xBE5691EF416BD60C, -1007, -284 },
        { 0x8DD01FAD907FFC3C,  -980, -276 },
        { 0xD3515C2831559A83,  -954, -268 },
        { 0x9D71AC8FADA6C9B5,  -927, -260 },
        { 0xEA9C227723EE8BCB,  -901, -252 },
        { 0xAECC49914078536D,  -874, -244 },
        { 0x823C12795DB6CE57,  -847, -236 },
        { 0xC21094364DFB5637,  -821, -228 },
        { 0x9096EA6F3848984F,  -794, -220 },
        { 0xD77485CB25823AC7,  -768, -212 },
        { 0xA086CFCD97BF97F4,  -741, -204 },
        { 0xEF340A98172AACE5,  -715, -196 },
        { 0xB23867FB2A35B28E,  -688, -188 },
        { 0x84C8D4DFD2C63F3B,  -661, -180 },
        { 0xC5DD44271AD3CDBA,  -635, -172 },
        { 0x936B9FCEBB25C996,  -608, -164 },
        { 0xDBAC6C247D62A584,  -582, -156 },
        { 0xA3AB66580D5FDAF6,  -555, -148 },
        { 0xF3E2F893DEC3F126,  -529, -140 },
        { 0xB5B5ADA8AAFF80B8,  -502, -132 },
        { 0x87625F056C7C4A8B,  -475, -124 },
        { 0xC9BCFF6034C13053,  -449, -116 },
        { 0x964E858C91BA2655,  -422, -108 },
        { 0xDFF9772470297EBD,  -396, -100 },
        { 0xA6DFBD9FB8E5B88F,  -369,  -92 },
        { 0xF8A95FCF88747D94,  -343,  -84 },
        { 0xB94470938FA89BCF,  -316,  -76 },
        { 0x8A08F0F8BF0F156B,  -289,  -68 },
        { 0xCDB02555653131B6,  -263,  -60 },
        { 0x993FE2C6D07B7FAC,  -236,  -52 },
        { 0xE45C10C42A2B3B06,  -210,  -44 },
        { 0xAA242499697392D3,  -183,  -36 }, // >>> single-precision (-183 + 80 + 64 = -39)
        { 0xFD87B5F28300CA0E,  -157,  -28 }, //
        { 0xBCE5086492111AEB,  -130,  -20 }, //
        { 0x8CBCCC096F5088CC,  -103,  -12 }, //
        { 0xD1B71758E219652C,   -77,   -4 }, //
        { 0x9C40000000000000,   -50,    4 }, //
        { 0xE8D4A51000000000,   -24,   12 }, //
        { 0xAD78EBC5AC620000,     3,   20 }, //
        { 0x813F3978F8940984,    30,   28 }, //
        { 0xC097CE7BC90715B3,    56,   36 }, //
        { 0x8F7E32CE7BEA5C70,    83,   44 }, // <<< single-precision (83 - 196 + 64 = -49)
        { 0xD5D238A4ABE98068,   109,   52 },
        { 0x9F4F2726179A2245,   136,   60 },
        { 0xED63A231D4C4FB27,   162,   68 },
        { 0xB0DE65388CC8ADA8,   189,   76 },
        { 0x83C7088E1AAB65DB,   216,   84 },
        { 0xC45D1DF942711D9A,   242,   92 },
        { 0x924D692CA61BE758,   269,  100 },
        { 0xDA01EE641A708DEA,   295,  108 },
        { 0xA26DA3999AEF774A,   322,  116 },
        { 0xF209787BB47D6B85,   348,  124 },
        { 0xB454E4A179DD1877,   375,  132 },
        { 0x865B86925B9BC5C2,   402,  140 },
        { 0xC83553C5C8965D3D,   428,  148 },
        { 0x952AB45CFA97A0B3,   455,  156 },
        { 0xDE469FBD99A05FE3,   481,  164 },
        { 0xA59BC234DB398C25,   508,  172 },
        { 0xF6C69A72A3989F5C,   534,  180 },
        { 0xB7DCBF5354E9BECE,   561,  188 },
        { 0x88FCF317F22241E2,   588,  196 },
        { 0xCC20CE9BD35C78A5,   614,  204 },
        { 0x98165AF37B2153DF,   641,  212 },
        { 0xE2A0B5DC971F303A,   667,  220 },
        { 0xA8D9D1535CE3B396,   694,  228 },
        { 0xFB9B7CD9A4A7443C,   720,  236 },
        { 0xBB764C4CA7A44410,   747,  244 },
        { 0x8BAB8EEFB6409C1A,   774,  252 },
        { 0xD01FEF10A657842C,   800,  260 },
        { 0x9B10A4E5E9913129,   827,  268 },
        { 0xE7109BFBA19C0C9D,   853,  276 },
        { 0xAC2820D9623BF429,   880,  284 },
        { 0x80444B5E7AA7CF85,   907,  292 },
        { 0xBF21E44003ACDD2D,   933,  300 },
        { 0x8E679C2F5E44FF8F,   960,  308 },
        { 0xD433179D9C8CB841,   986,  316 },
        { 0x9E19DB92B4E31BA9,  1013,  324 }, // <<< double-precision (1013 - 1137 + 64 = -60)
//      { 0xEB96BF6EBADF77D9,  1039,  332 },
//      { 0xAF87023B9BF0EE6B,  1066,  340 },
    };

    assert(index >= 0);
    assert(index < kCachedPowersSize);
    return kCachedPowers[index];
}

// For a normalized DiyFp w = f * 2^e, this function returns a (normalized)
// cached power-of-ten c = f_c * 2^e_c, such that the exponent of the product
// w * c satisfies (Definition 3.2 from [1])
//
//      alpha <= e_c + e + q <= gamma.
//
inline CachedPower GetCachedPowerForBinaryExponent(int e)
{
    // NB:
    // Actually this function returns c, such that -60 <= e_c + e + 64 <= -34.

    // This computation gives exactly the same results for k as
    //      k = ceil((kAlpha - e - 1) * 0.30102999566398114)
    // for |e| <= 1500, but doesn't require floating-point operations.
    // NB: log_10(2) ~= 78913 / 2^18
    assert(e >= -1500);
    assert(e <=  1500);
    int const f = kAlpha - e - 1;
    int const k = (f * 78913) / (1 << 18) + (f > 0);

    int const index = (-kCachedPowersMinDecExp + k + (kCachedPowersDecExpStep - 1)) / kCachedPowersDecExpStep;
    assert(index >= 0);
    assert(index < kCachedPowersSize);
    static_cast<void>(kCachedPowersSize);

    auto const cached = GetCachedPower(index);
    assert(kAlpha <= cached.e + e + 64);
    assert(kGamma >= cached.e + e + 64);

    return cached;
}

inline char* GenerateIntegralDigits(char* buf, uint32_t n)
{
    assert(n <= 798336123);

    uint32_t q;

    if (n >= 100000000)
    {
//L_9_digits:
        q = n / 10000000;
        n = n % 10000000;
        buf = Utoa100(buf, q);
L_7_digits:
        q = n / 100000;
        n = n % 100000;
        buf = Utoa100(buf, q);
L_5_digits:
        q = n / 1000;
        n = n % 1000;
        buf = Utoa100(buf, q);
L_3_digits:
        q = n / 10;
        n = n % 10;
        buf = Utoa100(buf, q);
L_1_digit:
        buf[0] = static_cast<char>('0' + n);
        buf++;
        return buf;
    }

    if (n >= 10000000)
    {
//L_8_digits:
        q = n / 1000000;
        n = n % 1000000;
        buf = Utoa100(buf, q);
L_6_digits:
        q = n / 10000;
        n = n % 10000;
        buf = Utoa100(buf, q);
L_4_digits:
        q = n / 100;
        n = n % 100;
        buf = Utoa100(buf, q);
L_2_digits:
        buf = Utoa100(buf, n);
        return buf;
    }

    if (n >=  1000000) goto L_7_digits;
    if (n >=   100000) goto L_6_digits;
    if (n >=    10000) goto L_5_digits;
    if (n >=     1000) goto L_4_digits;
    if (n >=      100) goto L_3_digits;
    if (n >=       10) goto L_2_digits;
    goto L_1_digit;
}

// Modifies the generated digits in the buffer to approach (round towards) w.
//
// Input:
//  * digits of H/10^kappa in [buffer, buffer + length)
//  * distance    = (H - w) * unit
//  * delta       = (H - L) * unit
//  * rest        = (H - buffer * 10^kappa) * unit
//  * ten_kappa   = 10^kappa * unit
inline void Grisu2Round(char* buffer, int length, uint64_t distance, uint64_t delta, uint64_t rest, uint64_t ten_kappa)
{
    assert(length >= 1);
    assert(distance <= delta);
    assert(rest <= delta);
    assert(ten_kappa > 0);

    // By generating the digits of H we got the largest (closest to H) buffer
    // that is still in the interval [L, H]. In the case where w < B <= H we
    // try to decrement the buffer.
    //
    //                                  <---- distance ----->
    //               <--------------------------- delta ---->
    //                                       <---- rest ---->
    //                       <-- ten_kappa -->
    // --------------[------------------+----+--------------]--------------
    //               L                  w    B              H
    //                                       = digits * 10^kappa
    //
    // ten_kappa represents a unit-in-the-last-place in the decimal
    // representation stored in the buffer.
    //
    // There are three stopping conditions:
    // (The position of the numbers is measured relative to H.)
    //
    //  1)  B is already <= w
    //          rest >= distance
    //
    //  2)  Decrementing B would yield a number B' < L
    //          rest + ten_kappa > delta
    //
    //  3)  Decrementing B would yield a number B' < w and farther away from
    //      w than the current number B: w - B' > B - w
    //          rest + ten_kappa > distance &&
    //          rest + ten_kappa - distance >= distance - rest

    // The tests are written in this order to avoid overflow in unsigned
    // integer arithmetic.

    int digit = buffer[length - 1] - '0';

    while (rest < distance
        && delta - rest >= ten_kappa
        && (rest + ten_kappa <= distance || rest + ten_kappa - distance < distance - rest))
    {
        assert(digit != 0);
        digit--;
        rest += ten_kappa;
    }

    buffer[length - 1] = static_cast<char>('0' + digit);
}

// Generates V = buffer * 10^exponent, such that L <= V <= H.
// L and H must be normalized and share the same exponent -60 <= e <= -32.
inline void Grisu2DigitGen(char* buffer, int& length, int& exponent, DiyFp L, DiyFp w, DiyFp H)
{
    static_assert(DiyFp::kPrecision == 64, "internal error");
    static_assert(kAlpha >= -60, "internal error");
    static_assert(kGamma <= -32, "internal error");

    // Generates the digits (and the exponent) of a decimal floating-point
    // number V = buffer * 10^exponent in the range [L, H].
    // The DiyFp's w, L and H share the same exponent e, which satisfies
    // alpha <= e <= gamma.
    //
    //                                  <---- distance ----->
    //               <--------------------------- delta ---->
    // --------------[------------------+-------------------]--------------
    //               L                  w                   H
    //
    // This routine generates the digits of H from left to right and stops as
    // soon as V is in [L, H].

    assert(w.e >= kAlpha);
    assert(w.e <= kGamma);
    assert(w.e == L.e);
    assert(w.e == H.e);

    uint64_t distance = Subtract(H, w).f; // (significand of (H - w), implicit exponent is e)
    uint64_t delta    = Subtract(H, L).f; // (significand of (H - L), implicit exponent is e)
    uint64_t rest;
    uint64_t ten_kappa;

    // Split H = f * 2^e into two parts p1 and p2 (note: e < 0):
    //
    //      H = f * 2^e
    //           = ((f div 2^-e) * 2^-e + (f mod 2^-e)) * 2^e
    //           = ((p1        ) * 2^-e + (p2        )) * 2^e
    //           = p1 + p2 * 2^e

    DiyFp const one(uint64_t{1} << -H.e, H.e); // one = 2^-e * 2^e

    uint32_t p1 = static_cast<uint32_t>(H.f >> -one.e); // p1 = f div 2^-e (Since -e >= 32, p1 fits into a 32-bit int.)
    uint64_t p2 = H.f & (one.f - 1);                    // p2 = f mod 2^-e

    assert(p1 >= 4);            // (2^(64-2) - 1) >> 60
    assert(p1 <= 798336123);    // test.cc: FindMaxP1 (depends on index computation in GetCachedPowerForBinaryExponent!)

    // Generate the digits of the integral part p1 = d[n-1]...d[1]d[0]
    //
    //      10^(k-1) <= p1 < 10^k
    //
    //      p1 = (p1 div 10^(k-1)) * 10^(k-1) + (p1 mod 10^(k-1))
    //         = (d[k-1]         ) * 10^(k-1) + (p1 mod 10^(k-1))
    //
    //      H = p1                                             + p2 * 2^e
    //        = d[k-1] * 10^(k-1) + (p1 mod 10^(k-1))          + p2 * 2^e
    //        = d[k-1] * 10^(k-1) + ((p1 mod 10^(k-1)) * 2^-e + p2) * 2^e
    //        = d[k-1] * 10^(k-1) + (                         rest) * 2^e
    //
    // Now generate the digits d[n] of p1 from left to right (n = k-1,...,0)
    //
    //      p1 = d[k-1]...d[n] * 10^n + d[n-1]...d[0]
    //
    // but stop as soon as
    //
    //      rest * 2^e = (d[n-1]...d[0] * 2^-e + p2) * 2^e <= delta * 2^e

    // The common case is that all the digits of p1 are needed.
    // Optimize for this case and correct later if required.
    length = static_cast<int>(GenerateIntegralDigits(buffer, p1) - buffer);

    if (p2 > delta)
    {
        // The digits of the integral part have been generated (and all of them
        // are significand):
        //
        //      H = d[k-1]...d[1]d[0] + p2 * 2^e
        //        = buffer            + p2 * 2^e
        //
        // Now generate the digits of the fractional part p2 * 2^e.
        //
        // Note:
        // No decimal point is generated: the exponent is adjusted instead.
        //
        // p2 actually represents the fraction
        //
        //      p2 * 2^e
        //          = p2 / 2^-e
        //          = d[-1] / 10^1 + d[-2] / 10^2 + ...
        //
        // Now generate the digits d[-m] of p1 from left to right (m = 1,2,...)
        //
        //      p2 * 2^e = d[-1]d[-2]...d[-m] * 10^-m
        //                      + 10^-m * (d[-m-1] / 10^1 + d[-m-2] / 10^2 + ...)
        //
        // using
        //
        //      10^m * p2 = ((10^m * p2) div 2^-e) * 2^-e + ((10^m * p2) mod 2^-e)
        //                = (                   d) * 2^-e + (                   r)
        //
        // or
        //      10^m * p2 * 2^e = d + r * 2^e
        //
        // i.e.
        //
        //      H = buffer + p2 * 2^e
        //        = buffer + 10^-m * (d + r * 2^e)
        //        = (buffer * 10^m + d) * 10^-m + 10^-m * r * 2^e
        //
        // and stop as soon as 10^-m * r * 2^e <= delta * 2^e

        // unit = 1
        int m = 0;
        for (;;)
        {
            // !!! assert(length < max_digits10) !!!
            assert(length < 17);

            //
            //      H = buffer * 10^-m + 10^-m * (d[-m-1] / 10 + d[-m-2] / 10^2 + ...) * 2^e
            //        = buffer * 10^-m + 10^-m * (p2                                 ) * 2^e
            //        = buffer * 10^-m + 10^-m * (1/10 * (10 * p2)                   ) * 2^e
            //        = buffer * 10^-m + 10^-m * (1/10 * ((10*p2 div 2^-e) * 2^-e + (10*p2 mod 2^-e)) * 2^e
            //
            assert(p2 <= UINT64_MAX / 10);
            p2 *= 10;
            uint64_t const d = p2 >> -one.e;     // d = (10 * p2) div 2^-e
            uint64_t const r = p2 & (one.f - 1); // r = (10 * p2) mod 2^-e
            assert(d <= 9);
            //
            //      H = buffer * 10^-m + 10^-m * (1/10 * (d * 2^-e + r) * 2^e
            //        = buffer * 10^-m + 10^-m * (1/10 * (d + r * 2^e))
            //        = (buffer * 10 + d) * 10^(-m-1) + 10^(-m-1) * r * 2^e
            //
            buffer[length++] = static_cast<char>('0' + d); // buffer := buffer * 10 + d
            //
            //      H = buffer * 10^(-m-1) + 10^(-m-1) * r * 2^e
            //
            p2 = r;
            m++;
            //
            //      H = buffer * 10^-m + 10^-m * p2 * 2^e
            //

            // Keep the units in sync. (unit *= 10)
            delta    *= 10;
            distance *= 10;

            // Check if enough digits have been generated.
            //
            //      10^-m * p2 * 2^e <= delta * 2^e
            //              p2 * 2^e <= 10^m * delta * 2^e
            //                    p2 <= 10^m * delta
            if (p2 <= delta)
            {
                // V = buffer * 10^-m, with L <= V <= H.
                exponent = -m;

                rest = p2;

                // 1 ulp in the decimal representation is now 10^-m.
                // Since delta and distance are now scaled by 10^m, we need to do
                // the same with ulp in order to keep the units in sync.
                //
                //      10^m * 10^-m = 1 = 2^-e * 2^e = ten_m * 2^e
                //
                ten_kappa = one.f; // one.f == 2^-e

                break;
            }
        }
    }
    else // p2 <= delta
    {
        assert((uint64_t{p1} << -one.e) + p2 > delta); // Loop terminates.

        // In this case: Too many digits of p1 might have been generated.
        //
        // Find the largest 0 <= n < k = length, such that
        //
        //      H = (p1 div 10^n) * 10^n + ((p1 mod 10^n) * 2^-e + p2) * 2^e
        //        = (p1 div 10^n) * 10^n + (                     rest) * 2^e
        //
        // and rest <= delta.
        //
        // Compute rest * 2^e = H mod 10^n = p1 + p2 * 2^e = (p1 * 2^-e + p2) * 2^e
        // and check if enough digits have been generated:
        //
        //      rest * 2^e <= delta * 2^e
        //

        int const k = length;
        assert(k >= 0);
        assert(k <= 9);

        rest = p2;

        // 10^n is now 1 ulp in the decimal representation V. The rounding
        // procedure works with DiyFp's with an implicit exponent of e.
        //
        //      10^n = (10^n * 2^-e) * 2^e = ten_kappa * 2^e
        //
        ten_kappa = one.f; // Start with 2^-e

        for (int n = 0; /**/; ++n)
        {
            assert(n <= k - 1);
            assert(rest <= delta);

            // rn = d[n]...d[0] * 2^-e + p2
            uint32_t const dn = static_cast<uint32_t>(buffer[k - 1 - n] - '0');
            uint64_t const rn = dn * ten_kappa + rest;

            if (rn > delta)
            {
                length = k - n;
                exponent = n;
                break;
            }

            rest = rn;
            ten_kappa *= 10;
        }
    }

    // The buffer now contains a correct decimal representation of the input
    // number w = buffer * 10^exponent.

    Grisu2Round(buffer, length, distance, delta, rest, ten_kappa);
}

// v = buffer * 10^exponent
// length is the length of the buffer (number of decimal digits)
// The buffer must be large enough, i.e. >= max_digits10.
inline void Grisu2(char* buffer, int& length, int& exponent, DiyFp m_minus, DiyFp v, DiyFp m_plus)
{
    assert(v.e == m_minus.e);
    assert(v.e == m_plus.e);

    //  --------+-----------------------+-----------------------+--------    (A)
    //          m-                      v                       m+
    //
    //  --------------------+-----------+-----------------------+--------    (B)
    //                      m-          v                       m+
    //
    // First scale v (and m- and m+) such that the exponent is in the range
    // [alpha, gamma].

    auto const cached = GetCachedPowerForBinaryExponent(v.e);

    DiyFp const c_minus_k(cached.f, cached.e); // = c ~= 10^-k

    DiyFp const w       = Multiply(v,       c_minus_k);
    DiyFp const w_minus = Multiply(m_minus, c_minus_k);
    DiyFp const w_plus  = Multiply(m_plus,  c_minus_k);

    // The exponent of the products is = v.e + c_minus_k.e + q and is in the
    // range [alpha, gamma].
    assert(w.e >= kAlpha);
    assert(w.e <= kGamma);

    // Note:
    // The result of Multiply() is **NOT** neccessarily normalized.
    // But since m+ and c are normalized, w_plus.f >= 2^(q - 2).
    assert(w_plus.f >= (uint64_t{1} << (64 - 2)));

    //  ----(---+---)---------------(---+---)---------------(---+---)----
    //          w-                      w                       w+
    //          = c*m-                  = c*v                   = c*m+
    //
    // Multiply rounds its result and c_minus_k is approximated too. w, w- and
    // w+ are now off by a small amount.
    // In fact:
    //
    //      w - v * 10^-k < 1 ulp
    //
    // To account for this inaccuracy, add resp. subtract 1 ulp.
    // Note: ulp(w-) = ulp(w) = ulp(w+).
    //
    //  ----(---+---[---------------(---+---)---------------]---+---)----
    //          w-  L                   w                   H   w+
    //
    // Now any number in [L, H] (bounds included) will round to w when input,
    // regardless of how the input rounding algorithm breaks ties.
    //
    // And DigitGen generates the shortest possible such number in [L, H].
    // Note that this does not mean that Grisu2 always generates the shortest
    // possible number in the interval (m-, m+).
    DiyFp const L(w_minus.f + 1, w_minus.e);
    DiyFp const H(w_plus.f  - 1, w_plus.e );

    Grisu2DigitGen(buffer, length, exponent, L, w, H);
    // w = buffer * 10^exponent

    // v = w * 10^k
    exponent += -cached.k; // cached.k = -k
    // v = buffer * 10^exponent
}

// v = buffer * 10^exponent
// length is the length of the buffer (number of decimal digits)
// The buffer must be large enough, i.e. >= max_digits10.
template <typename Fp>
inline void Grisu2(char* buffer, int& length, int& exponent, Fp value)
{
    static_assert(DiyFp::kPrecision >= std::numeric_limits<Fp>::digits + 3,
        "Grisu2 requires at least three extra bits of precision");

    assert(std::isfinite(value));
    assert(value > 0);

#if 0
    // If the neighbors (and boundaries) of 'value' are always computed for
    // double-precision numbers, all float's can be recovered using strtod
    // (and strtof). However, the resulting decimal representations are not
    // exactly "short".
    //
    // If the neighbors are computed for single-precision numbers, there is a
    // single float (7.0385307e-26f) which can't be recovered using strtod.
    // (The resulting double precision is off by 1 ulp.)
    auto const boundaries = grisu::ComputeBoundaries(static_cast<double>(value));
#else
    auto const boundaries = grisu::ComputeBoundaries(value);
#endif

    return Grisu2(buffer, length, exponent, boundaries.m_minus, boundaries.v, boundaries.m_plus);
}

//--------------------------------------------------------------------------------------------------
// Dtoa
//--------------------------------------------------------------------------------------------------

constexpr int kDtoaPositiveMaxLength = 24;

// Appends a decimal representation of 'value' to buffer.
// Returns a pointer to the element following the digits.
//
// PRE: -1000 < value < 1000
inline char* Itoa1000(char* buffer, int value)
{
    assert(value > -1000);
    assert(value <  1000);

    if (value < 0)
    {
        value = -value;
        *buffer++ = '-';
    }
    else
    {
        *buffer++ = '+';
    }

    uint32_t const k = static_cast<uint32_t>(value);
    if (k < 10)
    {
        *buffer++ = static_cast<char>('0' + k);
    }
    else if (k < 100)
    {
        buffer = Utoa100(buffer, k);
    }
    else
    {
        uint32_t const q = k / 100;
        uint32_t const r = k % 100;
        *buffer++ = static_cast<char>('0' + q);
        buffer = Utoa100(buffer, r);
    }

    return buffer;
}

inline char* FormatFixed(char* buffer, int length, int decimal_point, bool force_trailing_dot_zero)
{
    assert(buffer != nullptr);
    assert(length >= 1);

    if (length <= decimal_point)
    {
        // digits[000]
        // assert(buffer_length >= decimal_point + (force_trailing_dot_zero ? 2 : 0));

        std::memset(buffer + length, '0', static_cast<size_t>(decimal_point - length));
        buffer += decimal_point;
        if (force_trailing_dot_zero)
        {
            *buffer++ = '.';
            *buffer++ = '0';
        }
        return buffer;
    }
    else if (0 < decimal_point)
    {
        // dig.its
        // assert(buffer_length >= length + 1);

        std::memmove(buffer + (decimal_point + 1), buffer + decimal_point, static_cast<size_t>(length - decimal_point));
        buffer[decimal_point] = '.';
        return buffer + (length + 1);
    }
    else // decimal_point <= 0
    {
        // 0.[000]digits
        // assert(buffer_length >= 2 + (-decimal_point) + length);

        std::memmove(buffer + (2 + -decimal_point), buffer, static_cast<size_t>(length));
        buffer[0] = '0';
        buffer[1] = '.';
        std::memset(buffer + 2, '0', static_cast<size_t>(-decimal_point));
        return buffer + (2 + (-decimal_point) + length);
    }
}

inline char* FormatExponential(char* buffer, int length, int decimal_point)
{
    assert(buffer != nullptr);
    assert(length >= 1);

    if (length == 1)
    {
        // dE+123
        // assert(buffer_length >= length + 5);

        buffer += 1;
    }
    else
    {
        // d.igitsE+123
        // assert(buffer_length >= length + 1 + 5);

        std::memmove(buffer + 2, buffer + 1, static_cast<size_t>(length - 1));
        buffer[1] = '.';
        buffer += 1 + length;
    }

    *buffer++ = 'e';

    int const exponent = decimal_point - 1;
    return Itoa1000(buffer, exponent);
}

inline char* StrCopy(char* next, char* last, char const* source)
{
    static_cast<void>(last); // Fix warning

    assert(source != nullptr);

    auto const len = std::strlen(source);
    assert(next <= last);
    assert(static_cast<size_t>(last - next) >= len);

    std::memcpy(next, source, len);
    return next + len;
}

// Generates a decimal representation of the floating-point number `value` in
// the buffer `[next, last)`.
//
// Note: The input `value` must be strictly positive
// Note: The buffer must be large enough (>= kDtoaPositiveMaxLength)
// Note: The result is _not_ null-terminated
template <typename Fp>
inline char* DtoaPositive(char* next, char* last, Fp value, bool force_trailing_dot_zero = false)
{
    assert(last - next >= kDtoaPositiveMaxLength);
    static_cast<void>(last); // Fix warning

    assert(std::isfinite(value));
    assert(value > 0);

    // Compute v = buffer * 10^exponent.
    // The decimal digits are stored in the buffer, which needs to be
    // interpreted as an unsigned decimal integer.
    // length is the length of the buffer, i.e. the number of decimal digits.
    int length = 0;
    int exponent = 0;
    grisu::Grisu2(next, length, exponent, value);

    // Grisu2 generates at most max_digits10 decimal digits.
    assert(length <= std::numeric_limits<Fp>::max_digits10);

    // The position of the decimal point relative to the start of the buffer.
    int const decimal_point = length + exponent;

    // Just appending the exponent would yield a correct decimal representation
    // for the input value.

#if 0
    // Format the digits similar to printf's %g style.
    //
    // NB:
    // These are the values used by JavaScript's ToString applied to Number
    // type. Printf uses the values -4 and max_digits10 resp.
    constexpr int kMinExp = -6;
    constexpr int kMaxExp = 21;

    bool const use_fixed = kMinExp < decimal_point && decimal_point <= kMaxExp;
#else
    // NB:
    // Integers <= 2^p = kMaxVal are exactly representable as Fp's.
    constexpr auto kMinExp = -6;
    constexpr auto kMaxVal = static_cast<Fp>(uint64_t{1} << std::numeric_limits<Fp>::digits); // <= 16 digits

    bool const use_fixed = kMinExp < decimal_point && value <= kMaxVal;
#endif

    char* const end = use_fixed
        ? FormatFixed(next, length, decimal_point, force_trailing_dot_zero)
        : FormatExponential(next, length, decimal_point);

    assert(end - next <= kDtoaPositiveMaxLength);
    return end;
}

// Generates a decimal representation of the floating-point number `value` in
// the buffer `[next, last)`.
//
// Note: The buffer must be large enough.
//       Max(1 + kDtoaPositiveMaxLength, len(nan_string), 1 + len(inf_string))
//       is sufficient.
// Note: The result is _not_ null-terminated.
template <typename Fp>
inline char* Dtoa(
    char*       next,
    char*       last,
    Fp          value,
    bool        force_trailing_dot_zero = false,
    char const* nan_string = "NaN",
    char const* inf_string = "Infinity")
{
    if (std::isnan(value))
        return StrCopy(next, last, nan_string);

    // Use signbit(value) instead of (value < 0) since signbit works for -0.
    if (std::signbit(value))
    {
        value = -value;
        *next++ = '-';
    }

    if (std::isinf(value))
        return StrCopy(next, last, inf_string);

    if (value == 0)
    {
        *next++ = '0';
        if (force_trailing_dot_zero)
        {
            *next++ = '.';
            *next++ = '0';
        }
        return next;
    }

    return grisu::DtoaPositive(next, last, value, force_trailing_dot_zero);
}

} // namespace grisu

/*
Copyright (c) 2009 Florian Loitsch

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use,
copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.
*/
