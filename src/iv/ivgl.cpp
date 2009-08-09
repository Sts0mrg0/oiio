/*
  Copyright 2008 Larry Gritz and the other authors and contributors.
  All Rights Reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:
  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
  * Neither the name of the software's owners nor the names of its
    contributors may be used to endorse or promote products derived from
    this software without specific prior written permission.
  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  (This is the Modified BSD License)
*/

#include "imageviewer.h"

#include <iostream>

// This needs to be included before GL.h
// (which is included by QtOpenGL and QGLFormat)
#include <glew.h>

#include <half.h>
#include <ImathFun.h>
#include <QGLFormat>

#include <boost/foreach.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/compare.hpp>

#include "strutil.h"
#include "fmath.h"
#include "timer.h"



#define GLERRPRINT(msg)                                           \
    for (GLenum err = glGetError();  err != GL_NO_ERROR;  err = glGetError()) \
        std::cerr << "GL error " << msg << " " << (int)err <<  " - " << (const char *)gluErrorString(err) << "\n";      \



IvGL::IvGL (QWidget *parent, ImageViewer &viewer)
    : QGLWidget(parent), m_viewer(viewer), 
      m_shaders_created(false), m_tex_created(false),
      m_zoom(1.0), m_centerx(0), m_centery(0), m_dragging(false),
      m_use_shaders(false), m_use_halffloat(false), m_use_float(false),
      m_use_srgb(false), m_texture_height(1), m_texture_width(1),
      m_shaders_using_extensions(false), m_current_image(NULL), 
      m_last_texbuf_used(0), m_use_pbo(false), m_last_pbo_used(0)
{
#if 0
    QGLFormat format;
    format.setRedBufferSize (32);
    format.setGreenBufferSize (32);
    format.setBlueBufferSize (32);
    format.setAlphaBufferSize (32);
    format.setDepth (true);
    setFormat (format);
#endif
    setMouseTracking (true);
}



IvGL::~IvGL ()
{
}



void
IvGL::initializeGL ()
{
    GLenum glew_error = glewInit ();
    if (glew_error != GLEW_OK) {
        std::cerr << "GLEW init error " << glewGetErrorString (glew_error) << "\n";
    }

    glClearColor (0.05, 0.05, 0.05, 1.0);
    glShadeModel (GL_FLAT);
    glEnable (GL_DEPTH_TEST);
    glDisable (GL_CULL_FACE);
    glEnable (GL_BLEND);
    glEnable (GL_TEXTURE_2D);
    // glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glHint (GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
    // Make sure initial matrix is identity (returning to this stack level loads
    // back this matrix).
    glLoadIdentity();
    // There's this small detail in the OpenGL 2.1 (probably earlier versions
    // too) spec:
    //
    // (For TexImage3D, TexImage2D and TexImage1D):
    // The values of UNPACK ROW LENGTH and UNPACK ALIGNMENT control the row-to-
    // row spacing in these images in the same manner as DrawPixels.
    //
    // UNPACK_ALIGNMENT has a default value of 4 according to the spec. Which
    // means that it was expecting images to be Aligned to 4-bytes, and making
    // several odd "skew-like effects" in the displayed images. Setting the
    // alignment to 1 byte fixes this problems.
    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);          

    // here we check what OpenGL extensions are available, and take action
    // if needed
    check_gl_extensions ();

    create_textures ();

    create_shaders ();
}



void
IvGL::create_textures (void)
{
    if (m_tex_created)
        return;

    // FIXME: Determine this dynamically.
    const int total_texbufs = 4;
    GLuint textures[total_texbufs];

    glGenTextures (total_texbufs, textures);

    // Initialize texture objects
    for (int i = 0; i < total_texbufs; i++) {
        m_texbufs.push_back (TexBuffer());
        glBindTexture (GL_TEXTURE_2D, textures[i]);
        GLERRPRINT ("bind tex");
        glTexImage2D (GL_TEXTURE_2D, 0 /*mip level*/,
                      4 /*internal format - color components */,
                      1 /*width*/, 1 /*height*/, 0 /*border width*/,
                      GL_RGBA /*type - GL_RGB, GL_RGBA, GL_LUMINANCE */,
                      GL_FLOAT /*format - GL_FLOAT */,
                      NULL /*data*/);
        GLERRPRINT ("tex image 2d");
        // Initialize tex parameters.
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        GLERRPRINT ("After tex parameters");
        m_texbufs.back().tex_object = textures[i];
        m_texbufs.back().x = 0;
        m_texbufs.back().y = 0;
        m_texbufs.back().width = 0;
        m_texbufs.back().height = 0;
    }

    // Create another texture for the pixelview.
    glGenTextures (1, &m_pixelview_tex);
    glBindTexture (GL_TEXTURE_2D, m_pixelview_tex);
    glTexImage2D (GL_TEXTURE_2D, 0, 
                  4, closeuptexsize, closeuptexsize, 0, 
                  GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    if (m_use_pbo) {
        glGenBuffersARB(2, m_pbo_objects);
        glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, m_pbo_objects[0]);
        glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, m_pbo_objects[1]);
        glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
    }

    m_tex_created = true;
}



void
IvGL::create_shaders (void)
{
    static const GLchar *vertex_source = 
        "varying vec2 vTexCoord;\n"
        "void main ()\n"
        "{\n"
        "    vTexCoord = gl_MultiTexCoord0.xy;\n"
        "    gl_Position = ftransform();\n"
        "}\n";

    static const GLchar *fragment_source = 
        "uniform sampler2D imgtex;\n"
        "varying vec2 vTexCoord;\n"
        "uniform float gain;\n"
        "uniform float gamma;\n"
        "uniform int channelview;\n"
        "uniform int imgchannels;\n"
        "uniform int pixelview;\n"
        "uniform int linearinterp;\n"
        "uniform int width;\n"
        "uniform int height;\n"
        "void main ()\n"
        "{\n"
        "    vec2 st = vTexCoord;\n"
        "    float black = 0.0;\n"
        "    if (pixelview != 0 || linearinterp == 0) {\n"
        "        vec2 wh = vec2(width,height);\n"
        "        vec2 onehalf = vec2(0.5,0.5);\n"
        "        vec2 st_res = st * wh /* + onehalf */ ;\n"
        "        vec2 st_pix = floor (st_res);\n"
        "        vec2 st_rem = st_res - st_pix;\n"
        "        st = (st_pix + onehalf) / wh;\n"
        "        if (pixelview != 0) {\n"
        "            if (st.x < 0.0 || st.x >= 1.0 || \n"
        "                    st.y < 0.0 || st.y >= 1.0 || \n"
        "                    st_rem.x < 0.05 || st_rem.x >= 0.95 || \n"
        "                    st_rem.y < 0.05 || st_rem.y >= 0.95)\n"
        "                black = 1.0;\n"
        "        }\n"
        "    }\n"
        "    vec4 C = texture2D (imgtex, st);\n"
        "    C = mix (C, vec4(0.05,0.05,0.05,1.0), black);\n"
        "    if (pixelview != 0)\n"
        "        C.a = 1.0;\n"
        "    if (imgchannels <= 2)\n"
        "        C.xyz = C.xxx;\n"
        "    if (channelview == -1) {\n"
        "    }\n"
        "    else if (channelview == 0)\n"
        "        C.xyz = C.xxx;\n"
        "    else if (channelview == 1)\n"
        "        C.xyz = C.yyy;\n"
        "    else if (channelview == 2)\n"
        "        C.xyz = C.zzz;\n"
        "    else if (channelview == 3)\n"
        "        C.xyz = C.www;\n"
        "    else if (channelview == -2) {\n"
        "        float lum = dot (C.xyz, vec3(0.2126, 0.7152, 0.0722));\n"
        "        C.xyz = vec3 (lum, lum, lum);\n"
        "    }\n"
        "    C.xyz *= gain;\n"
        "    float invgamma = 1.0/gamma;\n"
        "    C.xyz = pow (C.xyz, vec3 (invgamma, invgamma, invgamma));\n"
        "    gl_FragColor = C;\n"
        "}\n";

    if (!m_use_shaders) {
        std::cerr << "Not using shaders!\n";
        return;
    }
    if (m_shaders_created)
        return;

    // When using extensions to support shaders, we need to load the function
    // entry points (which is actually done by GLEW) and then call them. So
    // we have to get the functions through the right symbols otherwise
    // extension-based shaders won't work.
    if (m_shaders_using_extensions) {
        m_shader_program = glCreateProgramObjectARB ();
    }
    else {
        m_shader_program = glCreateProgram ();
    }
    GLERRPRINT ("create progam");

    // This holds the compilation status
    GLint status;

    if (m_shaders_using_extensions) {
        m_vertex_shader = glCreateShaderObjectARB (GL_VERTEX_SHADER_ARB);
        glShaderSourceARB (m_vertex_shader, 1, &vertex_source, NULL);
        glCompileShaderARB (m_vertex_shader);
        glGetObjectParameterivARB (m_vertex_shader,
                GL_OBJECT_COMPILE_STATUS_ARB, &status);
    } else {
        m_vertex_shader = glCreateShader (GL_VERTEX_SHADER);
        glShaderSource (m_vertex_shader, 1, &vertex_source, NULL);
        glCompileShader (m_vertex_shader);
        glGetShaderiv (m_vertex_shader, GL_COMPILE_STATUS, &status);
    }
    if (! status) {
        // FIXME: How to handle this error?
        std::cerr << "vertex shader compile status: failed\n";
    }
    if (m_shaders_using_extensions) {
        glAttachObjectARB (m_shader_program, m_vertex_shader);
    } else {
        glAttachShader (m_shader_program, m_vertex_shader);
    }
    GLERRPRINT ("After attach vertex shader.");

    if (m_shaders_using_extensions) {
        m_fragment_shader = glCreateShaderObjectARB (GL_FRAGMENT_SHADER_ARB);
        glShaderSourceARB (m_fragment_shader, 1, &fragment_source, NULL);
        glCompileShaderARB (m_fragment_shader);
        glGetObjectParameterivARB (m_fragment_shader,
                GL_OBJECT_COMPILE_STATUS_ARB, &status);
    } else {
        m_fragment_shader = glCreateShader (GL_FRAGMENT_SHADER);
        glShaderSource (m_fragment_shader, 1, &fragment_source, NULL);
        glCompileShader (m_fragment_shader);
        glGetShaderiv (m_fragment_shader, GL_COMPILE_STATUS, &status);
    }
    if (! status) {
        std::cerr << "fragment shader compile status: " << status << "\n";
        char buf[10000];
        buf[0] = 0;
        GLsizei len;
        if (m_shaders_using_extensions) {
            glGetInfoLogARB (m_fragment_shader, sizeof(buf), &len, buf);
        } else {
            glGetShaderInfoLog (m_fragment_shader, sizeof(buf), &len, buf);
        }
        std::cerr << "compile log:\n" << buf << "---\n";
        // FIXME: How to handle this error?
    }
    if (m_shaders_using_extensions) {
        glAttachObjectARB (m_shader_program, m_fragment_shader);
    } else {
        glAttachShader (m_shader_program, m_fragment_shader);
    }
    GLERRPRINT ("After attach fragment shader");

    if (m_shaders_using_extensions) {
        glLinkProgramARB (m_shader_program);
    } else {
        glLinkProgram (m_shader_program);
    }
    GLERRPRINT ("link");
    GLint linked;
    if (m_shaders_using_extensions) {
        glGetObjectParameterivARB (m_shader_program,
                GL_OBJECT_LINK_STATUS_ARB, &linked);
    } else {
        glGetProgramiv (m_shader_program, GL_LINK_STATUS, &linked);
    }
    if (! linked) {
        std::cerr << "NOT LINKED\n";
        // FIXME: How to handle this error?
    }

    m_shaders_created = true;
}



void
IvGL::resizeGL (int w, int h)
{
    GLERRPRINT ("resizeGL entry");
    glViewport (0, 0, w, h);
    glMatrixMode (GL_PROJECTION);
    glLoadIdentity ();
    glOrtho (-w/2.0, w/2.0, -h/2.0, h/2.0, 0, 10);
    // Main GL viewport is set up for orthographic view centered at
    // (0,0) and with width and height equal to the window dimensions IN
    // PIXEL UNITS.
    glMatrixMode (GL_MODELVIEW);

    clamp_view_to_window ();
    GLERRPRINT ("resizeGL exit");
}



static void
gl_rect (float xmin, float ymin, float xmax, float ymax, float z = 0,
         float smin = 0, float tmin = 0, float smax = 1, float tmax = 1,
         int rotate = 0)
{
    float tex[] = { smin, tmin, smax, tmin, smax, tmax, smin, tmax };
    glBegin (GL_POLYGON);
    glTexCoord2f (tex[(0+2*rotate)&7], tex[(1+2*rotate)&7]);
    glVertex3f (xmin,  ymin, z);
    glTexCoord2f (tex[(2+2*rotate)&7], tex[(3+2*rotate)&7]);
    glVertex3f (xmax,  ymin, z);
    glTexCoord2f (tex[(4+2*rotate)&7], tex[(5+2*rotate)&7]);
    glVertex3f (xmax, ymax, z);
    glTexCoord2f (tex[(6+2*rotate)&7], tex[(7+2*rotate)&7]);
    glVertex3f (xmin, ymax, z);
    glEnd ();
}



void
IvGL::paintGL ()
{
#ifdef DEBUG
    Timer paint_image_time;
    paint_image_time.start();
#endif
    //std::cerr << "paintGL " << m_viewer.current_image() << " with zoom " << m_zoom << "\n";
    glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    IvImage *img = m_current_image;
    if (! img || ! img->image_valid())
        return;

    const ImageSpec &spec (img->spec());
    float z = m_zoom;

    glPushMatrix ();
    // Transform is now same as the main GL viewport -- window pixels as
    // units, with (0,0) at the center of the visible unit.
    glTranslatef (0, 0, -5.0);
    // Pushed away from the camera 5 units.
    glScalef (1, -1, 1);
    // Flip y, because OGL's y runs from bottom to top.
    glScalef (z, z, 1);
    // Scaled by zoom level.  So now xy units are image pixels as
    // displayed at the current zoom level, with the origin at the
    // center of the visible window.

    // Handle the orientation with OpenGL *before* translating our center.
    float real_centerx = m_centerx;
    float real_centery = m_centery;
    switch (img->orientation()) {
    case 2: // flipped horizontally
        glScalef (-1, 1, 1);
        real_centerx = spec.width - m_centerx;
        break;
    case 3: // bottom up, rigth to left (rotated 180).
        glScalef (-1, -1, 1);
        real_centerx = spec.width - m_centerx;
        real_centery = spec.height - m_centery;
        break;
    case 4: // flipped vertically.
        glScalef (1, -1, 1);
        real_centery = spec.height - m_centery;
        break;
    case 5: // transposed (flip horizontal & rotated 90 ccw).
        glScalef (-1, 1, 1);
        glRotatef (90, 0, 0, 1);
        real_centerx = m_centery;
        real_centery = m_centerx;
        break;
    case 6: // rotated 90 cw.
        glRotatef (-270.0, 0, 0, 1);
        real_centerx = m_centery;
        real_centery = spec.height - m_centerx;
        break;
    case 7: // transverse, (flip horizontal & rotated 90 cw, r-to-l, b-to-t)
        glScalef (-1, 1, 1);
        glRotatef (-90, 0, 0, 1);
        real_centerx = spec.width - m_centery;
        real_centery = spec.height - m_centerx;
        break;
    case 8: // rotated 90 ccw.
        glRotatef (-90, 0, 0, 1);
        real_centerx = spec.width - m_centery;
        real_centery = m_centerx;
        break;
    case 1: // horizontal
    case 0: // unknown
    default:
        break;
    }
    glTranslatef (-real_centerx, -real_centery, 0.0f);
    // Recentered so that the pixel space (m_centerx,m_centery) position is
    // at the center of the visible window.

    useshader (m_texture_width, m_texture_height);

    float smin = 0, smax = 1.0;
    float tmin = 0, tmax = 1.0;
    // Image pixels shown from the center to the edge of the window.
    int wincenterx = (int) ceil (width()/(2*m_zoom));
    int wincentery = (int) ceil (height()/(2*m_zoom));
    if (img->orientation() > 4) {
        std::swap (wincenterx, wincentery);
    }

    int xbegin = (int) floor (real_centerx) - wincenterx;
    xbegin = std::max (spec.x, xbegin - (xbegin % m_texture_width));
    int ybegin = (int) floor (real_centery) - wincentery;
    ybegin = std::max (spec.y, ybegin - (ybegin % m_texture_height));
    int xend   = (int) floor (real_centerx) + wincenterx;
    xend = std::min (spec.x + spec.width, xend + m_texture_width - (xend % m_texture_width));
    int yend   = (int) floor (real_centery) + wincentery;
    yend = std::min (spec.y + spec.height, yend + m_texture_height - (yend % m_texture_height));
    //std::cerr << "(" << xbegin << ',' << ybegin << ") - (" << xend << ',' << yend << ")\n";

    // Provide some feedback
    int total_tiles = (int) (ceilf(float(xend-xbegin)/m_texture_width) * ceilf(float(yend-ybegin)/m_texture_height));
    float tile_advance = 1.0f/total_tiles;
    float percent = tile_advance;
    m_viewer.statusViewInfo->hide ();
    m_viewer.statusProgress->show ();

    // FIXME: change the code path so we can take full advantage of async DMA
    // when using PBO.
    for (int ystart = ybegin ; ystart < yend; ystart += m_texture_height) {
        for (int xstart = xbegin ; xstart < xend; xstart += m_texture_width) {
            int tile_width = std::min (xend - xstart, m_texture_width);
            int tile_height = std::min (yend - ystart, m_texture_height);
            smax = tile_width/float (m_texture_width);
            tmax = tile_height/float (m_texture_height);

            //std::cerr << "xstart: " << xstart << ". ystart: " << ystart << "\n";
            //std::cerr << "tile_width: " << tile_width << ". tile_height: " << tile_height << "\n";

            // FIXME: This can get too slow. Some ideas: avoid sending the tex
            // images more than necessary, figure an optimum texture size, use
            // multiple texture objects.
            load_texture (xstart, ystart, tile_width, tile_height, percent);
            gl_rect (xstart, ystart, xstart+tile_width, ystart+tile_height, 0,
                     smin, tmin, smax, tmax);
            percent += tile_advance;
        }
    }

    glPopMatrix ();

    if (m_viewer.pixelviewOn()) {
        paint_pixelview ();
    }

    // Show the status info again.
    m_viewer.statusProgress->hide ();
    m_viewer.statusViewInfo->show ();
    unsetCursor ();

#ifdef DEBUG
    std::cerr << "paintGL elapsed time: " << paint_image_time() << " seconds\n";
#endif
}



void
IvGL::shadowed_text (float x, float y, float z, const std::string &s,
                     const QFont &font)
{
    QString q (s.c_str());
#if 0
    glColor4f (0, 0, 0, 1);
    const int b = 2;  // blur size
    for (int i = -b;  i <= b;  ++i)
        for (int j = -b;  j <= b;  ++j)
            renderText (x+i, y+j, q, font);
#endif
    glColor4f (1, 1, 1, 1);
    renderText (x, y, z, q, font);
}



void
IvGL::paint_pixelview ()
{
    IvImage *img = m_current_image;
    const ImageSpec &spec (img->spec());

    // (xw,yw) are the window coordinates of the mouse.
    int xw, yw;
    get_focus_window_pixel (xw, yw);

    // (xp,yp) are the image-space [0..res-1] position of the mouse.
    int xp, yp;
    get_focus_image_pixel (xp, yp);

    glPushMatrix ();
    // Transform is now same as the main GL viewport -- window pixels as
    // units, with (0,0) at the center of the visible window.

    glTranslatef (0, 0, -1);
    // Pushed away from the camera 1 unit.  This makes the pixel view
    // elements closer to the camera than the main view.

    if (m_viewer.pixelviewFollowsMouse()) {
        // Display closeup overtop mouse -- translate the coordinate system
        // so that it is centered at the mouse position.
        glTranslatef (xw - width()/2, -yw + height()/2, 0);
    } else {
        // Display closeup in upper left corner -- translate the coordinate
        // system so that it is centered near the upper left of the window.
        glTranslatef (closeupsize*0.5f + 5 - width()/2,
                      -closeupsize*0.5f - 5 + height()/2, 0);
    }
    // In either case, the GL coordinate system is now scaled to window
    // pixel units, and centered on the middle of where the closeup
    // window is going to appear.  All other coordinates from here on
    // (in this procedure) should be relative to the closeup window center.

    glPushAttrib (GL_ENABLE_BIT | GL_TEXTURE_BIT);
    useshader (closeuptexsize, closeuptexsize, true);

    float smin, tmin, smax, tmax;
    if (xp >= 0 && xp < img->oriented_width() && yp >= 0 && yp < img->oriented_height()) {
        // Keep the view within ncloseuppixels pixels.
        int xpp = std::min(std::max (xp, ncloseuppixels/2), spec.width - ncloseuppixels/2 - 1);
        int ypp = std::min(std::max (yp, ncloseuppixels/2), spec.height - ncloseuppixels/2 - 1);
        // Calculate patch of the image to use for the pixelview.
        int xbegin = std::max (xpp - ncloseuppixels/2, 0);
        int ybegin = std::max (ypp - ncloseuppixels/2, 0);
        int xend   = std::min (xpp + ncloseuppixels/2+1, spec.width);
        int yend   = std::min (ypp + ncloseuppixels/2+1, spec.height);
        smin = 0.0;
        tmin = 0.0;
        smax = float (xend-xbegin)/closeuptexsize;
        tmax = float (yend-ybegin)/closeuptexsize;
        //std::cerr << "img (" << xbegin << "," << ybegin << ") - (" << xend << "," << yend << ")\n";
        //std::cerr << "tex (" << smin << "," << tmin << ") - (" << smax << "," << tmax << ")\n";

        void *zoombuffer = NULL;
        if (m_use_shaders) {
            zoombuffer  = alloca ((xend-xbegin)*(xend-xbegin)*spec.pixel_bytes());
            img->copy_pixels (spec.x + xbegin, spec.x + xend,
                              spec.y + ybegin, spec.y + yend,
                              spec.format, zoombuffer);
        } else {
            zoombuffer = img->pixeladdr (spec.x + xbegin, spec.y + ybegin);
            glPixelStorei (GL_UNPACK_ROW_LENGTH, spec.width);
        }

        GLenum glformat, gltype, glinternalformat;
        typespec_to_opengl (spec, gltype, glformat, glinternalformat);
        // Use pixelview's own texture, and upload the corresponding image patch.
        if (m_use_pbo) {
            glBindBufferARB (GL_PIXEL_UNPACK_BUFFER_ARB, 0);
        }
        glBindTexture (GL_TEXTURE_2D, m_pixelview_tex);
        glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, 
                         xend-xbegin, yend-ybegin,
                         glformat, gltype,
                         zoombuffer);
        GLERRPRINT ("After tsi2d");
    } else {
        glDisable (GL_TEXTURE_2D);
        glColor3f (0.1,0.1,0.1);
    }
    if (! m_use_shaders) {
        glDisable (GL_BLEND);
    }

    // This square is the closeup window itself
    gl_rect (-0.5f*closeupsize, 0.5f*closeupsize,
            0.5f*closeupsize, -0.5f*closeupsize, 0,
            smin, tmin, smax, tmax);
    glPopAttrib ();

    // Draw a second window, slightly behind the closeup window, as a
    // backdrop.  It's partially transparent, having the effect of
    // darkening the main image view beneath the closeup window.  It
    // extends slightly out from the closeup window (making it more
    // clearly visible), and also all the way down to cover the area
    // where the text will be printed, so it is very readable.
    const int yspacing = 18;

    glPushAttrib (GL_ENABLE_BIT);
    glDisable (GL_TEXTURE_2D);
    if (m_use_shaders) {
        // Disable shaders for this.
        gl_use_program (0);
    }
    float extraspace = yspacing * (1 + spec.nchannels) + 4;
    glColor4f (0.1, 0.1, 0.1, 0.5);
    gl_rect (-0.5f*closeupsize-2, 0.5f*closeupsize+2,
             0.5f*closeupsize+2, -0.5f*closeupsize - extraspace, -0.1);

    if (xp >= 0 && xp < img->oriented_width() && yp >= 0 && yp < img->oriented_height()) {
        // Now we print text giving the mouse coordinates and the numerical
        // values of the pixel that the mouse is over.
        QFont font;
        font.setFixedPitch (true);
        float *fpixel = (float *) alloca (spec.nchannels*sizeof(float));
        int textx = - closeupsize/2 + 4;
        int texty = - closeupsize/2 - yspacing;
        std::string s = Strutil::format ("(%d, %d)", xp+spec.x, yp+spec.y);
        shadowed_text (textx, texty, 0.0f, s, font);
        texty -= yspacing;
        img->getpixel (xp+spec.x, yp+spec.y, fpixel);
        for (int i = 0;  i < spec.nchannels;  ++i) {
            switch (spec.format.basetype) {
            case TypeDesc::UINT8 : {
                ImageBuf::ConstIterator<unsigned char,unsigned char> p (*img, xp+spec.x, yp+spec.y);
                s = Strutil::format ("%s: %3d  (%5.3f)",
                                     spec.channelnames[i].c_str(),
                                     (int)(p[i]), fpixel[i]);
                }
                break;
            case TypeDesc::UINT16 : {
                ImageBuf::ConstIterator<unsigned short,unsigned short> p (*img, xp+spec.x, yp+spec.y);
                s = Strutil::format ("%s: %3d  (%5.3f)",
                                     spec.channelnames[i].c_str(),
                                     (int)(p[i]), fpixel[i]);
                }
                break;
            default:  // everything else, treat as float
                s = Strutil::format ("%s: %5.3f",
                                     spec.channelnames[i].c_str(), fpixel[i]);
            }
            shadowed_text (textx, texty, 0.0f, s, font);
            texty -= yspacing;
        }
    }

    glPopAttrib ();
    glPopMatrix ();
}



void
IvGL::useshader (int tex_width, int tex_height, bool pixelview)
{
    IvImage *img = m_viewer.cur();
    if (! img)
        return;

    if (!m_use_shaders) {
        glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        BOOST_FOREACH (TexBuffer &tb, m_texbufs) {
            glBindTexture (GL_TEXTURE_2D, tb.tex_object);
            if (m_viewer.linearInterpolation ()) {
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            }
            else {
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            }
        }
        return;
    }

    const ImageSpec &spec (img->spec());

    gl_use_program (m_shader_program);
    GLERRPRINT ("After use program");

    GLint loc;

    loc = gl_get_uniform_location ("imgtex");
    // This is the texture unit, not the texture object
    gl_uniform (loc, 0);

    loc = gl_get_uniform_location ("gain");

    float gain = powf (2.0, img->exposure ());
    gl_uniform (loc, gain);

    loc = gl_get_uniform_location ("gamma");
    gl_uniform (loc, img->gamma ());

    loc = gl_get_uniform_location ("channelview");
    gl_uniform (loc, m_viewer.current_channel ());

    loc = gl_get_uniform_location ("imgchannels");
    gl_uniform (loc, spec.nchannels);

    loc = gl_get_uniform_location ("pixelview");
    gl_uniform (loc, pixelview);

    loc = gl_get_uniform_location ("linearinterp");
    gl_uniform (loc, m_viewer.linearInterpolation ());

    loc = gl_get_uniform_location ("width");
    gl_uniform (loc, tex_width);

    loc = gl_get_uniform_location ("height");
    gl_uniform (loc, tex_height);
    GLERRPRINT ("After settting uniforms");
}



void
IvGL::update ()
{
    //std::cerr << "update image\n";
    
    IvImage* img = m_viewer.cur();
    if (! img)
        return;

    const ImageSpec &spec (img->spec());

    GLenum gltype = GL_UNSIGNED_BYTE;
    GLenum glformat = GL_RGB;
    GLenum glinternalformat = GL_RGB;
    typespec_to_opengl (spec, gltype, glformat, glinternalformat);

    m_texture_width = std::min (pow2roundup(spec.width), m_max_texture_size);
    m_texture_height= std::min (pow2roundup(spec.height), m_max_texture_size);

    if (m_use_pbo) {
        // Otherwise OpenGL will confuse the NULL with an index into one of
        // the PBOs.
        glBindBufferARB (GL_PIXEL_UNPACK_BUFFER_ARB, 0);
    }
    // We need to reupload the texture only when changing images or when not
    // using GLSL and changing channel/exposure/gamma.
    BOOST_FOREACH (TexBuffer &tb, m_texbufs) {
        tb.width = 0;
        tb.height= 0;
        glBindTexture (GL_TEXTURE_2D, tb.tex_object);
        glTexImage2D (GL_TEXTURE_2D, 0 /*mip level*/,
                      glinternalformat,
                      m_texture_width,  m_texture_height,
                      0 /*border width*/,
                      glformat, gltype, 
                      NULL /*data*/);
        GLERRPRINT ("Setting up texture");
    }

    // Set the right type for the texture used for pixelview.
    glBindTexture (GL_TEXTURE_2D, m_pixelview_tex);
    glTexImage2D (GL_TEXTURE_2D, 0, glinternalformat,
                  closeuptexsize, closeuptexsize, 0,
                  glformat, gltype, NULL);
    GLERRPRINT ("Setting up pixelview texture");


    if (m_use_shaders && !m_use_pbo) {
        // Resize the buffer at once, rather than create one each drawing.
        m_tex_buffer.resize (m_texture_width * m_texture_height * spec.pixel_bytes());
    }
    m_current_image = img;
}



void
IvGL::view (float xcenter, float ycenter, float zoom, bool redraw)
{
    m_centerx = xcenter;
    m_centery = ycenter;
    m_zoom = zoom;

    IvImage *img = m_viewer.cur();
    if (img) {
        clamp_view_to_window ();
    }
    if (redraw)
        trigger_redraw ();
}



void
IvGL::pan (float dx, float dy)
{
    center (m_centerx + dx, m_centery + dy);
}



void
IvGL::remember_mouse (const QPoint &pos)
{
    m_mousex = pos.x();
    m_mousey = pos.y();
}



void
IvGL::clamp_view_to_window ()
{
    IvImage *img = m_current_image;
    if (! img)
        return;
    int w = width(), h = height();
    float zoomedwidth  = m_zoom * img->oriented_full_width();
    float zoomedheight = m_zoom * img->oriented_full_height();
#if 0
    float left    = m_centerx - 0.5 * ((float)w / m_zoom);
    float top     = m_centery - 0.5 * ((float)h / m_zoom);
    float right   = m_centerx + 0.5 * ((float)w / m_zoom);
    float bottom  = m_centery + 0.5 * ((float)h / m_zoom);
    std::cerr << "Window size is " << w << " x " << h << "\n";
    std::cerr << "Center (pixel coords) is " << m_centerx << ", " << m_centery << "\n";
    std::cerr << "Top left (pixel coords) is " << left << ", " << top << "\n";
    std::cerr << "Bottom right (pixel coords) is " << right << ", " << bottom << "\n";
#endif

    int xmin = std::min (img->oriented_x(), img->oriented_full_x());
    int xmax = std::max (img->oriented_x()+img->oriented_width(),
                         img->oriented_full_x()+img->oriented_full_width());
    int ymin = std::min (img->oriented_y(), img->oriented_full_y());
    int ymax = std::max (img->oriented_y()+img->oriented_height(),
                         img->oriented_full_y()+img->oriented_full_height());

    // Don't let us scroll off the edges
    if (zoomedwidth >= w) {
        m_centerx = Imath::clamp (m_centerx, xmin + 0.5f*w/m_zoom, xmax - 0.5f*w/m_zoom);
    } else {
        m_centerx = img->oriented_full_x() + img->oriented_full_width()/2;
    }

    if (zoomedheight >= h) {
        m_centery = Imath::clamp (m_centery, ymin + 0.5f*h/m_zoom, ymax - 0.5f*h/m_zoom);
    } else {
        m_centery = img->oriented_full_y() + img->oriented_full_height()/2;
    }
}



void
IvGL::mousePressEvent (QMouseEvent *event)
{
    remember_mouse (event->pos());
    int mousemode = m_viewer.mouseModeComboBox->currentIndex ();
    bool Alt = (event->modifiers() & Qt::AltModifier);
    m_drag_button = event->button();
    switch (event->button()) {
    case Qt::LeftButton :
        if (mousemode == ImageViewer::MouseModeZoom && !Alt)
            m_viewer.zoomIn();
        else
            m_dragging = true;
        return;
    case Qt::RightButton :
        if (mousemode == ImageViewer::MouseModeZoom && !Alt)
            m_viewer.zoomOut();
        else
            m_dragging = true;
        return;
    case Qt::MidButton :
        m_dragging = true;
        // FIXME: should this be return rather than break?
        break;
    default:
        break;
    }
    parent_t::mousePressEvent (event);
}



void
IvGL::mouseReleaseEvent (QMouseEvent *event)
{
    remember_mouse (event->pos());
    m_drag_button = Qt::NoButton;
    m_dragging = false;
    parent_t::mouseReleaseEvent (event);
}



void
IvGL::mouseMoveEvent (QMouseEvent *event)
{
    QPoint pos = event->pos();
    // FIXME - there's probably a better Qt way than tracking the button
    // myself.
    bool Alt = (event->modifiers() & Qt::AltModifier);
    int mousemode = m_viewer.mouseModeComboBox->currentIndex ();
    bool do_pan = false, do_zoom = false, do_wipe = false;
    bool do_select = false, do_annotate = false;
    switch (mousemode) {
    case ImageViewer::MouseModeZoom :
        if ((m_drag_button == Qt::MidButton) ||
            (m_drag_button == Qt::LeftButton && Alt)) {
            do_pan = true;
        } else if (m_drag_button == Qt::RightButton && Alt) {
            do_zoom = true;
        }
        break;
    case ImageViewer::MouseModePan :
        if (m_drag_button != Qt::NoButton)
            do_pan = true;
        break;
    case ImageViewer::MouseModeWipe :
        if (m_drag_button != Qt::NoButton)
            do_wipe = true;
        break;
    case ImageViewer::MouseModeSelect :
        if (m_drag_button != Qt::NoButton)
            do_select = true;
        break;
    case ImageViewer::MouseModeAnnotate :
        if (m_drag_button != Qt::NoButton)
            do_annotate = true;
        break;
    }
    if (do_pan) {
        float dx = (pos.x() - m_mousex) / m_zoom;
        float dy = (pos.y() - m_mousey) / m_zoom;
        pan (-dx, -dy);
    } else if (do_zoom) {
        float dx = (pos.x() - m_mousex);
        float dy = (pos.y() - m_mousey);
        float z = m_viewer.zoom() * (1.0 + 0.005 * (dx + dy));
        z = Imath::clamp (z, 0.01f, 256.0f);
        m_viewer.zoom (z);
        m_viewer.fitImageToWindowAct->setChecked (false);
    } else if (do_wipe) {
        // FIXME -- unimplemented
    } else if (do_select) {
        // FIXME -- unimplemented
    } else if (do_annotate) {
        // FIXME -- unimplemented
    }
    remember_mouse (pos);
    if (m_viewer.pixelviewOn())
        trigger_redraw ();
    parent_t::mouseMoveEvent (event);
}



void
IvGL::wheelEvent (QWheelEvent *event)
{
    if (event->orientation() == Qt::Vertical) {
        int degrees = event->delta() / 8;
        if (true || (event->modifiers() & Qt::AltModifier)) {
            // Holding down Alt while wheeling makes smooth zoom of small
            // increments
            float z = m_viewer.zoom();
            z *= 1.0 + 0.005*degrees;
            z = Imath::clamp (z, 0.01f, 256.0f);
            m_viewer.zoom (z);
            m_viewer.fitImageToWindowAct->setChecked (false);
        } else {
            if (degrees > 5)
                m_viewer.zoomIn ();
            else if (degrees < -5)
                m_viewer.zoomOut ();
        }
        event->accept();
    }
}



void
IvGL::get_focus_window_pixel (int &x, int &y)
{
    x = m_mousex;
    y = m_mousey;
}



void
IvGL::get_focus_image_pixel (int &x, int &y)
{
    // w,h are the dimensions of the visible window, in pixels
    int w = width(), h = height();
    float z = m_zoom;
    // left,top,right,bottom are the borders of the visible window, in 
    // pixel coordinates
    float left    = m_centerx - 0.5 * w / z;
    float top     = m_centery - 0.5 * h / z;
    float right   = m_centerx + 0.5 * w / z;
    float bottom  = m_centery + 0.5 * h / z;
    // normx,normy are the position of the mouse, in normalized (i.e. [0..1])
    // visible window coordinates.
    float normx = (float)(m_mousex + 0.5f) / w;
    float normy = (float)(m_mousey + 0.5f) / h;
    // imgx,imgy are the position of the mouse, in pixel coordinates
    float imgx = Imath::lerp (left, right, normx);
    float imgy = Imath::lerp (top, bottom, normy);
    // So finally x,y are the coordinates of the image pixel (on [0,res-1])
    // underneath the mouse cursor.
    //FIXME: Shouldn't this take image rotation into account?
    x = imgx;
    y = imgy;
#if 0
    std::cerr << "get_focus_pixel\n";
    std::cerr << "    mouse window pixel coords " << m_mousex << ' ' << m_mousey << "\n";
    std::cerr << "    mouse window NDC coords " << normx << ' ' << normy << '\n';
    std::cerr << "    center image coords " << m_centerx << ' ' << m_centery << "\n";
    std::cerr << "    left,top = " << left << ' ' << top << "\n";
    std::cerr << "    right,bottom = " << right << ' ' << bottom << "\n";
    std::cerr << "    mouse image coords " << imgx << ' ' << imgy << "\n";
    std::cerr << "    mouse pixel image coords " << x << ' ' << y << "\n";
#endif
}



inline void
IvGL::gl_use_program (int program)
{
    if (m_shaders_using_extensions) 
        glUseProgramObjectARB (program);
    else
        glUseProgram (program);
}



inline GLint
IvGL::gl_get_uniform_location (const char *uniform)
{
    if (m_shaders_using_extensions)
        return glGetUniformLocationARB (m_shader_program, uniform);
    else
        return glGetUniformLocation (m_shader_program, uniform);
}



inline void
IvGL::gl_uniform (GLint location, float value)
{
    if (m_shaders_using_extensions)
        glUniform1fARB (location, value);
    else
        glUniform1f (location, value);
}



inline void
IvGL::gl_uniform (GLint location, int value)
{
    if (m_shaders_using_extensions)
        glUniform1iARB (location, value);
    else
        glUniform1i (location, value);
}



void
IvGL::check_gl_extensions (void)
{
#ifndef FORCE_OPENGL_1
    m_use_shaders = glewIsSupported("GL_VERSION_2_0");

    if (!m_use_shaders && glewIsSupported("GL_ARB_shader_objects "
                                          "GL_ARB_vertex_shader "
                                          "GL_ARB_fragment_shader")) {
        m_use_shaders = true;
        m_shaders_using_extensions = true;
    }

    m_use_srgb = glewIsSupported("GL_VERSION_2_1") ||
                 glewIsSupported("GL_EXT_texture_sRGB");

    m_use_halffloat = glewIsSupported("GL_VERSION_3_0") ||
                      glewIsSupported("GL_ARB_half_float_pixel") ||
                      glewIsSupported("GL_NV_half_float_pixel");

    m_use_float = glewIsSupported("GL_VERSION_3_0") ||
                  glewIsSupported("GL_ARB_texture_float") ||
                  glewIsSupported("GL_ATI_texture_float");

    m_use_pbo = glewIsSupported("GL_VERSION_1_5") ||
                glewIsSupported("GL_ARB_pixel_buffer_object");
#else
    std::cerr << "Not checking GL extensions\n";
#endif

    m_max_texture_size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_max_texture_size);
    // FIXME: Need a smarter way to handle (video) memory.
    // Don't assume that systems capable of using 8k^2 textures have enough
    // resources to use more than one of those at the same time.
    m_max_texture_size = std::min(m_max_texture_size, 4096);

#ifdef DEBUG
    // Report back...
    std::cerr << "OpenGL Shading Language supported: " << m_use_shaders << "\n";
    if (m_shaders_using_extensions) {
        std::cerr << "\t(with extensions)\n";
    }
    std::cerr << "OpenGL sRGB color space textures supported: " << m_use_srgb << "\n";
    std::cerr << "OpenGL half-float pixels supported: " << m_use_halffloat << "\n";
    std::cerr << "OpenGL float texture storage supported: " << m_use_float << "\n";
    std::cerr << "OpenGL pixel buffer object supported: " << m_use_pbo << "\n";
    std::cerr << "OpenGL max texture dimension: " << m_max_texture_size << "\n";
#endif
}



void
IvGL::typespec_to_opengl (const ImageSpec &spec, GLenum &gltype, GLenum &glformat, 
                          GLenum &glinternalformat) const
{
    switch (spec.format.basetype) {
    case TypeDesc::FLOAT  : gltype = GL_FLOAT;          break;
    case TypeDesc::HALF   : if (m_use_halffloat) {
                                gltype = GL_HALF_FLOAT_ARB;
                            } else {
                                // If we reach here then something really wrong
                                // happened: When half-float is not supported,
                                // the image should be loaded as UINT8 (no GLSL
                                // support) or FLOAT (GLSL support).
                                // See IvImage::loadCurrentImage()
                                std::cerr << "Tried to load an unsupported half-float image.\n";
                                gltype = GL_INVALID_ENUM;
                            }
                            break;
    case TypeDesc::INT    : gltype = GL_INT;            break;
    case TypeDesc::UINT   : gltype = GL_UNSIGNED_INT;   break;
    case TypeDesc::INT16  : gltype = GL_SHORT;          break;
    case TypeDesc::UINT16 : gltype = GL_UNSIGNED_SHORT; break;
    case TypeDesc::INT8   : gltype = GL_BYTE;           break;
    case TypeDesc::UINT8  : gltype = GL_UNSIGNED_BYTE;  break;
    default:
        gltype = GL_UNSIGNED_BYTE;  // punt
        break;
    }

    glinternalformat = spec.nchannels;
    if (spec.nchannels == 1) {
        glformat = GL_LUMINANCE;
        if (m_use_srgb && spec.linearity == ImageSpec::sRGB) {
            if (spec.format.basetype == TypeDesc::UINT8) {
                glinternalformat = GL_SLUMINANCE8;
            } else {
                glinternalformat = GL_SLUMINANCE;
            }
        } else if (spec.format.basetype == TypeDesc::UINT8) {
            glinternalformat = GL_LUMINANCE8;
        } else if (spec.format.basetype == TypeDesc::UINT16) {
            glinternalformat = GL_LUMINANCE16;
        } else if (m_use_float && spec.format.basetype == TypeDesc::FLOAT) {
            glinternalformat = GL_LUMINANCE32F_ARB;
        } else if (m_use_float && spec.format.basetype == TypeDesc::HALF) {
            glinternalformat = GL_LUMINANCE16F_ARB;
        }
    } else if (spec.nchannels == 2) {
        glformat = GL_LUMINANCE_ALPHA;
        if (m_use_srgb && spec.linearity == ImageSpec::sRGB) {
            if (spec.format.basetype == TypeDesc::UINT8) {
                glinternalformat = GL_SLUMINANCE8_ALPHA8;
            } else {
                glinternalformat = GL_SLUMINANCE_ALPHA;
            }
        } else if (spec.format.basetype == TypeDesc::UINT8) {
            glinternalformat = GL_LUMINANCE8_ALPHA8;
        } else if (spec.format.basetype == TypeDesc::UINT16) {
            glinternalformat = GL_LUMINANCE16_ALPHA16;
        } else if (m_use_float && spec.format.basetype == TypeDesc::FLOAT) {
            glinternalformat = GL_LUMINANCE_ALPHA32F_ARB;
        } else if (m_use_float && spec.format.basetype == TypeDesc::HALF) {
            glinternalformat = GL_LUMINANCE_ALPHA16F_ARB;
        }
    } else if (spec.nchannels == 3) {
        glformat = GL_RGB;
        if (m_use_srgb && spec.linearity == ImageSpec::sRGB) {
            if (spec.format.basetype == TypeDesc::UINT8) {
                glinternalformat = GL_SRGB8;
            } else {
                glinternalformat = GL_SRGB;
            }
        } else if (spec.format.basetype == TypeDesc::UINT8) {
            glinternalformat = GL_RGB8;
        } else if (spec.format.basetype == TypeDesc::UINT16) {
            glinternalformat = GL_RGB16;
        } else if (m_use_float && spec.format.basetype == TypeDesc::FLOAT) {
            glinternalformat = GL_RGB32F_ARB;
        } else if (m_use_float && spec.format.basetype == TypeDesc::HALF) {
            glinternalformat = GL_RGB16F_ARB;
        }
    } else if (spec.nchannels == 4) {
        glformat = GL_RGBA;
        if (m_use_srgb && spec.linearity == ImageSpec::sRGB) {
            if (spec.format.basetype == TypeDesc::UINT8) {
                glinternalformat = GL_SRGB8_ALPHA8;
            } else {
                glinternalformat = GL_SRGB_ALPHA;
            }
        } else if (spec.format.basetype == TypeDesc::UINT8) {
            glinternalformat = GL_RGBA8;
        } else if (spec.format.basetype == TypeDesc::UINT16) {
            glinternalformat = GL_RGBA16;
        } else if (m_use_float && spec.format.basetype == TypeDesc::FLOAT) {
            glinternalformat = GL_RGBA32F_ARB;
        } else if (m_use_float && spec.format.basetype == TypeDesc::HALF) {
            glinternalformat = GL_RGBA16F_ARB;
        }
    } else {
        //FIXME: What to do here?
        std::cerr << "I don't know how to handle more than 4 channels\n";
        glformat = GL_INVALID_ENUM;
        glinternalformat = GL_INVALID_ENUM;
    }
}



void
IvGL::load_texture (int x, int y, int width, int height, float percent)
{
    const ImageSpec &spec = m_current_image->spec ();
    // Find if this has already been loaded.
    BOOST_FOREACH (TexBuffer &tb, m_texbufs) {
        if (tb.x == x && tb.y == y && tb.width == width && tb.height == height) {
            glBindTexture (GL_TEXTURE_2D, tb.tex_object);
            return;
        }
    }

    // Make it somewhat obvious to the user that some progress is happening
    // here.
    m_viewer.statusProgress->setValue ((int)(percent*100));
    // FIXME: Check whether this works ok (ie, no 'recursive repaint' messages)
    // on all platforms.
    m_viewer.statusProgress->repaint ();
    setCursor (Qt::WaitCursor);

    GLenum gltype, glformat, glinternalformat;
    typespec_to_opengl (spec, gltype, glformat, glinternalformat);

    TexBuffer &tb = m_texbufs[m_last_texbuf_used];
    tb.x = x;
    tb.y = y;
    tb.width = width;
    tb.height = height;
    if (m_use_shaders) {
        if (m_use_pbo) {
            // When using PBO the buffer is allocated by the OpenGL driver,
            // this should help speed up loading of the texture since the copy
            // from the PBO to the texture can be done asynchronously by the
            // driver. We use two PBOs so we don't have to wait for the first
            // transfer to end before starting the second.
            glBindBufferARB (GL_PIXEL_UNPACK_BUFFER_ARB, 
                             m_pbo_objects[m_last_pbo_used]);
            glBufferDataARB (GL_PIXEL_UNPACK_BUFFER_ARB, 
                             width * height * spec.pixel_bytes(),
                             NULL,
                             GL_STREAM_DRAW_ARB);
            GLERRPRINT ("After buffer data");
            void *buffer = glMapBufferARB (GL_PIXEL_UNPACK_BUFFER_ARB, GL_WRITE_ONLY_ARB);
            if (!buffer) {
                // FIXME: What to do here?
                GLERRPRINT ("Couldn't map Pixel memory");
                return;
            }
            m_current_image->copy_pixels (x, x + width, y, y + height,
                                          spec.format, buffer);
            glUnmapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB);
            m_last_pbo_used = (m_last_pbo_used + 1) % 2;
        } else {
            // Copy the imagebuf pixels we need, that's the only way we can do
            // it safely once ImageBuf has a cache underneath and the whole image
            // may not be resident at once.
            m_current_image->copy_pixels (x, x + width, y, y + height,
                                          spec.format, &m_tex_buffer[0]);
        }
    }

    void *data;
    if (m_use_shaders) {
        if (m_use_pbo) {
            data = 0;
        } else {
            data = &m_tex_buffer[0];
        }
    } else {
        data = m_current_image->pixeladdr(x, y);
    }
    glBindTexture (GL_TEXTURE_2D, tb.tex_object);
    GLERRPRINT ("After bind texture");
    glTexSubImage2D (GL_TEXTURE_2D, 0,
                     0, 0,
                     width, height,
                     glformat, gltype,
                     data);
    GLERRPRINT ("After loading sub image");
    m_last_texbuf_used = (m_last_texbuf_used + 1) % m_texbufs.size();
}
