/*
 * EspAppLoader
 * File: Preferences.h
 * Purpose: Supplies an in-memory NVS blob for provisioning state tests.
 * Copyright (c) 2026 Darryl Cornish
 */
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstddef>
#include <cstring>
#include <vector>
extern bool preferencesAvailable, preferencesWriteOk;
extern std::vector<unsigned char> preferencesBlob;
class Preferences {
public:
  bool begin(const char *, bool){return preferencesAvailable;}
  void end(){}
  bool isKey(const char *){return !preferencesBlob.empty();}
  size_t getBytesLength(const char *){return preferencesBlob.size();}
  size_t getBytes(const char *,void *out,size_t n){
    if(n!=preferencesBlob.size())return 0;
    memcpy(out,preferencesBlob.data(),n);return n;
  }
  size_t putBytes(const char *,const void *data,size_t n){
    if(!preferencesWriteOk)return 0;
    auto b=static_cast<const unsigned char *>(data);
    preferencesBlob.assign(b,b+n);return n;
  }
};
