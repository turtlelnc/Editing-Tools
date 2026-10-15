#include <framework/mlt.h>
#include <stdio.h>
#include <stdint.h>
int main(void) {
 int count=0,first=-1,max=0;
 for(int i=0;i<=108000;i++) {
  int64_t actual=mlt_audio_calculate_samples_to_position((float)(30000.0/1001),48000,i);
  int64_t exact=((int64_t)i*48000*1001+15000)/30000;
  int error=(int)(actual-exact);if(error){count++;if(first<0)first=i;if(error>max)max=error;}
 }
 printf("{\"fps\":[30000,1001],\"sample_rate\":48000,\"tested_positions\":108001,\"mismatches\":%d,\"first_mismatch_frame\":%d,\"max_sample_offset\":%d}\n",count,first,max);
}
