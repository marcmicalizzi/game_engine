#pragma once

// Compiler, platform, and build-configuration detection plus small portability macros.
// Every macro here is defined to 0 or 1 so that `#if ENGINE_X` is always well-formed.

#if defined(__clang__)
#define ENGINE_COMPILER_CLANG 1
#define ENGINE_COMPILER_MSVC 0
#define ENGINE_COMPILER_GCC 0
#elif defined(_MSC_VER)
#define ENGINE_COMPILER_CLANG 0
#define ENGINE_COMPILER_MSVC 1
#define ENGINE_COMPILER_GCC 0
#elif defined(__GNUC__)
#define ENGINE_COMPILER_CLANG 0
#define ENGINE_COMPILER_MSVC 0
#define ENGINE_COMPILER_GCC 1
#else
#error "Unsupported compiler"
#endif

#if defined(_WIN32)
#define ENGINE_PLATFORM_WINDOWS 1
#define ENGINE_PLATFORM_LINUX 0
#elif defined(__linux__)
#define ENGINE_PLATFORM_WINDOWS 0
#define ENGINE_PLATFORM_LINUX 1
#else
#error "Unsupported platform"
#endif

#if defined(NDEBUG)
#define ENGINE_DEBUG 0
#else
#define ENGINE_DEBUG 1
#endif

#if ENGINE_COMPILER_MSVC
#define ENGINE_FORCE_INLINE __forceinline
#define ENGINE_NO_INLINE __declspec(noinline)
#define ENGINE_RESTRICT __restrict
#define ENGINE_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#define ENGINE_DEBUG_BREAK() __debugbreak()
#else
#define ENGINE_FORCE_INLINE inline __attribute__((always_inline))
#define ENGINE_NO_INLINE __attribute__((noinline))
#define ENGINE_RESTRICT __restrict__
#define ENGINE_NO_UNIQUE_ADDRESS [[no_unique_address]]
#define ENGINE_DEBUG_BREAK() __builtin_trap()
#endif

#define ENGINE_STRINGIZE_IMPL(x) #x
#define ENGINE_STRINGIZE(x) ENGINE_STRINGIZE_IMPL(x)

#define ENGINE_CONCAT_IMPL(a, b) a##b
#define ENGINE_CONCAT(a, b) ENGINE_CONCAT_IMPL(a, b)

// Marks a type as non-copyable in one line inside the class body.
#define ENGINE_NON_COPYABLE(Type) \
  Type(const Type&) = delete;     \
  Type& operator=(const Type&) = delete
