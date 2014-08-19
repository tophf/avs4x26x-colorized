
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#define VERSION_MAJOR  0
#define VERSION_MINOR  9
#define VERSION_BUGFIX 1

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* the AVS interface currently uses __declspec to link function declarations to their definitions in the dll.
   this has a side effect of preventing program execution if the avisynth dll is not found,
   so define __declspec(dllimport) to nothing and work around this */
#undef __declspec
#define __declspec(i)
#undef EXTERN_C

#include "avisynth_c.h"
#include "version.h"

#define DEFAULT_BINARY_PATH "x264_64"

#define PIPE_BUFFER_SIZE (DWORD)0//1048576 // values bigger than 250000 break the application

/* AVS uses a versioned interface to control backwards compatibility */
/* YV12 support is required */
#define AVS_INTERFACE_YV12 2
/* when AVS supports other planar colorspaces, a workaround is required */
#define AVS_INTERFACE_OTHER_PLANAR 5

/* maximum size of the sequence of filters to try on non script files */
#define AVS_MAX_SEQUENCE 5

#define LOAD_AVS_FUNC(name, continue_on_fail) \
{\
    h->func.name = (void*)GetProcAddress( h->library, #name );\
    if( !continue_on_fail && !h->func.name )\
        goto fail;\
}

typedef struct
{
    AVS_Clip *clip;
    AVS_ScriptEnvironment *env;
    HMODULE library;
    /* declare function pointers for the utilized functions to be loaded without __declspec,
       as the avisynth header does not compensate for this type of usage */
    struct
    {
        const char *(__stdcall *avs_clip_get_error)( AVS_Clip *clip );
        AVS_ScriptEnvironment *(__stdcall *avs_create_script_environment)( int version );
        void (__stdcall *avs_delete_script_environment)( AVS_ScriptEnvironment *env );
        AVS_VideoFrame *(__stdcall *avs_get_frame)( AVS_Clip *clip, int n );
        int (__stdcall *avs_get_version)( AVS_Clip *clip );
        const AVS_VideoInfo *(__stdcall *avs_get_video_info)( AVS_Clip *clip );
        int (__stdcall *avs_function_exists)( AVS_ScriptEnvironment *env, const char *name );
        AVS_Value (__stdcall *avs_invoke)( AVS_ScriptEnvironment *env, const char *name,
            AVS_Value args, const char **arg_names );
        void (__stdcall *avs_release_clip)( AVS_Clip *clip );
        void (__stdcall *avs_release_value)( AVS_Value value );
        void (__stdcall *avs_release_video_frame)( AVS_VideoFrame *frame );
        AVS_Clip *(__stdcall *avs_take_clip)( AVS_Value, AVS_ScriptEnvironment *env );
    } func;
} avs_hnd_t;

HANDLE h_console;
CONSOLE_SCREEN_BUFFER_INFO console_default;

void print_colored(WORD color, const char *msg, ...)
{
    va_list args;

    SetConsoleTextAttribute(h_console, color);
    va_start(args, msg);
    vfprintf(stderr, msg, args);
    va_end(args);
    SetConsoleTextAttribute(h_console, console_default.wAttributes);
}

#define CONSOLE_WHITE      (FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define CONSOLE_YELLOW     (FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN)
#define CONSOLE_RED        (FOREGROUND_INTENSITY | FOREGROUND_RED)
#define CONSOLE_CYAN       (FOREGROUND_INTENSITY | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define CONSOLE_DARKCYAN   (FOREGROUND_GREEN | FOREGROUND_BLUE)
#define CONSOLE_DARKGRAY   (FOREGROUND_INTENSITY)

#define print_error(...)   print_colored(CONSOLE_RED, ##__VA_ARGS__)
#define print_warning(...) print_colored(CONSOLE_YELLOW, ##__VA_ARGS__)
#define print_details(...) print_colored(CONSOLE_DARKCYAN, ##__VA_ARGS__)
#define print_info(...)    print_colored(CONSOLE_CYAN, ##__VA_ARGS__)

#define print_trying(filter) print_details("avs4x264 [info]: trying \"%s\"\n", filter);
#define print_indexing()     print_details("avs4x264 [info]: indexing...\n");
#define print_success()      print_details("avs4x264 [info]: succeeded\n" );
#define print_avs_error(res) print_colored(CONSOLE_RED, "avs [error]: %s\n", avs_as_string(res));

/* load the library and functions we require from it */
static int avs_load_library( avs_hnd_t *h )
{
    h->library = LoadLibrary( "avisynth" );
    if( !h->library )
        return -1;
    LOAD_AVS_FUNC( avs_clip_get_error, 0 );
    LOAD_AVS_FUNC( avs_create_script_environment, 0 );
    LOAD_AVS_FUNC( avs_delete_script_environment, 1 );
    LOAD_AVS_FUNC( avs_get_frame, 0 );
    LOAD_AVS_FUNC( avs_get_version, 0 );
    LOAD_AVS_FUNC( avs_get_video_info, 0 );
    LOAD_AVS_FUNC( avs_function_exists, 0 );
    LOAD_AVS_FUNC( avs_invoke, 0 );
    LOAD_AVS_FUNC( avs_release_clip, 0 );
    LOAD_AVS_FUNC( avs_release_value, 0 );
    LOAD_AVS_FUNC( avs_release_video_frame, 0 );
    LOAD_AVS_FUNC( avs_take_clip, 0 );
    return 0;
fail:
    FreeLibrary( h->library );
    return -1;
}

/*
 * @deprecated Use get_avs_version_string instead
 *
static float get_avs_version_number( avs_hnd_t avs_h )
{
    if( !avs_h.func.avs_function_exists( avs_h.env, "VersionNumber" ) )
    {
       print_error("avs [error]: VersionNumber does not exist\n" );
       return -1;
    }
    AVS_Value ver = avs_h.func.avs_invoke( avs_h.env, "VersionNumber", avs_new_value_array( NULL, 0 ), NULL );
    if( avs_is_error( ver ) )
    {
       print_error("avs [error]: Unable to determine avisynth version: %s\n", avs_as_error( ver ) );
       return -1;
    }
    if( !avs_is_float( ver ) )
    {
       print_error("avs [error]: VersionNumber did not return a float value\n" );
       return -1;
    }
    float ret = avs_as_float( ver );
    avs_h.func.avs_release_value( ver );
    return ret;
}
 *
 */

static char const *get_avs_version_string( avs_hnd_t avs_h )
{
    if( !avs_h.func.avs_function_exists( avs_h.env, "VersionString" ) )
    {
       print_error("avs [error]: VersionString does not exist\n" );
       return "AviSynth: unknown version";
    }
    AVS_Value ver = avs_h.func.avs_invoke( avs_h.env, "VersionString", avs_new_value_array( NULL, 0 ), NULL );
    if( avs_is_error( ver ) )
    {
       print_error("avs [error]: Unable to determine avisynth version: %s\n", avs_as_error( ver ) );
       return "AviSynth: unknown version";
    }
    if( !avs_is_string( ver ) )
    {
       print_error("avs [error]: VersionString did not return a string value\n" );
       return "AviSynth: unknown version";
    }
    const char *ret = avs_as_string( ver );
    avs_h.func.avs_release_value( ver );
    return ret;
}

static AVS_Value update_clip( avs_hnd_t avs_h, const AVS_VideoInfo *vi, AVS_Value res, AVS_Value release )
{
    avs_h.func.avs_release_clip( avs_h.clip );
    avs_h.clip = avs_h.func.avs_take_clip( res, avs_h.env );
    avs_h.func.avs_release_value( release );
    vi = avs_h.func.avs_get_video_info( avs_h.clip );
    return res;
}

char* generate_new_commandline(int argc, char *argv[], int b_hbpp_vfw, int i_frame_total, int i_fps_num, int i_fps_den, int i_width, int i_height, char* infile, const char* csp, int b_tc, int i_encode_frames )
{
    int i;
    char *cmd, *buf;
    int b_add_fps      = 1;
    int b_add_csp      = 1;
    int b_add_res      = 1;
    int b_add_timebase = b_tc;
    char *x264_binary;
    x264_binary = DEFAULT_BINARY_PATH;
    buf = malloc(20);
    *buf=0;
    cmd = malloc(8192);
    for (i=1;i<argc;i++)
    {
        if( !strncmp(argv[i], "--x264-binary", 13) || !strncmp(argv[i], "-L", 2) )
        {
            if( !strcmp(argv[i], "--x264-binary") || !strcmp(argv[i], "-L") )
            {
                x264_binary = argv[i+1];
                for (int k=i;k<argc-2;k++)
                    argv[k] = argv[k+2];
                argc -= 2;
            }
            else if( !strncmp(argv[i], "--x264-binary=", 14) )
            {
                x264_binary = argv[i]+14;
                for (int k=i;k<argc-1;k++)
                    argv[k] = argv[k+1];
                argc--;
            }
            else if( !strncmp(argv[i], "-L=", 3) )
            {
                x264_binary = argv[i]+3;
                for (int k=i;k<argc-1;k++)
                    argv[k] = argv[k+1];
                argc--;
            }
            else                           /* else argv[i] should have structure like -Lx264 */
            {
                x264_binary = argv[i]+2;
                for (int k=i;k<argc-1;k++)
                    argv[k] = argv[k+1];
                argc--;
            }
            i--;
        }
    }
    if ( b_tc )
    {
        b_add_fps = 0;
        for (i=1;i<argc;i++)
        {
            if( !strncmp(argv[i], "--timebase", 10) )
            {
                b_add_timebase = 0;
                break;
            }
        }
    }
    else
    {
        for (i=1;i<argc;i++)
        {
            if( !strncmp(argv[i], "--fps", 5) )
            {
                b_add_fps = 0;
                break;
            }
        }
    }
    for (i=1;i<argc;i++)
    {
        if( !strncmp(argv[i], "--input-csp", 11) )
        {
            b_add_csp = 0;
            break;
        }
    }
    for (i=1;i<argc;i++)
    {
        if( !strncmp(argv[i], "--input-res", 11) )
        {
            b_add_res = 0;
            break;
        }
    }
    for (i=argc-1;i>0;i--)
    {
#define FIND_HBPP                                                                  \
{                                                                                  \
  i_width >>= 1;                                                                   \
  print_info("avs4x264 [info]: High bit depth detected, resolution corrected\n" ); \
  break;                                                                           \
}
        if( b_hbpp_vfw == 1 )
            FIND_HBPP
        else if( !strncmp(argv[i], "--input-depth", 13) )
        {
            if( !strcmp(argv[i], "--input-depth") )
            {
                if( strcmp(argv[++i], "8") )
                    FIND_HBPP
            }
            else if( strcmp(argv[i], "--input-depth=8") )
                FIND_HBPP
            break;
        }
#undef FIND_HBPP
    }

    sprintf(cmd, "\"%s\" - ", x264_binary);

    for (i=1;i<argc;i++)
    {
        if ( infile!=argv[i] || !strcmp(argv[i-1], "--audiofile") )
        {
            if (strrchr(argv[i], ' '))
            {
                strcat(cmd, "\"");
                strcat(cmd, argv[i]);
                strcat(cmd, "\"");
            }
            else
                strcat(cmd, argv[i]);
            if(i<argc-1)
                strcat(cmd, " ");
        }
    }

    sprintf(buf, " --frames %d", i_encode_frames);
    strcat(cmd, buf);

    if ( b_add_fps )
    {
        sprintf(buf, " --fps %d/%d", i_fps_num, i_fps_den);
        strcat(cmd, buf);
    }
    if ( b_add_timebase )
    {
        sprintf(buf, " --timebase %d", i_fps_den);
        strcat(cmd, buf);
    }
    if ( b_hbpp_vfw )
    {
        sprintf(buf, " --input-depth 16");
        strcat(cmd, buf);
    }
    if ( b_add_res )
    {
        sprintf(buf, " --input-res %dx%d", i_width, i_height);
        strcat(cmd, buf);
    }
    if ( b_add_csp )
    {
        sprintf(buf, " --input-csp %s", csp);
        strcat(cmd, buf);
    }
    free(buf);
    return cmd;
}

int main(int argc, char *argv[])
{
    //avs related
    avs_hnd_t avs_h;
    AVS_Value arg;
    AVS_Value res;
    char *filter = NULL;
    // float avs_version_number;
    const char *avs_version_string;
    AVS_VideoFrame *frm;
    //createprocess related
    HANDLE h_process, h_stdOut, h_stdErr, h_pipeRead, h_pipeWrite;
    SECURITY_ATTRIBUTES saAttr;
    STARTUPINFO si_info;
    PROCESS_INFORMATION pi_info;
    DWORD exitcode = 0;
    /*Video Info*/
    int i_width;
    int i_height;
    int i_fps_num;
    int i_fps_den;
    int i_frame_start=0;
    int i_frame_total;
    int b_hbpp_vfw=0;
    int b_interlaced=0;
    int b_qp=0;
    int b_tc=0;
    int b_seek_safe=0;
    int b_change_frame_total=0;
    int i_encode_frames;
    /*Video Info End*/
    char *planeY, *planeU, *planeV;
    unsigned int frame,len,chroma_height,chroma_width;
    int i,j;
    char *cmd;
    char *infile = NULL;
    const char *csp = NULL;
    const char *csp_human = NULL;

    h_console = GetStdHandle(STD_ERROR_HANDLE);
    GetConsoleScreenBufferInfo(h_console, &console_default);

    if (argc>1)
    {
        //get the script file and other informations from the commandline

        for (i=1;i<argc;i++)
        {
            if( !strcmp(argv[i], "--interlaced") || !strcmp(argv[i], "--tff") || !strcmp(argv[i], "--bff") )
            {
                print_warning("%s found.\n", argv[i]);
                b_interlaced=1;
                break;
            }
        }
        for (i=1;i<argc;i++)
        {
            if( !strncmp(argv[i], "--qpfile", 8) )
            {
                b_qp = 1;
                break;
            }
        }
        for (i=1;i<argc;i++)
        {
            if( !strncmp(argv[i], "--tcfile-in", 11) )
            {
                b_tc = 1;
                break;
            }
        }
        for (i=1;i<argc;i++)
        {
            if( !strncmp(argv[i], "--seek-mode", 11) )
            {
                if( !strcmp(argv[i], "--seek-mode") )
                {
                    if( !strcasecmp(argv[i+1], "safe" ) )
                    {
                        b_seek_safe = 1;
                    }
                    else if( !strcasecmp(argv[i+1], "fast" ) )
                    {
                        b_seek_safe = 0;
                    }
                    else
                    {
                        print_error("avs4x264 [error]: invalid seek-mode\n" );
                        return -1;
                    }
                    for (int k=i;k<argc-2;k++)
                        argv[k] = argv[k+2];
                    argc -= 2;
                }
                else if( !strncmp(argv[i], "--seek-mode=", 12) )
                {
                    if( !strcasecmp(argv[i]+12, "safe" ) )
                    {
                        b_seek_safe = 1;
                    }
                    else if( !strcasecmp(argv[i]+12, "fast" ) )
                    {
                        b_seek_safe = 0;
                    }
                    else
                    {
                        print_error("avs4x264 [error]: invalid seek-mode\n" );
                        return -1;
                    }
                    for (int k=i;k<argc-1;k++)
                        argv[k] = argv[k+1];
                    argc--;
                }
                i--;
            }
        }

        //avs open
        if( avs_load_library( &avs_h ) )
        {
           print_error("avs [error]: failed to load avisynth\n" );
           return -1;
        }
        avs_h.env = avs_h.func.avs_create_script_environment( AVS_INTERFACE_YV12 );
        if( !avs_h.env )
        {
           print_error("avs [error]: failed to initiate avisynth\n" );
           goto avs_fail;
        }

        #define simple_avs_exists(filter) {                                     \
            if (!avs_h.func.avs_function_exists(avs_h.env, filter)) {           \
                print_error("avs4x264 [error]: \"%s\" not found\n", filter);    \
                goto avs_fail;                                                  \
            }                                                                   \
        }

        #define abort_on_fail 1
        #define print_on_success 2
        #define print_and_break_on_success (2|4)
        #define simple_avs_invoke(filter, action) {                     \
            infile = argv[i];                                           \
            arg = avs_new_value_string( infile );                       \
            res = avs_h.func.avs_invoke(avs_h.env, filter, arg, NULL);  \
            if( avs_is_error( res ) ) {                                 \
                print_avs_error(res);                                   \
                if (action & abort_on_fail)                             \
                    goto avs_fail;                                      \
            }                                                           \
            else {                                                      \
                if (action & print_on_success)                          \
                    print_success();                                    \
                if (action & print_and_break_on_success == print_and_break_on_success) \
                    break;                                              \
            }                                                           \
        }

        for (i=1;i<argc;i++)
        {
            len =  strlen(argv[i]);
            char *ext = strrchr(argv[i], '.');

            if ( !strncmp(argv[i], "--audiofile=", 12) || !strncmp(argv[i], "--output=", 9) )
                continue;
            else if ( !strncmp(argv[i], "-o", 2) && strcmp(argv[i], "-o") )                   // special case: -ofilename.ext equals to --output filename.ext
                continue;
            else if ( !strcmp(argv[i], "--output") || !strcmp(argv[i], "-o") || !strcmp(argv[i], "--audiofile") )
            {
                i++;
                continue;
            }
            else if (ext == NULL || strlen(ext) > 5)
            {
                continue; // do nothing as the extension is either empty or too long
            }
            else if (strcasecmp(ext, ".avs") == 0)
            {
                simple_avs_invoke("Import", 0);
                break;
            }

            else if (strcasecmp(ext, ".d2v") == 0)
            {
                filter = "MPEG2Source";
                print_trying(filter);
                simple_avs_exists(filter);
                simple_avs_invoke(filter, print_and_break_on_success | abort_on_fail);
            }

            else if (strcasecmp(ext, ".dga") == 0)
            {
                filter = "AVCSource";
                print_trying(filter);
                simple_avs_exists(filter);
                simple_avs_invoke(filter, print_and_break_on_success | abort_on_fail);
            }

            else if (strcasecmp(ext, ".dgi") == 0)
            {
                filter = "DGSource";
                print_trying(filter);
                simple_avs_exists(filter);
                simple_avs_invoke(filter, print_and_break_on_success | abort_on_fail);
            }

            else if (strcasecmp(ext, ".vpy") == 0)
            {
                filter = "VSImport";
                if( avs_h.func.avs_function_exists( avs_h.env, filter ) )
                {
                    print_trying(filter);
                    simple_avs_invoke(filter, print_and_break_on_success);
                }

                filter = "AVISource";
                print_trying(filter);
                simple_avs_invoke(filter, print_and_break_on_success);

                // Might be high bpp csp
                filter = "HBVFWSource";
                if( avs_h.func.avs_function_exists( avs_h.env, filter ) )
                {
                    print_trying(filter);
                    simple_avs_invoke(filter, print_on_success | abort_on_fail);
                    b_hbpp_vfw = 1;
                    break;
                }
            }

            else if (strcasecmp(ext, ".avi") == 0)
            {
                filter = "AVISource";
                print_trying(filter);
                simple_avs_invoke(filter, print_and_break_on_success);
                goto source_lwl_ffms_general;
            }

            else if (strcasecmp(ext, ".m2ts") == 0
                  || strcasecmp(ext, ".mpeg") == 0
                  || strcasecmp(ext, ".vob") == 0
                  || strcasecmp(ext, ".mpg") == 0
                  || strcasecmp(ext, ".ogv") == 0
                  || strcasecmp(ext, ".ogm") == 0
                  || strcasecmp(ext, ".ts") == 0
                  || strcasecmp(ext, ".tp") == 0
                  || strcasecmp(ext, ".ps") == 0) /* We don't trust ffms's non-linear seeking for these formats */
            {
                infile = argv[i];

                filter = "LWLibavVideoSource";
                if( avs_h.func.avs_function_exists( avs_h.env, filter ) )
                {
                    print_trying(filter);
                    print_indexing();
                    AVS_Value arg_arr[] = { avs_new_value_string( infile ), avs_new_value_int( 1 ) };
                    const char *arg_name[] = { "source", "threads" };
                    res = avs_h.func.avs_invoke( avs_h.env, filter, avs_new_value_array( arg_arr, 2 ), arg_name );
                    if( !avs_is_error( res ) )
                    {
                        print_success();
                        break;
                    }
                    print_avs_error(res);
                }

                filter = "FFIndex";
                if( avs_h.func.avs_function_exists( avs_h.env, filter ) )
                {
                    print_trying("FFVideoSource");
                    print_indexing();
                    AVS_Value arg_arr[] = { avs_new_value_string( infile ), avs_new_value_string( "lavf" ) };
                    const char *arg_name[] = { "source", "demuxer" };
                    res = avs_h.func.avs_invoke( avs_h.env, filter, avs_new_value_array( arg_arr, 2 ), arg_name );
                    if( avs_is_error( res ) )
                    {
                        print_avs_error(res);
                        goto source_dss;
                    }
                }
                else
                    goto source_dss;

                filter = "FFVideoSource";
                if( avs_h.func.avs_function_exists( avs_h.env, filter ) )
                {
                    AVS_Value arg_arr[] = { avs_new_value_string( infile ), avs_new_value_int( 1 ), avs_new_value_int( -1 ) };
                    const char *arg_name[] = { "source", "threads", "seekmode" };
                    res = avs_h.func.avs_invoke( avs_h.env, filter, avs_new_value_array( arg_arr, 3 ), arg_name );
                    if( avs_is_error( res ) )
                    {
                        print_avs_error(res);
                        goto source_dss;
                    }
                    else
                    {
                        print_success();
                        print_details("avs4x264 [info]: No safe non-linear seeking guaranteed for input file, force seek-mode=safe\n");
                        b_seek_safe = 1;
                    }
                }
                else
                    goto source_dss;
                break;
            }

            else if (strcasecmp(ext, ".mp4") == 0
                  || strcasecmp(ext, ".m4v") == 0
                  || strcasecmp(ext, ".mov") == 0
                  || strcasecmp(ext, ".3gp") == 0
                  || strcasecmp(ext, ".3g2") == 0
                  || strcasecmp(ext, ".qt") == 0) /* LSMASHVideoSource works perfect for them */
            {
                infile = argv[i];
                filter = "LSMASHVideoSource";
                if( avs_h.func.avs_function_exists( avs_h.env, filter ) )
                {
                    print_trying(filter);
                    print_indexing();
                    AVS_Value arg_arr[] = { avs_new_value_string( infile ), avs_new_value_int( 1 ) };
                    const char *arg_name[] = { "source", "threads" };
                    res = avs_h.func.avs_invoke( avs_h.env, filter, avs_new_value_array( arg_arr, 2 ), arg_name );
                    if( avs_is_error( res ) )
                    {
                        print_avs_error(res);
                        goto source_lwl_ffms_general;
                    }
                    else
                    {
                        print_success();
                        break;
                    }
                }
                else
                    goto source_lwl_ffms_general;
            }

            else if (strcasecmp(ext, ".mkv") == 0
                  || strcasecmp(ext, ".flv") == 0
                  || strcasecmp(ext, ".webm") == 0) /* Non-linear seeking seems to be reliable for these formats */
            {
                infile = argv[i];
source_lwl_ffms_general:

                filter = "LWLibavVideoSource";
                if( avs_h.func.avs_function_exists( avs_h.env, filter ) )
                {
                    print_trying(filter);
                    print_indexing();
                    AVS_Value arg_arr[] = { avs_new_value_string( infile ), avs_new_value_int( 1 ) };
                    const char *arg_name[] = { "source", "threads" };
                    res = avs_h.func.avs_invoke( avs_h.env, filter, avs_new_value_array( arg_arr, 2 ), arg_name );
                    if( !avs_is_error( res ) )
                    {
                        print_success();
                        break;
                    }
                    else
                        print_avs_error(res);
                }

                filter = "FFVideoSource";
                if( avs_h.func.avs_function_exists( avs_h.env, filter ) )
                {
                    print_trying(filter);
                    print_indexing();
                    AVS_Value arg_arr[] = { avs_new_value_string( infile ), avs_new_value_int( 1 ) };
                    const char *arg_name[] = { "source", "threads" };
                    res = avs_h.func.avs_invoke( avs_h.env, filter, avs_new_value_array( arg_arr, 2 ), arg_name );
                    if( avs_is_error( res ) )
                    {
                        print_avs_error(res);
                        goto source_dss;
                    }
                    else
                    {
                        print_success();
                        break;
                    }
                }
                    goto source_dss;
            }

            else if (strcasecmp(ext, ".rmvb") == 0
                  || strcasecmp(ext, ".divx") == 0
                  || strcasecmp(ext, ".wmv") == 0
                  || strcasecmp(ext, ".wmp") == 0
                  || strcasecmp(ext, ".asf") == 0
                  || strcasecmp(ext, ".rm") == 0
                  || strcasecmp(ext, ".wm") == 0) /* Only use DSS2/DirectShowSource for these formats */
            {
                infile = argv[i];
source_dss:
                filter = "DSS2";
                if( avs_h.func.avs_function_exists( avs_h.env, filter ) )
                {
                    print_trying(filter);
                    simple_avs_invoke(filter, print_and_break_on_success);
                }

                filter = "DirectShowSource";
                if( avs_h.func.avs_function_exists( avs_h.env, filter ) )
                {
                    print_trying(filter);
                    AVS_Value arg_arr[] = { avs_new_value_string( infile ), avs_new_value_bool( 0 ) };
                    const char *arg_name[] = { NULL, "audio" };
                    res = avs_h.func.avs_invoke( avs_h.env, filter, avs_new_value_array( arg_arr, 2 ), arg_name );
                    if( !avs_is_error( res ) )
                    {
                        print_success();
                        break;
                    }
                    print_avs_error(res);
                }

                goto avs_fail;
                break;
            }

        }

        if (!infile)
        {
            print_error("avs4x264 [error]: No supported input file found.\n");
            goto avs_fail;
        }

        if (filter)
            print_info("avs4x264 [info]: using \"%s\" as source filter\n", filter );

        /* check if the user is using a multi-threaded script and apply distributor if necessary.
           adapted from avisynth's vfw interface */
        AVS_Value mt_test = avs_h.func.avs_invoke( avs_h.env, "GetMTMode", avs_new_value_bool( 0 ), NULL );
        int mt_mode = avs_is_int( mt_test ) ? avs_as_int( mt_test ) : 0;
        avs_h.func.avs_release_value( mt_test );
        if( mt_mode > 0 && mt_mode < 5 )
        {
            AVS_Value temp = avs_h.func.avs_invoke( avs_h.env, "Distributor", res, NULL );
            avs_h.func.avs_release_value( res );
            res = temp;
        }

        if( !avs_is_clip( res ) )
        {
            print_error("avs [error]: `%s' didn't return a video clip\n", infile );
            goto avs_fail;
        }
        avs_h.clip = avs_h.func.avs_take_clip( res, avs_h.env );
        // avs_version_number = get_avs_version_number( avs_h );
        // print_details("avs [info]: AviSynth version: %.2f\n", avs_version_number );
        avs_version_string = get_avs_version_string( avs_h );
        print_colored(CONSOLE_WHITE, "avs [info]: %s\n", avs_version_string );
        const AVS_VideoInfo *vi = avs_h.func.avs_get_video_info( avs_h.clip );
        if( !avs_has_video( vi ) )
        {
            print_error("avs [error]: `%s' has no video data\n", infile );
            goto avs_fail;
        }
        if( vi->width&1 || vi->height&1 )
        {
            print_error("avs [error]: input clip width or height not divisible by 2 (%dx%d)\n",
                     vi->width, vi->height );
            goto avs_fail;
        }
        if ( avs_is_yv12( vi ) )
        {
            csp = "i420";
            chroma_width = vi->width >> 1;
            chroma_height = vi->height >> 1;
            csp_human = "YV12";
        }
        else if ( avs_is_yv24( vi ) )
        {
            csp = "i444";
            chroma_width = vi->width;
            chroma_height = vi->height;
            csp_human = "YV24";
        }
        else if ( avs_is_yv16( vi ) )
        {
            csp = "i422";
            chroma_width = vi->width >> 1;
            chroma_height = vi->height;
            csp_human = "YV16";
        }
        else
        {
            print_warning("avs [warning]: Converting input clip to YV12\n" );
            const char *arg_name[2] = { NULL, "interlaced" };
            AVS_Value arg_arr[2] = { res, avs_new_value_bool( b_interlaced ) };
            AVS_Value res2 = avs_h.func.avs_invoke( avs_h.env, "ConvertToYV12", avs_new_value_array( arg_arr, 2 ), arg_name );
            if( avs_is_error( res2 ) )
            {
                print_error("avs [error]: Couldn't convert input clip to YV12\n" );
                goto avs_fail;
            }
            res = update_clip( avs_h, vi, res2, res );
            csp = "i420";
            csp_human = "YV12";
            chroma_width = vi->width >> 1;
            chroma_height = vi->height >> 1;
        }

        for (i=1;i<argc;i++)
        {
            if( !strncmp(argv[i], "--seek", 6) )
            {
                if( !strcmp(argv[i], "--seek") )
                {
                    i_frame_start = atoi(argv[i+1]);
                    if( !b_tc && !b_qp && !b_seek_safe )   /* delete seek parameters if no timecodes/qpfile and seek-mode=fast */
                    {
                        for (int k=i;k<argc-2;k++)
                            argv[k] = argv[k+2];
                        argc -= 2;
                        i--;
                    }
                }
                else
                {
                    i_frame_start = atoi(argv[i]+7);
                    if( !b_tc && !b_qp && !b_seek_safe )   /* delete seek parameters if no timecodes/qpfile and seek-mode=fast */
                    {
                        for (int k=i;k<argc-1;k++)
                            argv[k] = argv[k+1];
                        argc -= 1;
                        i--;
                    }
                }
            }
        }
        if ( !b_seek_safe && i_frame_start && ( b_qp || b_tc ) )
        {
            print_info("avs4x264 [info]: seek-mode=fast with qpfile or timecodes in, freeze first %d %s for fast processing\n", i_frame_start, i_frame_start==1 ? "frame" : "frames" );
            AVS_Value arg_arr[4] = { res, avs_new_value_int( 0 ), avs_new_value_int( i_frame_start ), avs_new_value_int( i_frame_start ) };
            AVS_Value res2 = avs_h.func.avs_invoke( avs_h.env, "FreezeFrame", avs_new_value_array( arg_arr, 4 ), NULL );
            if( avs_is_error( res2 ) )
            {
                print_error("avs [error]: Couldn't freeze first %d %s\n", i_frame_start, i_frame_start==1 ? "frame" : "frames" );
                goto avs_fail;
            }
            res = update_clip( avs_h, vi, res2, res );
        }

        avs_h.func.avs_release_value( res );

        i_width = vi->width;
        i_height = vi->height;
        i_fps_num = vi->fps_numerator;
        i_fps_den = vi->fps_denominator;
        i_frame_total = vi->num_frames;

        if( i_fps_den != 1 )
        {
            double f_fps = (double)i_fps_num / i_fps_den;
            int i_nearest_NTSC_num = (int)(f_fps * 1.001 + 0.5);
            const double f_epsilon = 0.01;

            if( fabs(f_fps - i_nearest_NTSC_num / 1.001) < f_epsilon )
            {
                i_fps_num = i_nearest_NTSC_num * 1000;
                i_fps_den = 1001;
            }
        }

        print_colored(CONSOLE_YELLOW, "avs [info]: Video: %dx%d, %s, %d/%d fps, %d frames\n",
                 i_width, i_height, csp_human, i_fps_num, i_fps_den, i_frame_total);

        //execute the commandline
        h_process = GetCurrentProcess();
        h_stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
        h_stdErr = GetStdHandle(STD_ERROR_HANDLE);

        if (h_stdOut==INVALID_HANDLE_VALUE || h_stdErr==INVALID_HANDLE_VALUE)
        {
            print_error("Error: Couldn\'t get standard handles!");
            goto avs_fail;
        }

        saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
        saAttr.bInheritHandle = TRUE;
        saAttr.lpSecurityDescriptor = NULL;

        if (!CreatePipe(&h_pipeRead, &h_pipeWrite, &saAttr, PIPE_BUFFER_SIZE))
        {
            print_error("Error: Pipe creation failed!");
            goto avs_fail;
        }

        if ( !SetHandleInformation(h_pipeWrite, HANDLE_FLAG_INHERIT, 0) )
        {
            print_error("Error: SetHandleInformation");
            goto pipe_fail;
        }

        ZeroMemory( &pi_info, sizeof(PROCESS_INFORMATION) );
        ZeroMemory( &si_info, sizeof(STARTUPINFO) );
        si_info.cb = sizeof(STARTUPINFO);
        si_info.dwFlags = STARTF_USESTDHANDLES;
        si_info.hStdInput = h_pipeRead;
        si_info.hStdOutput = h_stdOut;
        si_info.hStdError = h_stdErr;

        for (i=1;i<argc;i++)
        {
            if( !strncmp(argv[i], "--frames", 8) )
            {
                if( !strcmp(argv[i], "--frames") )
                {
                    i_frame_total = atoi(argv[i+1]);
                    for (int k=i;k<argc-2;k++)
                        argv[k] = argv[k+2];
                    argc -= 2;
                }
                else
                {
                    i_frame_total = atoi(argv[i]+9);
                    for (int k=i;k<argc-1;k++)
                        argv[k] = argv[k+1];
                    argc -= 1;
                }
                i--;
                b_change_frame_total = 1;
            }
        }
        if ( b_change_frame_total )
            i_frame_total += i_frame_start; /* ending frame should add offset of i_frame_start, not needed if not set as will be clamped */

        if ( vi->num_frames < i_frame_total )
        {
            print_warning("avs4x264 [warning]: x264 is trying to encode until frame %d, but input clip has only %d %s\n",
                     i_frame_total, vi->num_frames, vi->num_frames > 1 ? "frames" : "frame" );
            i_frame_total = vi->num_frames;
        }

        i_encode_frames = i_frame_total - i_frame_start;

        if ( b_tc || b_qp || b_seek_safe )      /* don't skip the number --seek defines if has timecodes/qpfile or seek-mode=safe */
        {
            i_frame_start = 0;
        }
        else if ( i_frame_start != 0 )
        {
            print_details("avs4x264 [info]: Convert \"--seek %d\" to internal frame skipping\n", i_frame_start );
        }

        cmd = generate_new_commandline(argc, argv, b_hbpp_vfw, i_frame_total, i_fps_num, i_fps_den, i_width, i_height, infile, csp, b_tc, i_encode_frames );
        print_colored(CONSOLE_DARKGRAY, "avs4x264 [info]: %s\n", cmd);

        if (!CreateProcess(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si_info, &pi_info))
        {
            print_error("Error: Failed to create process <%d>!", (int)GetLastError());
            free(cmd);
            goto pipe_fail;
        }
        //cleanup before writing to pipe
        CloseHandle(h_pipeRead);
        free(cmd);

        //write
        for ( frame=i_frame_start; frame<i_frame_total; frame++ )
        {
            frm = avs_h.func.avs_get_frame( avs_h.clip, frame );
            const char *err = avs_h.func.avs_clip_get_error( avs_h.clip );
            if( err )
            {
                print_error("\navs [error]: %s occurred while reading frame %d\n", err, frame );
                goto process_fail;
            }
            planeY = (char*)(frm->vfb->data + frm->offset);
            for (j=0; j<i_height; j++){
               if( !WriteFile(h_pipeWrite, planeY, i_width, (PDWORD)&i, NULL) )
               {
                   print_error("\navs [error]: Error occurred while writing frame %d\n(Maybe x264 closed)\n", frame );
                   goto process_fail;
               }
               planeY += frm->pitch;
            }
            planeU = (char*)(frm->vfb->data + frm->offsetU);
            for (j=0; j<chroma_height; j++){
               if( !WriteFile(h_pipeWrite, planeU, chroma_width, (PDWORD)&i, NULL) )
               {
                   print_error("\navs [error]: Error occurred while writing frame %d\n(Maybe x264 closed)\n", frame );
                   goto process_fail;
               }
               planeU += frm->pitchUV;
            }
            planeV = (char*)(frm->vfb->data + frm->offsetV);
            for (j=0; j<chroma_height; j++){
               if( !WriteFile(h_pipeWrite, planeV, chroma_width, (PDWORD)&i, NULL) )
               {
                   print_error("\navs [error]: Error occurred while writing frame %d\n(Maybe x264 closed)\n", frame );
                   goto process_fail;
               }
               planeV += frm->pitchUV;
            }
            avs_h.func.avs_release_video_frame( frm );
        }
        //close & cleanup
    process_fail:// everything created
        CloseHandle(h_pipeWrite);// h_pipeRead already closed
        WaitForSingleObject(pi_info.hProcess, INFINITE);
        GetExitCodeProcess(pi_info.hProcess,&exitcode);
        CloseHandle(pi_info.hProcess);
        goto avs_cleanup;// pipes already closed
    pipe_fail://pipe created but failed after that
        CloseHandle(h_pipeRead);
        CloseHandle(h_pipeWrite);
    avs_fail://avs enviormnet created but failed after that
        exitcode = -1;
    avs_cleanup:
        avs_h.func.avs_release_clip( avs_h.clip );
        if( avs_h.func.avs_delete_script_environment )
            avs_h.func.avs_delete_script_environment( avs_h.env );
        FreeLibrary( avs_h.library );
    }
    else
    {
        printf("\n"
               "avs4x264mod - simple AviSynth pipe tool for x264\n"
               "Version: %d.%d.%d.%d, built on %s, %s\n\n", VERSION_MAJOR, VERSION_MINOR, VERSION_BUGFIX, VERSION_GIT, __DATE__, __TIME__);
        printf("Usage: avs4x264mod [avs4x264mod options] [x264 options] <input>\n"
               "\n"
               "Supported input formats:\n"
               "     .avs\n"
               "     .d2v: requires DGDecode.dll\n"
               "     .dga: requires DGAVCDecode.dll\n"
               "     .dgi: requires DGAVCDecodeDI.dll or DGDecodeNV.dll according to dgi file\n"
               "     .vpy: try to use VSImport -> AVISource -> HBVFWSource\n"
               "           (HBVFWSource requires HBVFWSource.dll, and will force input-depth=16)\n"
               "     .avi: try to use AVISource -> LWLibavVideoSource -> FFVideoSource(normal)\n"
               "                      -> DSS2 -> DirectShowSource\n"
               "     .mp4/.m4v/.mov/.3gp/.3g2/.qt:\n"
               "           try to use LSMASHVideoSource -> LWLibavVideoSource\n"
               "                      -> FFVideoSource(normal) -> DSS2 -> DirectShowSource\n"
               "     .m2ts/.mpeg/.vob/.m2v/.mpg/.ogm/.ogv/.ts/.tp/.ps:\n"
               "           try to use LWLibavVideoSource\n"
               "                      -> FFVideoSource(demuxer=\"lavf\" and seekmode=-1)\n"
               "                      -> DSS2 -> DirectShowSource\n"
               "           seek-mode will be forced to \"safe\" for these formats if ffms is used\n"
               "     .mkv/.flv/.webm:\n"
               "           try to use LWLibavVideoSource -> FFVideoSource(normal) -> DSS2\n"
               "                      -> DirectShowSource\n"
               "     .rmvb/.divx/.wmv/.wmp/.asf/.rm/.wm:\n"
               "           try to use DSS2 -> DirectShowSource\n"
               "\n");
        printf("Options:\n"
               " -L, --x264-binary <file>   User defined x264 binary path. [Default=\"%s\"]\n", DEFAULT_BINARY_PATH);
        printf("     --seek-mode <string>   Set seek mode when using --seek. [Default=\"fast\"]\n"
               "                                - fast: Skip process of frames before seek number as x264 does if no\n"
               "                                        --tcfile-in/--qpfile specified;\n"
               "                                        otherwise freeze frames before seek number to skip process, \n"
               "                                        but keep frame number as-is.\n"
               "                                        ( x264 treats tcfile-in/qpfile as timecodes/qpfile of input \n"
               "                                        video, not output video )\n"
               "                                        Normally safe enough for randomly seekable AviSynth scripts.\n"
               "                                        May break scripts which can only be linearly seeked, such as\n"
               "                                        TDecimate(mode=3)\n"
               "                                - safe: Process and deliver every frame to x264.\n"
               "                                        Should give accurate result with every AviSynth script.\n"
               "                                        Significantly slower when the process is heavy.\n");
        return -1;
    }
    CloseHandle(h_console);
    return exitcode;
}
