/*
 * EspAppLoader
 * File: provision_state_test.cpp
 * Purpose: Tests persisted handoff and fail-closed handling of invalid state.
 * Copyright (c) 2026 Darryl Cornish
 */
// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include "../EspAppLoader/AppLoaderProvisionState.h"
bool preferencesAvailable=true,preferencesWriteOk=true;
std::vector<unsigned char> preferencesBlob;
int main(){
  using State=esp_apploader::AppLoaderProvisionStateT<96>;
  State state;State::Record r;
  assert(state.load(r) && r.mode==State::None);
  r.mode=State::Install;r.size=5000;strcpy(r.filename,"app.bin");r.digest[0]=42;
  assert(state.save(r));State afterRestart;State::Record restored;
  assert(afterRestart.load(restored) && restored.mode==State::Install);
  assert(restored.size==5000 && restored.digest[0]==42 && !strcmp(restored.filename,"app.bin"));
  r.mode=State::Hold;assert(state.save(r));
  assert(afterRestart.load(restored) && restored.mode==State::Hold);
  assert(state.clear() && afterRestart.load(restored) && restored.mode==State::None);
  preferencesWriteOk=false;assert(!state.save(r));preferencesWriteOk=true;
  preferencesAvailable=false;assert(!state.load(restored));preferencesAvailable=true;
  r.version=2;assert(state.save(r) && !state.load(restored));r.version=1;
  r.mode=99;assert(state.save(r) && !state.load(restored));r.mode=State::Install;
  r.size=0;assert(state.save(r) && !state.load(restored));r.size=5000;
  memset(r.filename,'x',sizeof(r.filename));assert(state.save(r) && !state.load(restored));
  preferencesBlob.resize(3);assert(!state.load(restored));
}
