# CLAUDE.md — monitor_web 前端

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
