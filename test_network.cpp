#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "StreamSender.h"
#include "StreamReceiver.h"
#include "NetworkProtocol.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

std::atomic<int> framesReceived = 0;
std::atomic<bool> running = true;

int main() {
    timeBeginPeriod(1);
    StreamSender   sender(9000);
    StreamReceiver receiver("127.0.0.1", 9000);

    receiver.onVideoFrame = [](EncodedFrame frame) {
        framesReceived++;
        std::cout << "Frame alindi | size=" << frame.nalUnits.size()
                  << " bytes | keyframe=" << frame.isKeyframe
                  << " | toplam=" << framesReceived.load() << "\n";
    };

    if (!sender.start())   { std::cerr << "Sender baslatilamadi\n"; return 1; }
    if (!receiver.start()) { std::cerr << "Receiver baslatilamadi\n"; return 1; }

    std::cout << "Test basladi - 5 saniye boyunca 60fps sahte frame gonderiliyor\n\n";

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    uint32_t frameIndex = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        EncodedFrame frame;
        frame.timestampUs = frameIndex * 16667;
        frame.isKeyframe  = (frameIndex % 60 == 0);

        // Sahte NAL verisi (2400 byte — fragmentasyon test eder)
        frame.nalUnits.assign(2400, static_cast<uint8_t>(frameIndex & 0xFF));
        frame.nalUnits[0] = 0x00;
        frame.nalUnits[1] = 0x00;
        frame.nalUnits[2] = 0x00;
        frame.nalUnits[3] = 0x01;

        sender.sendFrame(frame);
        frameIndex++;

        std::this_thread::sleep_for(std::chrono::microseconds(16667));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    sender.stop();
    receiver.stop();

    std::cout << "\n--- SONUC ---\n";
    std::cout << "Gonderilen : " << frameIndex << " frame\n";
    std::cout << "Alinan     : " << framesReceived.load() << " frame\n";

    auto stats = receiver.getStats();
    std::cout << "Paket kaybi: " << stats.packetLossPct << "%\n";
    std::cout << "Bitrate    : " << stats.bitrateKbps << " kbps\n";

    if (framesReceived.load() > 270)
        std::cout << "\nAG KATMANI: GECTI\n";
    else
        std::cout << "\nAG KATMANI: BASARISIZ - frame kaybi yüksek\n";

    timeEndPeriod(1);
    return 0;
}