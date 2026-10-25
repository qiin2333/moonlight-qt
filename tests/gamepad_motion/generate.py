"""Compile the real SDL callbacks with recording SDK/SDL boundaries, without a device."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
header = (root / "app/streaming/input/input.h").read_text(encoding="utf-8")
state = header[header.index("struct GamepadState {"):header.index("struct DualSenseOutputReport")]
source = (root / "app/streaming/input/gamepad.cpp").read_text(encoding="utf-8")
start = source.index("void SdlInputHandler::handleControllerSensorEvent(")
handler = source[start:source.index("void SdlInputHandler::handleControllerTouchpadEvent(", start)]
start = source.index("void SdlInputHandler::setMotionEventState(")
control = source[start:source.index("void SdlInputHandler::setControllerLED(", start)]
fixture = r'''
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>
extern "C" {
#include "Limelight.h"
}
#define SDL_VERSION_ATLEAST(a,b,c) 1
#define SDL_arraysize(x) (sizeof(x)/sizeof((x)[0]))
constexpr int MAX_GAMEPADS=16, SDL_SENSOR_ACCEL=1, SDL_SENSOR_GYRO=2;
constexpr int SDL_TRUE=1, SDL_FALSE=0, SDL_LOG_CATEGORY_APPLICATION=0;
using SDL_JoystickID=int32_t;
using SDL_TimerID=uintptr_t;
struct SDL_GameController {};
struct SDL_ControllerSensorEvent {uint32_t timestamp{};int which{},sensor{};float data[3]{};};
struct Reading {uint8_t sensor;float x,y,z;};
static std::vector<Reading> sent;
static int stops=0;
static bool reject=false, overrideDisabled=false;
int LiSendControllerMotionEvent(uint8_t, uint8_t type, float x, float y, float z) {
    sent.push_back({type,x,y,z});return reject ? -1 : 0;
}
int LiStopControllerMotionEvent(uint8_t) {stops++;return 0;}
void SDL_LogWarn(int,const char*,...) {}
int SDL_GameControllerSetSensorEnabled(SDL_GameController*,int,int) {return 0;}
namespace Utils {
bool getEnvironmentVariableOverride(const char*,uint16_t* rate) {if(overrideDisabled)*rate=0;return overrideDisabled;}
}
// STATE
class SdlInputHandler {
public:
    GamepadState m_GamepadState[16]{};
    SdlInputHandler(){m_GamepadState[0].controller=reinterpret_cast<SDL_GameController*>(1);}
    GamepadState* findStateForGamepad(int id){return id==0?&m_GamepadState[0]:nullptr;}
    void handleControllerSensorEvent(SDL_ControllerSensorEvent*);
    void setMotionEventState(uint16_t,uint8_t,uint16_t);
    void sample(uint32_t time, float dps){
        SDL_ControllerSensorEvent e{};e.timestamp=time;e.sensor=SDL_SENSOR_GYRO;
        e.data[0]=dps/57.2957795f;handleControllerSensorEvent(&e);
    }
};
// HANDLER
// CONTROL
int main() {
    SdlInputHandler input; input.setMotionEventState(0,2,100);
    input.sample(100,10);input.sample(104,0);
    assert(sent.size()==2 && sent.back().x==0);sent.clear();
    reject=true;input.sample(112,0);reject=false;input.sample(116,0);
    assert(sent.size()==2);sent.clear(); // A failed zero cannot poison a duplicate cache.
    for(int i=0;i<1250;i++) {
        float x=static_cast<float>(std::round(std::sin(2*3.141592653589793*80*i/250)*16)/16);
        input.sample(200+i*4,x);
    }
    assert(sent.size()==1250);
    double integral=0;for(const auto& reading:sent)integral+=reading.x*.004;
    assert(std::abs(integral)<1e-6);
    input.setMotionEventState(0,2,0);assert(!input.m_GamepadState[0].gyroSensorEnabled && stops==1);
    overrideDisabled=true;input.setMotionEventState(0,2,100);assert(!input.m_GamepadState[0].gyroSensorEnabled && stops==2);
    const auto disabled=sent.size();input.sample(6000,9);assert(sent.size()==disabled);
    const auto before=sent.size();SDL_ControllerSensorEvent unknown{};unknown.which=99;
    input.handleControllerSensorEvent(&unknown);assert(sent.size()==before);
    std::cout<<"SDL motion adapters preserve samples, final zeros and hardware disable\n";
}
'''
fixture = fixture.replace("// STATE", state).replace("// HANDLER", handler).replace("// CONTROL", control)
Path(sys.argv[1]).write_text(fixture, encoding="utf-8")
