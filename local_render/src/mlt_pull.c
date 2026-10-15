/* Sequential, eager MLT pull. Warm-up consumes both video and audio. No encoding. */
#include <framework/mlt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static void write_all(const void *p, size_t n) {
    if (fwrite(p, 1, n, stdout) != n) { perror("write"); exit(6); }
}
int main(int argc, char **argv) {
    if (argc != 6 && argc != 7) { fprintf(stderr,"usage: mlt_pull XML PROFILE replay_begin output_begin output_end\n"); return 2; }
    int replay=atoi(argv[3]), begin=atoi(argv[4]), end=atoi(argv[5]);
    if (replay<0 || begin<replay || end<=begin) return 2;
    mlt_factory_init(NULL);
    mlt_profile profile=mlt_profile_init(argv[2]);
    mlt_producer p=mlt_factory_producer(profile,"xml",argv[1]);
    if (!p || end>mlt_producer_get_length(p)) { fprintf(stderr,"invalid producer or range\n"); return 3; }
    mlt_properties_set_int(MLT_PRODUCER_PROPERTIES(p),"eof",0);
    mlt_producer_set_speed(p,1.0);
    FILE *sequence=argc==7 ? fopen(argv[6],"r") : NULL;
    if (argc==7 && !sequence) return 2;
    do {
    if (sequence && fscanf(sequence,"%d %d %d",&replay,&begin,&end)!=3) break;
    if(replay<0 || replay>begin || begin>=end || end>mlt_producer_get_length(p)) return 2;
    mlt_producer_seek(p,replay);
    for (int f=replay; f<end; ++f) {
        mlt_frame frame=NULL;
        if (mlt_service_get_frame(MLT_PRODUCER_SERVICE(p),&frame,0) || !frame) return 4;
        mlt_properties fp=MLT_FRAME_PROPERTIES(frame);
        mlt_properties_set_int(fp,"consumer_progressive",1);
        mlt_properties_set_int(fp,"consumer_deinterlace",0);
        mlt_position actual=mlt_frame_get_position(frame);
        uint8_t *image=NULL; mlt_image_format vf=mlt_image_rgba;
        int w=profile->width,h=profile->height;
        int vr=mlt_frame_get_image(frame,&image,&vf,&w,&h,0);
        void *audio=NULL; mlt_audio_format af=mlt_audio_f32le;
        int frequency=48000,channels=2;
        int samples=mlt_audio_calculate_frame_samples(mlt_profile_fps(profile),frequency,f);
        int ar=mlt_frame_get_audio(frame,&audio,&af,&frequency,&channels,&samples);
        if (vr || ar || !image || !audio || vf!=mlt_image_rgba || af!=mlt_audio_f32le || w!=profile->width || h!=profile->height || frequency!=48000 || channels!=2 || actual!=f) {
            fprintf(stderr,"frame %d actual=%d vr=%d ar=%d vf=%d af=%d w=%d h=%d rate=%d channels=%d\n",f,actual,vr,ar,vf,af,w,h,frequency,channels); return 5;
        }
        if (f>=begin) {
            uint64_t head[6]={(uint64_t)f,(uint64_t)actual,(uint64_t)mlt_frame_original_position(frame),
                (uint64_t)mlt_audio_calculate_samples_to_position(mlt_profile_fps(profile),frequency,f),
                (uint64_t)samples,(uint64_t)w*h*4};
            write_all(head,sizeof(head));write_all(image,(size_t)w*h*4);write_all(audio,(size_t)samples*channels*4);
        }
        mlt_frame_close(frame);
    }
    } while(sequence);
    if(sequence) fclose(sequence);
    mlt_producer_close(p); mlt_profile_close(profile);mlt_factory_close();
    return fflush(stdout) ? 6 : 0;
}
