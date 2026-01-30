# PDF Push-Based Resource Loading Design Document

## 目标

将PDF的资源加载逻辑从当前的pull方式修改为push方式，同时去掉人为添加的2ms定时器和中间不必要的临时缓存，以提高性能。

## 当前实现分析（Pull方式）

### 数据流

```
网络层(Blink) --> UrlLoader --> URLLoaderWrapperImpl --> DocumentLoaderImpl --> PDFium
                    |                  |                        |
                 缓存数据          2ms定时器                  临时缓存
               (circular_deque)   (read_starter_)            (buffer_)
```

### 关键组件和代码位置

#### 1. 2ms定时器
- **文件**: `pdf/loader/url_loader_wrapper_impl.cc`
- **代码行**: 第36行
```cpp
constexpr base::TimeDelta kReadDelayMs = base::Milliseconds(2);
```
- **用途**: 第165-168行，在`ReadResponseBody`中使用，延迟读取响应体以防止阻塞UI线程

#### 2. UrlLoader中间缓存
- **文件**: `pdf/loader/url_loader.h`
- **代码行**: 第195行
```cpp
base::circular_deque<uint8_t> buffer_;
```
- **用途**: 存储从网络接收的数据，等待上层调用`ReadResponseBody`来读取

#### 3. DocumentLoaderImpl临时缓存
- **文件**: `pdf/loader/document_loader_impl.cc`
- **代码行**: 第36行、第77行
```cpp
constexpr size_t kReadBufferSize = 256 * 1024;  // 256KB
buffer_(kReadBufferSize)  // 构造函数中初始化
```
- **用途**: 第298-301行的`ReadMore`函数中用于接收URL loader读取的数据

### Pull方式的工作流程

1. `UrlLoader::DidReceiveData()` 接收网络数据，存入 `buffer_` (url_loader.cc:211-228)
2. `DocumentLoaderImpl::ReadMore()` 调用 `loader_->ReadResponseBody()` 发起读取请求 (document_loader_impl.cc:298-302)
3. `URLLoaderWrapperImpl::ReadResponseBody()` 使用 `read_starter_` 定时器延迟2ms后执行实际读取 (url_loader_wrapper_impl.cc:161-169)
4. `URLLoaderWrapperImpl::ReadResponseBodyImpl()` 调用底层loader读取数据 (url_loader_wrapper_impl.cc:171-176)
5. `UrlLoader::RunReadCallback()` 从 `buffer_` 复制数据到调用者的缓冲区 (url_loader.cc:275-303)
6. `DocumentLoaderImpl::DidRead()` 处理读取的数据 (document_loader_impl.cc:304-331)
7. `DocumentLoaderImpl::SaveBuffer()` 将数据保存到chunk流中 (document_loader_impl.cc:333-374)

## 新实现设计（Push方式）

### 设计目标

1. 消除2ms定时器延迟
2. 消除不必要的中间缓存拷贝
3. 支持通过feature flag控制pull/push模式切换
4. 保持向后兼容性，便于回退

### 数据流（Push方式）

```
网络层(Blink) --> UrlLoader --> URLLoaderWrapperImpl --> DocumentLoaderImpl --> PDFium
                    |                                          |
              直接推送数据                                   直接存储
            (无中间缓存)                                  (无临时缓存)
```

### Feature Flag设计

- **文件**: `pdf/pdf_features.h`, `pdf/pdf_features.cc`
- **Feature名称**: `kPdfPushBasedLoading`
- **默认状态**: `FEATURE_DISABLED_BY_DEFAULT`（保守起见，默认使用现有pull方式）

```cpp
// pdf/pdf_features.h
BASE_DECLARE_FEATURE(kPdfPushBasedLoading);

// pdf/pdf_features.cc
BASE_FEATURE(kPdfPushBasedLoading, base::FEATURE_DISABLED_BY_DEFAULT);
```

### 接口修改

#### 1. URLLoaderWrapper接口扩展

在 `pdf/loader/url_loader_wrapper.h` 中添加push模式支持：

```cpp
class URLLoaderWrapper {
 public:
  // 数据推送回调接口
  using DataPushedCallback =
      base::RepeatingCallback<void(base::span<const uint8_t>)>;
  // 完成回调接口（result为0表示成功，负数表示错误）
  using LoadingCompleteCallback = base::OnceCallback<void(int result)>;

  // 启用push模式，设置数据接收回调
  virtual void EnablePushMode(DataPushedCallback data_callback,
                              LoadingCompleteCallback complete_callback) {}

  // 返回是否启用了push模式
  virtual bool IsPushModeEnabled() const { return false; }

  // ... 现有接口保持不变
};
```

#### 2. URLLoaderWrapperImpl实现修改

在 `pdf/loader/url_loader_wrapper_impl.h` 和 `.cc` 中：

- 新增 push 模式相关成员变量
- 实现 `EnablePushMode()` 和 `IsPushModeEnabled()`
- Push模式下绕过2ms定时器，直接调用数据回调

#### 3. UrlLoader修改

在 `pdf/loader/url_loader.h` 和 `.cc` 中：

- 新增 `EnablePushMode()` 方法和相关成员变量
- `DidReceiveData()` 在push模式下直接调用数据回调，绕过内部buffer
- `DidFinishLoading()` 和 `DidFail()` 在push模式下调用完成回调

#### 4. DocumentLoaderImpl修改

在 `pdf/loader/document_loader_impl.h` 和 `.cc` 中：

- 根据feature flag选择使用pull或push模式
- 新增 `is_push_mode_enabled()` 访问器
- 新增 `SetupPushModeIfEnabled()` 设置push模式
- 新增 `OnDataPushed()` 处理推送的数据
- 新增 `OnLoadingComplete()` 处理加载完成
- Push模式下：
  - 设置数据回调到URLLoaderWrapper
  - 数据直接从回调写入chunk_stream，绕过中间buffer_
  - `ReadMore()` 在push模式下直接返回（不发起读取请求）
- Pull模式下：保持现有逻辑不变

### 代码结构

为保持代码清晰和便于后期移除pull模式：

1. Pull模式相关代码保持在原有位置
2. Push模式代码添加feature flag判断
3. 使用清晰的命名区分两种模式的方法

### 测试策略

1. **保留现有测试**：确保pull模式继续正常工作
2. **添加push模式测试**：
   - 基本数据接收测试
   - 大文件分块接收测试
   - 错误处理测试
   - Feature flag切换测试

## 实现步骤

1. ✅ 添加feature flag `kPdfPushBasedLoading`
2. ✅ 扩展 `URLLoaderWrapper` 接口
3. ✅ 实现 `URLLoaderWrapperImpl` 的push模式支持
4. ✅ 修改 `UrlLoader` 支持push模式数据传递
5. ✅ 修改 `DocumentLoaderImpl` 使用push模式
6. ✅ 添加单元测试
7. ✅ 代码审查和安全检查

## 风险和缓解措施

| 风险 | 缓解措施 |
|------|----------|
| Push模式可能导致UI线程阻塞 | 使用feature flag，发现问题可快速回退 |
| 数据处理时序问题 | 完善单元测试覆盖各种场景 |
| 兼容性问题 | 默认禁用push模式，逐步灰度发布 |

## 代码位置参考

| 文件 | 关键代码行 | 说明 |
|------|-----------|------|
| `url_loader_wrapper_impl.cc` | L36 | 2ms定时器常量 |
| `url_loader_wrapper_impl.cc` | L165-168 | 使用定时器延迟读取 |
| `url_loader.h` | L195 | 中间缓存定义 |
| `url_loader.cc` | L211-228 | 数据接收和缓存 |
| `document_loader_impl.cc` | L36, L77 | 临时缓存定义 |
| `document_loader_impl.cc` | L298-302 | ReadMore发起读取 |
| `pdf_features.h` | L22-34 | Feature flag声明 |
| `pdf_features.cc` | L18-58 | Feature flag实现 |
