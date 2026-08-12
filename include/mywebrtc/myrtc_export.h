/**
 * myrtc_export.h — 符号导出宏 (Layer A)
 *
 * 对外 ABI 隔离：所有 mywebrtc 对外符号统一用 MYRTC_API 修饰。
 * 原始 rtc* 符号不从此库再导出（链接 datachannel 静态库或
 * 通过 -fvisibility=hidden / .def 文件隐藏）。
 *
 * 此文件保持纯 C 兼容（C99）。
 */
#ifndef MYRTC_EXPORT_H
#define MYRTC_EXPORT_H

#ifdef RTC_STATIC
#define MYRTC_API
#else
#ifdef _WIN32
#ifdef MYRTC_EXPORTS
#define MYRTC_API __declspec(dllexport)
#else
#define MYRTC_API __declspec(dllimport)
#endif
#else
#define MYRTC_API __attribute__((visibility("default")))
#endif
#endif

#ifdef _WIN32
#ifdef CAPI_STDCALL
#define MYRTC_CALL __stdcall
#else
#define MYRTC_CALL
#endif
#else
#define MYRTC_CALL
#endif

#endif /* MYRTC_EXPORT_H */
