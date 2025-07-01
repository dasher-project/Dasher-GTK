#pragma once

#include "DasherInput.h"
#include <thread> 
#include <chrono> 

using namespace std::chrono_literals;

class TestInput : public Dasher::CDasherVectorInput {
public:
  TestInput() : Dasher::CDasherVectorInput("TestInput") {
    timer = std::thread([this](){
        const float extend = 0.9;
        while(true){
            // for(float i = -extend; i < extend; i += 2.0f*extend/1000.0f){
            //    x = i;
            //    std::this_thread::sleep_for(1ms);
            // }
            
            // for(float i = extend; i > -extend; i -= 2.0f*extend/1000.0f){
            //    y = i;
            //    std::this_thread::sleep_for(1ms);
            // }
            
            // for(float i = extend; i > -extend; i -= 2.0f*extend/1000.0f){
            //     x = i;
            //     std::this_thread::sleep_for(1ms);
            // }
            
            // for(float i = -extend; i < extend; i += 2.0f*extend/1000.0f){
            //    y = i;
            //    std::this_thread::sleep_for(1ms);
            // }

            while(true){
                for(float i = 0.0f; i < 360.0f; i += 360.0f/2000.0f){
                    const float angle = i * 3.14159f / 180.0f;
                    const float l = sin(angle*5.0f) / 2.0f + 0.5f;
                    x = cos(angle)*l;
                    y = sin(angle)*l;
                    std::this_thread::sleep_for(1ms);
                }
            }
        }
    });
  }
  
  virtual bool GetVectorCoords(float &VectorX, float &VectorY){
    const float cap = std::max(1.0f,std::sqrtf(x*x + y*y));
    VectorX = x / cap;
    VectorY = y / cap;
    return true;
  };

  std::thread timer;
  float x = 0;
  float y = 0;
};
