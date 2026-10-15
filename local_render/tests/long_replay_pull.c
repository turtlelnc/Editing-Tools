/* Experimental long-history pull. Production mlt_pull remains unchanged. */
#include <framework/mlt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/resource.h>

typedef struct { int begin, end; } window;
static double now(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9; }
static void bytes(const void *p,size_t n) { if(fwrite(p,1,n,stdout)!=n)exit(7); }
int main(int argc,char **argv) {
 if(argc!=8){fprintf(stderr,"XML PROFILE replay end video_history WINDOWS STATS\n");return 2;}
 int replay=atoi(argv[3]),end=atoi(argv[4]),history=atoi(argv[5]);
 window ws[32];int count=0;FILE *wf=fopen(argv[6],"r");if(!wf)return 2;
 while(count<32 && fscanf(wf,"%d %d",&ws[count].begin,&ws[count].end)==2)count++;
 fclose(wf);if(!count)return 2;
 double start=now(),first=-1;int vp=0,ap=0,emitted=0;
 mlt_factory_init(NULL);mlt_profile profile=mlt_profile_init(argv[2]);
 mlt_producer p=mlt_factory_producer(profile,"xml",argv[1]);
 if(!p || replay<0 || end>mlt_producer_get_length(p))return 3;
 mlt_producer_set_speed(p,1);mlt_producer_seek(p,replay);
 for(int f=replay;f<end;f++) {
  int emit=0,pullvideo=history<0;
  for(int j=0;j<count;j++) {
   if(f>=ws[j].begin && f<ws[j].end)emit=1;
   if(f>=ws[j].begin-history && f<ws[j].end)pullvideo=1;
  }
  mlt_frame frame=NULL;if(mlt_service_get_frame(MLT_PRODUCER_SERVICE(p),&frame,0)||!frame)return 4;
  mlt_properties_set_int(MLT_FRAME_PROPERTIES(frame),"consumer_progressive",1);
  mlt_properties_set_int(MLT_FRAME_PROPERTIES(frame),"consumer_deinterlace",0);
  int pos=mlt_frame_get_position(frame),w=profile->width,h=profile->height;
  uint8_t *image=NULL;mlt_image_format vf=mlt_image_rgba;
  if(pullvideo){if(mlt_frame_get_image(frame,&image,&vf,&w,&h,0)||!image||vf!=mlt_image_rgba)return 5;vp++;}
  void *audio=NULL;mlt_audio_format af=mlt_audio_f32le;int rate=48000,channels=2;
  int samples=mlt_audio_calculate_frame_samples(mlt_profile_fps(profile),rate,f);
  if(mlt_frame_get_audio(frame,&audio,&af,&rate,&channels,&samples)||!audio||af!=mlt_audio_f32le||rate!=48000||channels!=2||pos!=f)return 6;
  ap++;
  if(emit) {
   if(!pullvideo||w!=profile->width||h!=profile->height)return 5;
   if(first<0)first=now()-start;
   uint64_t header[6]={(uint64_t)f,(uint64_t)pos,(uint64_t)mlt_frame_original_position(frame),(uint64_t)mlt_audio_calculate_samples_to_position(mlt_profile_fps(profile),rate,f),(uint64_t)samples,(uint64_t)w*h*4};
   bytes(header,sizeof(header));bytes(image,(size_t)w*h*4);bytes(audio,(size_t)samples*channels*4);emitted++;
  }
  mlt_frame_close(frame);
  if((f+1)%9000==0)fprintf(stderr,"progress frame=%d wall_seconds=%.3f\n",f+1,now()-start);
 }
 fflush(stdout);double total=now()-start;struct rusage usage;getrusage(RUSAGE_SELF,&usage);
#ifdef __APPLE__
 long rss=usage.ru_maxrss;
#else
 long rss=usage.ru_maxrss*1024;
#endif
 FILE *stats=fopen(argv[7],"w");if(!stats)return 8;
 fprintf(stats,"{\"wall_seconds\":%.9f,\"first_output_ready_seconds\":%.9f,\"video_pulls\":%d,\"audio_pulls\":%d,\"emitted_frames\":%d,\"peak_rss_bytes\":%ld,\"user_cpu_seconds\":%.6f,\"system_cpu_seconds\":%.6f}\n",total,first,vp,ap,emitted,rss,usage.ru_utime.tv_sec+usage.ru_utime.tv_usec/1e6,usage.ru_stime.tv_sec+usage.ru_stime.tv_usec/1e6);
 fclose(stats);mlt_producer_close(p);mlt_profile_close(profile);mlt_factory_close();return 0;
}
