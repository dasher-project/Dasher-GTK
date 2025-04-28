#include "RenderingCanvas.h"
#include "ColorPalette.h"
#include "DasherTypes.h"
#include "gtkmm/enums.h"
#include "gtkmm/eventcontrollermotion.h"
#include "gtkmm/gestureclick.h"
#include "gtkmm/widget.h"
#include <chrono>
#include <gtkmm/eventcontroller.h>
#include <gdkmm/frameclock.h>
#include <memory>
#include <vector>


const long long RenderingCanvas::getCurrentMS(){
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
}

RenderingCanvas::RenderingCanvas(): Dasher::CDasherScreen(100,100), CScreenCoordInput("Mouse Input") {
    //Set Inital Size
    set_size_request(500,500);
    set_hexpand(true);
    set_vexpand(true);
    set_valign(Gtk::Align::FILL);
    set_halign(Gtk::Align::FILL);

    // Capture Mouse Movement
    mouseController = Gtk::EventControllerMotion::create();
    mouseController->signal_motion().connect([this](double x, double y){
        mousePos = {static_cast<int>(x),static_cast<int>(y)};
    });
    add_controller(mouseController);
    
    mouseClickController = Gtk::GestureClick::create();
    mouseClickController->signal_pressed().connect([this](int, double, double){
        dasherController->KeyDown(getCurrentMS(), Dasher::Keys::Primary_Input);
    });
    mouseClickController->signal_released().connect([this](int, double, double){
        dasherController->KeyUp(getCurrentMS(), Dasher::Keys::Primary_Input);
    });
    add_controller(mouseClickController);

    Settings = std::make_shared<Dasher::XmlSettingsStore>("Settings.xml", this);
	Settings->Load();
	Settings->Save();
	dasherController = std::make_shared<DasherController>(Settings);
	dasherController->GetModuleManager()->RegisterInputDeviceModule(this, true);
    dasherController->Initialize();
	dasherController->ChangeScreen(this);
	dasherController->SetBuffer(0);

    // Enable Resizing
    signal_resize().connect([this](int width, int height){
        resize(width, height);
        dasherController->ScreenResized(this);
    });

    // Enable Drawing
    startTime = std::chrono::steady_clock::now();
    set_draw_func(sigc::mem_fun(*this, &RenderingCanvas::GtkDraw));
    add_tick_callback([this](Glib::RefPtr<Gdk::FrameClock> clock){
        dasherController->Render(getCurrentMS());
        return true;
    });
}

std::pair<Dasher::screenint, Dasher::screenint> RenderingCanvas::TextSize(Label* label, unsigned iFontSize) {
	std::pair<Dasher::screenint, Dasher::screenint> returnValue;
	return returnValue;
}

void RenderingCanvas::DrawString(Label* label, Dasher::screenint x, Dasher::screenint y, unsigned iFontSize, const Dasher::ColorPalette::Color& Color) {
    BackBuffer->emplace_back(std::make_unique<Text>(label, Dasher::point(x,y), iFontSize, Color));
}

void RenderingCanvas::DrawRectangle(Dasher::screenint x1, Dasher::screenint y1, Dasher::screenint x2, Dasher::screenint y2, const Dasher::ColorPalette::Color& Color, const Dasher::ColorPalette::Color& OutlineColor, int iThickness) {
    if(OutlineColor == Dasher::ColorPalette::noColor) iThickness = 0;

    BackBuffer->emplace_back(std::make_unique<Rectangle>(Dasher::point(x1,y1), Dasher::point(x2-x1, y2-y1), Color, OutlineColor, iThickness));
}

void RenderingCanvas::DrawCircle(Dasher::screenint iCX, Dasher::screenint iCY, Dasher::screenint iR, const Dasher::ColorPalette::Color& iFillColor, const Dasher::ColorPalette::Color& LineColor, int iLineWidth) {
}

void RenderingCanvas::Polyline(Dasher::point* Points, int Number, int iWidth, const Dasher::ColorPalette::Color& Color) {
    std::vector<Dasher::point> ps;
    ps.reserve(Number);
    for(int i = 0; i < Number; i++){
        ps.push_back(Points[i]);
    }

    BackBuffer->emplace_back(std::make_unique<PolyLine>(ps, iWidth, Color));
}

void RenderingCanvas::Polygon(Dasher::point* Points, int Number, const Dasher::ColorPalette::Color& fillColor, const Dasher::ColorPalette::Color& outlineColor, int lineWidth) {
}
void RenderingCanvas::Display() {
    // Swap Buffers and clear BackBuffer
    std::swap(FrontBuffer, BackBuffer);
	BackBuffer->clear();
    queue_draw();
}

void RenderingCanvas::GtkDraw(Glib::RefPtr<Cairo::Context> cr, int width, int height){

    Rectangle* r;
	Text* t;
	PolyLine* l;

	std::vector<std::unique_ptr<DasherDrawGeometry>>& GeometryBuffer = *FrontBuffer;
	for (std::unique_ptr<DasherDrawGeometry>& GeneralObject : GeometryBuffer)
	{
		switch (GeneralObject->Type)
		{
		case GT_Rectanlge:
			r = static_cast<Rectangle*>(GeneralObject.get());
            cr->rectangle(r->topLeft.x, r->topLeft.y, r->size.x,r->size.y);
            
            cr->set_source_rgba(r->color.Red, r->color.Green, r->color.Blue, r->color.Alpha);
            cr->fill_preserve();

            if(r->thickness > 0){
                cr->set_source_rgba(r->outlineColor.Red, r->outlineColor.Green, r->outlineColor.Blue, r->outlineColor.Alpha);
                cr->set_line_width(r->thickness);
                cr->stroke();
            }
            cr->begin_new_path(); //Clear current path if not done so far

            break;
		case GT_Text:
			t = static_cast<Text*>(GeneralObject.get());

            cr->set_font_size(t->size);
            cr->set_source_rgba(t->color.Red, t->color.Green, t->color.Blue, t->color.Alpha);
            cr->move_to(t->pos.x, t->pos.y);
            // cr->show_text(t->label->m_strText);

			break;
		case GT_PolyLine:
			l = static_cast<PolyLine*>(GeneralObject.get());

            cr->set_source_rgba(l->color.Red, l->color.Green, l->color.Blue, l->color.Alpha);
            cr->set_line_width(l->linewidth);
            
            for(int i = 0; i < l->points.size(); i++){
                if(i == 0) cr->move_to(l->points[i].x, l->points[i].y);
                else cr->line_to(l->points[i].x, l->points[i].y);
            }
            cr->stroke();
			break;
		default: break;
		}
	}

}

bool RenderingCanvas::IsPointVisible(Dasher::screenint x, Dasher::screenint y) {
	return true;
}
bool RenderingCanvas::GetScreenCoords(Dasher::screenint& iX, Dasher::screenint& iY, Dasher::CDasherView* pView) {
    iX = mousePos.x;
    iY = mousePos.y;
	return true;
}
