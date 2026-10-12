/* This file is part of the Pangolin Project.
 * http://github.com/stevenlovegrove/Pangolin
 *
 * Copyright (c) 2011 Steven Lovegrove
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <thread>
#include <mutex>
#include <iostream>
#include <iomanip>

#include <pangolin/display/widgets.h>
#include <pangolin/display/display.h>
#include <pangolin/display/default_font.h>
#include <pangolin/gl/gldraw.h>
#include <pangolin/var/varextra.h>
#include <pangolin/utils/file_utils.h>

#include "pangolin_gl.h"


using namespace std;

namespace pangolin
{

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

namespace process
{
// Last known mouse position in window coordinates (see process.cpp)
extern float last_x;
extern float last_y;
}

namespace
{

struct Rgba
{
    GLfloat r, g, b, a;

    Rgba WithAlpha(GLfloat alpha) const { return {r, g, b, alpha}; }
    void Apply() const { glColor4f(r, g, b, a); }
};

constexpr Rgba Hex(uint32_t rgb, GLfloat a = 1.0f)
{
    return { ((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, a };
}

namespace theme
{
const Rgba panel_bg        = Hex(0x18191d);
const Rgba panel_border    = Hex(0x2a2c33);
const Rgba widget_bg       = Hex(0x25272e);
const Rgba widget_hover    = Hex(0x2e3139);
const Rgba widget_border   = Hex(0x33363f);
const Rgba row_hover       = Hex(0x202228);
const Rgba input_bg        = Hex(0x111216);
const Rgba accent          = Hex(0x4f8cff);
const Rgba accent_hover    = Hex(0x6a9dff);
const Rgba accent_pressed  = Hex(0x3a73e0);
const Rgba slider_bg       = Hex(0x1d1f25);
const Rgba slider_bg_hov   = Hex(0x23252c);
const Rgba slider_fill     = Hex(0x2f4f8a);
const Rgba slider_fill_hov = Hex(0x365a9c);
const Rgba selection       = Hex(0x4f8cff, 0.45f);
const Rgba toggle_off      = Hex(0x3a3d46);
const Rgba toggle_off_hov  = Hex(0x464a55);
const Rgba knob            = Hex(0xf4f5f7);
const Rgba shadow          = Hex(0x000000, 0.35f);
const Rgba text            = Hex(0xe6e8ed);
const Rgba text_dim        = Hex(0x9aa0ad);
const Rgba text_on_accent  = Hex(0xffffff);

const float radius         = 6.0f;
const float text_pad       = 8.0f;
const float toggle_w       = 30.0f;
const float toggle_h       = 16.0f;
}

// Continuous (pixel-edge) rectangle. With the pixel orthographic projection
// used by the panel, integer coordinates are pixel centres.
struct Rectf
{
    GLfloat l, b, r, t;

    Rectf Inset(GLfloat d) const { return {l + d, b + d, r - d, t - d}; }
    Rectf Offset(GLfloat dx, GLfloat dy) const { return {l + dx, b + dy, r + dx, t + dy}; }
    GLfloat w() const { return r - l; }
    GLfloat h() const { return t - b; }
};

Rectf ToRect(const Viewport& v)
{
    return { v.l - 0.5f, v.b - 0.5f, v.l + v.w - 0.5f, v.b + v.h - 0.5f };
}

// Fill a rounded rectangle with an anti-aliased edge of width 'feather'
// (we don't rely on multisampling being available).
void FillRoundedRect(const Rectf& r, GLfloat radius, const Rgba& c, GLfloat feather = 1.0f)
{
    if(r.w() <= 0.0f || r.h() <= 0.0f) return;

    radius = std::max(0.0f, std::min(radius, 0.5f * std::min(r.w(), r.h())));
    constexpr int seg = 10;
    const GLfloat cx[4] = {r.r - radius, r.l + radius, r.l + radius, r.r - radius};
    const GLfloat cy[4] = {r.t - radius, r.t - radius, r.b + radius, r.b + radius};
    const GLfloat hf = 0.5f * feather;

    // Perimeter positions and outward normals, counter-clockwise
    std::vector<GLfloat> fan, strip, strip_col;
    fan.reserve(2 * (4 * (seg + 1) + 2));
    strip.reserve(4 * (4 * (seg + 1) + 1));
    fan.push_back(0.5f * (r.l + r.r));
    fan.push_back(0.5f * (r.b + r.t));

    for(int c = 0; c < 4; ++c) {
        for(int i = 0; i <= seg; ++i) {
            const GLfloat a = (GLfloat)((c + (double)i / seg) * M_PI / 2.0);
            const GLfloat nx = std::cos(a), ny = std::sin(a);
            const GLfloat px = cx[c] + radius * nx, py = cy[c] + radius * ny;
            fan.push_back(px - hf * nx);
            fan.push_back(py - hf * ny);
            strip.insert(strip.end(), {px - hf * nx, py - hf * ny, px + hf * nx, py + hf * ny});
        }
    }
    // Close the loops
    fan.push_back(fan[2]);
    fan.push_back(fan[3]);
    strip.insert(strip.end(), {strip[0], strip[1], strip[2], strip[3]});

    // Inner edge of the feather has the full colour, outer edge is transparent
    strip_col.reserve(2 * strip.size());
    for(size_t i = 0; i < strip.size() / 4; ++i) {
        strip_col.insert(strip_col.end(), {c.r, c.g, c.b, c.a, c.r, c.g, c.b, 0.0f});
    }

    glEnableClientState(GL_VERTEX_ARRAY);
    c.Apply();
    glVertexPointer(2, GL_FLOAT, 0, fan.data());
    glDrawArrays(GL_TRIANGLE_FAN, 0, (GLsizei)(fan.size() / 2));

    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_FLOAT, 0, strip_col.data());
    glVertexPointer(2, GL_FLOAT, 0, strip.data());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, (GLsizei)(strip.size() / 2));
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

// Rounded rectangle with a 1px border
void DrawRoundedBox(const Rectf& r, GLfloat radius, const Rgba& fill, const Rgba& border)
{
    FillRoundedRect(r, radius, border);
    FillRoundedRect(r.Inset(1.0f), radius - 1.0f, fill);
}

// Soft drop shadow around a rounded rectangle
void DrawShadow(const Rectf& r, GLfloat radius, GLfloat spread, const Rgba& c)
{
    FillRoundedRect(r.Inset(-0.5f * spread), radius + 0.5f * spread, c, spread);
}

void FillRect(const Rectf& r, const Rgba& c)
{
    GLfloat vs[] = { r.l, r.b, r.l, r.t, r.r, r.t, r.r, r.b };
    c.Apply();
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, vs);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableClientState(GL_VERTEX_ARRAY);
}

// Vertical gradient between colour at bottom and colour at top
void FillRectGradient(const Rectf& r, const Rgba& bottom, const Rgba& top)
{
    GLfloat vs[] = { r.l, r.b, r.l, r.t, r.r, r.t, r.r, r.b };
    GLfloat cs[] = { bottom.r, bottom.g, bottom.b, bottom.a,
                     top.r, top.g, top.b, top.a,
                     top.r, top.g, top.b, top.a,
                     bottom.r, bottom.g, bottom.b, bottom.a };
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, vs);
    glColorPointer(4, GL_FLOAT, 0, cs);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

bool IsHovered(const Viewport& v)
{
    return v.Contains((int)process::last_x, (int)process::last_y);
}

}

// TODO: It doesn't look like this is doing anything meaningful right now...
std::mutex display_mutex;

// Render at (x,y) in window coordinates.
inline void DrawWindow(GlText& text, GLfloat x, GLfloat y, GLfloat z = 0.0)
{
    // Backup viewport
    GLint    view[4];
    glGetIntegerv(GL_VIEWPORT, view );

    auto& d = DisplayBase();
    d.Activate();
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    // Pixel continuous coords
    ProjectionMatrixOrthographic(-0.5, d.v.w-0.5, -0.5, d.v.h-0.5, -1.0, 1.0).Load();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glTranslatef( std::floor(x), std::floor(y), z);
    text.Draw();

    // Restore viewport
    glViewport(view[0],view[1],view[2],view[3]);

    // Restore modelview / project matrices
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

static inline int tab_h()
{
    return (int)(default_font().Height() * 1.4);
}

static inline float x_width()
{
  return default_font().Text("x").Width();
}

// Baseline for text vertically centred in v. We centre on the cap height so
// that the baseline doesn't jump around depending on the glyphs in a string.
static inline GLfloat text_baseline(const Viewport& v)
{
    static const GLfloat cap_height = default_font().Text("H").Height();
    return std::floor(v.b + (v.h - cap_height) / 2.0f);
}

template<typename T>
inline void MarkVarChangedByGui(VarValueT<T>& var)
{
    var.Meta().gui_changed = true;
    FlagVarChanged();
}

void glLine(GLfloat vs[4])
{
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, vs);
    glDrawArrays( GL_LINE_STRIP, 0, 2);
    glDisableClientState(GL_VERTEX_ARRAY);
}

// Shared look of push buttons
static void RenderPushButton(const Viewport& v, GlText& gltext, bool down)
{
    const bool hover = IsHovered(v);
    const Rectf r = ToRect(v);
    if(down) {
        DrawRoundedBox(r, theme::radius, theme::accent_pressed, theme::accent_pressed);
    }else{
        DrawRoundedBox(r, theme::radius, hover ? theme::widget_hover : theme::widget_bg,
                       hover ? theme::accent.WithAlpha(0.6f) : theme::widget_border);
    }
    (down ? theme::text_on_accent : theme::text).Apply();
    DrawWindow(gltext, v.l + (v.w - gltext.Width()) / 2.0f, text_baseline(v));
}

Panel::Panel()
{
    handler = &StaticHandlerScroll;
    layout = LayoutVertical;
}

Panel::Panel(const std::string& auto_register_var_prefix)
    : auto_register_var_prefix(auto_register_var_prefix)
{
    handler = &StaticHandlerScroll;
    layout = LayoutVertical;

    // Register for notifications on var additions
    var_added_connection = VarState::I().RegisterForVarEvents(
        std::bind(&Panel::NewVarCallback,this,std::placeholders::_1),
        true
    );
}

void Panel::NewVarCallback(const VarState::Event& e)
{
    const std::string name = e.var->Meta().full_name;
    if(!StartsWith(name, auto_register_var_prefix))
        return;

    switch(e.action) {
    case VarState::Event::Action::Added:
        AddVariable(name, e.var);
        break;
    case VarState::Event::Action::Removed:
        RemoveVariable(name);
        break;
    }
}

void Panel::AddVariable(const std::string& name, const std::shared_ptr<VarValueGeneric>& var)
{
    const string& title = var->Meta().friendly;
    
    display_mutex.lock();
    
    ViewMap::iterator pnl = GetCurrentContext()->named_managed_views.find(name);
    
    // Only add if a widget by the same name doesn't already exist
    if( pnl == GetCurrentContext()->named_managed_views.end() )
    {
        View* nv = NULL;
        if( !strcmp(var->TypeId(), typeid(bool).name()) ) {
            nv = (var->Meta().flags & META_FLAG_TOGGLE) ? (View*)new Checkbox(title,var) : (View*)new Button(title,var);
        } else if (!strcmp(var->TypeId(), typeid(double).name()) ||
                   !strcmp(var->TypeId(), typeid(float).name()) ||
                   !strcmp(var->TypeId(), typeid(int8_t).name()) ||
                   !strcmp(var->TypeId(), typeid(uint8_t).name()) ||
                   !strcmp(var->TypeId(), typeid(int16_t).name()) ||
                   !strcmp(var->TypeId(), typeid(uint16_t).name()) ||
                   !strcmp(var->TypeId(), typeid(int32_t).name()) ||
                   !strcmp(var->TypeId(), typeid(uint32_t).name()) ||
                   !strcmp(var->TypeId(), typeid(int64_t).name()) ||
                   !strcmp(var->TypeId(), typeid(uint64_t).name())
                   )
        {
            nv = new Slider(title, var);
        } else if (!strcmp(var->TypeId(), typeid(std::function<void(void)>).name() ) ) {
            nv = (View*)new FunctionButton(title, var);
        }else if(var->str){
            nv = new TextInput(title,var);
        }
        if(nv) {
            GetCurrentContext()->named_managed_views[name] = nv;
            views.push_back( nv );
            ResizeChildren();
        }
    }

    display_mutex.unlock();
}

void Panel::RemoveVariable(const std::string& name)
{
    display_mutex.lock();

    ViewMap::iterator pnl = GetCurrentContext()->named_managed_views.find(name);

    if( pnl != GetCurrentContext()->named_managed_views.end() ) {
      views.erase(std::remove(views.begin(), views.end(), pnl->second), views.end());
      ResizeChildren();

      delete pnl->second;
      GetCurrentContext()->named_managed_views.erase(pnl);
    }

    display_mutex.unlock();
}

void Panel::Render()
{
#ifndef HAVE_GLES
    glPushAttrib(GL_CURRENT_BIT | GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_SCISSOR_BIT | GL_VIEWPORT_BIT | GL_COLOR_BUFFER_BIT | GL_TRANSFORM_BIT);
#endif
    glEnable (GL_BLEND);
    glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    DisplayBase().ActivatePixelOrthographic();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_LINE_SMOOTH);
    glDisable( GL_COLOR_MATERIAL );
    glLineWidth(1.0);

    const Rectf r = ToRect(v);
    FillRect(r, theme::panel_bg);
    FillRect({r.r - 1.0f, r.b, r.r, r.t}, theme::panel_border);

    RenderChildren();

    // Fade out content that is cut off at the top / bottom of the panel to
    // hint that the panel can be scrolled.
    const GLfloat fade = 28.0f;
    // The last shown child is the bottom-most one (it never scrolls out)
    bool clipped_bottom = false;
    for(auto it = views.rbegin(); it != views.rend(); ++it) {
        if((*it)->IsShown()) {
            clipped_bottom = (*it)->v.b < v.b;
            break;
        }
    }
    const Rectf inner = {r.l, r.b, r.r - 1.0f, r.t};
    if(clipped_bottom) {
        FillRectGradient({inner.l, inner.b, inner.r, inner.b + fade}, theme::panel_bg, theme::panel_bg.WithAlpha(0.0f));
    }
    if(scroll_offset > 0) {
        FillRectGradient({inner.l, inner.t - fade, inner.r, inner.t}, theme::panel_bg.WithAlpha(0.0f), theme::panel_bg);
    }

#ifndef HAVE_GLES
    glPopAttrib();
#else
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_DEPTH_TEST);
#endif
}

void Panel::ResizeChildren()
{
    View::ResizeChildren();
}


View& CreatePanel(const std::string& name)
{
    if(GetCurrentContext()->named_managed_views.find(name) != GetCurrentContext()->named_managed_views.end()) {
        throw std::runtime_error("Panel already registered with this name.");
    }
    Panel * p = new Panel(name);
    GetCurrentContext()->named_managed_views[name] = p;
    GetCurrentContext()->base.views.push_back(p);
    return *p;
}

Button::Button(string title, const std::shared_ptr<VarValueGeneric>& tv)
    : Widget<bool>(title,tv), down(false)
{
    top = 1.0; bottom = Attach::Pix(-tab_h());
    left = 0.0; right = 1.0;
    hlock = LockLeft;
    vlock = LockBottom;
    gltext = default_font().Text(title);
}

void Button::Mouse(View&, MouseButton button, int /*x*/, int /*y*/, bool pressed, int /*mouse_state*/)
{
    if(button == MouseButtonLeft )
    {
        down = pressed;
        if( !pressed ) {
            var->Set(!var->Get());
            MarkVarChangedByGui(*var);
        }
    }
}

void Button::Render()
{
    if(gltext.Text() != var->Meta().friendly) {
        gltext = default_font().Text(var->Meta().friendly);
    }
    RenderPushButton(v, gltext, down);
}

void Button::ResizeChildren()
{
    raster[0] = floor(v.l + (v.w-gltext.Width())/2.0f);
    raster[1] = text_baseline(v);
}

FunctionButton::FunctionButton(string title, const std::shared_ptr<VarValueGeneric> &tv)
    : Widget<std::function<void(void)> >(title, tv), down(false)
{
    top = 1.0; bottom = Attach::Pix(-tab_h());
    left = 0.0; right = 1.0;
    hlock = LockLeft;
    vlock = LockBottom;
    gltext = default_font().Text(title);
}

void FunctionButton::Mouse(View&, MouseButton button, int /*x*/, int /*y*/, bool pressed, int /*mouse_state*/)
{
    if (button == MouseButtonLeft)
    {
        down = pressed;
        if (!pressed) {
            var->Get()();
            MarkVarChangedByGui(*var);
        }
    }
}

void FunctionButton::Render()
{
    if(gltext.Text() != var->Meta().friendly) {
        gltext = default_font().Text(var->Meta().friendly);
    }
    RenderPushButton(v, gltext, down);
}

void FunctionButton::ResizeChildren()
{
    raster[0] = v.l + (v.w - gltext.Width()) / 2.0f;
    raster[1] = text_baseline(v);
}

Checkbox::Checkbox(std::string title, const std::shared_ptr<VarValueGeneric> &tv)
    : Widget<bool>(title,tv)
{
    top = 1.0; bottom = Attach::Pix(-tab_h());
    left = 0.0; right = 1.0;
    hlock = LockLeft;
    vlock = LockBottom;
    handler = this;
    gltext = default_font().Text(title);
}

void Checkbox::Mouse(View&, MouseButton button, int /*x*/, int /*y*/, bool pressed, int /*mouse_state*/)
{
    if( button == MouseButtonLeft && pressed ) {
        var->Set(!var->Get());
        MarkVarChangedByGui(*var);
    }
}

void Checkbox::ResizeChildren()
{
    // Label on the left, toggle switch on the right
    raster[0] = v.l + theme::text_pad;
    raster[1] = text_baseline(v);
    const int sw = (int)theme::toggle_w;
    const int sh = (int)theme::toggle_h;
    vcb = Viewport(v.l + v.w - sw - (int)theme::text_pad + 2, v.b + (v.h - sh) / 2, sw, sh);
}

void Checkbox::Render()
{
    const bool val = var->Get();
    const bool hover = IsHovered(v);

    if(hover) {
        FillRoundedRect(ToRect(v), theme::radius, theme::row_hover);
    }

    // Track
    const Rectf track = ToRect(vcb);
    const Rgba track_col = val ? (hover ? theme::accent_hover : theme::accent)
                               : (hover ? theme::toggle_off_hov : theme::toggle_off);
    FillRoundedRect(track, track.h() / 2.0f, track_col);

    // Knob
    const GLfloat d = track.h() - 4.0f;
    const GLfloat kl = val ? track.r - 2.0f - d : track.l + 2.0f;
    const Rectf knob = {kl, track.b + 2.0f, kl + d, track.b + 2.0f + d};
    DrawShadow(knob.Offset(0.0f, -0.5f), d / 2.0f, 2.0f, theme::shadow);
    FillRoundedRect(knob, d / 2.0f, theme::knob);

    if(gltext.Text() != var->Meta().friendly) {
        gltext = default_font().Text(var->Meta().friendly);
    }
    (val ? theme::text : theme::text_dim).Apply();
    DrawWindow(gltext, raster[0], raster[1]);
}

inline bool IsIntegral(const char* typeidname)
{
    // TODO: There must be a better way of doing this...
    return !strcmp(typeidname, typeid(char).name()) ||
           !strcmp(typeidname, typeid(short).name()) ||
           !strcmp(typeidname, typeid(int).name())  ||
           !strcmp(typeidname, typeid(long).name()) ||
           !strcmp(typeidname, typeid(unsigned char).name()) ||
           !strcmp(typeidname, typeid(unsigned short).name()) ||
           !strcmp(typeidname, typeid(unsigned int).name()) ||
           !strcmp(typeidname, typeid(unsigned long).name());
}

Slider::Slider(std::string title, const std::shared_ptr<VarValueGeneric> &tv)
    : Widget<double>(title+":", tv), lock_bounds(true)
{
    top = 1.0; bottom = Attach::Pix(-tab_h());
    left = 0.0; right = 1.0;
    hlock = LockLeft;
    vlock = LockBottom;
    handler = this;
    logscale = (int)tv->Meta().logscale;
    gltext = default_font().Text(title);
    is_integral_type = IsIntegral(tv->TypeId());
}

void Slider::Keyboard(View&, unsigned char key, int /*x*/, int /*y*/, bool pressed)
{
    if( pressed && var->Meta().range[0] < var->Meta().range[1] )
    {
        double val = !logscale ? var->Get() : log(var->Get());

        if(key=='-' || key=='_' || key=='=' || key=='+') {
            double inc = var->Meta().increment;
            if (key == '-') inc *= -1.0;
            if (key == '_') inc *= -0.1;
            if (key == '+') inc *=  0.1;
            const double newval = max(var->Meta().range[0], min(var->Meta().range[1], val + inc));
            var->Set( logscale ? exp(newval) : newval );
            MarkVarChangedByGui(*var);
        }else if(key == 'r'){
            Reset();
            MarkVarChangedByGui(*var);
        }else{
            return;
        }
    }
}

void Slider::Mouse(View& view, MouseButton button, int x, int y, bool pressed, int mouse_state)
{
    if(pressed)
    {
        // Wheel
        if( button == MouseWheelUp || button == MouseWheelDown )
        {
            // Change scale around current value
            const double frac = max(0.0,min(1.0,(double)(x - v.l)/(double)v.w));
            double val = frac * (var->Meta().range[1] - var->Meta().range[0]) + var->Meta().range[0];

            if (logscale)
            {
                if (val<=0)
                    val = std::numeric_limits<double>::min();
                else
                    val = log(val);
            }

            const double scale = (button == MouseWheelUp ? 1.2 : 1.0 / 1.2 );
            var->Meta().range[1] = val + (var->Meta().range[1] - val)*scale;
            var->Meta().range[0] = val - (val - var->Meta().range[0])*scale;
        }else{
            lock_bounds = (button == MouseButtonLeft);
            MouseMotion(view,x,y,mouse_state);
        }
    }else{
        if(!lock_bounds)
        {
            double val = !logscale ? var->Get() : log(var->Get());

            var->Meta().range[0] = min(var->Meta().range[0], val);
            var->Meta().range[1] = max(var->Meta().range[1], val);
        }
    }
}

void Slider::MouseMotion(View&, int x, int /*y*/, int /*mouse_state*/)
{
    if( var->Meta().range[0] != var->Meta().range[1] )
    {
        const double range = (var->Meta().range[1] - var->Meta().range[0]);
        const double frac = (double)(x - v.l)/(double)v.w;
        double val;

        if( lock_bounds )
        {
            const double bfrac = max(0.0,min(1.0,frac));
            val = bfrac * range + var->Meta().range[0] ;
        }else{
            val = frac * range + var->Meta().range[0];
        }

        if (logscale) {
            val = exp(val);
        }

        if( is_integral_type ) {
            val = std::round(val);
        }

        var->Set(val);
        MarkVarChangedByGui(*var);
    }
}


void Slider::ResizeChildren()
{
    raster[0] = v.l + theme::text_pad;
    raster[1] = text_baseline(v);
}

void Slider::Render()
{
    const double val = var->Get();
    const bool hover = IsHovered(v);
    const Rectf r = ToRect(v);

    DrawRoundedBox(r, theme::radius, hover ? theme::slider_bg_hov : theme::slider_bg,
                   hover ? theme::accent.WithAlpha(0.6f) : theme::widget_border);

    if( var->Meta().range[0] != var->Meta().range[1] )
    {
        double rval = val;
        if (logscale)
        {
            rval = log(val);
        }
        const double norm_val = max(0.0,min(1.0,(rval - var->Meta().range[0]) / (var->Meta().range[1] - var->Meta().range[0])));
        const GLfloat fill_r = r.l + (GLfloat)(r.w() * norm_val);

        if(norm_val > 0.0) {
            // Clip the rounded fill to the current value
            glEnable(GL_SCISSOR_TEST);
            glScissor(v.l, v.b, (GLsizei)std::ceil(fill_r - r.l), v.h);
            FillRoundedRect(r.Inset(1.0f), theme::radius - 1.0f, hover ? theme::slider_fill_hov : theme::slider_fill);
            glDisable(GL_SCISSOR_TEST);
        }
    }

    if(gltext.Text() != var->Meta().friendly) {
        gltext = default_font().Text(var->Meta().friendly);
    }
    theme::text.Apply();
    DrawWindow(gltext, raster[0], raster[1]);

    std::ostringstream oss;
    oss << setprecision(4) << val;
    string str = oss.str();
    GlText glval = default_font().Text(str);
    const float l = glval.Width() + theme::text_pad;
    DrawWindow(glval,  v.l + v.w - l, raster[1] );
}


TextInput::TextInput(std::string title, const std::shared_ptr<VarValueGeneric> &tv)
    : Widget<std::string>(title+":", tv), can_edit(!(tv->Meta().flags & META_FLAG_READONLY)), do_edit(false)
{
    top = 1.0; bottom = Attach::Pix(-2 * tab_h());
    left = 0.0; right = 1.0;
    hlock = LockLeft;
    vlock = LockBottom;
    handler = this;
    sel[0] = -1;
    sel[1] = -1;
    gltext = default_font().Text(title);
}

void TextInput::Keyboard(View&, unsigned char key, int /*x*/, int /*y*/, bool pressed)
{
    if(can_edit && pressed && do_edit)
    {
        const bool selection = sel[1] > sel[0] && sel[0] >= 0;

        if(key == 13)
        {
            var->Set(edit);
            MarkVarChangedByGui(*var);

            do_edit = false;
            sel[0] = sel[1] = -1;
        }else if(key == 8) {
            // backspace
            if(selection)
            {
                edit = edit.substr(0,sel[0]) + edit.substr(sel[1],edit.length()-sel[1]);
                sel[1] = sel[0];
            }else{
                if(sel[0] >0)
                {
                    edit = edit.substr(0,sel[0]-1) + edit.substr(sel[0],edit.length()-sel[0]);
                    sel[0]--;
                    sel[1]--;
                }
            }
        }else if(key == 127){
            // delete
            if(selection)
            {
                edit = edit.substr(0,sel[0]) + edit.substr(sel[1],edit.length()-sel[1]);
                sel[1] = sel[0];
            }else{
                if(sel[0] < (int)edit.length())
                {
                    edit = edit.substr(0,sel[0]) + edit.substr(sel[0]+1,edit.length()-sel[0]+1);
                }
            }
        }else if(key == 230){
            // right
            sel[0] = min((int)edit.length(),sel[0]+1);
            sel[1] = sel[0];
        }else if(key == 228){
            // left
            sel[0] = max(0,sel[0]-1);
            sel[1] = sel[0];
        }else if(key == 234){
            // home
            sel[0] = sel[1] = 0;
        }else if(key == 235){
            // end
            sel[0] = sel[1] = (int)edit.length();
        }else if(key < PANGO_SPECIAL){
            edit = edit.substr(0,sel[0]).append(1,key) + edit.substr(sel[1],edit.length()-sel[1]);
            sel[1] = sel[0];
            sel[0]++;
            sel[1]++;
        }
        CalcVisibleEditPart();
    }
}

void TextInput::Mouse(View& /*view*/, MouseButton button, int x, int /*y*/, bool pressed, int /*mouse_state*/)
{
    if(can_edit && button != MouseWheelUp && button != MouseWheelDown )
    {

        if(do_edit)
        {
            const int sl = (int)gledit.Width() + horizontal_margin;
            const int rl = v.l + v.w - sl;
            std::string edit_visible = edit.substr(edit_visible_part[0], edit_visible_part[1]);
            int ep = (int)edit_visible.length();
            
            if( x < rl )
            {
                ep = 0;
            }else{
                for( unsigned i=0; i<edit_visible.length(); ++i )
                {
                    const int tl = (int)(rl + default_font().Text(edit_visible.substr(0,i)).Width());
                    if(x < tl+2)
                    {
                        ep = i;
                        break;
                    }
                }
            }
            if(pressed)
            {
                sel[0] = sel[1] = ep + edit_visible_part[0];
            }else{
                sel[1] = ep + edit_visible_part[0];
            }

            if(sel[0] > sel[1])
                std::swap(sel[0],sel[1]);
        }else{
            do_edit = !pressed;
            sel[0] = 0;
            sel[1] = (int)edit.length();
        }
    }
}

void TextInput::MouseMotion(View&, int x, int /*y*/, int /*mouse_state*/)
{
    if(can_edit && do_edit)
    {
        const int sl = (int)gledit.Width() + horizontal_margin;
        const int rl = v.l + v.w - sl;
        std::string edit_visible = edit.substr(edit_visible_part[0], edit_visible_part[1]);
        int ep = (int)edit_visible.length();
        
        if( x < rl )
        {
            ep = 0;
        }else{
            for( unsigned i=0; i<edit.length(); ++i )
            {
                const int tl = (int)(rl + default_font().Text(edit_visible.substr(0,i)).Width());
                if(x < tl+2)
                {
                    ep = i;
                    break;
                }
            }
        }
        
        sel[1] = ep + edit_visible_part[0];
    }
}


void TextInput::ResizeChildren()
{
    vertical_margin = (v.h-2.f*gltext.Height()) / 4.0f;
    input_width = v.w - 2 * horizontal_margin;
    int max_possible_chars = floor(input_width / x_width());
    edit_visible_part[1] = max_possible_chars;
    CalcVisibleEditPart();
}

void TextInput::CalcVisibleEditPart()
{
    edit_visible_part[0] =  0;

    if(default_font().Text(edit).Width() > input_width)
    {
      if(sel[1] >= 0 )
      {
        edit_visible_part[0] = max(0, sel[1] - edit_visible_part[1]);
      }
    }
};

void TextInput::Render()
{
    if(!do_edit) edit = var->Get();

    // Label on the top half, input field on the bottom half
    const Viewport label_v(v.l, v.b + v.h / 2, v.w, v.h - v.h / 2);
    const Viewport input_v(v.l,v.b,v.w,v.h / 2);

    if(can_edit) {
        const bool hover = IsHovered(input_v);
        DrawRoundedBox(ToRect(input_v), theme::radius, theme::input_bg,
                       do_edit ? theme::accent : (hover ? theme::accent.WithAlpha(0.6f) : theme::widget_border));
    }

    std::string edit_visible = edit.substr(edit_visible_part[0], edit_visible_part[1]);
    gledit = default_font().Text(edit_visible);

    const int sl = (int)gledit.Width() + horizontal_margin;
    const int rl = v.l + v.w - sl;
    const GLfloat baseline = text_baseline(input_v);

    if( do_edit && sel[0] >= 0)
    {
        const int tl = (int)(rl + default_font().Text(edit_visible.substr(0,sel[0] - edit_visible_part[0])).Width());
        const int tr = (int)(rl + default_font().Text(edit_visible.substr(0,sel[1] - edit_visible_part[0])).Width());
        const Rectf field = ToRect(input_v).Inset(4.0f);
        if(tr > tl) {
            FillRect({(GLfloat)tl, field.b, (GLfloat)tr, field.t}, theme::selection);
        }

        // Caret
        FillRect({(GLfloat)tr - 0.5f, field.b, (GLfloat)tr + 1.0f, field.t}, theme::accent_hover);
    }

    theme::text_dim.Apply();
    DrawWindow(gltext, v.l + horizontal_margin, text_baseline(label_v));

    theme::text.Apply();
    DrawWindow(gledit, (GLfloat)(rl), baseline);
}

}
