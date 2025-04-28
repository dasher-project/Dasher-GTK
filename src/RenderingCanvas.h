#pragma once

#include "DasherController.h"
#include "DasherTypes.h"
#include "FileUtils.h"
#include "cairomm/context.h"
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/box.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/gestureclick.h>

#include <DasherScreen.h>
#include <DasherInput.h>
#include <XmlSettingsStore.h>
#include <memory>
#include <chrono>

enum GeometryType
{
	GT_Rectanlge,
	GT_Text,
	GT_PolyLine
};

struct DasherDrawGeometry
{
	GeometryType Type;
	DasherDrawGeometry(GeometryType Type) : Type(Type) {}
	virtual ~DasherDrawGeometry() {};
};

struct Rectangle : DasherDrawGeometry{
	const Dasher::point topLeft;
	const Dasher::point size;
	const Dasher::ColorPalette::Color& color;
	const Dasher::ColorPalette::Color& outlineColor;
	const int thickness;

	Rectangle(Dasher::point TopLeft, Dasher::point Size, const Dasher::ColorPalette::Color& Color, const Dasher::ColorPalette::Color& OutlineColor, const int Thickness) : DasherDrawGeometry(GT_Rectanlge), topLeft(TopLeft), size(Size), color(Color), outlineColor(OutlineColor), thickness(Thickness){}
};

struct Text : DasherDrawGeometry{
	Dasher::CDasherScreen::Label *label;
	Dasher::point pos;
	int size;
	const Dasher::ColorPalette::Color& color;

	Text(Dasher::CDasherScreen::Label *Label, Dasher::point Pos, int Size, const Dasher::ColorPalette::Color& Color) : DasherDrawGeometry(GT_Text), label(Label), pos(Pos), size(Size), color(Color)  {}
};

struct PolyLine : DasherDrawGeometry{
	std::vector<Dasher::point> points;
	float linewidth;
	const Dasher::ColorPalette::Color& color;

	PolyLine(std::vector<Dasher::point> Points, float LineWidth, const Dasher::ColorPalette::Color& Color): DasherDrawGeometry(GT_PolyLine), points(Points), linewidth(LineWidth), color(Color)  {}
};


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
	void GtkDraw(Glib::RefPtr<Cairo::Context> cr, int width, int height);
	const long long getCurrentMS();

private:
	Glib::RefPtr<Gtk::EventControllerMotion> mouseController;
	Glib::RefPtr<Gtk::GestureClick> mouseClickController;
	std::shared_ptr<Dasher::XmlSettingsStore> Settings;
	std::shared_ptr<DasherController> dasherController;
	Dasher::point mousePos = {0,0};

	std::vector<std::unique_ptr<DasherDrawGeometry>> GeometryBufferA;
	std::vector<std::unique_ptr<DasherDrawGeometry>> GeometryBufferB;
	std::vector<std::unique_ptr<DasherDrawGeometry>>* BackBuffer = &GeometryBufferA;
	std::vector<std::unique_ptr<DasherDrawGeometry>>* FrontBuffer = &GeometryBufferB;

	std::chrono::time_point<std::chrono::steady_clock> startTime;
};