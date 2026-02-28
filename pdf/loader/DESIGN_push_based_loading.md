# PDF资源加载从Pull方式改为Push方式的详细设计方案

## 1. 背景与目标

### 1.1 当前架构分析

当前PDF资源加载使用**Pull（拉取）模式**：

```
网络层 → UrlLoader::DidReceiveData() → 内部缓冲区buffer_
                                              ↓
                                    (客户端轮询读取)
                                              ↓
URLLoaderWrapperImpl::ReadResponseBody() → 2ms定时器 → ReadResponseBodyImpl()
                                              ↓
                                    DocumentLoaderImpl::DidRead()
```

**关键代码位置：**

| 文件 | 行号 | 说明 |
|------|------|------|
| `pdf/loader/url_loader_wrapper_impl.cc` | 36 | `constexpr base::TimeDelta kReadDelayMs = base::Milliseconds(2);` - 2ms定时器常量 |
| `pdf/loader/url_loader_wrapper_impl.cc` | 161-169 | `ReadResponseBody()` - 使用定时器延迟读取 |
| `pdf/loader/url_loader_wrapper_impl.cc` | 171-176 | `ReadResponseBodyImpl()` - 实际读取实现 |
| `pdf/loader/url_loader.cc` | 211-228 | `DidReceiveData()` - 数据到达时的处理（数据实际是push过来的） |
| `pdf/loader/url_loader.cc` | 219 | `buffer_.insert(buffer_.end(), data.begin(), data.end());` - 数据暂存到内部缓冲区 |
| `pdf/loader/document_loader_impl.cc` | 298-302 | `ReadMore()` - 发起数据拉取请求 |
| `pdf/loader/document_loader_impl.cc` | 304-331 | `DidRead()` - 处理读取结果 |

### 1.2 问题分析

1. **2ms定时器延迟**：在`url_loader_wrapper_impl.cc:165-168`，每次读取都有2ms的人为延迟
   ```cpp
   read_starter_.Start(
       FROM_HERE, kReadDelayMs,  // 2ms延迟
       base::BindOnce(&URLLoaderWrapperImpl::ReadResponseBodyImpl,
                      base::Unretained(this), std::move(callback)));
   ```

2. **Pull模式的低效**：
   - 数据已经通过`DidReceiveData()`到达`UrlLoader`的内部缓冲区
   - 但客户端需要主动调用`ReadResponseBody()`来"拉取"数据
   - 每次拉取都有2ms延迟，降低了吞吐量

### 1.3 目标

将PDF资源加载从Pull方式修改为Push方式：
- 数据到达时直接推送给`DocumentLoaderImpl`
- 去除2ms定时器延迟
- 通过feature flag控制，方便回退

---

## 2. 方案设计

### 2.1 总体架构

**Push模式架构：**

```
网络层 → UrlLoader::DidReceiveData() → [直接回调] → DocumentLoaderImpl::OnDataPushed()
                                                              ↓
                                                       SaveBuffer()
                                                              ↓
                                                    Client::OnNewDataReceived()
```

### 2.2 Feature Flag设计

**文件：`pdf/pdf_features.h`（行号参考：22-34）**

添加新的feature flag：

```cpp
// 控制是否使用push方式加载PDF资源
// 启用时使用push方式，禁用时使用原有的pull方式
BASE_DECLARE_FEATURE(kPdfPushBasedLoading);
```

**文件：`pdf/pdf_features.cc`（行号参考：12-35）**

```cpp
// Push-based loading for PDF resources, eliminating the 2ms timer delay.
// When enabled, data is pushed directly from UrlLoader to DocumentLoaderImpl.
// TODO(crbug.com/xxxxx): Remove pull-based loading after push-based loading is stable.
BASE_FEATURE(kPdfPushBasedLoading, base::FEATURE_DISABLED_BY_DEFAULT);
```

### 2.3 接口设计

#### 方案一：在URLLoaderWrapper中添加Push回调接口

**优点：**
- 改动最小，保持现有层次结构
- 对DocumentLoaderImpl改动较小

**缺点：**
- URLLoaderWrapper需要同时支持push和pull两种模式
- 接口略显冗余

**文件修改：`pdf/loader/url_loader_wrapper.h`（基于现有接口）**

```cpp
class URLLoaderWrapper {
 public:
  // 新增：数据推送回调类型
  using DataPushCallback = base::RepeatingCallback<void(base::span<const uint8_t> data)>;
  using LoadCompleteCallback = base::OnceCallback<void(int result)>;

  // 新增：设置push模式回调（在Open成功后调用）
  virtual void SetDataPushCallback(DataPushCallback data_callback,
                                   LoadCompleteCallback complete_callback) = 0;

  // 现有的pull接口保持不变
  virtual void ReadResponseBody(base::span<uint8_t> buffer,
                                base::OnceCallback<void(int)> callback) = 0;
};
```

**文件修改：`pdf/loader/url_loader_wrapper_impl.h`**

```cpp
class URLLoaderWrapperImpl : public URLLoaderWrapper {
 public:
  // 实现新接口
  void SetDataPushCallback(DataPushCallback data_callback,
                           LoadCompleteCallback complete_callback) override;

 private:
  // Push模式相关
  DataPushCallback data_push_callback_;
  LoadCompleteCallback load_complete_callback_;
  bool push_mode_enabled_ = false;

  // 处理UrlLoader推送的数据
  void OnDataReceived(base::span<const uint8_t> data);
  void OnLoadComplete(int result);
};
```

#### 方案二：在UrlLoader中直接添加Push回调接口

**优点：**
- 在数据源头处理push逻辑
- 可以完全绕过中间的buffer，减少内存拷贝

**缺点：**
- 改动范围较大
- UrlLoader是更底层的组件，改动风险更高

**文件修改：`pdf/loader/url_loader.h`（基于现有接口142-152行）**

```cpp
class UrlLoader final : public blink::WebAssociatedURLLoaderClient {
 public:
  // 新增：push模式数据回调
  using DataPushCallback = base::RepeatingCallback<void(base::span<const char> data)>;

  // 新增：设置push模式（需在Open之前调用）
  void SetPushMode(DataPushCallback data_callback);

  // 现有接口保持不变
  void ReadResponseBody(base::span<uint8_t> buffer,
                        base::OnceCallback<void(int)> callback);
};
```

#### 方案三（推荐）：在DocumentLoaderImpl层面实现Push适配

**优点：**
- 改动集中在DocumentLoaderImpl
- 不需要修改底层的UrlLoader接口
- 利用现有的`UrlLoader::DidReceiveData()`机制

**实现思路：**
- 在`UrlLoader`中添加可选的数据推送回调
- `DocumentLoaderImpl`根据feature flag选择注册push回调或使用原有的pull方式

**文件修改：`pdf/loader/url_loader.h`**

在第93-126行的类定义中添加：

```cpp
class UrlLoader final : public blink::WebAssociatedURLLoaderClient {
 public:
  // 数据推送回调类型
  using OnDataCallback = base::RepeatingCallback<void(base::span<const char> data)>;
  using OnCompleteCallback = base::OnceCallback<void(Result result)>;

  // 设置push模式的数据回调。如果设置了，数据将直接推送给回调，
  // 而不是存储在内部buffer中。必须在Open()之前调用。
  void SetPushModeCallbacks(OnDataCallback on_data,
                            OnCompleteCallback on_complete);

  // 返回是否处于push模式
  bool is_push_mode() const { return !on_data_callback_.is_null(); }

 private:
  OnDataCallback on_data_callback_;
  OnCompleteCallback on_complete_callback_;
};
```

**文件修改：`pdf/loader/url_loader.cc`的`DidReceiveData()`（行211-228）**

```cpp
void UrlLoader::DidReceiveData(base::span<const char> data) {
  DCHECK_EQ(state_, LoadingState::kStreamingData);

  if (data.empty()) {
    return;
  }

  // Push模式：直接推送数据给回调
  if (is_push_mode()) {
    on_data_callback_.Run(data);
    return;
  }

  // Pull模式：存储到内部buffer（现有逻辑）
  buffer_.insert(buffer_.end(), data.begin(), data.end());

  if (!deferring_loading_ && buffer_.size() >= buffer_upper_threshold_) {
    deferring_loading_ = true;
    blink_loader_->SetDefersLoading(true);
  }

  RunReadCallback();
}
```

---

## 3. 详细实现方案（推荐方案三）

### 3.1 修改文件清单

| 文件 | 修改类型 | 说明 |
|------|----------|------|
| `pdf/pdf_features.h` | 新增 | 添加`kPdfPushBasedLoading` feature flag声明 |
| `pdf/pdf_features.cc` | 新增 | 添加`kPdfPushBasedLoading` feature flag定义 |
| `pdf/loader/url_loader.h` | 修改 | 添加push模式相关接口 |
| `pdf/loader/url_loader.cc` | 修改 | 实现push模式逻辑 |
| `pdf/loader/url_loader_wrapper.h` | 修改 | 添加push模式接口 |
| `pdf/loader/url_loader_wrapper_impl.h` | 修改 | 添加push模式成员 |
| `pdf/loader/url_loader_wrapper_impl.cc` | 修改 | 实现push模式，绕过2ms定时器 |
| `pdf/loader/document_loader_impl.h` | 修改 | 添加push模式处理方法 |
| `pdf/loader/document_loader_impl.cc` | 修改 | 根据feature flag选择push/pull模式 |
| `pdf/loader/document_loader_impl_unittest.cc` | 新增测试 | push模式测试用例 |
| `pdf/loader/url_loader_unittest.cc` | 新增测试 | push模式测试用例 |

### 3.2 代码实现细节

#### 3.2.1 Feature Flag

**pdf/pdf_features.h 新增（在第34行后）：**

```cpp
BASE_DECLARE_FEATURE(kPdfPushBasedLoading);
```

**pdf/pdf_features.cc 新增（在第35行后）：**

```cpp
// Push-based loading for PDF resources.
// When enabled, data is pushed directly from the network layer to DocumentLoader,
// eliminating the 2ms timer delay in the pull-based approach.
// TODO(crbug.com/xxxxx): Remove pull-based loading after push-based loading is stable.
BASE_FEATURE(kPdfPushBasedLoading, base::FEATURE_DISABLED_BY_DEFAULT);
```

#### 3.2.2 UrlLoader修改

**pdf/loader/url_loader.h 修改：**

在class UrlLoader定义中（约第93行后）添加：

```cpp
 public:
  // Push mode callbacks. When set, data is pushed directly to callbacks
  // instead of being buffered internally.
  using OnDataCallback = base::RepeatingCallback<void(base::span<const char> data)>;
  using OnCompleteCallback = base::OnceCallback<void(Result result)>;

  // Sets callbacks for push mode. Must be called before Open().
  // When push mode is active, ReadResponseBody() should not be called.
  void SetPushModeCallbacks(OnDataCallback on_data,
                            OnCompleteCallback on_complete);

  bool is_push_mode() const { return !on_data_callback_.is_null(); }

 private:
  // Push mode callbacks (empty when in pull mode)
  OnDataCallback on_data_callback_;
  OnCompleteCallback on_complete_callback_;
```

**pdf/loader/url_loader.cc 修改：**

添加`SetPushModeCallbacks`实现：

```cpp
void UrlLoader::SetPushModeCallbacks(OnDataCallback on_data,
                                     OnCompleteCallback on_complete) {
  DCHECK_EQ(state_, LoadingState::kWaitingToOpen);
  DCHECK(on_data);
  DCHECK(on_complete);
  on_data_callback_ = std::move(on_data);
  on_complete_callback_ = std::move(on_complete);
}
```

修改`DidReceiveData()`（行211-228）：

```cpp
void UrlLoader::DidReceiveData(base::span<const char> data) {
  DCHECK_EQ(state_, LoadingState::kStreamingData);

  if (data.empty()) {
    return;
  }

  // Push mode: directly push data to callback.
  if (is_push_mode()) {
    on_data_callback_.Run(data);
    return;
  }

  // Pull mode: buffer data internally (existing logic).
  buffer_.insert(buffer_.end(), data.begin(), data.end());

  if (!deferring_loading_ && buffer_.size() >= buffer_upper_threshold_) {
    deferring_loading_ = true;
    blink_loader_->SetDefersLoading(true);
  }

  RunReadCallback();
}
```

修改`DidFinishLoading()`（行231-236）：

```cpp
void UrlLoader::DidFinishLoading() {
  DCHECK_EQ(state_, LoadingState::kStreamingData);

  SetLoadComplete(Result::kSuccess);

  if (is_push_mode()) {
    std::move(on_complete_callback_).Run(Result::kSuccess);
    return;
  }

  RunReadCallback();
}
```

修改`DidFail()`以支持push模式。

#### 3.2.3 URLLoaderWrapper接口修改

**pdf/loader/url_loader_wrapper.h 新增（在第56行后）：**

```cpp
  // Push mode interface. When enabled, data is pushed via callback instead
  // of being pulled via ReadResponseBody().
  using OnDataCallback = base::RepeatingCallback<void(base::span<const uint8_t> data)>;
  using OnCompleteCallback = base::OnceCallback<void(int result)>;

  // Enable push mode. Must be called after successful response.
  virtual void EnablePushMode(OnDataCallback on_data,
                              OnCompleteCallback on_complete) = 0;

  // Returns true if push mode is enabled.
  virtual bool IsPushModeEnabled() const = 0;
```

#### 3.2.4 URLLoaderWrapperImpl实现

**pdf/loader/url_loader_wrapper_impl.h 修改：**

```cpp
 public:
  void EnablePushMode(OnDataCallback on_data,
                      OnCompleteCallback on_complete) override;
  bool IsPushModeEnabled() const override;

 private:
  // Push mode callbacks
  OnDataCallback on_data_callback_;
  OnCompleteCallback on_complete_callback_;
  bool push_mode_enabled_ = false;
```

**pdf/loader/url_loader_wrapper_impl.cc 修改：**

```cpp
void URLLoaderWrapperImpl::EnablePushMode(OnDataCallback on_data,
                                          OnCompleteCallback on_complete) {
  DCHECK(!push_mode_enabled_);
  DCHECK(on_data);
  DCHECK(on_complete);
  push_mode_enabled_ = true;
  on_data_callback_ = std::move(on_data);
  on_complete_callback_ = std::move(on_complete);

  // Start reading without the 2ms delay in push mode.
  // Data will be pushed directly via callbacks.
  ReadResponseBodyImplForPushMode();
}

bool URLLoaderWrapperImpl::IsPushModeEnabled() const {
  return push_mode_enabled_;
}

void URLLoaderWrapperImpl::ReadResponseBodyImplForPushMode() {
  // In push mode, we continuously read and push data without delay.
  url_loader_->ReadResponseBody(
      buffer_,
      base::BindOnce(&URLLoaderWrapperImpl::DidReadForPushMode,
                     weak_factory_.GetWeakPtr()));
}

void URLLoaderWrapperImpl::DidReadForPushMode(int32_t result) {
  if (result > 0) {
    // Push data to callback.
    on_data_callback_.Run(buffer_.first(static_cast<size_t>(result)));
    // Continue reading.
    ReadResponseBodyImplForPushMode();
  } else {
    // Loading complete or error.
    std::move(on_complete_callback_).Run(result);
  }
}
```

#### 3.2.5 DocumentLoaderImpl修改

**pdf/loader/document_loader_impl.h 新增：**

```cpp
 private:
  // Push mode support
  bool push_mode_enabled_ = false;

  // Called when data is pushed from the loader in push mode.
  void OnDataPushed(base::span<const uint8_t> data);
  // Called when loading is complete in push mode.
  void OnLoadingComplete(int result);
```

**pdf/loader/document_loader_impl.cc 修改：**

在`Init()`方法中（约第81行后）添加push模式初始化：

```cpp
bool DocumentLoaderImpl::Init(std::unique_ptr<URLLoaderWrapper> loader,
                              const std::string& url) {
  // ... existing checks ...

  url_ = url;
  loader_ = std::move(loader);

  // Check if push mode should be enabled
  push_mode_enabled_ = base::FeatureList::IsEnabled(features::kPdfPushBasedLoading);

  if (!loader_->IsContentEncoded())
    chunk_stream_.set_eof_pos(std::max(0, loader_->GetContentLength()));

  SetPartialLoadingEnabled(/* ... */);

  if (push_mode_enabled_) {
    // Enable push mode - data will be pushed directly via callbacks.
    loader_->EnablePushMode(
        base::BindRepeating(&DocumentLoaderImpl::OnDataPushed,
                           weak_factory_.GetWeakPtr()),
        base::BindOnce(&DocumentLoaderImpl::OnLoadingComplete,
                       weak_factory_.GetWeakPtr()));
  } else {
    // Pull mode - use existing ReadMore() approach.
    ReadMore();
  }
  return true;
}
```

添加push模式处理方法：

```cpp
void DocumentLoaderImpl::OnDataPushed(base::span<const uint8_t> data) {
  DCHECK(push_mode_enabled_);

  // Process pushed data similar to how SaveBuffer processes pulled data.
  bytes_received_ += data.size();

  // Copy data to chunk buffer and save when complete.
  auto input = data;
  while (!input.empty()) {
    if (chunk_.data_size == 0)
      chunk_.chunk_data = std::make_unique<DataStream::ChunkData>();

    const size_t new_chunk_data_len =
        std::min(DataStream::kChunkSize - chunk_.data_size, input.size());
    UNSAFE_TODO({
      memcpy(chunk_.chunk_data->data() + chunk_.data_size, input.data(),
             new_chunk_data_len);
    });
    chunk_.data_size += new_chunk_data_len;

    const uint32_t document_size = GetDocumentSize();
    if (chunk_.data_size == DataStream::kChunkSize ||
        (document_size > 0 && document_size <= EndOfCurrentChunk())) {
      pending_requests_.Subtract(
          gfx::Range(chunk_.chunk_index, chunk_.chunk_index + 1));
      SaveChunkData();
    }

    input = input.subspan(new_chunk_data_len);
  }

  client_->OnNewDataReceived();
}

void DocumentLoaderImpl::OnLoadingComplete(int result) {
  DCHECK(push_mode_enabled_);

  if (result < 0) {
    // Error occurred.
    ReadComplete();
    return;
  }

  // result == 0 means loading finished successfully.
  loader_.reset();
  if (!is_partial_loader_active_) {
    ReadComplete();
    return;
  }

  // For partial loading, continue with next chunk.
  ContinueDownload();
}
```

---

## 4. 测试设计

### 4.1 单元测试

**pdf/loader/document_loader_impl_unittest.cc 新增测试：**

```cpp
class DocumentLoaderImplPushModeTest : public testing::Test {
 protected:
  DocumentLoaderImplPushModeTest() {
    scoped_feature_list_.InitAndEnableFeature(features::kPdfPushBasedLoading);
  }

  base::test::ScopedFeatureList scoped_feature_list_;
};

TEST_F(DocumentLoaderImplPushModeTest, PushModeEnabled) {
  // Verify push mode is enabled when feature flag is on.
  TestClient client;
  DocumentLoaderImpl loader(&client);
  loader.Init(client.CreateFullPageLoader(), "http://url.com");
  // Verify no ReadResponseBody calls are made (push mode doesn't poll).
  EXPECT_FALSE(client.full_page_loader_data()->IsWaitRead());
}

TEST_F(DocumentLoaderImplPushModeTest, DataPushedDirectly) {
  // Test that data is processed immediately when pushed.
  TestClient client;
  DocumentLoaderImpl loader(&client);
  loader.Init(client.CreateFullPageLoader(), "http://url.com");

  // Simulate data push.
  std::vector<uint8_t> data(1024, 'A');
  // Push data directly through the push callback.
  // Verify data is saved without 2ms delay.
}

TEST_F(DocumentLoaderImplPushModeTest, FallbackToPullMode) {
  // Reset feature flag to verify fallback works.
  scoped_feature_list_.Reset();
  scoped_feature_list_.InitAndDisableFeature(features::kPdfPushBasedLoading);

  TestClient client;
  DocumentLoaderImpl loader(&client);
  loader.Init(client.CreateFullPageLoader(), "http://url.com");

  // Verify ReadResponseBody is called (pull mode).
  EXPECT_TRUE(client.full_page_loader_data()->IsWaitRead());
}
```

**pdf/loader/url_loader_unittest.cc 新增测试：**

```cpp
TEST_F(UrlLoaderTest, PushModeDataDelivery) {
  // Test that data is pushed to callback in push mode.
  std::vector<char> received_data;
  Result final_result = Result::kErrorFailed;

  loader_->SetPushModeCallbacks(
      base::BindRepeating([](std::vector<char>* out, base::span<const char> data) {
        out->insert(out->end(), data.begin(), data.end());
      }, &received_data),
      base::BindOnce([](Result* out, Result result) {
        *out = result;
      }, &final_result));

  OpenWithResponse();

  // Simulate data arrival.
  loader_->DidReceiveData(kFakeData);

  // Verify data was pushed directly.
  EXPECT_EQ(received_data.size(), kFakeData.size());

  loader_->DidFinishLoading();
  EXPECT_EQ(final_result, Result::kSuccess);
}

TEST_F(UrlLoaderTest, PullModeBuffering) {
  // Verify pull mode still buffers data when push mode is not enabled.
  OpenWithResponse();

  loader_->DidReceiveData(kFakeData);

  // Data should be in internal buffer, not pushed.
  // Read should return the buffered data.
  std::vector<uint8_t> buffer(100);
  loader_->ReadResponseBody(buffer, base::BindOnce([](int result) {
    EXPECT_EQ(result, static_cast<int>(kFakeData.size()));
  }));
}
```

### 4.2 集成测试

建议在以下场景进行集成测试：

1. **大文件加载性能测试**：比较push和pull模式下加载大PDF文件的时间
2. **部分加载测试**：验证push模式下partial loading功能正常
3. **错误恢复测试**：验证网络错误时push模式能正确回调错误

---

## 5. 方案对比

| 方案 | 优点 | 缺点 | 改动量 | 推荐度 |
|------|------|------|--------|--------|
| 方案一：URLLoaderWrapper层添加Push接口 | 改动适中，保持层次结构 | 接口冗余 | 中 | ★★★ |
| 方案二：UrlLoader层直接添加Push接口 | 减少内存拷贝 | 改动底层组件风险高 | 大 | ★★ |
| **方案三：DocumentLoaderImpl适配** | 改动集中，风险可控 | 需要在多层添加适配代码 | 中 | ★★★★★ |

**推荐方案三**，理由：
1. 改动集中在DocumentLoaderImpl层，便于维护
2. 利用现有的UrlLoader回调机制，不需要修改底层接口
3. Feature flag控制简单清晰
4. 后期移除pull模式时，只需删除相关分支代码

---

## 6. 后期移除Pull模式的考虑

设计中已考虑后期移除pull模式的便利性：

1. **Feature Flag集中控制**：所有push/pull模式的分支都通过`kPdfPushBasedLoading`控制
2. **代码隔离**：push模式的代码使用独立的方法（如`OnDataPushed()`），不与现有pull模式代码混合
3. **移除步骤**：
   - 移除feature flag检查
   - 删除`ReadMore()`、`DidRead()`相关的pull模式代码
   - 删除`URLLoaderWrapperImpl`中的2ms定时器相关代码
   - 移除feature flag声明和定义

---

## 7. 风险评估与缓解措施

| 风险 | 可能性 | 影响 | 缓解措施 |
|------|--------|------|----------|
| Push模式下数据处理不及时导致背压 | 中 | 高 | 监控buffer大小，必要时恢复deferring_loading机制 |
| 部分加载(partial loading)与push模式不兼容 | 低 | 中 | 充分测试partial loading场景 |
| Feature flag切换时状态不一致 | 低 | 中 | 确保Init()时确定模式，运行中不切换 |

---

## 8. 时间线估算

| 阶段 | 任务 | 预计时间 |
|------|------|----------|
| 1 | Feature flag添加 | 0.5天 |
| 2 | UrlLoader push模式支持 | 1天 |
| 3 | URLLoaderWrapperImpl修改 | 1天 |
| 4 | DocumentLoaderImpl适配 | 1.5天 |
| 5 | 单元测试编写 | 1天 |
| 6 | 集成测试与调试 | 1天 |
| 7 | 代码审查与修改 | 1天 |
| **总计** | | **7天** |

---

## 9. 参考资源

- `pdf/loader/url_loader_wrapper_impl.cc:36` - 2ms定时器定义
- `pdf/loader/document_loader_impl.cc:298` - ReadMore()方法
- `pdf/loader/url_loader.cc:211` - DidReceiveData()方法
- `pdf/pdf_features.cc` - 现有feature flag示例
- `pdf/loader/document_loader_impl_unittest.cc` - 现有测试示例
