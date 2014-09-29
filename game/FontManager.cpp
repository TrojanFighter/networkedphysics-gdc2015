#include "FontManager.h"
#include "Font.h"

#ifdef CLIENT

#include "Common.h"
#include "Global.h"
#include "Hash.h"

#include <sys/types.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>

FontManager::FontManager( protocol::Allocator & allocator )
    : m_fonts( allocator )
{
    protocol::hash::reserve( m_fonts, 256 );
    Reload();
}

FontManager::~FontManager()
{
    Unload();
}

void FontManager::Reload()
{
    Unload();
    Load();
}

Font * FontManager::GetFont( const char * name )
{
    const uint64_t key = protocol::hash_string( name );
    
    return protocol::hash::get( m_fonts, key, (Font*)nullptr );
}

void FontManager::Load()
{
    const char * fontDirectory = "data/fonts";
    
    DIR * dir = opendir( fontDirectory );
    
    while ( dirent * entry = readdir( dir ) )
    {
        const char * filename = entry->d_name;

        const int len = strlen( filename );

        if ( len > 5 && 
             filename[len-5] == '.' &&
             filename[len-4] == 'f' &&
             filename[len-3] == 'o' &&
             filename[len-2] == 'n' &&
             filename[len-1] == 't'
           )
        {
            const int MaxPath = 2048;

            char filename_without_extension[MaxPath];
            strcpy( filename_without_extension, entry->d_name );
            filename_without_extension[len-5] = '\0';

            char font_path[MaxPath];
            sprintf( font_path, "%s/%s.font", fontDirectory, filename_without_extension );

            // todo: use allocator
            Font * font = new Font();

            if ( !font->Load( font_path ) )
            {
                delete font;        // todo: use allocator
                continue;
            }

            uint32_t key = protocol::hash_string( filename_without_extension );

            protocol::hash::set( m_fonts, key, font );
        }
    }
    
    closedir( dir );    
}

void FontManager::Unload()
{
    for ( auto itor = protocol::hash::begin( m_fonts ); itor != protocol::hash::end( m_fonts ); ++itor )
    {
        Font * font = itor->value;
        printf( "%.2f: Delete font %p\n", global.timeBase.time, font );
        delete font;
    }
 
    protocol::hash::clear( m_fonts );
}

#endif