#ifndef MYAUDIO_DEVICE_H
#define MYAUDIO_DEVICE_H

#include <string>
#include <vector>

#include "myaudio/config.hpp"

namespace myaudio {

struct DeviceInfo {
    std::string id;
    std::string name;
    bool capture = false;
    bool isDefault = false;
    bool cable = false;
    Format format;
};

std::vector<DeviceInfo> devices(bool capture);
bool defaultDevice(bool capture, DeviceInfo& out);
bool findCable(DeviceInfo& render, DeviceInfo& capture);

struct MicRecord {
    std::string name;
    std::string share;
    std::string sink;
    std::string device;
    Format format;
    uint32_t pid = 0;
};

bool registerMic(const MicRecord& rec);
bool unregisterMic(const std::string& name);
bool queryMic(const std::string& name, MicRecord& out);
std::vector<MicRecord> registeredMics();

}

#endif
