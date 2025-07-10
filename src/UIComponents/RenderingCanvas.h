#pragma once

#include "ColorPalette.h"
#include "DasherController.h"
#include "DasherTypes.h"
#include "FileUtils.h"
#include "cairomm/context.h"
#include "gtkmm/shortcutcontroller.h"
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/box.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/gestureclick.h>
#include <cairomm/context.h>
#include <cairomm/surface.h>

#include <DasherScreen.h>
#include <DasherInput.h>
#include <XmlSettingsStore.h>
#include <memory>

class RenderingCanvas : public Gtk::DrawingArea, public Dasher::CDasherScreen, public Dasher::CScreenCoordInput, public Dasher::CommandlineErrorDisplay
{
public:
    RenderingCanvas();

    std::pair<Dasher::screenint, Dasher::screenint> TextSize(Label* label, unsigned iFontSize) override;
	void DrawString(Label* label, Dasher::screenint x, Dasher::screenint y, unsigned iFontSize, const Dasher::ColorPalette::Color& Color) override;
	void DrawRectangle(Dasher::screenint x1, Dasher::screenint y1, Dasher::screenint x2, Dasher::screenint y2, const Dasher::ColorPalette::Color& Color, const Dasher::ColorPalette::Color& OutlineColor, int iThickness) override;
	void DrawCircle(Dasher::screenint iCX, Dasher::screenint iCY, Dasher::screenint iR, const Dasher::ColorPalette::Color& iFillColor, const Dasher::ColorPalette::Color& LineColor, int iLineWidth) override;
	void Polyline(Dasher::point* Points, int Number, int iWidth, const Dasher::ColorPalette::Color& Color) override;
	void Polygon(Dasher::point* Points, int Number, const Dasher::ColorPalette::Color& fillColor, const Dasher::ColorPalette::Color& outlineColor, int lineWidth) override;
	void Display() override;
	bool IsPointVisible(Dasher::screenint x, Dasher::screenint y) override;

    bool GetScreenCoords(Dasher::screenint& iX, Dasher::screenint& iY, Dasher::CDasherView* pView) override;
	const long long getCurrentMS();
	void show_text(const std::string& utf8);

	virtual void Activate() override {inputActivated = true;};
    virtual void Deactivate() override {inputActivated = false;};

public:
	Glib::RefPtr<Gtk::EventControllerMotion> mouseController;
	Glib::RefPtr<Gtk::GestureClick> mouseLeftClickController;
	Glib::RefPtr<Gtk::GestureClick> mouseRightClickController;
	Glib::RefPtr<Gtk::ShortcutController> shortcutController;
	std::shared_ptr<Dasher::XmlSettingsStore> Settings;
	std::shared_ptr<DasherController> dasherController;
	Dasher::point mousePos = {0,0};
	bool inputActivated = false;

	Cairo::RefPtr<Cairo::RecordingSurface> recordingSurface;
	Cairo::RefPtr<Cairo::Context> renderingBackend;
	bool generatePDFNextFrame = false;


public:
	static const std::string MouseButtonNamePrimary;
	static const std::string MouseButtonNameSecondary;
};