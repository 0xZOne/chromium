// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PDF_LOADER_URL_LOADER_WRAPPER_H_
#define PDF_LOADER_URL_LOADER_WRAPPER_H_

#include <stdint.h>

#include <string>

#include "base/containers/span.h"
#include "base/functional/callback.h"

namespace chrome_pdf {

class URLLoaderWrapper {
 public:
  // Callback for receiving data in push mode.
  using OnDataReceivedCallback =
      base::RepeatingCallback<void(base::span<const uint8_t>)>;
  // Callback for completion in push mode. The result is 0 for success,
  // negative for errors.
  using OnLoadCompleteCallback = base::OnceCallback<void(int result)>;

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

  // Push mode interface: Set callbacks for receiving data in push mode.
  // When push mode is enabled, data is pushed directly to the caller via
  // the data_callback, eliminating the need for ReadResponseBody polling.
  // `data_callback` is called for each chunk of data received.
  // `complete_callback` is called when loading completes (successfully or
  // with an error).
  virtual void EnablePushMode(OnDataReceivedCallback data_callback,
                              OnLoadCompleteCallback complete_callback) {}

  // Returns true if push mode is enabled for this loader.
  virtual bool IsPushModeEnabled() const { return false; }
};

}  // namespace chrome_pdf

#endif  // PDF_LOADER_URL_LOADER_WRAPPER_H_
