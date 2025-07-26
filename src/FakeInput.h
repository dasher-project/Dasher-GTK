#pragma once

#include "DasherInput.h"
#include "UIComponents/EnumDropdown.h"
#include "UIComponents/PopoverMenuButtonInfo.h"
#include "Preferences/DeviceSettingsProvider.h"
#include "gtkmm/grid.h"
#include "gtkmm/label.h"
#include "gtkmm/object.h"
#include <thread> 
#include <chrono> 
#include <cmath> 

using namespace std::chrono_literals;


class FakeInput : public Dasher::CDasherVectorInput, public DeviceSettingsProvider {
  public:
  FakeInput() : Dasher::CDasherVectorInput("Fake Input") {
    optionsDropdown.property_selected().signal_changed().connect([this](){
      currentTraceForm = static_cast<traceForm>(optionsDropdown.GetSelected());
    });
  }

  ~FakeInput(){}

  void Activate() override {
    if(timerThread) return;
    keepRunning = true;
    timerThread = std::make_unique<std::thread>([this](){
      while(keepRunning){
        if(currentTraceForm == square){
          for(float i = -extend; i < extend; i += 2.0f*extend/1000.0f){
            x = i;
            std::this_thread::sleep_for(1ms);
            if(!keepRunning) break;
          }
            
          for(float i = extend; i > -extend; i -= 2.0f*extend/1000.0f){
            y = i;
            std::this_thread::sleep_for(1ms);
            if(!keepRunning) break;
          }
          
          for(float i = extend; i > -extend; i -= 2.0f*extend/1000.0f){
            x = i;
            std::this_thread::sleep_for(1ms);
            if(!keepRunning) break;
          }
          
          for(float i = -extend; i < extend; i += 2.0f*extend/1000.0f){
            y = i;
            std::this_thread::sleep_for(1ms);
            if(!keepRunning) break;
          }
        }
        else
        {
          for(float i = 0.0f; i < 360.0f; i += 360.0f/2000.0f){
            const float angle = i * 3.14159f / 180.0f;
            const float l = (currentTraceForm == flower) ? std::sin(angle*5.0f) / 2.0f + 0.5f : 1.0f;
            x = std::cos(angle)*l*extend;
            y = std::sin(angle)*l*extend;
            std::this_thread::sleep_for(1ms);
            if(!keepRunning) break;
          }
        }
      }
    });
  }

  void Deactivate() override {
    keepRunning = false;
    if(timerThread->joinable()) timerThread->join();
    timerThread.reset();
  }

  bool FillInputDeviceSettings(Gtk::Grid* grid) override {
    grid->attach(*Gtk::make_managed<Gtk::Label>("Traceform"), 0, 0);
    grid->attach(optionsDropdown, 1, 0);
    grid->attach(*Gtk::make_managed<PopoverMenuButtonInfo>("Form that is traced on the screen"), 2, 0);
    return true;
  }
  
  virtual bool GetVectorCoords(float &VectorX, float &VectorY) override {
    const float cap = (currentTraceForm != square) ? std::max(1.0f,std::sqrt(x*x + y*y)) : 1.0f;
    VectorX = x / cap;
    VectorY = y / cap;
    return true;
  };
  
  // thread that is used to trace the shape
  std::unique_ptr<std::thread> timerThread;
  bool keepRunning = true;
  
  // computed coords
  const float extend = 0.9;
  float x = -extend;
  float y = extend;
  
  // can be used to switch the traced shape
  enum traceForm {
    square,
    ellipse,
    flower
  };
  traceForm currentTraceForm = square;
  EnumDropdown optionsDropdown = EnumDropdown({{"Square",traceForm::square},{"Ellipse",traceForm::ellipse},{"Flower",traceForm::flower}}, traceForm::square);
};
