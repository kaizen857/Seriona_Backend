# Task 10 Evidence - seriona-vibe-bug-refactor

## Scope
- Task 10 本身没有引入新的产品代码，只做 full-suite 验证、架构契约复核和 scope drift 检查。
- 如果 full suite 暴露了回流问题，只把失败定位回既有任务域处理，不在 Task 10 里扩展实现范围。

## Full build
- 命令: `cmake --build build`
- 结果: `ninja: no work to do.`

## Full test
- 命令: `ctest --test-dir build --output-on-failure`
- 结果: 43/43 tests passed, 0 failed, total real time 3.25 sec.

## Task 10 期间的回流修复摘要
- 音频侧回流来自 Task 6 之后的测试时序假设。full suite 暴露了部分音频测试仍默认 fake backend 状态会在下一条断言前同步稳定，实际应先通过既有命令队列屏障再读 fake backend 或手动触发消费。这个问题已回流到 Task 6 归属域处理并提交。
- scanner 侧回流来自 Task 5 之后的测试时序假设。full suite 暴露了部分 scanner 测试仍默认 `scan()` 完成时 public snapshot 已同步稳定，实际应等待公开快照而不是内部 fake TagReader 计数。这个问题已回流到 Task 5 归属域处理并提交。
- Task 10 只确认这些回流已经被既有任务域修复后纳入 full suite，未新增任何新修复代码。

## renderCallback 禁止操作 grep 结论
- 检查文件: `src/audio/device/audio_output_device.cpp`
- 结论: `AudioOutputDevice::renderCallback()` 没有发现 FFmpeg、logging、locking、sink dispatch、device lifecycle 的 forbidden 调用。
- 证据点: 回调只读取原子状态、从 `PcmBufferQueue` 取 PCM 或补静音、应用音量/静音、更新计数器，没有出现 `av_`、日志、互斥锁、`BackendEventSink`、`start/stop/uninitialize` 这类调用。

## 当前工作区脏文件说明
- `git status --short` 仍显示这些非源码脏文件: `.omo/boulder.json`、`.omo/notepads/seriona-vibe-bug-refactor/learnings.md`、`.omo/drafts/seriona-vibe-bug-refactor.md`、`.omo/evidence/task-0-seriona-vibe-bug-refactor.md`、`.omo/plans/seriona-vibe-bug-refactor.md`、`.omo/run-continuation/ses_10b603cc8ffen5tF2A35g3FUpo.json`、`DESIGN.md`、`VIBE_CODING_BUG_REPORT.md`。
- 这些脏文件不是 Task 10 新增的源码修改，Task 10 没有写入它们，也没有做任何 git 写操作。

## 最近提交
- `git log --oneline -6` 显示最近提交已经包含 Task 5 到 Task 9 的回流修复提交，Task 10 只负责最终验证。
