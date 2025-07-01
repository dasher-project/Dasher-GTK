#pragma once

#include "DasherInput.h"
#include <thread> 
#include <chrono> 
#include <cmath> 

using namespace std::chrono_literals;

class FakeInput : public Dasher::CDasherVectorInput {
  public:
  FakeInput() : Dasher::CDasherVectorInput("Fake Input") {
    timerThread = std::thread([this](){
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

  ~FakeInput(){
    keepRunning = false;
    if(timerThread.joinable()) timerThread.join();
  }
  
  virtual bool GetVectorCoords(float &VectorX, float &VectorY){
    const float cap = (currentTraceForm != square) ? std::max(1.0f,std::sqrt(x*x + y*y)) : 1.0f;
    VectorX = x / cap;
    VectorY = y / cap;
    return true;
  };
  
  // thread that is used to trace the shape
  std::thread timerThread;
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
  const traceForm currentTraceForm = square;
};
