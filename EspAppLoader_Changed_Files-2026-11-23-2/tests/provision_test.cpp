/*
 * EspAppLoader
 * File: provision_test.cpp
 * Purpose: Exercises provisioning copy guards and injected OTA/read failures.
 * Copyright (c) 2026 Darryl Cornish
 */
// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <cstring>
#include <vector>
#include "../EspAppLoader/AppLoaderProvisionMgr.h"
int verifyResult=0, descriptionResult=0, hashResult=0;
uint32_t imageLength=9001;
esp_partition_t app0{0,0x10000,16384}, app1{0,0x3d0000,16384};
const esp_partition_t *running=&app0, *boot=&app0;
std::vector<uint8_t> src(16384), dst(16384,255);
int beginResult=0, writeResult=0, endResult=0, readFailure=-1;
int beginCalls=0, abortCalls=0, endCalls=0, readCalls=0;
size_t written=0;
bool corrupt=false;
const esp_partition_t *esp_ota_get_running_partition(){return running;}
const esp_partition_t *esp_ota_get_boot_partition(){return boot;}
esp_err_t esp_ota_begin(const esp_partition_t *p,size_t,esp_ota_handle_t *h){
  assert(p==&app1);++beginCalls;written=0;*h=1;return beginResult;
}
esp_err_t esp_ota_write(esp_ota_handle_t,const void *b,size_t n){
  if(writeResult)return writeResult;
  memcpy(dst.data()+written,b,n);written+=n;return 0;
}
esp_err_t esp_ota_end(esp_ota_handle_t){++endCalls;if(corrupt)dst[6000]^=1;return endResult;}
esp_err_t esp_ota_abort(esp_ota_handle_t){++abortCalls;return 0;}
esp_err_t esp_partition_read(const esp_partition_t *p,size_t o,void *b,size_t n){
  if(readCalls++==readFailure)return 99;
  memcpy(b,(p==&app0?src:dst).data()+o,n);return 0;
}
void reset(){beginCalls=abortCalls=endCalls=readCalls=0;written=0;beginResult=writeResult=endResult=0;
 readFailure=-1;corrupt=false;verifyResult=0;imageLength=9001;running=boot=&app0;}
int main(){
  for(size_t i=0;i<src.size();++i)src[i]=static_cast<uint8_t>(i*31);
  esp_apploader::AppLoaderProvisionMgr m;
  assert(m.bootOption(nullptr)==-1 && m.bootOption("")==-1);
  assert(m.bootOption("0")==0 && m.bootOption("1")==1);
  for(const char *bad : {"2","-1","01","1 extra","0 1","yes"})
    assert(m.bootOption(bad)==-1);
  esp_apploader::AppLoaderProvisionIdentity identity = {};
  strcpy(identity.marker,m.identityMarker());
  strcpy(identity.version,"2026-11-23-2");strcpy(identity.build,"2026-11-23-2");
  // Deliberately split the identity record across the 4096-byte read boundary.
  memcpy(src.data()+4080,&identity,sizeof(identity));
  const auto original=src;
  auto copy=[&](){return m.copy(false,&app0,&app1,&app0,identity);};
  reset();assert(copy()==0);assert(written==imageLength);assert(src==original);
  assert(memcmp(src.data(),dst.data(),imageLength)==0);assert(dst[imageLength]==255);
  assert(boot==&app0 && abortCalls==0 && endCalls==1);
  reset();assert(m.copy(true,&app0,&app1,&app0,identity)!=0 && beginCalls==0);
  reset();assert(m.copy(false,nullptr,&app1,&app0,identity)!=0 && beginCalls==0);
  reset();assert(m.copy(false,&app0,&app0,&app0,identity)!=0 && beginCalls==0);
  reset();auto overlap=app1;overlap.address=app0.address+100;
  assert(m.copy(false,&app0,&overlap,&app0,identity)!=0 && beginCalls==0);
  reset();boot=&app1;assert(copy()!=0 && beginCalls==0);
  reset();running=&app1;assert(copy()!=0 && beginCalls==0);
  reset();assert(m.copy(false,&app0,&app1,&app1,identity)!=0 && beginCalls==0);
  reset();app1.type=1;assert(copy()!=0 && beginCalls==0);app1.type=0;
  reset();imageLength=0;assert(copy()!=0 && beginCalls==0);
  reset();imageLength=16385;assert(copy()!=0 && beginCalls==0);
  reset();app1.size=4096;assert(copy()!=0 && beginCalls==0);app1.size=16384;
  reset();verifyResult=10;assert(copy()==10 && beginCalls==0);
  reset();beginResult=11;assert(copy()==11 && abortCalls==0);
  reset();writeResult=12;assert(copy()==12 && abortCalls==1 && endCalls==0);
  reset();readFailure=3;assert(copy()==99 && abortCalls==1 && endCalls==0);
  reset();endResult=13;assert(copy()==13 && abortCalls==0 && endCalls==1);
  reset();readFailure=6;assert(copy()==99 && endCalls==1);
  reset();corrupt=true;assert(copy()==ESP_ERR_INVALID_CRC && boot==&app0);
  reset();assert(copy()==0 && boot==&app0);
  assert(!strcmp(m.sourceIdentity().version,"2026-11-23-2"));
  reset();src[4080]^=1;assert(copy()==ESP_ERR_NOT_FOUND && beginCalls==0);src=original;
  reset();src[4080+64]^=1;assert(copy()==ESP_ERR_NOT_FOUND && beginCalls==0);src=original;
  reset();src[4080+96]^=1;assert(copy()==ESP_ERR_NOT_FOUND && beginCalls==0);src=original;
  reset();imageLength=4090;assert(copy()==ESP_ERR_NOT_FOUND && beginCalls==0);
  reset();readFailure=0;assert(copy()==99 && beginCalls==0);
  reset();assert(copy()==0);assert(src==original);
}
