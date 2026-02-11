# PDF Push-Based Resource Loading Design Document

## Goal

Switch PDF resource loading from the current pull-based flow to a push-based
flow, while removing the artificial 2ms timer and intermediate buffers to
improve performance.

## Current Implementation (Pull Mode)

### Data Flow

```
Network (Blink) --> UrlLoader --> URLLoaderWrapperImpl --> DocumentLoaderImpl --> PDFium
                    |                  |                        |
                 Buffering          2ms timer                 Temporary buffer
               (circular_deque)   (read_starter_)              (buffer_)
```

### Key Components and Code Locations

#### 1. 2ms Timer
- **File**: `pdf/loader/url_loader_wrapper_impl.cc`
- **Line**: 36
```cpp
constexpr base::TimeDelta kReadDelayMs = base::Milliseconds(2);
```
- **Usage**: Used in `ReadResponseBody` (lines 165-168) to delay reading the
  response body and avoid blocking the UI thread.

#### 2. UrlLoader Intermediate Buffer
- **File**: `pdf/loader/url_loader.h`
- **Line**: 195
```cpp
base::circular_deque<uint8_t> buffer_;
```
- **Usage**: Stores data received from the network while waiting for
  `ReadResponseBody` to be called by the upper layer.

#### 3. DocumentLoaderImpl Temporary Buffer
- **File**: `pdf/loader/document_loader_impl.cc`
- **Lines**: 36, 77
```cpp
constexpr size_t kReadBufferSize = 256 * 1024;  // 256KB
buffer_(kReadBufferSize)  // Initialized in the constructor
```
- **Usage**: Used by `ReadMore` (lines 298-301) to receive data from the URL
  loader.

### Pull-Mode Workflow

1. `UrlLoader::DidReceiveData()` receives network data and stores it in
   `buffer_` (url_loader.cc:211-228).
2. `DocumentLoaderImpl::ReadMore()` calls `loader_->ReadResponseBody()` to pull
   data (document_loader_impl.cc:298-302).
3. `URLLoaderWrapperImpl::ReadResponseBody()` uses `read_starter_` to delay
   reading by 2ms (url_loader_wrapper_impl.cc:161-169).
4. `URLLoaderWrapperImpl::ReadResponseBodyImpl()` invokes the underlying loader
   to read data (url_loader_wrapper_impl.cc:171-176).
5. `UrlLoader::RunReadCallback()` copies data from `buffer_` into the caller’s
   buffer (url_loader.cc:275-303).
6. `DocumentLoaderImpl::DidRead()` processes the received data
   (document_loader_impl.cc:304-331).
7. `DocumentLoaderImpl::SaveBuffer()` writes data into the chunk stream
   (document_loader_impl.cc:333-374).

## New Design (Push Mode)

### Design Goals

1. Remove the 2ms timer delay.
2. Remove unnecessary intermediate buffer copies.
3. Allow pull/push switching via a feature flag.
4. Preserve backward compatibility for rollback.

### Data Flow (Push Mode)

```
Network (Blink) --> UrlLoader --> URLLoaderWrapperImpl --> DocumentLoaderImpl --> PDFium
                    |                                          |
               Direct push                                Direct storage
             (no intermediate buffer)                  (no temporary buffer)
```

### Feature Flag Design

- **Files**: `pdf/pdf_features.h`, `pdf/pdf_features.cc`
- **Feature name**: `kPdfPushBasedLoading`
- **Default**: `FEATURE_DISABLED_BY_DEFAULT` (conservative default, pull mode)

```cpp
// pdf/pdf_features.h
BASE_DECLARE_FEATURE(kPdfPushBasedLoading);

// pdf/pdf_features.cc
BASE_FEATURE(kPdfPushBasedLoading, base::FEATURE_DISABLED_BY_DEFAULT);
```

### Interface Changes

#### 1. URLLoaderWrapper Interface Extension

Add push-mode support in `pdf/loader/url_loader_wrapper.h`:

```cpp
class URLLoaderWrapper {
 public:
  // Data receive callback.
  using OnDataReceivedCallback =
      base::RepeatingCallback<void(base::span<const uint8_t>)>;
  // Completion callback (0 = success, negative = error).
  using OnLoadCompleteCallback = base::OnceCallback<void(int result)>;

  // Enable push mode and set data receive callback.
  virtual void EnablePushMode(OnDataReceivedCallback data_callback,
                              OnLoadCompleteCallback complete_callback) {}

  // Returns whether push mode is enabled.
  virtual bool IsPushModeEnabled() const { return false; }

  // ... existing interface unchanged ...
};
```

#### 2. URLLoaderWrapperImpl Changes

In `pdf/loader/url_loader_wrapper_impl.h` and `.cc`:

- Add push-mode member variables.
- Implement `EnablePushMode()` and `IsPushModeEnabled()`.
- Add `DidFinishPushModeLoading()` to handle push-mode completion.
- Bypass the 2ms timer in push mode by invoking data callbacks directly.

#### 3. UrlLoader Changes

In `pdf/loader/url_loader.h` and `.cc`:

- Add `EnablePushMode()` and related members.
- `DidReceiveData()` calls the data callback directly in push mode.
- `DidFinishLoading()` and `DidFail()` call the completion callback in push
  mode.

#### 4. DocumentLoaderImpl Changes

In `pdf/loader/document_loader_impl.h` and `.cc`:

- Select pull or push mode based on the feature flag.
- Add `is_push_mode_enabled()` accessor.
- Add `MaybeEnablePushMode()` to conditionally enable push mode.
- Add `OnDataReceived()` to process pushed data.
- Add `OnLoadComplete()` to handle completion.
- In push mode:
  - Set data callbacks on the URLLoaderWrapper.
  - Write data directly into `chunk_stream_`, bypassing `buffer_`.
  - `ReadMore()` returns early (no polling).
- In pull mode: keep existing logic unchanged.

### Code Structure

To keep the code clear and make future removal of pull mode easier:

1. Keep pull-mode code in its original locations.
2. Guard push-mode logic with feature-flag checks.
3. Use explicit naming to distinguish the two modes.

### Test Strategy

1. **Keep existing tests** to ensure pull mode continues to work.
2. **Add push-mode tests**:
   - Basic data reception.
   - Large file chunk reception.
   - Error handling.
   - Feature flag toggling.

## Implementation Steps

1. ✅ Add the feature flag `kPdfPushBasedLoading`.
2. ✅ Extend the `URLLoaderWrapper` interface.
3. ✅ Implement push-mode support in `URLLoaderWrapperImpl`.
4. ✅ Update `UrlLoader` to support push-mode delivery.
5. ✅ Update `DocumentLoaderImpl` to use push mode.
6. ✅ Add unit tests.
7. ✅ Code review and security checks.

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Push mode may block the UI thread | Gate with feature flag for fast rollback |
| Data sequencing issues | Expand unit test coverage |
| Compatibility issues | Default to pull mode and roll out gradually |

## Code Location Reference

| File | Key lines | Description |
|------|-----------|-------------|
| `url_loader_wrapper_impl.cc` | L36 | 2ms timer constant |
| `url_loader_wrapper_impl.cc` | L165-168 | Timer-based delayed reads |
| `url_loader.h` | L195 | Intermediate buffer definition |
| `url_loader.cc` | L211-228 | Data reception and buffering |
| `document_loader_impl.cc` | L36, L77 | Temporary buffer definition |
| `document_loader_impl.cc` | L298-302 | `ReadMore` pulling data |
| `pdf_features.h` | L22-34 | Feature flag declaration |
| `pdf_features.cc` | L18-58 | Feature flag definition |
