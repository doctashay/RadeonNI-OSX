/* Exercise independent UVD handles with the known H.264 fixture. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <IOKit/IOKitLib.h>
#include <CommonCrypto/CommonDigest.h>
#include "rdn_user.h"
#include "rdn_video.h"
#include "uvd_decode_fixture.h"
#include "rdn_probe_device.h"
#if !__LP64__
#error Build this probe for PPC64.
#endif
static void defaults(struct rdn_video_create *create,struct rdn_video_decode *decode)
{
 memset(create,0,sizeof(*create)); memset(decode,0,sizeof(*decode));
 create->version=RDN_VIDEO_API_VERSION; create->bytes=sizeof(*create); create->flags=0;
 create->config.version=RDN_UVD_H264_VERSION; create->config.profile_idc=100; create->config.level_idc=30;
 create->config.coded_width=RDN_UVD_FIXTURE_WIDTH; create->config.coded_height=RDN_UVD_FIXTURE_HEIGHT;
 create->config.max_num_ref_frames=2; create->config.chroma_format_idc=1; create->config.frame_mbs_only_flag=1;
 decode->version=RDN_VIDEO_API_VERSION; decode->bytes=sizeof(*decode); decode->request_sequence=1;
 decode->bitstream_bytes=sizeof(rdn_uvd_fixture_bitstream); decode->picture.version=RDN_UVD_H264_VERSION;
 decode->picture.sps_info_flags=5; decode->picture.pps_info_flags=0x88;
 decode->picture.log2_max_frame_num_minus4=1; decode->picture.log2_max_pic_order_cnt_lsb_minus4=3;
 decode->picture.pic_init_qp_minus26=2;
 memset(decode->picture.scaling_list_4x4,16,sizeof(decode->picture.scaling_list_4x4));
 memset(decode->picture.scaling_list_8x8,16,sizeof(decode->picture.scaling_list_8x8));
}

int main(int argc,char **argv)
{
 io_service_t service=0;io_connect_t connection[11]={0};
 struct rdn_video_session sessions[11];struct rdn_video_create create;
 struct rdn_video_decode decode;struct rdn_video_query query;
 mach_vm_address_t upload[10]={0},pixels[10]={0};mach_vm_size_t length[10]={0},pixelLength[10]={0};
 unsigned i,j,opened=0,created=0;size_t bytes;kern_return_t kr;int result=1;
 memset(sessions,0,sizeof(sessions));setbuf(stdout,0);
 if(argc!=3 || (strcmp(argv[1],"--query")&&strcmp(argv[1],"--run")&&strcmp(argv[1],"--admission"))) {
  fprintf(stderr,"usage: %s --query|--run|--admission pci-device-id\n",argv[0]);return 2;
 }
 service=rdn_probe_device(argv[2]);if(!service)goto done;
 kr=IOServiceOpen(service,mach_task_self(),RDN_VIDEO_UC_TYPE,&connection[0]);if(kr)goto done;opened=1;
 memset(&query,0,sizeof(query));bytes=sizeof(query);
 kr=IOConnectCallStructMethod(connection[0],RDN_VIDEO_QUERY,0,0,&query,&bytes);
 printf("query result=0x%x flags=0x%x reason=%u max_sessions=%u\n",kr,query.flags,query.unavailable_reason,query.max_sessions);
 if(kr||bytes!=sizeof(query))goto done;
 if(!strcmp(argv[1],"--query")){result=0;goto done;}
 if((!strcmp(argv[1],"--run")&&query.max_sessions!=10) || !(query.flags&RDN_VIDEO_CAP_PRODUCTION))goto done;
 defaults(&create,&decode);
 for(i=0;i<10;++i){
  if(i){kr=IOServiceOpen(service,mach_task_self(),RDN_VIDEO_UC_TYPE,&connection[i]);if(kr)goto done;++opened;}
  bytes=sizeof(sessions[i]);kr=IOConnectCallStructMethod(connection[i],RDN_VIDEO_CREATE,&create,sizeof(create),&sessions[i],&bytes);
  printf("create %u result=0x%x token=%08x:%08x\n",i,kr,sessions[i].session.high,sessions[i].session.low);
  if(kr){if(!strcmp(argv[1],"--admission")&&i==query.max_sessions&&kr==kIOReturnBusy)result=0;goto done;}++created;
  if(!strcmp(argv[1],"--admission"))continue;
  kr=IOConnectMapMemory(connection[i],sessions[i].upload_memory_type,mach_task_self(),&upload[i],&length[i],kIOMapAnywhere);
  if(kr||length[i]<sizeof(rdn_uvd_fixture_bitstream))goto done;
  memcpy((void*)(uintptr_t)upload[i],rdn_uvd_fixture_bitstream,sizeof(rdn_uvd_fixture_bitstream));
 }
 kr=IOServiceOpen(service,mach_task_self(),RDN_VIDEO_UC_TYPE,&connection[10]);if(kr)goto done;++opened;
 bytes=sizeof(sessions[10]);kr=IOConnectCallStructMethod(connection[10],RDN_VIDEO_CREATE,&create,sizeof(create),&sessions[10],&bytes);
 printf("eleventh result=0x%x expected=0x%x\n",kr,kIOReturnBusy);if(kr!=kIOReturnBusy)goto done;
 for(j=1;j<=3;++j)for(i=0;i<10;++i){
  struct rdn_video_output output;struct rdn_video_release release;
  unsigned char digest[32];char hex[65];unsigned n;
  decode.session=sessions[i].session;decode.request_sequence=j;memset(&output,0,sizeof(output));bytes=sizeof(output);
  kr=IOConnectCallStructMethod(connection[i],RDN_VIDEO_DECODE,&decode,sizeof(decode),&output,&bytes);
  if(kr){printf("decode %u/%u result=0x%x\n",i,j,kr);goto done;}
  if(output.slot!=0||output.output_bytes!=RDN_UVD_FIXTURE_WIDTH*RDN_UVD_FIXTURE_HEIGHT*3/2)goto done;
  if(!pixels[i]){kr=IOConnectMapMemory(connection[i],sessions[i].output0_memory_type,mach_task_self(),&pixels[i],&pixelLength[i],kIOMapAnywhere);if(kr||pixelLength[i]<output.output_bytes)goto done;}
  CC_SHA256((void*)(uintptr_t)pixels[i],output.output_bytes,digest);
  for(n=0;n<32;++n)sprintf(hex+2*n,"%02x",digest[n]);
  printf("decode %u/%u sha256=%s\n",i,j,hex);
  if(strcmp(hex,"f8128078aebc6e5dcf4f80afecd8ff1f01ce3f82ae6d543bb9dcb828f496250e"))goto done;
  memset(&release,0,sizeof(release));release.version=1;release.bytes=sizeof(release);release.session=sessions[i].session;
  release.output_sequence=output.output_sequence;release.slot=output.slot;bytes=0;
  kr=IOConnectCallStructMethod(connection[i],RDN_VIDEO_RELEASE,&release,sizeof(release),0,&bytes);if(kr)goto done;
 }
 result=0;
done:
 for(i=0;i<created&&i<10;++i){
  struct rdn_video_destroy destroy;
  if(pixels[i])IOConnectUnmapMemory(connection[i],sessions[i].output0_memory_type,mach_task_self(),pixels[i]);
  if(upload[i])IOConnectUnmapMemory(connection[i],sessions[i].upload_memory_type,mach_task_self(),upload[i]);
  memset(&destroy,0,sizeof(destroy));destroy.version=1;destroy.bytes=sizeof(destroy);destroy.session=sessions[i].session;bytes=0;
  kr=IOConnectCallStructMethod(connection[i],RDN_VIDEO_DESTROY,&destroy,sizeof(destroy),0,&bytes);
  printf("destroy %u result=0x%x\n",i,kr);if(kr)result=1;
 }
 for(i=0;i<opened;++i)IOServiceClose(connection[i]);
 if(service)IOObjectRelease(service);
 return result;
}
