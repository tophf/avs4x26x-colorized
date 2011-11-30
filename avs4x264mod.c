
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

// The code is based on on AVS-input
// from x264 (http://www.videolan.org/developers/x264.html).

// You can use this software to encode videos using the 32-bit version
// of Avisynth with the 64-bit version of x264 under Windows.
// The x264 executable needs to be named x264_64.exe and placed in the
// same folder as this program.

// Modified by 06_taro ( astrataro@gmail.com ).
// Modifications: 
// -- When x264's parameter "input-depth" set and is not equal to 8, 
//    divide "width" by 2. This make faked 16-bit avs output, i.e.,
//    MSB and LSB field interleaved clip, be treated correctly by x264.
//    If "input-depth" not defined or equals to 8, avs4x264mod acts in
//    the same way as original avs4x264.

//Compiling: gcc avs4x264.c -s -O2 -oavs4x264

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <string.h>
#include <ctype.h>

/* the AVS interface currently uses __declspec to link function declarations to their definitions in the dll.
   this has a side effect of preventing program execution if the avisynth dll is not found,
   so define __declspec(dllimport) to nothing and work around this */
#undef __declspec
#define __declspec(i)
#undef EXTERN_C

#include "avisynth_c.h"

#define X264_EXECUTABLE_NAME "x264_64"
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

char* generate_new_commadline(int argc, char *argv[], int i_frame_total, int i_fps_num, int i_fps_den, int i_width, int i_height, char* infile )
{
    int i;
    char *cmd, *buf;
    int b_tc = 0;
    int len = (unsigned int)strrchr(argv[0], '\\');
    if (len)
    {
        len -=(unsigned int)argv[0];
        buf = malloc(len+2<20 ? 20: len+2);
        strncpy(buf, argv[0], len);
        buf[len] = '\\';
        buf[len+1] = 0;
    }
    else
    {
        buf = malloc(20);
        *buf=0;
    }
    cmd = malloc(8192);
    for (i=1;i<argc;i++)
    {
        if( !strcmp(argv[i], "--input-depth") )
        {
            if( strcmp(argv[++i], "8") )
            {
                i_width /= 2;
                break;
            }
        }
    }
    for (i=1;i<argc;i++)
    {
        if( !strcmp(argv[i], "--tcfile-in") )
        {
            b_tc = 1;
            break;
        }
    }
    if( b_tc )
    {
        sprintf(cmd, "\"%s" X264_EXECUTABLE_NAME "\" --frames %d ", buf, i_frame_total);
    }
    else
    {
        sprintf(cmd, "\"%s" X264_EXECUTABLE_NAME "\" --frames %d --fps %d/%d ", buf, i_frame_total, i_fps_num, i_fps_den);
    }
    for (i=1;i<argc;i++)
    {
        if (infile!=argv[i])
        {
            if (strrchr(argv[i], ' '))
            {
                strcat(cmd, "\"");
                strcat(cmd, argv[i]);
                strcat(cmd, "\" ");
            }
            else
            {
                strcat(cmd, argv[i]);
                strcat(cmd, " ");
            }
        }
    }
    sprintf(buf, "- --input-res %dx%d", i_width, i_height);
    strcat(cmd, buf);
    free(buf);
    return cmd;
}

int main(int argc, char *argv[])
{
    //avs related
    avs_hnd_t avs_h;
    AVS_Value arg;
    AVS_Value res;
    int avs_version;
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
    int i_frame_total;
    int b_interlaced=0;
    /*Video Info End*/
    char *planeY, *planeU, *planeV;
    unsigned int frame,len,h_half,w_half;
    int i,j;
    char *cmd;
    char *infile = NULL;
    if (argc>1)
    {
        //get the script file and other informations from the commandline
        for (i=1;i<argc;i++)
        {
            len =  strlen(argv[i]);
            if (len>4 && (argv[i][len-4])== '.' && tolower(argv[i][len-3])== 'a' && tolower(argv[i][len-2])== 'v' && tolower(argv[i][len-1])== 's')
            {
                infile=argv[i];
                break;
            }
            else if((len==12)&&(strcmp(argv[i], "--interlaced")==0))
            {
                fprintf( stderr, "--interlaced found.\n");
                b_interlaced=1;
            }
        }
        if (!infile)
        {
            fprintf( stderr, "No avs script found.\n");
            return -1;
        }

        //avs open
        if( avs_load_library( &avs_h ) )
        {
           fprintf( stderr, "avs [error]: failed to load avisynth\n" );
           return -1;
        }
        avs_h.env = avs_h.func.avs_create_script_environment( AVS_INTERFACE_YV12 );
        if( !avs_h.env )
        {
           fprintf( stderr, "avs [error]: failed to initiate avisynth\n" );
           goto avs_fail;
        }
        arg = avs_new_value_string( infile );

        res = avs_h.func.avs_invoke( avs_h.env, "Import", arg, NULL );
        if( avs_is_error( res ) )
        {
            fprintf( stderr, "avs [error]: %s\n", avs_as_string( res ) );
            goto avs_fail;
        }
        
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
            fprintf( stderr, "avs [error]: `%s' didn't return a video clip\n", infile );
            goto avs_fail;
        }
        avs_h.clip = avs_h.func.avs_take_clip( res, avs_h.env );
        avs_version = avs_h.func.avs_get_version( avs_h.clip );
        const AVS_VideoInfo *vi = avs_h.func.avs_get_video_info( avs_h.clip );
        if( !avs_has_video( vi ) )
        {
            fprintf( stderr, "avs [error]: `%s' has no video data\n", infile );
            goto avs_fail;
        }
        if( vi->width&1 || vi->height&1 )
        {
            fprintf( stderr, "avs [error]: input clip width or height not divisible by 2 (%dx%d)\n",
                     vi->width, vi->height );
            goto avs_fail;
        }
        /* always call ConvertToYV12 to convert non YV12 planar colorspaces to YV12 when user's AVS supports them,
           as all planar colorspaces are flagged as YV12. If it is already YV12 in this case, the call does nothing */
        if( !avs_is_yv12( vi ) || avs_version >= AVS_INTERFACE_OTHER_PLANAR )
        {
            avs_h.func.avs_release_clip( avs_h.clip );
            fprintf( stderr, "avs %s\n", !avs_is_yv12( vi ) ? "[warning]: converting input clip to YV12"
               : "[info]: Avisynth 2.6+ detected, forcing conversion to YV12" );
            const char *arg_name[2] = { NULL, "interlaced" };
            AVS_Value arg_arr[2] = { res, avs_new_value_bool( b_interlaced ) };
            AVS_Value res2 = avs_h.func.avs_invoke( avs_h.env, "ConvertToYV12", avs_new_value_array( arg_arr, 2 ), arg_name );
            if( avs_is_error( res2 ) )
            {
                fprintf( stderr, "avs [error]: Couldn't convert input clip to YV12\n" );
                goto avs_fail;
            }
            avs_h.clip = avs_h.func.avs_take_clip( res2, avs_h.env );
            avs_h.func.avs_release_value( res2 );
            vi = avs_h.func.avs_get_video_info( avs_h.clip );
        }
        avs_h.func.avs_release_value( res );

        i_width = vi->width;
        i_height = vi->height;
        i_fps_num = vi->fps_numerator;
        i_fps_den = vi->fps_denominator;
        i_frame_total = vi->num_frames;
        
        //execute the commandline
        h_process = GetCurrentProcess();
        h_stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
        h_stdErr = GetStdHandle(STD_ERROR_HANDLE);

        if (h_stdOut==INVALID_HANDLE_VALUE || h_stdErr==INVALID_HANDLE_VALUE)
        {
            fprintf( stderr, "Error: Couldn\'t get standard handles!");
            goto avs_fail;
        }

        saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
        saAttr.bInheritHandle = TRUE;
        saAttr.lpSecurityDescriptor = NULL;

        if (!CreatePipe(&h_pipeRead, &h_pipeWrite, &saAttr, PIPE_BUFFER_SIZE))
        {
            fprintf( stderr, "Error: Pipe creation failed!");
            goto avs_fail;
        }

        if ( !SetHandleInformation(h_pipeWrite, HANDLE_FLAG_INHERIT, 0) )
        {
            fprintf( stderr, "Error: SetHandleInformation");
            goto pipe_fail;
        }

        ZeroMemory( &pi_info, sizeof(PROCESS_INFORMATION) );
        ZeroMemory( &si_info, sizeof(STARTUPINFO) );
        si_info.cb = sizeof(STARTUPINFO);
        si_info.dwFlags = STARTF_USESTDHANDLES;
        si_info.hStdInput = h_pipeRead;
        si_info.hStdOutput = h_stdOut;
        si_info.hStdError = h_stdErr;

        cmd = generate_new_commadline(argc, argv, i_frame_total, i_fps_num, i_fps_den, i_width, i_height, infile );

        if (!CreateProcess(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si_info, &pi_info))
        {
            fprintf( stderr, "Error: Failed to create process <%d>!", (int)GetLastError());
            free(cmd);
            goto pipe_fail;
        }
        //cleanup before writing to pipe
        CloseHandle(h_pipeRead);
        free(cmd);

        //prepare for writing
        w_half = i_width >> 1;
        h_half = i_height >> 1;

        //write
        for (frame=0; frame<i_frame_total; frame++)
        {
            frm = avs_h.func.avs_get_frame( avs_h.clip, frame );
            const char *err = avs_h.func.avs_clip_get_error( avs_h.clip );
            if( err )
            {
                fprintf( stderr, "avs [error]: %s occurred while reading frame %d\n", err, frame );
                goto process_fail;
            }            
            planeY = (char*)(frm->vfb->data + frm->offset);
            for (j=0; j<i_height; j++){
               if( !WriteFile(h_pipeWrite, planeY, i_width, (PDWORD)&i, NULL) )
               {
                   fprintf( stderr, "avs [error]: Error occurred while writing frame %d\n(Maybe x264_64.exe closed)\n", frame );
                   goto process_fail;
               }
               planeY += frm->pitch;
            }
            planeU = (char*)(frm->vfb->data + frm->offsetU);
            for (j=0; j<h_half; j++){
               if( !WriteFile(h_pipeWrite, planeU, w_half, (PDWORD)&i, NULL) )
               {
                   fprintf( stderr, "avs [error]: Error occurred while writing frame %d\n(Maybe x264_64.exe closed)\n", frame );
                   goto process_fail;
               }
               planeU += frm->pitchUV;
            } 
            planeV = (char*)(frm->vfb->data + frm->offsetV);
            for (j=0; j<h_half; j++){
               if( !WriteFile(h_pipeWrite, planeV, w_half, (PDWORD)&i, NULL) )
               {
                   fprintf( stderr, "avs [error]: Error occurred while writing frame %d\n(Maybe x264_64.exe closed)\n", frame );
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
    return exitcode;
}