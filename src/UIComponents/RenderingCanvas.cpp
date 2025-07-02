#include "RenderingCanvas.h"
#include "ColorPalette.h"
#include "DasherTypes.h"
#include "cairomm/context.h"
#include "cairomm/surface.h"
#include "gdk/gdk.h"
#include "gdk/gdkkeysyms.h"
#include "glibmm/datetime.h"
#include "gtkmm/enums.h"
#include "gtkmm/eventcontrollermotion.h"
#include "gtkmm/gestureclick.h"
#include "gtkmm/shortcut.h"
#include "gtkmm/shortcutaction.h"
#include "gtkmm/shortcuttrigger.h"
#include "gtkmm/shortcutcontroller.h"
#include "gtkmm/widget.h"
#include <chrono>
#include <gtkmm/eventcontroller.h>
#include <gdkmm/frameclock.h>
#include <memory>

const std::string RenderingCanvas::ButtonNamePrimary = "MouseLeft";
const std::string RenderingCanvas::ButtonNameSecondary = "MouseRight";

const long long RenderingCanvas::getCurrentMS(){
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
}

#pragma clang optimize off
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
    
    mouseLeftClickController = Gtk::GestureClick::create();
    mouseLeftClickController->set_button(GDK_BUTTON_PRIMARY);
    mouseLeftClickController->signal_pressed().connect([this](int, double, double){
        if(inputActivated) dasherController->MappedKeyDown(getCurrentMS(), ButtonNamePrimary);
    });
    mouseLeftClickController->signal_released().connect([this](int, double, double){
        if(inputActivated) dasherController->MappedKeyUp(getCurrentMS(), ButtonNamePrimary);
    });
    add_controller(mouseLeftClickController);

    mouseRightClickController = Gtk::GestureClick::create();
    mouseRightClickController->set_button(GDK_BUTTON_SECONDARY);
    mouseRightClickController->signal_pressed().connect([this](int, double, double){
        if(inputActivated) dasherController->MappedKeyDown(getCurrentMS(), ButtonNameSecondary);
    });
    mouseRightClickController->signal_released().connect([this](int, double, double){
        if(inputActivated) dasherController->MappedKeyUp(getCurrentMS(), ButtonNameSecondary);
    });
    add_controller(mouseRightClickController);

    //add shortcut
    shortcutController = Gtk::ShortcutController::create();
    shortcutController->add_shortcut(Gtk::Shortcut::create(Gtk::KeyvalTrigger::create(GDK_KEY_F8), Gtk::CallbackAction::create([this](Gtk::Widget&, const Glib::VariantBase&){
        generatePDFNextFrame = true;
        return true;
    })));
    shortcutController->set_scope(Gtk::ShortcutScope::GLOBAL);
    add_controller(shortcutController);

    // Initalize Cairo
    recordingSurface = Cairo::RecordingSurface::create();
    renderingBackend = Cairo::Context::create(recordingSurface);

    // Initialize Dasher
    Settings = std::make_shared<Dasher::XmlSettingsStore>("Settings.xml", this);
	Settings->Load();
	Settings->Save();
	dasherController = std::make_shared<DasherController>(Settings);
	dasherController->GetModuleManager()->RegisterInputDeviceModule(this, true);
	dasherController->ChangeScreen(this);
    dasherController->Initialize();
	dasherController->SetBuffer(0);

    // Enable Resizing
    signal_resize().connect([this](int width, int height){
        resize(width, height);
        dasherController->ScreenResized(this);
    });

    // Enable Drawing
    startTime = std::chrono::steady_clock::now();
    set_draw_func([this](Glib::RefPtr<Cairo::Context> cr, int width, int height){
        cr->set_source(recordingSurface, 0, 0);
        cr->paint();

        if(generatePDFNextFrame){
          auto pdfSurface =
              Cairo::PdfSurface::create(Glib::DateTime::create_now_local().format("Screenshot-%Y-%m-%d_%H-%M-%S.pdf"), width, height);
          auto pdfBackend = Cairo::Context::create(pdfSurface);

          pdfBackend->set_source(recordingSurface, 0, 0);
          pdfBackend->paint();

          generatePDFNextFrame = false;
        }

        //reset recording surface
        renderingBackend->save();
        renderingBackend->set_operator(Cairo::Context::Operator::CLEAR);
        renderingBackend->paint();
        renderingBackend->restore();
    });
    add_tick_callback([this](Glib::RefPtr<Gdk::FrameClock> clock){
        dasherController->Render(getCurrentMS());
        return true;
    });
}
#pragma clang optimize on

std::pair<Dasher::screenint, Dasher::screenint> RenderingCanvas::TextSize(Label* label, unsigned iFontSize) {
    Cairo::TextExtents te;
    renderingBackend->set_font_size(iFontSize);
    renderingBackend->get_text_extents(label->m_strText, te);
	return {static_cast<Dasher::screenint>(te.width), static_cast<Dasher::screenint>(te.height)};
}

void RenderingCanvas::DrawString(Label* label, Dasher::screenint x, Dasher::screenint y, unsigned iFontSize, const Dasher::ColorPalette::Color& Color) {
    Cairo::TextExtents te;
    renderingBackend->begin_new_path();
    renderingBackend->set_font_size(iFontSize);
    renderingBackend->set_source_rgba(Color.Red/255.0, Color.Green/255.0, Color.Blue/255.0, Color.Alpha/255.0);

    renderingBackend->get_text_extents(label->m_strText, te);
    renderingBackend->move_to(x - te.x_bearing, y - te.y_bearing);
    renderingBackend->show_text(label->m_strText);
}

void RenderingCanvas::DrawRectangle(Dasher::screenint x1, Dasher::screenint y1, Dasher::screenint x2, Dasher::screenint y2, const Dasher::ColorPalette::Color& Color, const Dasher::ColorPalette::Color& OutlineColor, int iThickness) {
    if(OutlineColor == Dasher::ColorPalette::noColor) iThickness = 0;

    renderingBackend->begin_new_path();
    renderingBackend->rectangle(x1, y1, x2 - x1,y2 - y1);
            
    renderingBackend->set_source_rgba(Color.Red/255.0, Color.Green/255.0, Color.Blue/255.0, Color.Alpha/255.0);
    renderingBackend->fill_preserve();

    if(iThickness > 0){
        renderingBackend->set_source_rgba(OutlineColor.Red/255.0, OutlineColor.Green/255.0, OutlineColor.Blue/255.0, OutlineColor.Alpha/255.0);
        renderingBackend->set_line_width(iThickness);
        renderingBackend->stroke();
    }
}

void RenderingCanvas::DrawCircle(Dasher::screenint iCX, Dasher::screenint iCY, Dasher::screenint iR, const Dasher::ColorPalette::Color& iFillColor, const Dasher::ColorPalette::Color& LineColor, int iLineWidth) {
    renderingBackend->begin_new_path();
    renderingBackend->arc(iCX, iCY, iR, 0.0, 6.28318530718); //2*PI

    renderingBackend->set_source_rgba(iFillColor.Red/255.0, iFillColor.Green/255.0, iFillColor.Blue/255.0, iFillColor.Alpha/255.0);
    renderingBackend->fill_preserve();

    if(iLineWidth > 0){
        renderingBackend->set_source_rgba(LineColor.Red/255.0, LineColor.Green/255.0, LineColor.Blue/255.0, LineColor.Alpha/255.0);
        renderingBackend->set_line_width(iLineWidth);
        renderingBackend->stroke();
    }
}

void RenderingCanvas::Polyline(Dasher::point* Points, int Number, int iWidth, const Dasher::ColorPalette::Color& Color) {
    renderingBackend->set_source_rgba(Color.Red/255.0, Color.Green/255.0, Color.Blue/255.0, Color.Alpha/255.0);
    renderingBackend->set_line_width(iWidth);
    
    renderingBackend->begin_new_path(); //Clear current path if not done so far
    for(int i = 0; i < Number; i++){
        if(i == 0) renderingBackend->move_to(Points[i].x, Points[i].y);
        else renderingBackend->line_to(Points[i].x, Points[i].y);
    }
    renderingBackend->stroke();
}

void RenderingCanvas::Polygon(Dasher::point* Points, int Number, const Dasher::ColorPalette::Color& fillColor, const Dasher::ColorPalette::Color& outlineColor, int lineWidth) {
    
    renderingBackend->begin_new_path();
    for(int i = 0; i < Number; i++){
        if(i == 0) renderingBackend->move_to(Points[i].x, Points[i].y);
        else renderingBackend->line_to(Points[i].x, Points[i].y);
    }
    renderingBackend->close_path();
    renderingBackend->set_source_rgba(fillColor.Red/255.0, fillColor.Green/255.0, fillColor.Blue/255.0, fillColor.Alpha/255.0);
    renderingBackend->fill_preserve();
    
    if(lineWidth > 0){
        renderingBackend->set_source_rgba(outlineColor.Red/255.0, outlineColor.Green/255.0, outlineColor.Blue/255.0, outlineColor.Alpha/255.0);
        renderingBackend->set_line_width(lineWidth);
        renderingBackend->stroke();
    }
    renderingBackend->stroke();
}

void RenderingCanvas::Display() {
    queue_draw();
}

bool RenderingCanvas::IsPointVisible(Dasher::screenint x, Dasher::screenint y) {
	return true;
}
bool RenderingCanvas::GetScreenCoords(Dasher::screenint& iX, Dasher::screenint& iY, Dasher::CDasherView* pView) {
    iX = mousePos.x;
    iY = mousePos.y;
	return true;
}
