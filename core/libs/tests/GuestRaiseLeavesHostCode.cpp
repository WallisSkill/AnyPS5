#include "SceTypes.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

extern "C" {
int APS5_VABI scePthreadCreate(Pthread*, const PthreadAttr*, PthreadEntry, void*, const char*);
int APS5_VABI scePthreadJoin(Pthread, void**);
int APS5_VABI sceKernelInstallExceptionHandler(int, void*);
int APS5_VABI sceKernelRemoveExceptionHandler(int);
int APS5_VABI sceKernelRaiseException(Pthread, int);
}

static constexpr int SigUsr1 = 30;
static constexpr int Rounds = 10;
static constexpr std::size_t RipOffset = 0xe0;
static constexpr std::size_t ConvertedBytes = 64u << 20u;
static void Require(bool value) { if (!value) std::abort(); }

static std::uintptr_t imageBegin = 0;
static std::uintptr_t imageEnd = 0;
static std::atomic<int> deliveries{0};
static std::atomic<int> outsideImage{0};
static std::atomic<bool> stop{false};
static std::atomic<int> blockedRound{-1};
static std::atomic<int> busyRound{-1};
static HANDLE release = nullptr;

static void APS5_VABI Handler(int, void* context) {
    std::uint64_t rip = 0;
    std::memcpy(&rip, static_cast<const unsigned char*>(context) + RipOffset, sizeof(rip));
    if (rip < imageBegin || rip >= imageEnd) outsideImage.fetch_add(1);
    deliveries.fetch_add(1);
}

static void* APS5_VABI Blocked(void*) {
    for (int round = 0; round < Rounds; ++round) {
        blockedRound.store(round);
        Require(WaitForSingleObject(release, INFINITE) == WAIT_OBJECT_0);
        while (blockedRound.load() == round) std::this_thread::yield();
    }
    return nullptr;
}

static void* APS5_VABI Busy(void*) {
    const std::string source(ConvertedBytes, 'a');
    std::vector<wchar_t> converted(ConvertedBytes);
    for (int round = 0; round < Rounds; ++round) {
        busyRound.store(round);
        Require(MultiByteToWideChar(CP_UTF8, 0, source.data(), static_cast<int>(source.size()), converted.data(), static_cast<int>(converted.size())) == static_cast<int>(source.size()));
        while (busyRound.load() == round) std::this_thread::yield();
    }
    return nullptr;
}

static void WaitDeliveries(int expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (deliveries.load() < expected) {
        Require(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

int main() {
    const auto* image = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + reinterpret_cast<const IMAGE_DOS_HEADER*>(image)->e_lfanew);
    imageBegin = reinterpret_cast<std::uintptr_t>(image);
    imageEnd = imageBegin + headers->OptionalHeader.SizeOfImage;
    release = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(release != nullptr);
    Require(sceKernelInstallExceptionHandler(SigUsr1, reinterpret_cast<void*>(&Handler)) == 0);

    Pthread blocked = nullptr;
    Require(scePthreadCreate(&blocked, nullptr, Blocked, nullptr, "blocked in a host wait") == 0);
    for (int round = 0; round < Rounds; ++round) {
        while (blockedRound.load() != round) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto raisedAt = std::chrono::steady_clock::now();
        Require(sceKernelRaiseException(blocked, SigUsr1) == 0);
        Require(std::chrono::steady_clock::now() - raisedAt < std::chrono::milliseconds(500));
        Require(SetEvent(release) != 0);
        WaitDeliveries(round + 1);
        blockedRound.store(round + Rounds);
    }
    Require(scePthreadJoin(blocked, nullptr) == 0);

    Pthread busy = nullptr;
    Require(scePthreadCreate(&busy, nullptr, Busy, nullptr, "busy in host code") == 0);
    for (int round = 0; round < Rounds; ++round) {
        while (busyRound.load() != round) std::this_thread::yield();
        Require(sceKernelRaiseException(busy, SigUsr1) == 0);
        WaitDeliveries(Rounds + round + 1);
        busyRound.store(round + Rounds);
    }
    Require(scePthreadJoin(busy, nullptr) == 0);

    Require(sceKernelRemoveExceptionHandler(SigUsr1) == 0);
    CloseHandle(release);
    std::printf("deliveries %d, outside the image %d\n", deliveries.load(), outsideImage.load());
    Require(outsideImage.load() == 0);
}
