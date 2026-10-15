#include <framework/mlt.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main(int argc,char **argv) {
 mlt_factory_init(NULL); mlt_profile profile=mlt_profile_init(argv[2]);
 mlt_producer p=mlt_factory_producer(profile,"xml",argv[1]);
 if(!p) return 2;
 printf("length=%d\n",mlt_producer_get_length(p));
 mlt_producer_seek(p,argc>3?atoi(argv[3]):0);
 mlt_frame frame=NULL; int ret=mlt_service_get_frame(MLT_PRODUCER_SERVICE(p),&frame,0);
 uint8_t *image=NULL; mlt_image_format format=mlt_image_yuv422; int w=640,h=360;
 if(!ret)ret=mlt_frame_get_image(frame,&image,&format,&w,&h,0);
 printf("frame_status=%d width=%d height=%d format=%d\n",ret,w,h,format);
 if(frame)mlt_frame_close(frame);mlt_producer_close(p);mlt_profile_close(profile);mlt_factory_close();return ret;
}
