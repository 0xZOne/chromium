// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PDF_LOADER_URL_LOADER_WRAPPER_H_
#define PDF_LOADER_URL_LOADER_WRAPPER_H_

#include <stdint.h>

#include <string>

#include "base/containers/span.h"
#include "base/functional/callback_forward.h"

namespace chrome_pdf {

class URLLoaderWrapper {
 public:
  virtual ~URLLoaderWrapper() {}

  // Returns length of content, will be -1, if it is unknown.
  virtual int GetContentLength() const = 0;

  // Returns if the response headers contains "accept-ranges".
  virtual bool IsAcceptRangesBytes() const = 0;

  // Returns if the content encoded in response.
  virtual bool IsContentEncoded() const = 0;

  // Returns response content type.
  virtual std::string GetContentType() const = 0;

  // Returns response content disposition.
  virtual std::string GetContentDisposition() const = 0;

  // Returns response status code.
  virtual int GetStatusCode() const = 0;

  // Returns if the response contains multi parts.
  virtual bool IsMultipart() const = 0;

  // If true, `start` contains the start of the byte range.
  // If false, response contains full document and `start` will be undefined.
  virtual bool GetByteRangeStart(int* start) const = 0;

  // Close connection.
  virtual void Close() = 0;

  // Open new connection and send http range request.
  virtual void OpenRange(
      const std::string& url,
      const std::string& referrer_url,
      uint32_t position,
      uint32_t size,
      base::OnceCallback<void(bool /*success*/)> callback) = 0;

  // Read the response body. The size of the buffer must be large enough to
  // hold the specified number of bytes to read.
  // This function might perform a partial read.
  virtual void ReadResponseBody(base::span<uint8_t> buffer,
                                base::OnceCallback<void(int)> callback) = 0;

  // Solution 2: Direct push mode at UrlLoader level.
  // Push mode interface where data is pushed directly from UrlLoader without
  // intermediate buffering. This bypasses the internal buffer completely.
  using OnDataCallback =
      base::RepeatingCallback<void(base::span<const char> data)>;
  using OnCompleteCallback = base::OnceCallback<void(int result)>;

  // Set push mode callbacks on the underlying UrlLoader. Must be called
  // BEFORE OpenRange() is called. When push mode is active, data is pushed
  // directly from UrlLoader::DidReceiveData() to the callback, completely
  // bypassing the internal buffer and the 2ms read delay.
  // After calling this, ReadResponseBody() should not be used.
  virtual void SetPushModeCallbacks(OnDataCallback on_data,
                                    OnCompleteCallback on_complete) = 0;

  // Returns true if push mode callbacks have been set.
  virtual bool IsPushModeEnabled() const = 0;
};

}  // namespace chrome_pdf

#endif  // PDF_LOADER_URL_LOADER_WRAPPER_H_
