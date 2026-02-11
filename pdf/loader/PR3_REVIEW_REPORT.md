# PR #3 Review Report: Add push-based PDF resource loading with feature flag

## Overview

**PR**: https://github.com/0xZOne/chromium/pull/3
**Title**: Add push-based PDF resource loading with feature flag
**Files Changed**: 12 (+687 -5)
**Status**: Open

This CL introduces a push-based data loading path for PDF resources, controlled
by a new feature flag `kPdfPushBasedLoading`. The goal is to eliminate the 2ms
timer delay (`kReadDelayMs`) and intermediate buffer copies present in the
existing pull-based approach.

The review follows four principles:
1. Ensure the new approach does not affect original logic and supports safe
   rollback
2. Facilitate future full migration to push mode and clean removal of pull mode
   code
3. Keep the CL focused on push mode refactoring without unrelated optimizations
4. Adhere to Chromium coding best practices

---

## Summary of Findings

| Category | Issue Count |
|----------|------------|
| **Critical (P0)** — Correctness or safety regression | 3 |
| **Major (P1)** — Significant design concern | 5 |
| **Minor (P2)** — Style or cleanup | 4 |
| **Informational** — Not blocking | 2 |

**Overall Verdict**: **Needs Revision** — Several issues may cause correctness
regressions or hinder future migration.

---

## Critical Issues (P0)

### P0-1. `OnDataReceived()` duplicates `SaveBuffer()` logic — should reuse it

**Files**: `document_loader_impl.cc` (lines 432–498 in PR)

`OnDataReceived()` contains a nearly complete copy of `SaveBuffer()`, which
processes data into chunks. The core data-chunking loop (`while (!data.empty())
{ ... }`) is almost line-for-line identical. This introduces several risks:

1. **Bug divergence**: Any future bug fix to `SaveBuffer()` must also be
   applied to `OnDataReceived()`, and vice versa. This is a maintenance
   time-bomb.
2. **Subtle behavioral difference**: `SaveBuffer()` returns a `bool` indicating
   whether a chunk was saved, and calls `ContinueDownload()` / `ReadMore()`
   based on the result. `OnDataReceived()` calls
   `client_->OnPendingRequestComplete()` conditionally — but the condition is
   slightly different: `SaveBuffer` returns `false` when `!chunk_saved` (causing
   `ReadMore()`), while `OnDataReceived()` does not have equivalent flow
   control. This difference matters for partial loading interactions.
3. **Missing `IsDocumentComplete()` handling**: In `SaveBuffer`, when
   `IsDocumentComplete()` returns `true`, it returns `true` leading the caller
   (`DidRead`) to call `ReadComplete()`. In `OnDataReceived()`, when
   `IsDocumentComplete()` is detected, the function just returns without calling
   `ReadComplete()`. This means that in push mode, `ReadComplete()` will NOT be
   called when the document completes via data reception — it relies entirely on
   `OnLoadComplete()` being called later. If the network signals EOF exactly
   when the last data chunk arrives, there's a race between `OnDataReceived()`
   finishing and `OnLoadComplete()` being called.

**Recommendation**: Extract the shared chunking logic into a common helper
method (e.g., `ProcessData(base::span<const uint8_t> data)`) used by both
`SaveBuffer()` and `OnDataReceived()`. This eliminates duplication and ensures
consistent behavior.

### P0-2. Push mode not re-established after `ContinueDownload()` creates new loader

**Files**: `document_loader_impl.cc` — `OnLoadComplete()` and
`ContinueDownload()`

When partial loading is active and `OnLoadComplete()` is called with
`result >= 0`, it invokes `ContinueDownload()`. Inside `ContinueDownload()`,
the existing `loader_` is reset and a new one is created via
`client_->CreateURLLoader()` followed by `OpenRange()`. However:

- `MaybeEnablePushMode()` is only called in `Init()`.
- The new loader created by `ContinueDownload()` is a partial range loader that
  does NOT go through `Init()`.
- Therefore, the new partial loader will NOT have push mode enabled.

This means that in push mode with partial loading enabled:
1. Initial full-page load uses push mode ✓
2. When partial loading kicks in, the new loader falls back to pull mode ✗
3. `ReadMore()` in `ContinueDownload()` → `ShouldCancelLoading()` → `ReadMore()`
   will call `loader_->ReadResponseBody()` with an empty `buffer_` (size 0
   because push mode set it so), causing a potential crash or undefined
   behavior.

**Recommendation**: Either:
- (a) Call `MaybeEnablePushMode()` after creating partial loaders in
  `ContinueDownload()` and `DidOpenPartial()`, OR
- (b) Clearly document that push mode and partial loading are mutually exclusive
  and add a `DCHECK(!partial_loading_enabled_)` guard, OR
- (c) Force `partial_loading_enabled_ = false` when push mode is active.

### P0-3. `buffer_` size is 0 in push mode, but `ReadMore()` can still be called

**Files**: `document_loader_impl.cc` — constructor and `ReadMore()`

The constructor sets `buffer_(push_mode_enabled_ ? 0 : kReadBufferSize)`.
However, `ReadMore()` guards with `if (push_mode_enabled_) return;` — this
guard depends on the `push_mode_enabled_` flag being consistent throughout the
loader's lifetime. While this is generally true for the initial full-page
loader, the partial loading path (`ContinueDownload()` → `DidOpenPartial()` →
`ReadMore()`) creates a new loader without push mode but shares the same
`DocumentLoaderImpl` instance with `buffer_` size 0.

This will call `loader_->ReadResponseBody(buffer_, ...)` with an empty
`buffer_`, leading to `Result::kErrorBadArgument` in `UrlLoader` (see
`UrlLoader::ReadResponseBody()` where `buffer.empty()` returns error).

**Recommendation**: This is the same root cause as P0-2. The fix should ensure
that either partial loading is disabled in push mode, or `buffer_` is
appropriately sized.

---

## Major Issues (P1)

### P1-1. `URLLoaderWrapperImpl::EnablePushMode()` passes `on_data_received_callback_` by copy to `UrlLoader`

**File**: `url_loader_wrapper_impl.cc` (lines 288-296 in PR)

```cpp
url_loader_->EnablePushMode(
    on_data_received_callback_,   // <-- copies the RepeatingCallback
    base::BindOnce(&URLLoaderWrapperImpl::DidFinishPushModeLoading,
                   weak_factory_.GetWeakPtr()));
```

This creates two copies of the `RepeatingCallback` — one stored in
`URLLoaderWrapperImpl::on_data_received_callback_` and one in
`UrlLoader::on_data_received_callback_`. Both will be invoked because `UrlLoader`
directly calls the callback in `DidReceiveData()`. The
`URLLoaderWrapperImpl::on_data_received_callback_` copy is stored but never
directly used for data dispatch — it's effectively dead state.

Furthermore, the `URLLoaderWrapperImpl` has its own `DidRead()` which handles
multipart response parsing (byte-range header stripping). In push mode, this
multipart handling is completely bypassed because data goes directly from
`UrlLoader::DidReceiveData()` to `DocumentLoaderImpl::OnDataReceived()` without
passing through `URLLoaderWrapperImpl::DidRead()`.

**Recommendation**:
- Remove the unused `on_data_received_callback_` member from
  `URLLoaderWrapperImpl` (the one in `UrlLoader` is sufficient).
- Consider whether push mode should handle multipart responses through
  `URLLoaderWrapperImpl::DidRead()` for consistency, or explicitly document
  that multipart is not supported in push mode.

### P1-2. Multipart handling in `OnDataReceived()` is incomplete

**File**: `document_loader_impl.cc` — `OnDataReceived()` (lines 434-442 in PR)

```cpp
if (loader_->IsMultipart()) {
    int start_pos = 0;
    if (!loader_->GetByteRangeStart(&start_pos)) {
      return ReadComplete();
    }
    DCHECK(!chunk_.chunk_data);
    chunk_.chunk_index = chunk_stream_.GetChunkIndex(start_pos);
  }
```

This multipart handling has issues:
1. In pull mode, `URLLoaderWrapperImpl::DidRead()` strips multipart headers
   from the data before passing it to `DocumentLoaderImpl`. In push mode, data
   bypasses `URLLoaderWrapperImpl::DidRead()`, so **raw multipart data
   including boundary markers and headers** will be passed to `OnDataReceived()`
   and stored directly into chunk data.
2. The `DCHECK(!chunk_.chunk_data)` will fire on every call after the first
   data chunk has been partially filled, since `chunk_data` is allocated and not
   released until a full chunk is saved.

**Recommendation**: Either:
- Explicitly disable/disallow multipart responses in push mode with a guard, OR
- Route push mode data through `URLLoaderWrapperImpl::DidRead()` for multipart
  handling.

### P1-3. Duplicate callback type definitions across `URLLoaderWrapper` and `UrlLoader`

**Files**: `url_loader_wrapper.h`, `url_loader.h`

Both classes independently define:
```cpp
using OnDataReceivedCallback =
    base::RepeatingCallback<void(base::span<const uint8_t>)>;
using OnLoadCompleteCallback = base::OnceCallback<void(int result)>;
```

These are semantically identical but are separate type definitions. This causes
confusion about which type is used where, and also creates redundancy.

**Recommendation**: Define the callback types only in `URLLoaderWrapper` (the
interface class) and have `UrlLoader` reference them, or define them in a common
location. This is important for future cleanup — when removing pull mode, having
a single definition makes it easier.

### P1-4. `#include "base/functional/callback_forward.h"` changed to `#include "base/functional/callback.h"` unnecessarily

**Files**: `url_loader_wrapper.h`, `url_loader_wrapper_impl.h`

The PR changes `#include "base/functional/callback_forward.h"` to
`#include "base/functional/callback.h"` in both `url_loader_wrapper.h` and
`url_loader_wrapper_impl.h`. While this is necessary for the callback type
definitions in the header, Chromium style prefers forward declarations where
possible to minimize header dependencies.

**Recommendation**: Consider putting the callback typedefs that require the full
definition into the `.cc` file, or accept this as a necessary trade-off for the
push mode interface.

### P1-5. `OnDataReceived()` does not call `ContinueDownload()` — behavior gap with pull mode

**File**: `document_loader_impl.cc` — `OnDataReceived()` vs `DidRead()` +
`SaveBuffer()` flow

In pull mode, the call chain is:
1. `DidRead()` → `SaveBuffer()` → returns `true` → `ContinueDownload()`
2. `ContinueDownload()` → `ShouldCancelLoading()` → decides whether to
   continue with current connection or start a new range request.

In push mode, `OnDataReceived()` never calls `ContinueDownload()`. This means
that in push mode with partial loading, the logic to cancel loading when
data is unexpected or to switch to range requests is entirely skipped.

While the tests disable partial loading for push mode tests, the code does not
enforce this — it's possible to have both push mode and partial loading enabled
simultaneously.

**Recommendation**: Add a DCHECK or runtime guard ensuring push mode and partial
loading are not both active, or implement `ContinueDownload()` calls in push
mode.

---

## Minor Issues (P2)

### P2-1. Design document contains Chinese text

**File**: `pdf/loader/PDF_PUSH_LOADING_DESIGN.md`

The design document is written primarily in Chinese. Chromium's codebase is
English-only. Design documents committed to the repository should follow this
convention.

**Recommendation**: Translate the design document to English, or move it out
of the repository to an external design doc.

### P2-2. `TestURLLoader::EnablePushMode()` doesn't override properly

**File**: `document_loader_impl_unittest.cc` (lines 209-218 in PR)

The `TestURLLoader::EnablePushMode()` override calls
`data_->SetPushModeEnabled(true)`, but the `EnablePushMode()` on the
`URLLoaderWrapper` base class has a default empty implementation. The test mock
does override correctly, but it stores the callbacks in `LoaderData` which is a
separate nested class rather than in the `TestURLLoader` itself.

This design means that `LoaderData` outlives `TestURLLoader` (since
`TestURLLoader` is moved into `DocumentLoaderImpl` via `Init()`), which is
correct. However, it would be clearer to document why this pattern is necessary.

### P2-3. `PushModeNoReadCallback` test is incomplete

**File**: `document_loader_impl_unittest.cc` (lines 1328-1339 in PR)

```cpp
TEST_F(DocumentLoaderImplPushModeTest, PushModeNoReadCallback) {
  // ...
  EXPECT_FALSE(client.full_page_loader_data()->IsWaitRead());
}
```

This test verifies that no read callback is pending in push mode, which is
correct. However, it would be more robust to also push some data and verify
the data is processed correctly, and then verify `IsWaitRead()` is still false.

### P2-4. Unused variable `complete_called` in `UrlLoaderTest::EnablePushMode`

**File**: `url_loader_unittest.cc` (line 669 in PR)

```cpp
bool complete_called = false;
```

This variable is set in the callback but never verified with an `EXPECT_*`.

**Recommendation**: Either verify `complete_called` or remove it.

---

## Informational Notes

### I-1. Feature flag interaction matrix

The CL introduces `kPdfPushBasedLoading` but does not document its interaction
with existing flags:

| `kPdfPartialLoading` | `kPdfPushBasedLoading` | Expected Behavior |
|---|---|---|
| OFF | OFF | Pull mode, full page only |
| ON | OFF | Pull mode, with partial loading |
| OFF | ON | Push mode, full page only |
| ON | ON | Push mode for initial loader, pull mode for partial range requests |

**Recommendation**: Add a comment or DCHECK documenting the interaction.

### I-2. Performance impact not measurable from tests alone

The design document claims elimination of the 2ms timer and intermediate buffer
copies. However, the tests only verify functional correctness, not performance.
A follow-up CL should add performance benchmarks or metrics.

---

## Detailed File-by-File Review

### `pdf/pdf_features.h` / `pdf/pdf_features.cc`
✅ **LGTM** — Feature flag declaration and definition follow Chromium
conventions. Default disabled is the correct choice.

### `pdf/loader/url_loader_wrapper.h`
⚠️ **P1-3, P1-4** — Duplicate callback types; include change from forward
declaration to full header.

The `EnablePushMode()` and `IsPushModeEnabled()` virtual methods have default
implementations in the base class, which is correct for an interface extension
that shouldn't break existing implementations.

### `pdf/loader/url_loader_wrapper_impl.h` / `.cc`
⚠️ **P1-1** — Stores callback copy that's never directly dispatched.

The `DidFinishPushModeLoading()` forwarding is clean.

### `pdf/loader/url_loader.h` / `.cc`
⚠️ **P1-3** — Duplicate callback type definitions.

The push mode integration in `DidReceiveData()`, `DidFinishLoading()`, and
`AbortLoad()` is clean and correctly bypasses buffering.

One concern: in `DidFinishLoading()`, after `SetLoadComplete(Result::kSuccess)`,
push mode returns early and skips `RunReadCallback()`. This is correct because
push mode doesn't use read callbacks, but there's no guard against someone
calling `ReadResponseBody()` while push mode is active — it would silently
hang (callback never fired).

### `pdf/loader/document_loader_impl.h` / `.cc`
❌ **P0-1, P0-2, P0-3, P1-2, P1-5** — Multiple critical and major issues.

The `buffer_(push_mode_enabled_ ? 0 : kReadBufferSize)` approach is clever but
creates a hidden dependency — anyone reading the constructor needs to understand
that `buffer_` size depends on the feature flag.

### `pdf/loader/document_loader_impl_unittest.cc`
⚠️ **P2-2, P2-3** — Tests could be more robust.

The test structure with separate fixture classes (`DocumentLoaderImplPushModeTest`
and `DocumentLoaderImplPullModeTest`) is a good pattern for feature flag
testing.

### `pdf/loader/url_loader_unittest.cc`
⚠️ **P2-4** — Unused variable.

Push mode tests cover the basic scenarios (enable, receive data, no buffering,
finish, error) adequately.

### `pdf/loader/PDF_PUSH_LOADING_DESIGN.md`
⚠️ **P2-1** — Written in Chinese; should be in English for Chromium.

The document provides good analysis of the existing pull mode architecture and
the push mode design. The code location references are useful.

---

## Recommendations for Safe Landing

1. **Fix P0-2/P0-3**: Support push mode with partial loading by ensuring:
   - `buffer_` is always allocated with `kReadBufferSize`
   - `ReadMore()` only skips reading when the current loader has push mode
     enabled (`loader_->IsPushModeEnabled()`)
   - `OnDataReceived()` calls `ContinueDownload()` after data processing
   - `OnLoadComplete()` handles `is_partial_loader_active_` correctly
   - Partial loaders created by `ContinueDownload()` use pull mode

2. **Extract shared chunking logic (P0-1)**: Create a `ProcessReceivedData()`
   helper to eliminate the duplication between `SaveBuffer()` and
   `OnDataReceived()`.

3. **Clean up callback chain (P1-1)**: Remove unused callback storage in
   `URLLoaderWrapperImpl`.

4. **Translate design doc (P2-1)**: Convert to English.

---

## Principle Assessment

### 1. Does the new approach affect original logic? Can it safely roll back?
**Partially** — The original pull mode logic is preserved and feature flag
defaults to disabled. However, the `buffer_` size change in the constructor
(`push_mode_enabled_ ? 0 : kReadBufferSize`) affects the object layout even in
pull mode (the conditional expression is evaluated at construction time). The
rollback is safe as long as the feature flag remains disabled.

⚠️ Concern: The change from `callback_forward.h` to `callback.h` affects all
code that includes these headers, even in pull mode.

### 2. Is it easy to migrate fully and remove pull mode code?
**Partially** — The push mode code is cleanly separated in new functions
(`OnDataReceived`, `OnLoadComplete`, `MaybeEnablePushMode`). However, the
duplicated chunking logic in `OnDataReceived()` vs `SaveBuffer()` means both
must be maintained until the full migration, increasing maintenance burden.

### 3. Is the CL focused on push mode?
**Yes** — The CL is well-scoped. The only non-push changes are the
`callback_forward.h` → `callback.h` include changes, which are necessary for
the callback type definitions.

### 4. Does the code follow Chromium best practices?
**Mostly** — Naming conventions are correct (after the rename commit). The
feature flag follows the standard `BASE_FEATURE` pattern. The design doc in
Chinese is the main deviation from Chromium norms.

---

## Applied Fixes

The following fixes have been applied to address the issues identified above:

### P0-1 Fix: Extract shared chunking logic
- Created `ProcessReceivedData(base::span<const uint8_t> data)` as a shared
  method that both `SaveBuffer()` (pull mode) and `OnDataReceived()` (push
  mode) delegate to. This eliminates the duplicated chunking loop and ensures
  consistent behavior across both modes.

### P0-2/P0-3 Fix: Push mode with partial loading support
- Push mode and partial loading now coexist correctly. The initial full-page
  loader uses push mode (data pushed via callbacks), while partial loaders
  created by `ContinueDownload()` use pull mode (ReadMore/ReadResponseBody).
- `buffer_` is always allocated with `kReadBufferSize` so partial loaders can
  use it.
- `ReadMore()` only skips reading when the current loader has push mode
  enabled (`loader_->IsPushModeEnabled()`), not unconditionally.
- `OnDataReceived()` calls `ContinueDownload()` after processing data, so
  partial loading can switch to range requests when needed.
- `OnLoadComplete()` handles `is_partial_loader_active_` by calling
  `ContinueDownload()`, matching the pull mode `DidRead()` flow.

### P1-1 Fix: Remove unused callback copy in URLLoaderWrapperImpl
- `URLLoaderWrapperImpl::EnablePushMode()` now passes the data callback
  directly to `UrlLoader` via `std::move()` instead of storing a copy.
  Only `on_load_complete_callback_` is stored for forwarding through
  `DidFinishPushModeLoading()`.

### P1-3 Fix: Unified callback typedefs
- Removed duplicate `OnDataReceivedCallback` and `OnLoadCompleteCallback`
  type definitions from `UrlLoader`. `UrlLoader` now references
  `URLLoaderWrapper::OnDataReceivedCallback` and
  `URLLoaderWrapper::OnLoadCompleteCallback` directly.

### P1-5 Fix: Proper completion flow in push mode
- `OnDataReceived()` calls `ContinueDownload()` after processing data,
  consistent with the pull mode flow (DidRead → SaveBuffer → ContinueDownload).
  This allows partial loading to switch to range requests when needed.
- `OnLoadComplete()` handles `is_partial_loader_active_` by calling
  `ContinueDownload()`, matching the pull mode `DidRead()` flow when
  result == 0.

### P2-4 Fix: Removed unused test variable
- Removed the unused `complete_called` variable from
  `UrlLoaderTest::EnablePushMode`.

### Test additions
- `DocumentLoaderImplPushModeTest::PushModeWithPartialLoading`: Verifies
  that push mode and partial loading coexist correctly when the server
  supports range requests.

---

*Report generated: 2026-02-11*
*Fixes applied: 2026-02-11*
