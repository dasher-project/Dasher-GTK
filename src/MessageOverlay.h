#include "DasherController.h"
#include "Parameters.h"
#include "gtkmm/box.h"
#include "gtkmm/overlay.h"
#include <algorithm>
#include <chrono>
#include <memory>
#include <vector>
#include "PopoverBox.h"

class MessageOverlay : public Gtk::Overlay
{

public:
    void EraseMessage(PopoverBox* box){
        MessageQueue.erase(std::remove_if(MessageQueue.begin(), MessageQueue.end(), [&](QueuedMessage const& m){return m.Widget == box;}),MessageQueue.end());
    }
    
    MessageOverlay() {
        add_overlay(m_box);
        m_box.append(m_message1);
        m_box.append(m_message2);

        m_box.set_valign(Gtk::Align::START);
        m_box.set_halign(Gtk::Align::CENTER);
        m_box.set_vexpand(false);
        m_box.set_hexpand(false);

        // Remove dismissed messages from queue
        m_message1.property_child_revealed().signal_changed().connect([this](){
            if(!m_message1.get_reveal_child()) EraseMessage(&m_message1);
        });
        m_message2.property_child_revealed().signal_changed().connect([this](){
            if(!m_message2.get_reveal_child()) EraseMessage(&m_message2);
        });

        add_tick_callback([this](Glib::RefPtr<Gdk::FrameClock> clock){
            //Remove timed-out messages
            for(auto& m : MessageQueue){
                if(m.timedMessage && m.Widget &&
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m.DisplayStartingTime).count()
                        > dasherController->GetLongParameter(Dasher::Parameter::LP_MESSAGE_TIME)){
                    m.Widget->set_reveal_child(false); // Remove Message
                    m.timedMessage = false;
                }
            }

            //Find not displayed ones
            for(auto& m : MessageQueue){
                if(m.Widget) continue;
                
                //find empty widget to display
                for(auto& c : m_box.get_children()){
                    PopoverBox* p = static_cast<PopoverBox*>(c);
                    if(!p->get_reveal_child() && !p->get_child_revealed()){
                        p->reveal(m.Message);
                        m.DisplayStartingTime = std::chrono::steady_clock::now();
                        m.Widget = p;
                        m_box.reorder_child_at_start(*p);
                        break;
                    }
                }
            }
            return true;
        });
    }

    void ConnectToDasher(std::shared_ptr<DasherController> controller){
        dasherController = controller;
        dasherController->OnMessage = [this](const std::string message, const bool timedMessage){
            MessageQueue.push_back({message, timedMessage});
        };
        dasherController->OnUnpause = [this](){
            for(auto& m : MessageQueue){
                if(!m.timedMessage && m.Widget && m.Widget->get_child_revealed()){
                    m.Widget->set_reveal_child(false); // Remove Message
                    m.timedMessage = false;
                }
            }

            // search for not yet seen or dismissed messages
            for(auto& m : MessageQueue){
                if(!m.Widget) return false;
            }
            return true;
        };
    }

protected:

    struct QueuedMessage {
        std::string Message = "";
        bool timedMessage;
        std::chrono::time_point<std::chrono::steady_clock> DisplayStartingTime;
        PopoverBox* Widget = nullptr;
    };

    std::shared_ptr<DasherController> dasherController;

    Gtk::Box m_box = Gtk::Box(Gtk::Orientation::VERTICAL);
    PopoverBox m_message1;
    PopoverBox m_message2;
    std::vector<struct QueuedMessage> MessageQueue;
};