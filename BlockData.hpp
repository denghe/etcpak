#ifndef __BLOCKDATA_HPP__
#define __BLOCKDATA_HPP__

#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <vector>

#include "Bitmap.hpp"
#include "Vector.hpp"

class BlockData
{
public:
    enum Type
    {
        Etc1,
        Etc2_RGB,
        Etc2_RGBA,
    };

    BlockData( const char* fn );
    BlockData( const char* fn, const v2i& size, bool mipmap, Type type );
    BlockData( const v2i& size, bool mipmap, Type type );
    ~BlockData();

    BitmapPtr Decode();
    void Dissect();

    void Process( const uint32_t* src, uint32_t blocks, size_t offset, size_t width, Channels type, bool dither );
    void ProcessRGBA( const uint32_t* src, uint32_t blocks, size_t offset, size_t width, bool dither );

private:
    BitmapPtr DecodeRGB();
    BitmapPtr DecodeRGBA();

    uint8_t* m_data;
    v2i m_size;
    size_t m_dataOffset;
    FILE* m_file;
    size_t m_maplen;
    Type m_type;
};

typedef std::shared_ptr<BlockData> BlockDataPtr;

#endif
