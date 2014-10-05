#include "game/Util.h"
#include "core/Core.h"
#include <stdio.h>
#include <stdlib.h>

bool WriteTGA( const char filename[], int width, int height, uint8_t * ptr )
{
    FILE * file = fopen( filename, "wb" );
    if ( !file )
        return false;

    putc( 0, file );
    putc( 0, file );
    putc( 10, file );                        /* compressed RGB */
    putc( 0, file ); putc( 0, file );
    putc( 0, file ); putc( 0, file );
    putc( 0, file );
    putc( 0, file ); putc( 0, file );           /* X origin */
    putc( 0, file ); putc( 0, file );           /* y origin */
    putc( ( width & 0x00FF ),file );
    putc( ( width & 0xFF00 ) >> 8,file );
    putc( ( height & 0x00FF ), file );
    putc( ( height & 0xFF00 ) >> 8, file );
    putc( 24, file );                         /* 24 bit bitmap */
    putc( 0, file );

    for ( int y = 0; y < height; ++y )
    {
        uint8_t * line = ptr + width * 3 * y;
        uint8_t * end_of_line = line + width * 3;
        uint8_t * pixel = line;
        while ( true )
        {
            if ( pixel >= end_of_line )
                break;

            uint8_t * start = pixel;
            uint8_t * finish = pixel + 128 * 3;
            if ( finish > end_of_line )
                finish = end_of_line;
            uint32_t previous = ( pixel[0] << 16 ) | ( pixel[1] << 8 ) | pixel[2];
            pixel += 3;
            int counter = 1;

            // RLE packet
            while ( pixel < finish )
            {
                CORE_ASSERT( pixel < end_of_line );
                uint32_t current = ( pixel[0] << 16 ) | ( pixel[1] << 8 ) | pixel[2];
                if ( current != previous )
                    break;
                previous = current;
                pixel += 3;
                counter++;
            }
            if ( counter > 1 )
            {
                CORE_ASSERT( counter <= 128 );
                putc( uint8_t( counter - 1 ) | 128, file );
                putc( start[0], file );
                putc( start[1], file );
                putc( start[2], file );
                continue;
            }

            // RAW packet
            while ( pixel < finish )
            {
                CORE_ASSERT( pixel < end_of_line );
                uint32_t current = ( pixel[0] << 16 ) | ( pixel[1] << 8 ) | pixel[2];
                if ( current == previous )
                    break;
                previous = current;
                pixel += 3;
                counter++;
            }
            CORE_ASSERT( counter >= 1 );
            CORE_ASSERT( counter <= 128 );
            putc( uint8_t( counter - 1 ), file );
            fwrite( start, counter * 3, 1, file );
        }
    }

    fclose( file );

    return true;
}
