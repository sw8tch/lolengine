//
// Lol Engine - Fractal tutorial
//
// Copyright: (c) 2011 Sam Hocevar <sam@hocevar.net>
//   This program is free software; you can redistribute it and/or
//   modify it under the terms of the Do What The Fuck You Want To
//   Public License, Version 2, as published by Sam Hocevar. See
//   http://sam.zoy.org/projects/COPYING.WTFPL for more details.
//

#if defined HAVE_CONFIG_H
#   include "config.h"
#endif

#include <cstring>

#include "core.h"
#include "lolgl.h"
#include "loldebug.h"

using namespace std;
using namespace lol;

#if USE_SDL && defined __APPLE__
#   include <SDL_main.h>
#endif

#if defined _WIN32
#   undef main /* FIXME: still needed? */
#   include <direct.h>
#endif

#ifdef __CELLOS_LV2__
static GLint const INTERNAL_FORMAT = GL_ARGB_SCE;
static GLenum const TEXTURE_FORMAT = GL_BGRA;
static GLenum const TEXTURE_TYPE = GL_UNSIGNED_INT_8_8_8_8_REV;
#elif defined __native_client__
static GLint const INTERNAL_FORMAT = GL_RGBA;
static GLenum const TEXTURE_FORMAT = GL_RGBA;
static GLenum const TEXTURE_TYPE = GL_UNSIGNED_BYTE;
#else
/* Seems efficient for little endian textures */
static GLint const INTERNAL_FORMAT = GL_RGBA;
static GLenum const TEXTURE_FORMAT = GL_BGRA;
static GLenum const TEXTURE_TYPE = GL_UNSIGNED_INT_8_8_8_8_REV;
#endif

class Fractal : public WorldEntity
{
public:
    Fractal(ivec2 const &size)
    {
        /* Ensure texture size is a multiple of 16 for better aligned
         * data access. Store the dimensions of a texel for our shader,
         * as well as the half-size of the screen. */
        m_size = size;
        m_size.x = (m_size.x + 15) & ~15;
        m_size.y = (m_size.y + 15) & ~15;
        m_texel_settings = vec4(vec2(1.0, 1.0) / (vec2)m_size,
                                vec2(0.5, 0.5) * (vec2)m_size);

        /* Window size decides the world aspect ratio. For instance, 640×480
         * will be mapped to (-0.66,-0.5) - (0.66,0.5). */
#if !defined __native_client__
        m_window_size = Video::GetSize();
#else
        /* FIXME: it's illegal to call this on the game thread! */
        m_window_size = ivec2(640, 480);
#endif
        if (m_window_size.y < m_window_size.x)
            m_window2world = 0.5 / m_window_size.y;
        else
            m_window2world = 0.5 / m_window_size.x;
        m_texel2world = (vec2)m_window_size / (vec2)m_size * m_window2world;

        m_oldmouse = ivec2(0, 0);

        m_pixels = new u8vec4[m_size.x * m_size.y];
        m_tmppixels = new u8vec4[m_size.x / 2 * m_size.y / 2];
        m_frame = -1;
        for (int i = 0; i < 4; i++)
        {
            m_deltashift[i] = 0.0;
            m_deltascale[i] = 1.0;
            m_dirty[i] = 2;
        }
#if defined __CELLOS_LV2__
        //m_center = f64cmplx(-.22815528839841, -1.11514249704382);
        //m_center = f64cmplx(0.001643721971153, 0.822467633298876);
        m_center = f64cmplx(-0.65823419062254, .50221777363480);
        m_zoom_speed = 0;//-0.0025;
#else
        m_center = -0.75;
        m_zoom_speed = 0.0;
#endif
        m_translate = 0;
        m_radius = 5.0;
        m_ready = false;

        m_palette = new u8vec4[(MAX_ITERATIONS + 1) * PALETTE_STEP];
        for (int i = 0; i < (MAX_ITERATIONS + 1) * PALETTE_STEP; i++)
        {
            double f = (double)i / PALETTE_STEP;

            double r = 0.5 * sin(f * 0.27 - 2.5) + 0.5;
            double g = 0.5 * sin(f * 0.13 + 1.1) + 0.5;
            double b = 0.5 * sin(f * 0.21 + 0.4) + 0.5;

            if (f < 7.0)
            {
                f = f < 1.0 ? 0.0 : (f - 1.0) / 6.0;
                r *= f;
                g *= f;
                b *= f;
            }

            uint8_t red = r * 255.99f;
            uint8_t green = g * 255.99f;
            uint8_t blue = b * 255.99f;
#if defined __native_client__
            m_palette[i] = u8vec4(red, green, blue, 255);
#else
            m_palette[i] = u8vec4(blue, green, red, 255);
#endif
        }

#if !defined __native_client__
        m_centertext = new Text(NULL, "gfx/font/ascii.png");
        m_centertext->SetPos(ivec3(5, m_window_size.y - 15, 1));
        Ticker::Ref(m_centertext);

        m_mousetext = new Text(NULL, "gfx/font/ascii.png");
        m_mousetext->SetPos(ivec3(5, m_window_size.y - 29, 1));
        Ticker::Ref(m_mousetext);

        m_zoomtext = new Text(NULL, "gfx/font/ascii.png");
        m_zoomtext->SetPos(ivec3(5, m_window_size.y - 43, 1));
        Ticker::Ref(m_zoomtext);
#endif

        position = ivec3(0, 0, 0);
        bbox[0] = position;
        bbox[1] = ivec3(m_window_size, 0);
        Input::TrackMouse(this);
    }

    ~Fractal()
    {
        Input::UntrackMouse(this);
#if !defined __native_client__
        Ticker::Unref(m_centertext);
        Ticker::Unref(m_mousetext);
        Ticker::Unref(m_zoomtext);
#endif
        delete m_pixels;
        delete m_tmppixels;
        delete m_palette;
    }

    inline f64cmplx TexelToWorldOffset(ivec2 texel)
    {
        double dx = (0.5 + texel.x - m_size.x / 2) * m_texel2world.x;
        double dy = (0.5 + m_size.y / 2 - texel.y) * m_texel2world.y;
        return m_radius * f64cmplx(dx, dy);
    }

    inline f64cmplx ScreenToWorldOffset(ivec2 pixel)
    {
        /* No 0.5 offset here, because we want to be able to position the
         * mouse at (0,0) exactly. */
        double dx = pixel.x - m_window_size.x / 2;
        double dy = m_window_size.y / 2 - pixel.y;
        return m_radius * m_window2world * f64cmplx(dx, dy);
    }

    virtual void TickGame(float deltams)
    {
        WorldEntity::TickGame(deltams);

        int prev_frame = m_frame;
        m_frame = (m_frame + 1) % 4;

        f64cmplx worldmouse = m_center + ScreenToWorldOffset(mousepos);

        ivec3 buttons = Input::GetMouseButtons();
#if !defined __CELLOS_LV2__
        if (buttons[1])
        {
            if (clicked[1])
                m_oldmouse = mousepos;
            m_translate = ScreenToWorldOffset(m_oldmouse)
                        - ScreenToWorldOffset(mousepos);
            m_oldmouse = mousepos;
        }
        else if (m_translate != 0.0)
        {
            m_translate *= pow(2.0, -deltams * 0.005);
            if (m_translate.norm() / m_radius < 1e-4)
                m_translate = 0.0;
        }

        if ((buttons[0] || buttons[2]) && mousepos.x != -1)
        {
            double zoom = buttons[0] ? -0.0005 : 0.0005;
            m_zoom_speed += deltams * zoom;
            if (m_zoom_speed / zoom > 5)
                m_zoom_speed = 5 * zoom;
        }
        else if (m_zoom_speed)
        {
            m_zoom_speed *= pow(2.0, -deltams * 0.005);
            if (abs(m_zoom_speed) < 1e-5)
                m_zoom_speed = 0.0;
        }
#endif

        if (m_zoom_speed || m_translate != 0.0)
        {
            f64cmplx oldcenter = m_center;
            double oldradius = m_radius;
            double zoom = pow(2.0, deltams * m_zoom_speed);
            if (m_radius * zoom > 8.0)
            {
                m_zoom_speed *= -1.0;
                zoom = 8.0 / m_radius;
            }
            else if (m_radius * zoom < 1e-14)
            {
                m_zoom_speed *= -1.0;
                zoom = 1e-14 / m_radius;
            }
            m_radius *= zoom;
#if !defined __CELLOS_LV2__
            m_center += m_translate;
            m_center = (m_center - worldmouse) * zoom + worldmouse;
            worldmouse = m_center + ScreenToWorldOffset(mousepos);
#endif

            /* Store the transformation properties to go from m_frame - 1
             * to m_frame. */
            m_deltashift[prev_frame] = (m_center - oldcenter) / oldradius;
            m_deltashift[prev_frame].x /= m_size.x * m_texel2world.x;
            m_deltashift[prev_frame].y /= m_size.y * m_texel2world.y;
            m_deltascale[prev_frame] = m_radius / oldradius;
            m_dirty[0] = m_dirty[1] = m_dirty[2] = m_dirty[3] = 2;
        }
        else
        {
            /* If settings didn't change, set transformation from previous
             * frame to identity. */
            m_deltashift[prev_frame] = 0.0;
            m_deltascale[prev_frame] = 1.0;
        }

        /* Transformation from current frame to current frame is always
         * identity. */
        m_zoom_settings[m_frame][0] = 0.0f;
        m_zoom_settings[m_frame][1] = 0.0f;
        m_zoom_settings[m_frame][2] = 1.0f;

        /* Compute transformation from other frames to current frame */
        for (int i = 0; i < 3; i++)
        {
            int prev_index = (m_frame + 4 - i) % 4;
            int cur_index = (m_frame + 3 - i) % 4;

            m_zoom_settings[cur_index][0] = m_zoom_settings[prev_index][0] * m_deltascale[cur_index] + m_deltashift[cur_index].x;
            m_zoom_settings[cur_index][1] = m_zoom_settings[prev_index][1] * m_deltascale[cur_index] + m_deltashift[cur_index].y;
            m_zoom_settings[cur_index][2] = m_zoom_settings[prev_index][2] * m_deltascale[cur_index];
        }

        /* Precompute texture offset change instead of doing it in GLSL */
        for (int i = 0; i < 4; i++)
        {
            m_zoom_settings[i][0] += 0.5 * (1.0 - m_zoom_settings[i][2]);
            m_zoom_settings[i][1] -= 0.5 * (1.0 - m_zoom_settings[i][2]);
        }

#if !defined __native_client__
        char buf[128];
        sprintf(buf, "center: %+16.14f%+16.14fi", m_center.x, m_center.y);
        m_centertext->SetText(buf);
        sprintf(buf, " mouse: %+16.14f%+16.14fi", worldmouse.x, worldmouse.y);
        m_mousetext->SetText(buf);
        sprintf(buf, "  zoom: %g", 1.0 / m_radius);
        m_zoomtext->SetText(buf);
#endif

        u8vec4 *m_pixelstart = m_pixels + m_size.x * m_size.y / 4 * m_frame;

        if (m_dirty[m_frame])
        {
            double const maxsqlen = 1024;
            double const k1 = 1.0 / (1 << 10) / log2(maxsqlen);

            m_dirty[m_frame]--;

            for (int j = ((m_frame + 1) % 4) / 2; j < m_size.y; j += 2)
            for (int i = m_frame % 2; i < m_size.x; i += 2)
            {

                f64cmplx z0 = m_center + TexelToWorldOffset(ivec2(i, j));
                f64cmplx r0 = z0;
                //f64cmplx r0(0.28693186889504513, 0.014286693904085048);
                //f64cmplx r0(0.001643721971153, 0.822467633298876);
                //f64cmplx r0(-1.207205434596, 0.315432814901);
                //f64cmplx r0(-0.79192956889854, -0.14632423080102);
                //f64cmplx r0(0.3245046418497685, 0.04855101129280834);
                f64cmplx z;
                int iter = MAX_ITERATIONS;
                for (z = z0; iter && z.sqlen() < maxsqlen; z = z * z + r0)
                    --iter;

                if (iter)
                {
                    double f = iter;
                    double n = z.sqlen();
                    if (n > maxsqlen * maxsqlen)
                        n = maxsqlen * maxsqlen;

                    /* Approximate log(sqrt(n))/log(sqrt(maxsqlen)) */
                    union { double n; uint64_t x; } u = { n };
                    double k = (u.x >> 42) - (((1 << 10) - 1) << 10);
                    k *= k1;

                    /* Approximate log2(k) in [1,2]. */
                    f += (- 0.344847817623168308695977510213252644185 * k
                          + 2.024664188044341212602376988171727038739) * k
                          - 1.674876738008591047163498125918330313237;

                    *m_pixelstart++ = m_palette[(int)(f * PALETTE_STEP)];
                }
                else
                {
                    *m_pixelstart++ = u8vec4(0, 0, 0, 255);
                }
            }
        }
    }

    virtual void TickDraw(float deltams)
    {
        WorldEntity::TickDraw(deltams);

        static float const vertices[] =
        {
             1.0f,  1.0f,
            -1.0f,  1.0f,
            -1.0f, -1.0f,
            -1.0f, -1.0f,
             1.0f, -1.0f,
             1.0f,  1.0f,
        };

        static float const texcoords[] =
        {
             1.0f,  1.0f,
             0.0f,  1.0f,
             0.0f,  0.0f,
             0.0f,  0.0f,
             1.0f,  0.0f,
             1.0f,  1.0f,
        };

        if (!m_ready)
        {
            /* Create a texture of half the width and twice the height
             * so that we can upload four different subimages each frame. */
            glGenTextures(1, &m_texid);
            glBindTexture(GL_TEXTURE_2D, m_texid);
            glTexImage2D(GL_TEXTURE_2D, 0, INTERNAL_FORMAT,
                         m_size.x / 2, m_size.y * 2, 0,
                         TEXTURE_FORMAT, TEXTURE_TYPE, m_pixels);
#if defined __CELLOS_LV2__
            /* We need this hint because by default the storage type is
             * GL_TEXTURE_SWIZZLED_GPU_SCE. */
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_ALLOCATION_HINT_SCE,
                            GL_TEXTURE_TILED_GPU_SCE);
#endif
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

            m_shader = Shader::Create(
#if !defined __CELLOS_LV2__
#if !defined HAVE_GLES_2X
                "#version 120\n"
#else
                "precision highp float;"
#endif
                ""
                "uniform mat4 u_ZoomSettings;"
                "uniform vec4 u_TexelSize;"
                ""
                "attribute vec2 a_TexCoord;"
                "attribute vec2 a_Vertex;"
                ""
                "varying vec2 A0, A1, A2, A3;"
                "varying vec2 B0, B1, B2, B3;"
                ""
                "void main(void)"
                "{"
                "    gl_Position = vec4(a_Vertex, 0.0, 1.0);"
                     /* Center point in [-.5,.5], apply zoom and translation
                      * transformation, and go back to texture coordinates
                      * in [0,1]. That's the ideal point we would like to
                      * compute the value for. Then add or remove half the
                      * size of a texel: the distance from this new point to
                      * the final point will be our error. */
                "    A0 = a_TexCoord * u_ZoomSettings[0][2]"
                "       + vec2(u_ZoomSettings[0][0], -u_ZoomSettings[0][1])"
                "       + 0.5 * u_TexelSize.xy;"
                "    A1 = a_TexCoord * u_ZoomSettings[1][2]"
                "       + vec2(u_ZoomSettings[1][0], -u_ZoomSettings[1][1])"
                "       + -0.5 * u_TexelSize.xy;"
                "    A2 = a_TexCoord * u_ZoomSettings[2][2]"
                "       + vec2(u_ZoomSettings[2][0], -u_ZoomSettings[2][1])"
                "       + vec2(0.5, -0.5) * u_TexelSize.xy;"
                "    A3 = a_TexCoord * u_ZoomSettings[3][2]"
                "       + vec2(u_ZoomSettings[3][0], -u_ZoomSettings[3][1])"
                "       + vec2(-0.5, 0.5) * u_TexelSize.xy;"
                     /* Precompute the multiple of one texel where our ideal
                      * point lies. The fragment shader will call floor() on
                      * this value. We add or remove a slight offset to avoid
                      * rounding issues at the image's edges. */
                "    B0 = (A0 * u_TexelSize.zw + vec2(-0.015625, -0.015625));"
                "    B1 = (A1 * u_TexelSize.zw + vec2( 0.015625,  0.015625));"
                "    B2 = (A2 * u_TexelSize.zw + vec2(-0.015625,  0.015625));"
                "    B3 = (A3 * u_TexelSize.zw + vec2( 0.015625, -0.015625));"
                "}",

#if !defined HAVE_GLES_2X
                "#version 120\n"
#else
                "precision highp float;"
#endif
                ""
                "uniform vec4 u_TexelSize;"
                "uniform sampler2D in_Texture;"
                ""
                "varying vec2 A0, A1, A2, A3;"
                "varying vec2 B0, B1, B2, B3;"
                ""
                "vec2 v05 = vec2(0.5, 0.5);"
                ""
                "float mylen(vec2 p)"
                "{"
                "    p *= u_TexelSize.zw;" /* Correct for aspect ratio */
                //"    return 0.001 + abs(p.x) + abs(p.y);"
                //"    return 0.1 + length(p);"
                "    vec2 q = p * p;"
                "    return 0.01 + q.x + q.y;"
                "}"
                ""
                /* Get the coordinate of the nearest point in slice 0 in xy,
                 * and the squared distance to that point in z.
                 * return value has the 0.25 Y scaling. */
                "vec3 nearest0()"
                "{"
                "    vec2 r = u_TexelSize.xy * (1.0 + 2.0 * floor(B0));"
                "    vec2 t = step(abs(r - v05), v05);"
                "    float l = t.x * t.y / mylen(r - A0);"
                "    return vec3(r * vec2(1.0, 0.25), l);"
                "}"
                ""
                "vec3 nearest1()"
                "{"
                "    vec2 r = u_TexelSize.xy * (1.0 + 2.0 * floor(B1));"
                "    vec2 t = step(abs(r - v05), v05);"
                "    float l = t.x * t.y / mylen(r - A1);"
                "    return vec3(r * vec2(1.0, 0.25) + vec2(0.0, 0.25), l);"
                "}"
                ""
                "vec3 nearest2()"
                "{"
                "    vec2 r = u_TexelSize.xy * (1.0 + 2.0 * floor(B2));"
                "    vec2 t = step(abs(r - v05), v05);"
                "    float l = t.x * t.y / mylen(r - A2);"
                "    return vec3(r * vec2(1.0, 0.25) + vec2(0.0, 0.50), l);"
                "}"
                ""
                "vec3 nearest3()"
                "{"
                "    vec2 r = u_TexelSize.xy * (1.0 + 2.0 * floor(B3));"
                "    vec2 t = step(abs(r - v05), v05);"
                "    float l = t.x * t.y / mylen(r - A3);"
                "    return vec3(r * vec2(1.0, 0.25) + vec2(0.0, 0.75), l);"
                "}"
                ""
                "void main(void)"
                "{"
                     /* Get a pixel coordinate from each slice */
                "    vec3 k0 = nearest0();"
                "    vec3 k1 = nearest1();"
                "    vec3 k2 = nearest2();"
                "    vec3 k3 = nearest3();"
                     /* Get the nearest pixel */
                "    float t01 = step(k0.z, k1.z);"
                "    k0 = mix(k0, k1, t01);"
                "    float t23 = step(k2.z, k3.z);"
                "    k2 = mix(k2, k3, t23);"
                "    float t02 = step(k0.z, k2.z);"
                "    k0 = mix(k0, k2, t02);"
                "    gl_FragColor = texture2D(in_Texture, k0.xy);"
                     /* Alternate version: some kind of linear interpolation */
//                "    vec4 p0 = texture2D(in_Texture, k0.xy);"
//                "    vec4 p1 = texture2D(in_Texture, k1.xy);"
//                "    vec4 p2 = texture2D(in_Texture, k2.xy);"
//                "    vec4 p3 = texture2D(in_Texture, k3.xy);"
//                "    gl_FragColor = 1.0 / (k0.z + k1.z + k2.z + k3.z)"
//                "          * (k0.z * p0 + k1.z * p1 + k2.z * p2 + k3.z * p3);"
                "}"
#else
                "void main(float4 in_Position : POSITION,"
                "          float2 a_TexCoord : TEXCOORD0,"
                "          out float4 out_Position : POSITION,"
                "          out float2 out_TexCoord : TEXCOORD0)"
                "{"
                "    out_TexCoord = a_TexCoord;"
                "    out_Position = in_Position;"
                "}",

                "float3 nearest0(float2 p, float4 u_TexelSize)"
                "{"
                "    float2 q = p + 0.5 * u_TexelSize.xy;"
                "    q -= fmod(q, 2.0 * u_TexelSize.xy);"
                "    q += 0.5 * u_TexelSize.xy;"
                "    return float3(q * float2(1.0, 0.25),"
                "                  length(q - p));"
                "}"
                ""
                "float3 nearest1(float2 p, float4 u_TexelSize)"
                "{"
                "    float2 q = p - 0.5 * u_TexelSize.xy;"
                "    q -= fmod(q, 2.0 * u_TexelSize.xy);"
                "    q += 1.5 * u_TexelSize.xy;"
                "    return float3(q * float2(1.0, 0.25) + float2(0.0, 0.25),"
                "                  length(q - p));"
                "}"
                ""
                "float3 nearest2(float2 p, float4 u_TexelSize)"
                "{"
                "    float2 q = p + float2(0.5, -0.5) * u_TexelSize.xy;"
                "    q -= fmod(q, 2.0 * u_TexelSize.xy);"
                "    q += float2(0.5, 1.5) * u_TexelSize.xy;"
                "    return float3(q * float2(1.0, 0.25) + float2(0.0, 0.50),"
                "                  length(q - p));"
                "}"
                ""
                "float3 nearest3(float2 p, float4 u_TexelSize)"
                "{"
                "    float2 q = p + float2(-0.5, 0.5) * u_TexelSize.xy;"
                "    q -= fmod(q, 2.0 * u_TexelSize.xy);"
                "    q += float2(1.5, 0.5) * u_TexelSize.xy;"
                "    return float3(q * float2(1.0, 0.25) + float2(0.0, 0.75),"
                "                  length(q - p));"
                "}"
                ""
                "void main(float2 a_TexCoord : TEXCOORD0,"
                "          uniform float4 u_TexelSize,"
                "          uniform sampler2D in_Texture,"
                "          out float4 out_FragColor : COLOR)"
                "{"
                "    float2 coord = a_TexCoord.xy;"
                "    coord -= 0.1 * u_TexelSize.xy;"
                "    float4 p0 = tex2D(in_Texture, nearest0(coord, u_TexelSize).xy);"
                "    float4 p1 = tex2D(in_Texture, nearest1(coord, u_TexelSize).xy);"
                "    float4 p2 = tex2D(in_Texture, nearest2(coord, u_TexelSize).xy);"
                "    float4 p3 = tex2D(in_Texture, nearest3(coord, u_TexelSize).xy);"
                "    out_FragColor = 0.25 * (p0 + p1 + p2 + p3);"
                "}"
#endif
            );
            m_vertexattrib = m_shader->GetAttribLocation("a_Vertex");
            m_texattrib = m_shader->GetAttribLocation("a_TexCoord");
            m_texeluni = m_shader->GetUniformLocation("u_TexelSize");
            m_zoomuni = m_shader->GetUniformLocation("u_ZoomSettings");
            m_ready = true;

#if !defined __CELLOS_LV2__ && !defined __ANDROID__
            /* Method 1: store vertex buffer on the GPU memory */
            glGenBuffers(1, &m_vbo);
            glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices,
                         GL_STATIC_DRAW);
            glGenBuffers(1, &m_tbo);
            glBindBuffer(GL_ARRAY_BUFFER, m_tbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(texcoords), texcoords,
                         GL_STATIC_DRAW);
#elif !defined __CELLOS_LV2__ && !defined __ANDROID__
            /* Method 2: upload vertex information at each frame */
#else
#endif

            /* FIXME: this object never cleans up */
        }

#if !defined HAVE_GLES_2X
        glEnable(GL_TEXTURE_2D);
#endif
        glBindTexture(GL_TEXTURE_2D, m_texid);

        if (m_dirty[m_frame])
        {
            m_dirty[m_frame]--;

#ifdef __CELLOS_LV2__
            /* glTexSubImage2D is extremely slow on the PS3, to the point
             * that uploading the whole texture is 40 times faster. */
            glTexImage2D(GL_TEXTURE_2D, 0, INTERNAL_FORMAT,
                         m_size.x / 2, m_size.y * 2, 0,
                         TEXTURE_FORMAT, TEXTURE_TYPE, m_pixels);
#else
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, m_frame * m_size.y / 2,
                            m_size.x / 2, m_size.y / 2,
                            TEXTURE_FORMAT, TEXTURE_TYPE,
                            m_pixels + m_size.x * m_size.y / 4 * m_frame);
#endif
        }

        m_shader->Bind();
        m_shader->SetUniform(m_texeluni, m_texel_settings);
        m_shader->SetUniform(m_zoomuni, m_zoom_settings);
#if !defined __CELLOS_LV2__ && !defined __ANDROID__
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glEnableVertexAttribArray(m_vertexattrib);
        glVertexAttribPointer(m_vertexattrib, 2, GL_FLOAT, GL_FALSE, 0, 0);

        glBindBuffer(GL_ARRAY_BUFFER, m_tbo);
        glEnableVertexAttribArray(m_texattrib);
        glVertexAttribPointer(m_texattrib, 2, GL_FLOAT, GL_FALSE, 0, 0);
#elif !defined __CELLOS_LV2__ && !defined __ANDROID__
        /* Never used for now */
        //glEnableVertexAttribArray(m_vertexattrib);
        //glVertexAttribPointer(m_vertexattrib, 2, GL_FLOAT, GL_FALSE, 0, vertices);
#else
        glEnableClientState(GL_VERTEX_ARRAY);
        glVertexPointer(2, GL_FLOAT, 0, vertices);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, 0, texcoords);
#endif

        glDrawArrays(GL_TRIANGLES, 0, 6);

#if !defined __CELLOS_LV2__ && !defined __ANDROID__
        glDisableVertexAttribArray(m_vertexattrib);
        glDisableVertexAttribArray(m_texattrib);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
#elif !defined __CELLOS_LV2__ && !defined __ANDROID__
        /* Never used for now */
        //glDisableVertexAttribArray(m_vertexattrib);
        //glDisableVertexAttribArray(m_texattrib);
#else
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
#endif
    }

private:
    static int const MAX_ITERATIONS = 170;
    static int const PALETTE_STEP = 32;

    ivec2 m_size, m_window_size, m_oldmouse;
    double m_window2world;
    f64vec2 m_texel2world;
    u8vec4 *m_pixels, *m_tmppixels, *m_palette;
    Shader *m_shader;
    GLuint m_texid;
#if !defined __CELLOS_LV2__ && !defined __ANDROID__
    GLuint m_vbo, m_tbo;
    GLuint m_tco;
#endif
    int m_vertexattrib, m_texattrib, m_texeluni, m_zoomuni;
    int m_frame, m_dirty[4];
    bool m_ready;

    f64cmplx m_center, m_translate;
    double m_zoom_speed, m_radius;
    vec4 m_texel_settings;
    mat4 m_zoom_settings;
    f64cmplx m_deltashift[4];
    double m_deltascale[4];

    /* Debug information */
#if !defined __native_client__
    Text *m_centertext, *m_mousetext, *m_zoomtext;
#endif
};

int main(int argc, char **argv)
{
#if defined _WIN32
    _chdir("../..");
#endif

    Application app("Tutorial 3: Fractal", ivec2(640, 480), 60.0f);

    new DebugFps(5, 5);
    new Fractal(ivec2(640, 480));
    //new DebugRecord("fractalol.ogm", 60.0f);

    app.Run();

    return EXIT_SUCCESS;
}

