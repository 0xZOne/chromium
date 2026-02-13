// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "pdf/loader/url_loader_wrapper.h"

namespace chrome_pdf {

void URLLoaderWrapper::EnablePushMode(OnDataReceivedCallback data_callback,
                                      OnLoadCompleteCallback complete_callback) {
}

bool URLLoaderWrapper::IsPushModeEnabled() const {
  return false;
}

}  // namespace chrome_pdf
