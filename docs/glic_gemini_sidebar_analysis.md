# Chromium Gemini Sidebar (GLIC) Implementation Analysis Report
# Chromium Gemini 侧边栏 (GLIC) 实现详细分析报告

## Table of Contents / 目录

1. [概述](#1-概述)
2. [本地编译启用 GLIC](#2-本地编译启用-glic)
3. [整体架构](#3-整体架构)
4. [核心组件详解](#4-核心组件详解)
5. [WebUI 前端架构](#5-webui-前端架构)
6. [通信机制](#6-通信机制)
7. [首次运行体验 (FRE)](#7-首次运行体验-fre)
8. [上下文和媒体处理](#8-上下文和媒体处理)
9. [用户偏好设置](#9-用户偏好设置)
10. [指标系统](#10-指标系统)
11. [安全和权限](#11-安全和权限)
12. [关键代码路径](#12-关键代码路径)
13. [总结](#13-总结)

---

## 1. 概述

### 1.1 什么是 GLIC

GLIC (Google Large Information Center) 是 Chromium 中 Gemini 侧边栏的内部代号。它是一个集成在 Chrome 浏览器中的 AI 助手界面，允许用户通过侧边栏或浮动窗口与 Gemini AI 进行交互。

### 1.2 主要功能

- **对话交互**: 用户可以通过文本或语音与 Gemini 进行对话
- **标签页上下文**: 可以获取当前标签页的内容作为 AI 对话的上下文
- **多实例支持**: 支持多个独立的对话实例 (Multi-instance)
- **浮动/侧边栏模式**: 支持附着在浏览器窗口或作为独立浮动窗口
- **Actor 系统**: 支持 AI 在网页上执行自动化操作

### 1.3 代码位置

主要代码位于以下目录：

```
chrome/browser/glic/                    # 核心浏览器端代码
chrome/browser/ui/views/side_panel/glic/ # 侧边栏 UI 视图
chrome/browser/resources/glic/           # WebUI 前端资源
chrome/browser/resources/settings/glic_page/ # 设置页面
```

---

## 2. 本地编译启用 GLIC

### 2.1 能否在本地编译的 Chromium 中启用 GLIC？

**简短回答：技术上可以编译，但无法完全正常使用。**

GLIC 功能在本地编译的 Chromium 中存在以下限制：

### 2.2 编译层面

GLIC 代码默认会被编译到 Chromium 中。在 `chrome/common/features.gni` 中：

```gni
# Enables inclusion of glic in the build.
enable_glic = is_mac || is_win || is_linux || is_chromeos || is_android
```

这意味着在 macOS、Windows、Linux、ChromeOS 和 Android 平台上，GLIC 代码会自动包含在编译中。

### 2.3 功能开关层面

主要的功能开关 `kGlic` 默认是**禁用**的：

```cpp
// chrome/common/chrome_features.cc
BASE_FEATURE(kGlic, base::FEATURE_DISABLED_BY_DEFAULT);
```

要启用该功能，需要通过命令行参数启动 Chrome：

```bash
./chrome --enable-features=Glic
```

### 2.4 为什么无法完全正常使用

即使通过命令行启用了 `kGlic` 功能开关，仍有以下关键限制：

#### 2.4.1 账户能力检查 (Account Capability)

GLIC 需要检查用户账户的 `can_use_gemini_in_chrome` 或 `can_use_model_execution_features` 能力：

```cpp
// chrome/browser/glic/public/glic_enabling.cc
signin::Tribool capability_value =
    primary_account.capabilities.can_use_model_execution_features();
if (base::FeatureList::IsEnabled(
        switches::kGlicEligibilitySeparateAccountCapability) &&
    (CanUseGeminiInChrome(primary_account.capabilities) !=
     signin::Tribool::kUnknown)) {
  capability_value = CanUseGeminiInChrome(primary_account.capabilities);
}
result.primary_account_not_capable =
    (capability_value != signin::Tribool::kTrue);
```

这些能力是由 Google 服务端根据用户账户类型和订阅状态返回的，**普通用户账户可能没有这些权限**。

#### 2.4.2 国家/地区限制

默认情况下，GLIC 仅在特定国家/地区启用：

```cpp
// Default enabled countries
constexpr char kDefaultEnabledCountries[] = "us,ca";

// Default enabled locales
constexpr char kDefaultEnabledLocales[] = "en-us";
```

可以通过禁用地区过滤来绕过：

```bash
./chrome --enable-features=Glic --disable-features=GlicCountryFiltering,GlicLocaleFiltering
```

#### 2.4.3 Web Client URL 依赖

GLIC 的 Web Client 加载自 Google 托管的 URL：

```cpp
const base::FeatureParam<std::string> kGlicGuestURL{
    &kGlicURLConfig, "glic-guest-url", "https://gemini.google.com/glic"};
```

这个 URL 是 Google 内部服务，**需要适当的认证和权限才能访问**，普通用户无法直接访问。

#### 2.4.4 用户状态检查

如果启用了 `kGlicUserStatusCheck`，还会向 Google 服务器验证用户状态：

```cpp
if (base::FeatureList::IsEnabled(features::kGlicUserStatusCheck)) {
    // Check cached user status from server
    switch (cached_user_status->user_status_code) {
        case UserStatusCode::DISABLED_BY_ADMIN:
            result.disallowed_by_remote_admin = true;
            break;
        // ...
    }
}
```

### 2.5 开发者测试选项

对于 Chromium 开发者/贡献者，可以使用以下方式进行测试：

#### 2.5.1 绕过启用检查 (仅限测试)

在测试代码中可以使用：

```cpp
GlicEnabling::SetBypassEnablementChecksForTesting(true);
```

#### 2.5.2 完整的开发模式启动参数

```bash
./chrome \
    --enable-features=Glic,GlicDebugWebview \
    --disable-features=GlicCountryFiltering,GlicLocaleFiltering,GlicUserStatusCheck
```

注意：即使使用这些参数，由于 Web Client URL 的限制，功能仍然无法完整工作。

### 2.6 总结

| 项目 | 状态 |
|------|------|
| 代码编译 | ✅ 可以 - 默认包含在编译中 |
| 功能开关启用 | ⚠️ 需要命令行参数 |
| 账户能力验证 | ❌ 需要 Google 账户特定权限 |
| Web Client 访问 | ❌ 需要访问 Google 内部服务 |
| 完整功能使用 | ❌ 无法正常使用 |

**结论**：本地编译的 Chromium 可以包含 GLIC 代码并启用功能开关，但由于依赖 Google 服务端的账户验证和 Web Client 托管服务，**普通开发者无法使用完整功能**。GLIC 本质上是 Google Chrome 的专有功能，依赖 Google 的后端服务。

---

## 3. 整体架构

### 2.1 架构图

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Chrome Browser                               │
├─────────────────────────────────────────────────────────────────────┤
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │                    GlicKeyedService                          │    │
│  │  (Profile级别的服务，管理所有Glic相关功能)                      │    │
│  └────────────┬───────────────────────────┬─────────────────────┘    │
│               │                           │                          │
│  ┌────────────▼────────────┐  ┌──────────▼──────────────┐           │
│  │  GlicWindowController   │  │    GlicFreController     │           │
│  │  (窗口控制器)            │  │    (首次运行体验)         │           │
│  └────────────┬────────────┘  └─────────────────────────┘           │
│               │                                                      │
│  ┌────────────▼────────────────────────────────────────────┐        │
│  │                      Host                                │        │
│  │  (管理WebUI内容和与Web Client的通信)                       │        │
│  └────────────┬────────────────────────────────────────────┘        │
│               │                                                      │
│  ┌────────────▼────────────────────────────────────────────┐        │
│  │              GlicPageHandler (Mojo)                      │        │
│  │  (WebUI页面处理器，实现Mojo接口)                           │        │
│  └────────────┬────────────────────────────────────────────┘        │
│               │                                                      │
├───────────────┼──────────────────────────────────────────────────────┤
│               │          Mojo IPC                                    │
├───────────────┼──────────────────────────────────────────────────────┤
│  ┌────────────▼────────────────────────────────────────────┐        │
│  │           chrome://glic (WebUI)                          │        │
│  │  ┌──────────────────────────────────────────────────┐   │        │
│  │  │           GlicAppController                       │   │        │
│  │  │  (TypeScript前端控制器)                            │   │        │
│  │  └────────────┬─────────────────────────────────────┘   │        │
│  │               │                                          │        │
│  │  ┌────────────▼─────────────────────────────────────┐   │        │
│  │  │        WebviewController                          │   │        │
│  │  │  (<webview>标签管理器)                             │   │        │
│  │  └────────────┬─────────────────────────────────────┘   │        │
│  │               │ postMessage                              │        │
│  │  ┌────────────▼─────────────────────────────────────┐   │        │
│  │  │     Gemini Web Client (第三方页面)                 │   │        │
│  │  │  (实际的Gemini AI界面)                             │   │        │
│  │  └──────────────────────────────────────────────────┘   │        │
│  └──────────────────────────────────────────────────────────┘        │
└─────────────────────────────────────────────────────────────────────┘
```

### 2.2 核心类关系

```
GlicKeyedService (KeyedService)
    ├── GlicEnabling              # Feature flags and permission control
    ├── GlicMetrics               # Metrics collection
    ├── GlicFreController         # First Run Experience control
    ├── GlicWindowController      # Window management
    │   └── HostManager           # Host manager
    │       └── Host              # WebUI content container
    │           └── GlicPageHandler # Mojo interface handler
    ├── GlicSharingManager        # Tab context sharing
    └── AuthController            # Authentication control
```

---

## 4. 核心组件详解

### 13.1 GlicKeyedService

**文件位置**: `chrome/browser/glic/public/glic_keyed_service.h`

GlicKeyedService 是整个 GLIC 系统的入口点，它是一个 Profile 级别的服务（每个用户配置文件一个实例）。

**主要职责**:
- 管理 GLIC 的生命周期
- 协调各子系统之间的通信
- 处理用户操作（打开、关闭、切换等）
- 管理标签页上下文共享

**关键方法**:
```cpp
// Toggle UI display state
virtual void ToggleUI(BrowserWindowInterface* bwi,
                      bool prevent_close,
                      mojom::InvocationSource source,
                      std::optional<std::string> prompt_suggestion);

// Close floating panel
virtual void CloseFloatingPanel();

// Check if window is showing
virtual bool IsWindowShowing() const;

// Get instance for active tab
GlicInstance* GetInstanceForActiveTab(BrowserWindowInterface* bwi);
```

### 13.2 GlicWindowController

**文件位置**: `chrome/browser/glic/widget/glic_window_controller.h`

GlicWindowController 负责管理 GLIC 窗口的状态和行为。

**窗口状态**:
```cpp
enum class State {
    kClosed,                  // Closed state
    kWaitingForGlicToLoad,    // Waiting for load
    kOpen,                    // Open state
    kDetaching,               // Detaching
    kWaitingForSidePanelToShow, // Waiting for side panel to show
};
```

**主要功能**:
- 控制窗口的显示/隐藏
- 管理附着/分离状态
- 处理窗口位置和大小
- 管理多实例

### 13.3 GlicSidePanelCoordinatorImpl

**文件位置**: `chrome/browser/ui/views/side_panel/glic/glic_side_panel_coordinator_impl.h`

这是侧边栏模式的核心协调器，负责将 GLIC 集成到 Chrome 的侧边栏系统中。

**关键实现**:
```cpp
class GlicSidePanelCoordinatorImpl : public GlicSidePanelCoordinator,
                                     public SidePanelEntryObserver {
public:
    void Show(bool suppress_animations) override;
    void Close(const CloseOptions& options) override;
    bool IsShowing() const override;
    State state() override;
    
    // Create and register side panel entry
    void CreateAndRegisterEntry();
    
    // Create view
    std::unique_ptr<views::View> CreateView(SidePanelEntryScope& scope);
};
```

**状态管理**:
```cpp
enum class State {
    kShown,        // Foreground display
    kBackgrounded, // Background (state preserved on tab switch)
    kClosed,       // Closed
};
```

### 13.4 Host

**文件位置**: `chrome/browser/glic/host/host.h`

Host 类是 WebUI 内容的容器，管理 chrome://glic 页面和与 Web Client 的通信。

**主要接口**:
```cpp
class Host : public GlicSharingManagerProvider {
public:
    // Embedder delegate interface
    class EmbedderDelegate {
        virtual void Resize(const gfx::Size& size, ...) = 0;
        virtual void EnableDragResize(bool enabled) = 0;
        virtual void Attach() = 0;
        virtual void Detach() = 0;
        virtual void ClosePanel() = 0;
    };
    
    // Instance delegate interface
    class InstanceDelegate {
        virtual tabs::TabInterface* CreateTab(...) = 0;
        virtual void CreateTask(...) = 0;
        virtual void PerformActions(...) = 0;
    };
    
    // Panel lifecycle
    void PanelWillOpen(...);
    void PanelWasClosed();
    void Shutdown();
};
```

### 13.5 GlicEnabling

**文件位置**: `chrome/browser/glic/public/glic_enabling.h`

GlicEnabling 类负责检查和管理 GLIC 功能的启用状态。

**启用条件检查**:
```cpp
class GlicEnabling {
public:
    // Global feature flag
    static bool IsEnabledByFlags();
    
    // Check if profile is eligible
    static bool IsProfileEligible(const Profile* profile);
    
    // Check if enabled for current profile
    static bool IsEnabledForProfile(Profile* profile);
    
    // Check if FRE is completed
    static bool HasConsentedForProfile(Profile* profile);
    
    // Check if ready
    static bool IsReadyForProfile(Profile* profile);
};
```

**启用状态结构**:
```cpp
struct ProfileEnablement {
    bool feature_disabled : 1 = false;
    bool not_regular_profile : 1 = false;
    bool not_rolled_out : 1 = false;
    bool primary_account_not_capable : 1 = false;
    bool disallowed_by_chrome_policy : 1 = false;
    bool disallowed_by_remote_admin : 1 = false;
    // ...
};
```

---

## 5. WebUI 前端架构

### 13.1 主要文件结构

```
chrome/browser/resources/glic/
├── glic.html              # 主 HTML 页面
├── glic.css               # 样式表
├── main.ts                # 入口文件
├── glic_app_controller.ts # 主控制器
├── browser_proxy.ts       # 浏览器通信代理
├── webview.ts             # WebView 控制器
├── observable.ts          # 响应式数据
├── glic_api/              # Glic API 定义
├── glic_api_impl/         # API 实现
│   ├── host/              # 宿主端实现
│   │   ├── glic_api_host.ts
│   │   ├── host_from_client.ts
│   │   └── host_to_client.ts
│   └── client/            # 客户端实现
└── fre/                   # 首次运行体验
```

### 13.2 GlicAppController

**文件位置**: `chrome/browser/resources/glic/glic_app_controller.ts`

GlicAppController 是前端的主控制器，管理 UI 状态和用户交互。

**UI 状态机**:
```typescript
enum WebUiState {
    kBeginLoad,      // Begin loading
    kShowLoading,    // Show loading indicator
    kHoldLoading,    // Hold loading animation
    kFinishLoading,  // Finish loading
    kError,          // Error state
    kOffline,        // Offline state
    kUnavailable,    // Unavailable
    kDisabledByAdmin,// Disabled by admin
    kReady,          // Ready
    kUnresponsive,   // Unresponsive
    kSignIn,         // Sign-in required
    kGuestError,     // Guest error
}
```

**面板类型**:
```typescript
type PanelId = 
    'loadingPanel' |   // Loading panel
    'guestPanel' |     // Guest panel (Gemini page)
    'offlinePanel' |   // Offline panel
    'errorPanel' |     // Error panel
    'unavailablePanel' | // Unavailable panel
    'disabledByAdminPanel' | // Disabled by admin panel
    'signInPanel';     // Sign-in panel
```

**关键方法**:
```typescript
class GlicAppController implements WebviewDelegate, ApiHostEmbedder {
    // State management
    private setState(newState: WebUiState): void;
    
    // Loading flow
    private async load(): Promise<void>;
    private beginLoad(): void;
    private showLoading(): void;
    
    // WebView delegate implementation
    webviewUnresponsive(): void;
    webviewError(reason: string): void;
    webviewPageCommit(type: PageType): void;
    
    // API host embedder implementation
    onGuestResizeRequest(request: {width: number, height: number}): void;
    enableDragResize(enabled: boolean): void;
    webClientReady(): void;
}
```

### 13.3 WebviewController

**文件位置**: `chrome/browser/resources/glic/webview.ts`

WebviewController 管理嵌入的 `<webview>` 元素，负责加载和控制 Gemini Web Client。

**关键实现**:
```typescript
class WebviewController {
    webview: chrome.webviewTag.WebView;
    private host?: GlicApiHost;
    private communicator?: GlicApiCommunicator;
    
    constructor(
        container: HTMLElement,
        browserProxy: BrowserProxyImpl,
        delegate: WebviewDelegate,
        hostEmbedder: ApiHostEmbedder,
        persistentState: WebviewPersistentState
    ) {
        this.webview = document.createElement('webview');
        this.webview.setAttribute('partition', 'persist:glicpart');
        // Set up event listeners
        this.eventTracker.add(this.webview, 'loadcommit', ...);
        this.eventTracker.add(this.webview, 'contentload', ...);
        this.webview.src = this.persistentState.useLoadUrl();
    }
}
```

**页面类型检测**:
```typescript
type PageType =
    'login' |       // Login page
    'regular' |     // Regular page
    'guestError' |  // Guest error
    'guestCaaError' | // CAA error
    'loadError';    // Load error
```

### 13.4 GlicApiHost

**文件位置**: `chrome/browser/resources/glic/glic_api_impl/host/glic_api_host.ts`

GlicApiHost 是 Glic API 的宿主端实现，处理来自 Web Client 的请求。

**Web Client 状态**:
```typescript
enum WebClientState {
    UNINITIALIZED,  // Uninitialized
    RESPONSIVE,     // Responsive
    UNRESPONSIVE,   // Unresponsive
    ERROR,          // Error (final state)
}
```

**通信机制**:
```typescript
class GlicApiHost implements PostMessageRequestHandler {
    // Handle requests from Web Client
    async handleRawRequest(type: string, payload: any, extras: ResponseExtras);
    
    // Send messages to Web Client
    sender: GatedSender;
    
    // Responsiveness check
    async responsiveCheckLoop();
    
    // Link handling
    openLinkInPopup(url: string, initialWidth: number, initialHeight: number);
    async openLinkInNewTab(url: string);
}
```

---

## 6. 通信机制

### 13.1 Mojo IPC

Chromium 使用 Mojo 进行进程间通信。GLIC 定义了丰富的 Mojo 接口。

**接口定义文件**: `chrome/browser/glic/host/glic.mojom`

**主要接口**:

```
// Page handler interface
interface PageHandler {
    // WebUI state change notification
    WebUiStateChanged(WebUiState state);
    
    // Create Web Client
    CreateWebClient(pending_receiver<WebClientHandler> handler);
    
    // Resize widget
    ResizeWidget(gfx.mojom.Size size, mojo_base.mojom.TimeDelta duration);
    
    // Close panel
    ClosePanel();
    
    // Attach/Detach
    AttachPanel();
    DetachPanel();
};

// Web Client handler interface
interface WebClientHandler {
    // Create tab
    CreateTab(url.mojom.Url url, bool open_in_background, int32? window_id)
        => (TabData? tab);
    
    // Get tab context
    GetTabContext(TabData tab, GetTabContextOptions options)
        => (TabContextResult result);
    
    // Actor task related
    CreateTask(TaskOptions options) => (TaskCreateResult result);
    PerformActions(array<uint8> actions_proto) => (ActionResultProto result);
};
```

### 13.2 PostMessage 通信

WebUI 和 Web Client 之间使用 PostMessage 进行通信。

**通信流程**:
```
┌────────────────┐     postMessage      ┌────────────────┐
│ GlicApiHost    │ ◄───────────────────► │ Web Client     │
│ (chrome://glic)│                       │ (Gemini page)  │
└────────────────┘                       └────────────────┘
```

**引导过程**:
```typescript
class GlicApiCommunicator {
    // Periodically send bootstrap ping
    private bootstrapPing() {
        this.windowProxy.postMessage(
            {
                type: 'glic-bootstrap',
                glicApiSource: loadTimeData.getString('glicGuestAPISource'),
            },
            this.embeddedOrigin
        );
    }
}
```

**请求处理**:
```typescript
class GlicApiHost implements PostMessageRequestHandler {
    async handleRawRequest(type: string, payload: any, extras: ResponseExtras) {
        const handlerFunction = (this.messageHandler as any)[type];
        if (typeof handlerFunction !== 'function') {
            console.warn(`Unknown message type ${type}`);
            return;
        }
        return await handlerFunction.call(this.messageHandler, payload, extras);
    }
}
```

---

## 7. 首次运行体验 (FRE)

### 13.1 FRE 控制器

**文件位置**: `chrome/browser/glic/fre/glic_fre_controller.h`

GlicFreController 管理首次运行体验流程。

**FRE 状态**:
```cpp
enum class FreStatus {
    kNotStarted = 0,   // Not started
    kCompleted = 1,    // Completed
    kIncomplete = 2,   // Incomplete (user closed)
};
```

**关键方法**:
```cpp
class GlicFreController {
public:
    // Check if FRE dialog should be shown
    bool ShouldShowFreDialog();
    
    // Show FRE dialog
    void ShowFreDialog(BrowserWindowInterface* browser, mojom::InvocationSource source);
    
    // Accept FRE
    void AcceptFre(GlicFrePageHandler* handler);
    
    // Reject FRE
    void RejectFre();
};
```

### 13.2 FRE 流程

1. 用户首次点击 GLIC 入口
2. 检查 `ShouldShowFreDialog()` 返回 true
3. 显示 FRE 对话框
4. 用户同意后调用 `AcceptFre()`
5. 设置 `kGlicCompletedFre` 偏好为 `kCompleted`
6. 打开 GLIC 主界面

---

## 8. 上下文和媒体处理

### 13.1 标签页上下文共享

**核心组件**:

| 类名 | 职责 |
|------|------|
| `GlicSharingManager` | 上下文共享的总体管理 |
| `GlicFocusedTabManager` | 管理当前焦点标签页 |
| `GlicPinnedTabManager` | 管理固定的标签页 |
| `GlicPageContextFetcher` | 获取页面内容 |
| `GlicScreenshotCapturer` | 截图功能 |

**上下文获取流程**:
```cpp
// 1. Get tab data
GlicTabData tabData = GetTabData(tab);

// 2. Get page context
GetTabContext(tab, options) {
    // Get page text content
    // Get screenshot
    // Get PDF content
    return TabContextResult;
}
```

### 13.2 媒体集成

**文件位置**: `chrome/browser/glic/media/`

**主要类**:
- `GlicMediaContext`: 媒体上下文管理
- `GlicMediaIntegration`: 媒体集成
- `MediaTranscriptProviderImpl`: 媒体转录提供者

---

## 9. 用户偏好设置

### 13.1 偏好设置定义

**文件位置**: `chrome/browser/glic/glic_pref_names.h`

**本地状态偏好** (Local State):
```cpp
// Launcher toggle
kGlicLauncherEnabled = "glic.launcher_enabled"

// Hotkey configuration
kGlicLauncherHotkey = "glic.launcher_hotkey"
kGlicFocusToggleHotkey = "glic.focus_toggle_hotkey"
```

**Profile 偏好**:
```cpp
// Toolbar pinning state
kGlicPinnedToTabstrip = "glic.pinned_to_tabstrip"

// Permission settings
kGlicGeolocationEnabled = "glic.geolocation_enabled"
kGlicMicrophoneEnabled = "glic.microphone_enabled"
kGlicTabContextEnabled = "glic.tab_context_enabled"

// FRE status
kGlicCompletedFre = "glic.completed_fre"

// Window position memory
kGlicPreviousPositionX = "glic.previous_bounds.x"
kGlicPreviousPositionY = "glic.previous_bounds.y"

// Enterprise policy
kGlicActuationOnWeb = "glic.actuation_on_web"
```

### 13.2 设置页面

**文件位置**: `chrome/browser/resources/settings/glic_page/`

设置页面允许用户配置:
- 标签页上下文权限
- 地理位置权限
- 麦克风权限
- Actor 功能开关

---

## 10. 指标系统

### 13.1 GlicMetrics

**文件位置**: `chrome/browser/glic/glic_metrics.h`

**收集的指标**:

```cpp
// Response segmentation
enum class ResponseSegmentation {
    kOsButtonAttachedText,     // OS button + attached + text
    kOsButtonDetachedAudio,    // OS button + detached + audio
    kButtonTopChromeAttachedText, // Top chrome button + attached + text
    // ...
};

// Window position
enum class DisplayPosition {
    kTopLeft,
    kCenterCenter,
    kBottomRight,
    // ...
};

// Entry point status
enum class EntryPointStatus {
    kBeforeFreNotEligible,
    kBeforeFreAndEligible,
    kAfterFreBrowserOnly,
    // ...
};
```

**关键事件**:
```cpp
void OnUserInputSubmitted(mojom::WebClientMode mode);
void OnResponseStarted();
void OnResponseStopped(mojom::ResponseStopCause cause);
void OnGlicWindowStartedOpening(bool attached, mojom::InvocationSource source);
void OnGlicWindowClose(...);
```

---

## 11. 安全和权限

### 13.1 权限检查

```cpp
// Geolocation permission
async shouldAllowGeolocationPermissionRequest(): Promise<boolean>;

// Media permission
async shouldAllowMediaPermissionRequest(): Promise<boolean>;
```

### 13.2 URL 白名单

WebView 只允许加载特定来源的 URL：

```typescript
function urlMatchesAllowedOrigin(url: string) {
    // Dev mode allows all
    if (loadTimeData.getBoolean('devMode')) {
        return true;
    }
    
    // Check if matches glicGuestURL origin
    const defaultUrl = new URL(loadTimeData.getString('glicGuestURL'));
    if (matcherForOrigin(defaultUrl.origin)?.test(url)) {
        return true;
    }
    
    // Check whitelist
    return loadTimeData.getString('glicAllowedOrigins')
        .split(' ')
        .some(origin => matcherForOrigin(origin.trim())?.test(url));
}
```

### 13.3 企业策略

```cpp
// Enterprise admin can disable GLIC
enum class SettingsPolicyState {
    kEnabled = 0,
    kDisabled = 1,
};

// Actor feature control
enum class GlicActuationOnWebPolicyState {
    kEnabled = 0,
    kDisabled = 1,
};
```

---

## 12. 关键代码路径

### 13.1 打开 GLIC 侧边栏

```
1. 用户点击 GLIC 按钮
   └── GlicKeyedService::ToggleUI()
       └── GlicWindowController::Toggle()
           └── GlicSidePanelCoordinatorImpl::Show()
               └── CreateAndRegisterEntry()
               └── SidePanelCoordinator::Show()
                   └── CreateView()
                       └── Host::CreateContents()
                           └── 加载 chrome://glic
                               └── GlicAppController 初始化
                                   └── WebviewController 创建
                                       └── 加载 Gemini Web Client
```

### 13.2 用户发送消息

```
1. 用户在 Gemini 页面输入消息
   └── Web Client 调用 API
       └── postMessage → GlicApiHost::handleRawRequest()
           └── HostMessageHandler 处理请求
               └── Mojo → GlicPageHandler
                   └── 调用相应的浏览器功能
```

### 13.3 获取标签页上下文

```
1. Web Client 请求标签页上下文
   └── glicBrowserGetTabContext
       └── GlicPageHandler::GetTabContext()
           └── GlicPageContextFetcher::FetchContext()
               └── 获取 innerText
               └── 获取截图
               └── 返回 TabContextResult
```

---

## 13. 总结

### 13.1 架构特点

1. **模块化设计**: 各组件职责明确，通过接口解耦
2. **多层通信**: Mojo (C++ ↔ WebUI) + PostMessage (WebUI ↔ Web Client)
3. **状态机驱动**: UI 和窗口状态通过状态机管理
4. **权限控制**: 多层权限检查（功能开关、Profile、企业策略）
5. **可观察模式**: 使用回调列表实现状态变更通知

### 13.2 技术亮点

- **WebView 隔离**: 使用 `<webview>` 标签隔离第三方内容
- **响应性监控**: 定期检查 Web Client 响应性
- **优雅降级**: 各种错误状态有对应的 UI 反馈
- **多实例支持**: 支持多个独立的对话实例

### 13.3 关键文件速查表

| 功能 | 文件路径 |
|------|----------|
| 服务入口 | `chrome/browser/glic/public/glic_keyed_service.h` |
| 侧边栏协调器 | `chrome/browser/ui/views/side_panel/glic/glic_side_panel_coordinator_impl.h` |
| 窗口控制器 | `chrome/browser/glic/widget/glic_window_controller.h` |
| Host | `chrome/browser/glic/host/host.h` |
| 功能开关 | `chrome/browser/glic/public/glic_enabling.h` |
| 前端控制器 | `chrome/browser/resources/glic/glic_app_controller.ts` |
| WebView 管理 | `chrome/browser/resources/glic/webview.ts` |
| API 宿主 | `chrome/browser/resources/glic/glic_api_impl/host/glic_api_host.ts` |
| Mojo 接口 | `chrome/browser/glic/host/glic.mojom` |
| 偏好设置 | `chrome/browser/glic/glic_pref_names.h` |
| 指标 | `chrome/browser/glic/glic_metrics.h` |
| FRE | `chrome/browser/glic/fre/glic_fre_controller.h` |

---

*本文档基于 Chromium 代码库的分析生成，版权归 The Chromium Authors 所有。*
