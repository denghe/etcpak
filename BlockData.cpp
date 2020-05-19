#include <assert.h>
#include <string.h>

#include "BlockData.hpp"
#include "ColorSpace.hpp"
#include "Debug.hpp"
#include "MipMap.hpp"
#include "mmap.hpp"
#include "ProcessRGB.hpp"
#include "ProcessDxtc.hpp"
#include "Tables.hpp"
#include "TaskDispatch.hpp"

#ifdef __ARM_NEON
#  include <arm_neon.h>
#endif

#ifdef __SSE4_1__
#  ifdef _MSC_VER
#    include <intrin.h>
#    include <Windows.h>
#    define _bswap(x) _byteswap_ulong(x)
#    define _bswap64(x) _byteswap_uint64(x)
#  else
#    include <x86intrin.h>
#  endif
#else
#  ifndef _MSC_VER
#    include <byteswap.h>
#    define _bswap(x) bswap_32(x)
#    define _bswap64(x) bswap_64(x)
#  endif
#endif

#ifndef _bswap
#  define _bswap(x) __builtin_bswap32(x)
#  define _bswap64(x) __builtin_bswap64(x)
#endif


BlockData::BlockData( const char* fn )
    : m_file( fopen( fn, "rb" ) )
{
    assert( m_file );
    fseek( m_file, 0, SEEK_END );
    m_maplen = ftell( m_file );
    fseek( m_file, 0, SEEK_SET );
    m_data = (uint8_t*)mmap( nullptr, m_maplen, PROT_READ, MAP_SHARED, fileno( m_file ), 0 );

    auto data32 = (uint32_t*)m_data;
    if( *data32 == 0x03525650 )
    {
        // PVR
        switch( *(data32+2) )
        {
        case 6:
            m_type = Etc1;
            break;
        case 7:
            m_type = Dxt1;
            break;
        case 11:
            m_type = Dxt5;
            break;
        case 22:
            m_type = Etc2_RGB;
            break;
        case 23:
            m_type = Etc2_RGBA;
            break;
        default:
            assert( false );
            break;
        }

        m_size.y = *(data32+6);
        m_size.x = *(data32+7);
        m_dataOffset = 52 + *(data32+12);
    }
    else if( *data32 == 0x58544BAB )
    {
        // KTX
        switch( *(data32+7) )
        {
        case 0x9274:
            m_type = Etc2_RGB;
            break;
        case 0x9278:
            m_type = Etc2_RGBA;
            break;
        default:
            assert( false );
            break;
        }

        m_size.x = *(data32+9);
        m_size.y = *(data32+10);
        m_dataOffset = sizeof( uint32_t ) * 17 + *(data32+15);
    }
    else
    {
        assert( false );
    }
}

static uint8_t* OpenForWriting( const char* fn, size_t len, const v2i& size, FILE** f, int levels, BlockData::Type type )
{
    *f = fopen( fn, "wb+" );
    assert( *f );
    fseek( *f, len - 1, SEEK_SET );
    const char zero = 0;
    fwrite( &zero, 1, 1, *f );
    fseek( *f, 0, SEEK_SET );

    auto ret = (uint8_t*)mmap( nullptr, len, PROT_WRITE, MAP_SHARED, fileno( *f ), 0 );
    auto dst = (uint32_t*)ret;

    *dst++ = 0x03525650;  // version
    *dst++ = 0;           // flags
    switch( type )        // pixelformat[0]
    {
    case BlockData::Etc1:
        *dst++ = 6;
        break;
    case BlockData::Etc2_RGB:
        *dst++ = 22;
        break;
    case BlockData::Etc2_RGBA:
        *dst++ = 23;
        break;
    case BlockData::Dxt1:
        *dst++ = 7;
        break;
    case BlockData::Dxt5:
        *dst++ = 11;
        break;
    default:
        assert( false );
        break;
    }
    *dst++ = 0;           // pixelformat[1]
    *dst++ = 0;           // colourspace
    *dst++ = 0;           // channel type
    *dst++ = size.y;      // height
    *dst++ = size.x;      // width
    *dst++ = 1;           // depth
    *dst++ = 1;           // num surfs
    *dst++ = 1;           // num faces
    *dst++ = levels;      // mipmap count
    *dst++ = 0;           // metadata size

    return ret;
}

static int AdjustSizeForMipmaps( const v2i& size, int levels )
{
    int len = 0;
    v2i current = size;
    for( int i=1; i<levels; i++ )
    {
        assert( current.x != 1 || current.y != 1 );
        current.x = std::max( 1, current.x / 2 );
        current.y = std::max( 1, current.y / 2 );
        len += std::max( 4, current.x ) * std::max( 4, current.y ) / 2;
    }
    assert( current.x == 1 && current.y == 1 );
    return len;
}

BlockData::BlockData( const char* fn, const v2i& size, bool mipmap, Type type )
    : m_size( size )
    , m_dataOffset( 52 )
    , m_maplen( m_size.x*m_size.y/2 )
    , m_type( type )
{
    assert( m_size.x%4 == 0 && m_size.y%4 == 0 );

    uint32_t cnt = m_size.x * m_size.y / 16;
    DBGPRINT( cnt << " blocks" );

    int levels = 1;

    if( mipmap )
    {
        levels = NumberOfMipLevels( size );
        DBGPRINT( "Number of mipmaps: " << levels );
        m_maplen += AdjustSizeForMipmaps( size, levels );
    }

    if( type == Etc2_RGBA || type == Dxt5 ) m_maplen *= 2;

    m_maplen += m_dataOffset;
    m_data = OpenForWriting( fn, m_maplen, m_size, &m_file, levels, type );
}

BlockData::BlockData( const v2i& size, bool mipmap, Type type )
    : m_size( size )
    , m_dataOffset( 52 )
    , m_file( nullptr )
    , m_maplen( m_size.x*m_size.y/2 )
    , m_type( type )
{
    assert( m_size.x%4 == 0 && m_size.y%4 == 0 );
    if( mipmap )
    {
        const int levels = NumberOfMipLevels( size );
        m_maplen += AdjustSizeForMipmaps( size, levels );
    }

    if( type == Etc2_RGBA || type == Dxt5 ) m_maplen *= 2;

    m_maplen += m_dataOffset;
    m_data = new uint8_t[m_maplen];
}

BlockData::~BlockData()
{
    if( m_file )
    {
        munmap( m_data, m_maplen );
        fclose( m_file );
    }
    else
    {
        delete[] m_data;
    }
}

void BlockData::Process( const uint32_t* src, uint32_t blocks, size_t offset, size_t width, Channels type, bool dither )
{
    auto dst = ((uint64_t*)( m_data + m_dataOffset )) + offset;

    if( type == Channels::Alpha )
    {
        if( m_type != Etc1 )
        {
            CompressEtc2Alpha( src, dst, blocks, width );
        }
        else
        {
            CompressEtc1Alpha( src, dst, blocks, width );
        }
    }
    else
    {
        switch( m_type )
        {
        case Etc1:
            if( dither )
            {
                CompressEtc1RgbDither( src, dst, blocks, width );
            }
            else
            {
                CompressEtc1Rgb( src, dst, blocks, width );
            }
            break;
        case Etc2_RGB:
            CompressEtc2Rgb( src, dst, blocks, width );
            break;
        case Dxt1:
            if( dither )
            {
                CompressDxt1Dither( src, dst, blocks, width );
            }
            else
            {
                CompressDxt1( src, dst, blocks, width );
            }
            break;
        default:
            assert( false );
            break;
        }
    }
}

void BlockData::ProcessRGBA( const uint32_t* src, uint32_t blocks, size_t offset, size_t width )
{
    auto dst = ((uint64_t*)( m_data + m_dataOffset )) + offset * 2;

    switch( m_type )
    {
    case Etc2_RGBA:
        CompressEtc2Rgba( src, dst, blocks, width );
        break;
    case Dxt5:
        CompressDxt5( src, dst, blocks, width );
        break;
    default:
        assert( false );
        break;
    }
}

namespace
{

static etcpak_force_inline int32_t expand6(uint32_t value)
{
    return (value << 2) | (value >> 4);
}

static etcpak_force_inline int32_t expand7(uint32_t value)
{
    return (value << 1) | (value >> 6);
}

static etcpak_force_inline void DecodePlanar( uint64_t block, uint32_t* dst, uint32_t w )
{
    const auto bv = expand6((block >> ( 0 + 32)) & 0x3F);
    const auto gv = expand7((block >> ( 6 + 32)) & 0x7F);
    const auto rv = expand6((block >> (13 + 32)) & 0x3F);

    const auto bh = expand6((block >> (19 + 32)) & 0x3F);
    const auto gh = expand7((block >> (25 + 32)) & 0x7F);

    const auto rh0 = (block >> (32 - 32)) & 0x01;
    const auto rh1 = ((block >> (34 - 32)) & 0x1F) << 1;
    const auto rh = expand6(rh0 | rh1);

    const auto bo0 = (block >> (39 - 32)) & 0x07;
    const auto bo1 = ((block >> (43 - 32)) & 0x3) << 3;
    const auto bo2 = ((block >> (48 - 32)) & 0x1) << 5;
    const auto bo = expand6(bo0 | bo1 | bo2);
    const auto go0 = (block >> (49 - 32)) & 0x3F;
    const auto go1 = ((block >> (56 - 32)) & 0x01) << 6;
    const auto go = expand7(go0 | go1);
    const auto ro = expand6((block >> (57 - 32)) & 0x3F);

#ifdef __ARM_NEON
    uint64_t init = uint64_t(uint16_t(rh-ro)) | ( uint64_t(uint16_t(gh-go)) << 16 ) | ( uint64_t(uint16_t(bh-bo)) << 32 );
    int16x8_t chco = vreinterpretq_s16_u64( vdupq_n_u64( init ) );
    init = uint64_t(uint16_t( (rv-ro) - 4 * (rh-ro) )) | ( uint64_t(uint16_t( (gv-go) - 4 * (gh-go) )) << 16 ) | ( uint64_t(uint16_t( (bv-bo) - 4 * (bh-bo) )) << 32 );
    int16x8_t cvco = vreinterpretq_s16_u64( vdupq_n_u64( init ) );
    init = uint64_t(4*ro+2) | ( uint64_t(4*go+2) << 16 ) | ( uint64_t(4*bo+2) << 32 ) | ( uint64_t(0xFFF) << 48 );
    int16x8_t col = vreinterpretq_s16_u64( vdupq_n_u64( init ) );

    for( int j=0; j<4; j++ )
    {
        for( int i=0; i<4; i++ )
        {
            uint8x8_t c = vqshrun_n_s16( col, 2 );
            vst1_lane_u32( dst+j*w+i, vreinterpret_u32_u8( c ), 0 );
            col = vaddq_s16( col, chco );
        }
        col = vaddq_s16( col, cvco );
    }
#elif defined __AVX2__
    const auto R0 = 4*ro+2;
    const auto G0 = 4*go+2;
    const auto B0 = 4*bo+2;
    const auto RHO = rh-ro;
    const auto GHO = gh-go;
    const auto BHO = bh-bo;

    __m256i cvco = _mm256_setr_epi16( rv - ro, gv - go, bv - bo, 0, rv - ro, gv - go, bv - bo, 0, rv - ro, gv - go, bv - bo, 0, rv - ro, gv - go, bv - bo, 0 );
    __m256i col = _mm256_setr_epi16( R0, G0, B0, 0xFFF, R0+RHO, G0+GHO, B0+BHO, 0xFFF, R0+2*RHO, G0+2*GHO, B0+2*BHO, 0xFFF, R0+3*RHO, G0+3*GHO, B0+3*BHO, 0xFFF );

    for( int j=0; j<4; j++ )
    {
        __m256i c = _mm256_srai_epi16( col, 2 );
        __m128i s = _mm_packus_epi16( _mm256_castsi256_si128( c ), _mm256_extracti128_si256( c, 1 ) );
        _mm_storeu_si128( (__m128i*)(dst+j*w), s );
        col = _mm256_add_epi16( col, cvco );
    }
#elif defined __SSE4_1__
    __m128i chco = _mm_setr_epi16( rh - ro, gh - go, bh - bo, 0, 0, 0, 0, 0 );
    __m128i cvco = _mm_setr_epi16( (rv - ro) - 4 * (rh - ro), (gv - go) - 4 * (gh - go), (bv - bo) - 4 * (bh - bo), 0, 0, 0, 0, 0 );
    __m128i col = _mm_setr_epi16( 4*ro+2, 4*go+2, 4*bo+2, 0xFFF, 0, 0, 0, 0 );

    for( int j=0; j<4; j++ )
    {
        for( int i=0; i<4; i++ )
        {
            __m128i c = _mm_srai_epi16( col, 2 );
            __m128i s = _mm_packus_epi16( c, c );
            dst[j*w+i] = _mm_cvtsi128_si32( s );
            col = _mm_add_epi16( col, chco );
        }
        col = _mm_add_epi16( col, cvco );
    }
#else
    for( int j=0; j<4; j++ )
    {
        for( int i=0; i<4; i++ )
        {
            const uint32_t r = (i * (rh - ro) + j * (rv - ro) + 4 * ro + 2) >> 2;
            const uint32_t g = (i * (gh - go) + j * (gv - go) + 4 * go + 2) >> 2;
            const uint32_t b = (i * (bh - bo) + j * (bv - bo) + 4 * bo + 2) >> 2;
            if( ( ( r | g | b ) & ~0xFF ) == 0 )
            {
                dst[j*w+i] = r | ( g << 8 ) | ( b << 16 ) | 0xFF000000;
            }
            else
            {
                const auto rc = clampu8( r );
                const auto gc = clampu8( g );
                const auto bc = clampu8( b );
                dst[j*w+i] = rc | ( gc << 8 ) | ( bc << 16 ) | 0xFF000000;
            }
        }
    }
#endif
}

static etcpak_force_inline void DecodePlanarAlpha( uint64_t block, uint64_t alpha, uint32_t* dst, uint32_t w )
{
    const auto bv = expand6((block >> ( 0 + 32)) & 0x3F);
    const auto gv = expand7((block >> ( 6 + 32)) & 0x7F);
    const auto rv = expand6((block >> (13 + 32)) & 0x3F);

    const auto bh = expand6((block >> (19 + 32)) & 0x3F);
    const auto gh = expand7((block >> (25 + 32)) & 0x7F);

    const auto rh0 = (block >> (32 - 32)) & 0x01;
    const auto rh1 = ((block >> (34 - 32)) & 0x1F) << 1;
    const auto rh = expand6(rh0 | rh1);

    const auto bo0 = (block >> (39 - 32)) & 0x07;
    const auto bo1 = ((block >> (43 - 32)) & 0x3) << 3;
    const auto bo2 = ((block >> (48 - 32)) & 0x1) << 5;
    const auto bo = expand6(bo0 | bo1 | bo2);
    const auto go0 = (block >> (49 - 32)) & 0x3F;
    const auto go1 = ((block >> (56 - 32)) & 0x01) << 6;
    const auto go = expand7(go0 | go1);
    const auto ro = expand6((block >> (57 - 32)) & 0x3F);

    const int32_t base = alpha >> 56;
    const int32_t mul = ( alpha >> 52 ) & 0xF;
    const auto tbl = g_alpha[( alpha >> 48 ) & 0xF];

#ifdef __ARM_NEON
    uint64_t init = uint64_t(uint16_t(rh-ro)) | ( uint64_t(uint16_t(gh-go)) << 16 ) | ( uint64_t(uint16_t(bh-bo)) << 32 );
    int16x8_t chco = vreinterpretq_s16_u64( vdupq_n_u64( init ) );
    init = uint64_t(uint16_t( (rv-ro) - 4 * (rh-ro) )) | ( uint64_t(uint16_t( (gv-go) - 4 * (gh-go) )) << 16 ) | ( uint64_t(uint16_t( (bv-bo) - 4 * (bh-bo) )) << 32 );
    int16x8_t cvco = vreinterpretq_s16_u64( vdupq_n_u64( init ) );
    init = uint64_t(4*ro+2) | ( uint64_t(4*go+2) << 16 ) | ( uint64_t(4*bo+2) << 32 );
    int16x8_t col = vreinterpretq_s16_u64( vdupq_n_u64( init ) );

    for( int j=0; j<4; j++ )
    {
        for( int i=0; i<4; i++ )
        {
            const auto amod = tbl[(alpha >> ( 45 - j*3 - i*12 )) & 0x7];
            const uint32_t a = clampu8( base + amod * mul );
            uint8x8_t c = vqshrun_n_s16( col, 2 );
            dst[j*w+i] = vget_lane_u32( vreinterpret_u32_u8( c ), 0 ) | ( a << 24 );
            col = vaddq_s16( col, chco );
        }
        col = vaddq_s16( col, cvco );
    }
#elif defined __SSE4_1__
    __m128i chco = _mm_setr_epi16( rh - ro, gh - go, bh - bo, 0, 0, 0, 0, 0 );
    __m128i cvco = _mm_setr_epi16( (rv - ro) - 4 * (rh - ro), (gv - go) - 4 * (gh - go), (bv - bo) - 4 * (bh - bo), 0, 0, 0, 0, 0 );
    __m128i col = _mm_setr_epi16( 4*ro+2, 4*go+2, 4*bo+2, 0, 0, 0, 0, 0 );

    for( int j=0; j<4; j++ )
    {
        for( int i=0; i<4; i++ )
        {
            const auto amod = tbl[(alpha >> ( 45 - j*3 - i*12 )) & 0x7];
            const uint32_t a = clampu8( base + amod * mul );
            __m128i c = _mm_srai_epi16( col, 2 );
            __m128i s = _mm_packus_epi16( c, c );
            dst[j*w+i] = _mm_cvtsi128_si32( s ) | ( a << 24 );
            col = _mm_add_epi16( col, chco );
        }
        col = _mm_add_epi16( col, cvco );
    }
#else
    for (auto j = 0; j < 4; j++)
    {
        for (auto i = 0; i < 4; i++)
        {
            const uint32_t r = (i * (rh - ro) + j * (rv - ro) + 4 * ro + 2) >> 2;
            const uint32_t g = (i * (gh - go) + j * (gv - go) + 4 * go + 2) >> 2;
            const uint32_t b = (i * (bh - bo) + j * (bv - bo) + 4 * bo + 2) >> 2;
            const auto amod = tbl[(alpha >> ( 45 - j*3 - i*12 )) & 0x7];
            const uint32_t a = clampu8( base + amod * mul );
            if( ( ( r | g | b ) & ~0xFF ) == 0 )
            {
                dst[j*w+i] = r | ( g << 8 ) | ( b << 16 ) | ( a << 24 );
            }
            else
            {
                const auto rc = clampu8( r );
                const auto gc = clampu8( g );
                const auto bc = clampu8( b );
                dst[j*w+i] = rc | ( gc << 8 ) | ( bc << 16 ) | ( a << 24 );
            }
        }
    }
#endif
}

}

BitmapPtr BlockData::Decode()
{
    switch( m_type )
    {
    case Etc1:
    case Etc2_RGB:
        return DecodeRGB();
    case Etc2_RGBA:
        return DecodeRGBA();
    case Dxt1:
        return DecodeDxt1();
    case Dxt5:
    default:
        assert( false );
        return nullptr;
    }
}

static etcpak_force_inline uint64_t ConvertByteOrder( uint64_t d )
{
    uint32_t word[2];
    memcpy( word, &d, 8 );
    word[0] = _bswap( word[0] );
    word[1] = _bswap( word[1] );
    memcpy( &d, word, 8 );
    return d;
}

static etcpak_force_inline void DecodeRGBPart( uint64_t d, uint32_t* dst, uint32_t w )
{
    d = ConvertByteOrder( d );

    uint32_t br[2], bg[2], bb[2];

    if( d & 0x2 )
    {
        int32_t dr, dg, db;

        uint32_t r0 = ( d & 0xF8000000 ) >> 27;
        uint32_t g0 = ( d & 0x00F80000 ) >> 19;
        uint32_t b0 = ( d & 0x0000F800 ) >> 11;

        dr = ( int32_t(d) << 5 ) >> 29;
        dg = ( int32_t(d) << 13 ) >> 29;
        db = ( int32_t(d) << 21 ) >> 29;

        int32_t r1 = int32_t(r0) + dr;
        int32_t g1 = int32_t(g0) + dg;
        int32_t b1 = int32_t(b0) + db;

        // T and H modes are not handled
        if( (b1 < 0) || (b1 > 31) )
        {
            DecodePlanar( d, dst, w );
            return;
        }

        br[0] = ( r0 << 3 ) | ( r0 >> 2 );
        br[1] = ( r1 << 3 ) | ( r1 >> 2 );
        bg[0] = ( g0 << 3 ) | ( g0 >> 2 );
        bg[1] = ( g1 << 3 ) | ( g1 >> 2 );
        bb[0] = ( b0 << 3 ) | ( b0 >> 2 );
        bb[1] = ( b1 << 3 ) | ( b1 >> 2 );
    }
    else
    {
        br[0] = ( ( d & 0xF0000000 ) >> 24 ) | ( ( d & 0xF0000000 ) >> 28 );
        br[1] = ( ( d & 0x0F000000 ) >> 20 ) | ( ( d & 0x0F000000 ) >> 24 );
        bg[0] = ( ( d & 0x00F00000 ) >> 16 ) | ( ( d & 0x00F00000 ) >> 20 );
        bg[1] = ( ( d & 0x000F0000 ) >> 12 ) | ( ( d & 0x000F0000 ) >> 16 );
        bb[0] = ( ( d & 0x0000F000 ) >> 8  ) | ( ( d & 0x0000F000 ) >> 12 );
        bb[1] = ( ( d & 0x00000F00 ) >> 4  ) | ( ( d & 0x00000F00 ) >> 8  );
    }

    unsigned int tcw[2];
    tcw[0] = ( d & 0xE0 ) >> 5;
    tcw[1] = ( d & 0x1C ) >> 2;

    uint32_t b1 = ( d >> 32 ) & 0xFFFF;
    uint32_t b2 = ( d >> 48 );

    b1 = ( b1 | ( b1 << 8 ) ) & 0x00FF00FF;
    b1 = ( b1 | ( b1 << 4 ) ) & 0x0F0F0F0F;
    b1 = ( b1 | ( b1 << 2 ) ) & 0x33333333;
    b1 = ( b1 | ( b1 << 1 ) ) & 0x55555555;

    b2 = ( b2 | ( b2 << 8 ) ) & 0x00FF00FF;
    b2 = ( b2 | ( b2 << 4 ) ) & 0x0F0F0F0F;
    b2 = ( b2 | ( b2 << 2 ) ) & 0x33333333;
    b2 = ( b2 | ( b2 << 1 ) ) & 0x55555555;

    uint32_t idx = b1 | ( b2 << 1 );

    if( d & 0x1 )
    {
        for( int i=0; i<4; i++ )
        {
            for( int j=0; j<4; j++ )
            {
                const auto mod = g_table[tcw[j/2]][idx & 0x3];
                const auto r = br[j/2] + mod;
                const auto g = bg[j/2] + mod;
                const auto b = bb[j/2] + mod;
                if( ( ( r | g | b ) & ~0xFF ) == 0 )
                {
                    dst[j*w+i] = r | ( g << 8 ) | ( b << 16 ) | 0xFF000000;
                }
                else
                {
                    const auto rc = clampu8( r );
                    const auto gc = clampu8( g );
                    const auto bc = clampu8( b );
                    dst[j*w+i] = rc | ( gc << 8 ) | ( bc << 16 ) | 0xFF000000;
                }
                idx >>= 2;
            }
        }
    }
    else
    {
        for( int i=0; i<4; i++ )
        {
            const auto tbl = g_table[tcw[i/2]];
            const auto cr = br[i/2];
            const auto cg = bg[i/2];
            const auto cb = bb[i/2];

            for( int j=0; j<4; j++ )
            {
                const auto mod = tbl[idx & 0x3];
                const auto r = cr + mod;
                const auto g = cg + mod;
                const auto b = cb + mod;
                if( ( ( r | g | b ) & ~0xFF ) == 0 )
                {
                    dst[j*w+i] = r | ( g << 8 ) | ( b << 16 ) | 0xFF000000;
                }
                else
                {
                    const auto rc = clampu8( r );
                    const auto gc = clampu8( g );
                    const auto bc = clampu8( b );
                    dst[j*w+i] = rc | ( gc << 8 ) | ( bc << 16 ) | 0xFF000000;
                }
                idx >>= 2;
            }
        }
    }
}

static etcpak_force_inline void DecodeRGBAPart( uint64_t d, uint64_t alpha, uint32_t* dst, uint32_t w )
{
    d = ConvertByteOrder( d );
    alpha = _bswap64( alpha );

    uint32_t br[2], bg[2], bb[2];

    if( d & 0x2 )
    {
        int32_t dr, dg, db;

        uint32_t r0 = ( d & 0xF8000000 ) >> 27;
        uint32_t g0 = ( d & 0x00F80000 ) >> 19;
        uint32_t b0 = ( d & 0x0000F800 ) >> 11;

        dr = ( int32_t(d) << 5 ) >> 29;
        dg = ( int32_t(d) << 13 ) >> 29;
        db = ( int32_t(d) << 21 ) >> 29;

        int32_t r1 = int32_t(r0) + dr;
        int32_t g1 = int32_t(g0) + dg;
        int32_t b1 = int32_t(b0) + db;

        // T and H modes are not handled
        if( (b1 < 0) || (b1 > 31) )
        {
            DecodePlanarAlpha( d, alpha, dst, w );
            return;
        }

        br[0] = ( r0 << 3 ) | ( r0 >> 2 );
        br[1] = ( r1 << 3 ) | ( r1 >> 2 );
        bg[0] = ( g0 << 3 ) | ( g0 >> 2 );
        bg[1] = ( g1 << 3 ) | ( g1 >> 2 );
        bb[0] = ( b0 << 3 ) | ( b0 >> 2 );
        bb[1] = ( b1 << 3 ) | ( b1 >> 2 );
    }
    else
    {
        br[0] = ( ( d & 0xF0000000 ) >> 24 ) | ( ( d & 0xF0000000 ) >> 28 );
        br[1] = ( ( d & 0x0F000000 ) >> 20 ) | ( ( d & 0x0F000000 ) >> 24 );
        bg[0] = ( ( d & 0x00F00000 ) >> 16 ) | ( ( d & 0x00F00000 ) >> 20 );
        bg[1] = ( ( d & 0x000F0000 ) >> 12 ) | ( ( d & 0x000F0000 ) >> 16 );
        bb[0] = ( ( d & 0x0000F000 ) >> 8  ) | ( ( d & 0x0000F000 ) >> 12 );
        bb[1] = ( ( d & 0x00000F00 ) >> 4  ) | ( ( d & 0x00000F00 ) >> 8  );
    }

    unsigned int tcw[2];
    tcw[0] = ( d & 0xE0 ) >> 5;
    tcw[1] = ( d & 0x1C ) >> 2;

    uint32_t b1 = ( d >> 32 ) & 0xFFFF;
    uint32_t b2 = ( d >> 48 );

    b1 = ( b1 | ( b1 << 8 ) ) & 0x00FF00FF;
    b1 = ( b1 | ( b1 << 4 ) ) & 0x0F0F0F0F;
    b1 = ( b1 | ( b1 << 2 ) ) & 0x33333333;
    b1 = ( b1 | ( b1 << 1 ) ) & 0x55555555;

    b2 = ( b2 | ( b2 << 8 ) ) & 0x00FF00FF;
    b2 = ( b2 | ( b2 << 4 ) ) & 0x0F0F0F0F;
    b2 = ( b2 | ( b2 << 2 ) ) & 0x33333333;
    b2 = ( b2 | ( b2 << 1 ) ) & 0x55555555;

    uint32_t idx = b1 | ( b2 << 1 );

    const int32_t base = alpha >> 56;
    const int32_t mul = ( alpha >> 52 ) & 0xF;
    const auto atbl = g_alpha[( alpha >> 48 ) & 0xF];

    if( d & 0x1 )
    {
        for( int i=0; i<4; i++ )
        {
            for( int j=0; j<4; j++ )
            {
                const auto mod = g_table[tcw[j/2]][idx & 0x3];
                const auto r = br[j/2] + mod;
                const auto g = bg[j/2] + mod;
                const auto b = bb[j/2] + mod;
                const auto amod = atbl[(alpha >> ( 45 - j*3 - i*12 )) & 0x7];
                const uint32_t a = clampu8( base + amod * mul );
                if( ( ( r | g | b ) & ~0xFF ) == 0 )
                {
                    dst[j*w+i] = r | ( g << 8 ) | ( b << 16 ) | ( a << 24 );
                }
                else
                {
                    const auto rc = clampu8( r );
                    const auto gc = clampu8( g );
                    const auto bc = clampu8( b );
                    dst[j*w+i] = rc | ( gc << 8 ) | ( bc << 16 ) | ( a << 24 );
                }
                idx >>= 2;
            }
        }
    }
    else
    {
        for( int i=0; i<4; i++ )
        {
            const auto tbl = g_table[tcw[i/2]];
            const auto cr = br[i/2];
            const auto cg = bg[i/2];
            const auto cb = bb[i/2];

            for( int j=0; j<4; j++ )
            {
                const auto mod = tbl[idx & 0x3];
                const auto r = cr + mod;
                const auto g = cg + mod;
                const auto b = cb + mod;
                const auto amod = atbl[(alpha >> ( 45 - j*3 - i*12 )) & 0x7];
                const uint32_t a = clampu8( base + amod * mul );
                if( ( ( r | g | b ) & ~0xFF ) == 0 )
                {
                    dst[j*w+i] = r | ( g << 8 ) | ( b << 16 ) | ( a << 24 );
                }
                else
                {
                    const auto rc = clampu8( r );
                    const auto gc = clampu8( g );
                    const auto bc = clampu8( b );
                    dst[j*w+i] = rc | ( gc << 8 ) | ( bc << 16 ) | ( a << 24 );
                }
                idx >>= 2;
            }
        }
    }
}

BitmapPtr BlockData::DecodeRGB()
{
    auto ret = std::make_shared<Bitmap>( m_size );

    const uint64_t* src = (const uint64_t*)( m_data + m_dataOffset );
    uint32_t* dst = ret->Data();

    for( int y=0; y<m_size.y/4; y++ )
    {
        for( int x=0; x<m_size.x/4; x++ )
        {
            uint64_t d = *src++;
            DecodeRGBPart( d, dst, m_size.x );
            dst += 4;
        }
        dst += m_size.x*3;
    }

    return ret;
}

BitmapPtr BlockData::DecodeRGBA()
{
    auto ret = std::make_shared<Bitmap>( m_size );

    const uint64_t* src = (const uint64_t*)( m_data + m_dataOffset );
    uint32_t* dst = ret->Data();

    for( int y=0; y<m_size.y/4; y++ )
    {
        for( int x=0; x<m_size.x/4; x++ )
        {
            uint64_t a = *src++;
            uint64_t d = *src++;
            DecodeRGBAPart( d, a, dst, m_size.x );
            dst += 4;
        }
        dst += m_size.x*3;
    }

    return ret;
}

static etcpak_force_inline void DecodeDxt1Part( uint64_t d, uint32_t* dst, uint32_t w )
{
    uint8_t* in = (uint8_t*)&d;
    uint16_t c0, c1;
    uint32_t idx;
    memcpy( &c0, in, 2 );
    memcpy( &c1, in+2, 2 );
    memcpy( &idx, in+4, 4 );

    uint8_t r0 = ( ( c0 & 0xF800 ) >> 8 ) | ( ( c0 & 0xF800 ) >> 13 );
    uint8_t g0 = ( ( c0 & 0x07E0 ) >> 3 ) | ( ( c0 & 0x07E0 ) >> 9 );
    uint8_t b0 = ( ( c0 & 0x001F ) << 3 ) | ( ( c0 & 0x001F ) >> 2 );

    uint8_t r1 = ( ( c1 & 0xF800 ) >> 8 ) | ( ( c1 & 0xF800 ) >> 13 );
    uint8_t g1 = ( ( c1 & 0x07E0 ) >> 3 ) | ( ( c1 & 0x07E0 ) >> 9 );
    uint8_t b1 = ( ( c1 & 0x001F ) << 3 ) | ( ( c1 & 0x001F ) >> 2 );

    uint32_t dict[4];

    dict[0] = 0xFF000000 | ( b0 << 16 ) | ( g0 << 8 ) | r0;
    dict[1] = 0xFF000000 | ( b1 << 16 ) | ( g1 << 8 ) | r1;

    uint32_t r, g, b;
    if( c0 > c1 )
    {
        r = (2*r0+r1)/3;
        g = (2*g0+g1)/3;
        b = (2*b0+b1)/3;
        dict[2] = 0xFF000000 | ( b << 16 ) | ( g << 8 ) | r;
        r = (2*r1+r0)/3;
        g = (2*g1+g0)/3;
        b = (2*b1+b0)/3;
        dict[3] = 0xFF000000 | ( b << 16 ) | ( g << 8 ) | r;
    }
    else
    {
        r = (int(r0)+r1)/2;
        g = (int(g0)+g1)/2;
        b = (int(b0)+b1)/2;
        dict[2] = 0xFF000000 | ( b << 16 ) | ( g << 8 ) | r;
        dict[3] = 0xFF000000;
    }

    memcpy( dst+0, dict + (idx & 0x3), 4 );
    idx >>= 2;
    memcpy( dst+1, dict + (idx & 0x3), 4 );
    idx >>= 2;
    memcpy( dst+2, dict + (idx & 0x3), 4 );
    idx >>= 2;
    memcpy( dst+3, dict + (idx & 0x3), 4 );
    idx >>= 2;
    dst += w;

    memcpy( dst+0, dict + (idx & 0x3), 4 );
    idx >>= 2;
    memcpy( dst+1, dict + (idx & 0x3), 4 );
    idx >>= 2;
    memcpy( dst+2, dict + (idx & 0x3), 4 );
    idx >>= 2;
    memcpy( dst+3, dict + (idx & 0x3), 4 );
    idx >>= 2;
    dst += w;

    memcpy( dst+0, dict + (idx & 0x3), 4 );
    idx >>= 2;
    memcpy( dst+1, dict + (idx & 0x3), 4 );
    idx >>= 2;
    memcpy( dst+2, dict + (idx & 0x3), 4 );
    idx >>= 2;
    memcpy( dst+3, dict + (idx & 0x3), 4 );
    idx >>= 2;
    dst += w;

    memcpy( dst+0, dict + (idx & 0x3), 4 );
    idx >>= 2;
    memcpy( dst+1, dict + (idx & 0x3), 4 );
    idx >>= 2;
    memcpy( dst+2, dict + (idx & 0x3), 4 );
    idx >>= 2;
    memcpy( dst+3, dict + (idx & 0x3), 4 );
}

BitmapPtr BlockData::DecodeDxt1()
{
    auto ret = std::make_shared<Bitmap>( m_size );

    const uint64_t* src = (const uint64_t*)( m_data + m_dataOffset );
    uint32_t* dst = ret->Data();

    for( int y=0; y<m_size.y/4; y++ )
    {
        for( int x=0; x<m_size.x/4; x++ )
        {
            uint64_t d = *src++;
            DecodeDxt1Part( d, dst, m_size.x );
            dst += 4;
        }
        dst += m_size.x*3;
    }

    return ret;
}
