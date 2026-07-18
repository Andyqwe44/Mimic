# CLAUDE.md — monitor_web 前端

## 铁律：设计令牌统一管理 — `src/lib/design.ts`

所有可复用的 UI 常量（尺寸、间距、字号、圆角、组件预设）**必须**定义在 `src/lib/design.ts` 中。
组件引用 `design.ts` 的导出常量，**禁止**在 `.tsx` 中手写 magic Tailwind 值。

| 类别 | 导出 | 示例 |
|------|------|------|
| 高度 | `H` | `H.control` = `h-7` (28px) |
| 按钮宽度 | `BTN_SIZE_CLASS`, `btnAutoSize()` | `BTN_SIZE_CLASS.sm` = `w-20` |
| 图标 | `H.iconSm`, `H.icon`, `H.iconLg` | `w-3.5 h-3.5` / `w-4 h-4` / `w-5 h-5` |
| 弹窗宽度 | `MODAL_W` | `MODAL_W.picker` = `w-[520px]` |
| 间距 | `GAP`, `PAD`, `PAD_X`, `PAD_Y` | `GAP.md` = `gap-2` |
| 字号 | `TEXT` | `TEXT.xs` = `text-xs` |
| 圆角 | `RADIUS` | `RADIUS.lg` = `rounded-lg` |
| 组件预设 | `MODAL_CARD`, `DIFF_CONTAINER`, `DIFF_COL` | 弹窗卡片、diff 折叠条列宽 |
| 软色填充 | `SOFT`, `NAV.pillBg` | `bg-accent-soft` / `ring-accent-ring`（rgba，禁 `/N` opacity） |

**原则**：不改布局，只改引用源。新组件必须用 design.ts；旧组件渐进迁移。

**Android WebView**：禁止 `bg-accent/10` 等 Tailwind opacity 修饰（底层 `color-mix` 会变成实心色块）。用主题软色 `bg-accent-soft`、`bg-success-soft`、`ring-accent-ring` 等（见 `index.css` + `SOFT`）。

---

## 铁律：禁止 className 静默覆盖组件内部样式

### 问题本质

Tailwind 是全局原子 CSS——所有 `h-7` 共用同一条 CSS 规则。当组件内部写了 `h-7`、调用方又通过 `className` 传入 `h-8`，最终谁生效**不取决于** HTML class 属性的书写顺序，而取决于 Tailwind 生成的 CSS 文件中 `.h-7` 和 `.h-8` 谁先声明。这个声明顺序受 Tailwind 的 `source` 扫描路径、文件扫描先后、类名在源码中首次出现的位置三者共同决定——**每次构建都可能不同**。

这导致：两处用了同一个组件，看着不一样；改了组件内部样式，某个调用方没变；TypeScript 零错误，肉眼才能发现。任何传入 `className` 的地方都可能成为覆盖源。

### 排查范围

`src/` 下所有组件，凡接受 `className` prop 的——包括但不限于：

- `ActionBtn`（Toolkit.tsx）
- `Tooltip`（Toolkit.tsx）
- 所有 `ScreenshotPanel`、`LogPanel`、`ConnectionPanel`、`MonitorView`、`TopBar`、`BottomBar`、`TargetPickerModal` 等
- 任何 `div` / `span` / `button` 被封装成组件后暴露了 `className`
- `index.css` 中的自定义 CSS 类与 Tailwind 原子类的优先级冲突

### 症状

- 同一组件在两处渲染不一致（高/宽/间距/颜色不同）
- 改了组件内部样式，某页面纹丝不动
- 浏览器 DevTools Computed 面板显示预期值被另一条规则覆盖
- 删除某个"看起来没用"的 className 后布局变了

### 排查方法

1. 列出所有接受 `className` prop 的组件
2. 对每个组件，列出所有调用处传入的 `className` 值
3. 逐一比对：外部 `className` 中是否有与组件内部同名的 Tailwind 原子类（`h-*`、`w-*`、`p-*`、`m-*`、`text-*`、`bg-*`、`border-*`、`rounded-*`、`flex-*`、`grid-*` 等）
4. 有则视为冲突，必须修复

### 修复原则

- 尺寸（`h-*` `w-*` `px-*` `py-*`）→ 由组件暴露 `size` prop 控制，禁止外部 className 覆盖
- 颜色（`text-*` `bg-*` `border-*`）→ 由组件暴露 `variant` prop 控制
- 边距（`m-*`）→ 父容器负责间距，子组件不设 `margin`；如需例外，暴露 `spacing` prop
- `className` 仅用于**布局定位**（如 `flex-1`、`shrink-0`、`self-center`）和**不影响组件内部尺寸的扩展**

## ActionBtn 尺寸系统

黄金比例模数比例（×√φ ≈ 1.272），高度固定 h-7 (28px)。
自动选档按**视觉单位**（拉丁=1，CJK/全角≈2），非裸 `label.length`：

| size | 宽度 | Tailwind | 适用单位 | 示例 |
|------|------|----------|----------|------|
| `xs` | 64px | `w-16` | ≤3 | — |
| `sm` | 80px | `w-20` | 4–6 | Start, 选择 |
| `md` | 104px | `w-[104px]` | 7 | Snapshot |
| `lg` | 132px | `w-[132px]` | 8–14 | Check Update, 检查更新 |
| `xl` | 168px | `w-[168px]` | 15+ | 在 GitHub 上 Star |

- `size` prop 可选，省略时 `btnAutoSize(label)` 自动选档；按钮 `whitespace-nowrap`
- 需要精确控制时传 `size="sm"` 等显式覆盖
- 禁止外部 `className` 传 `w-*`/`h-*` 覆盖尺寸
