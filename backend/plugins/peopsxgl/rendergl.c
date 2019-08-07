#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/gl.h>
#include <GL/glx.h>

typedef int bool;
#define true 1
#define false 0

// print a line on screen using printf syntax.
// Usage: Uses same syntax and semantics as printf.
// Output: <filename>:<line number>: <message>
#ifndef MSG
#define _printf_ printf
#define MSG(msg,...) do {                       \
        _printf_(__FILE__":%d: " msg "\n",      \
                 __LINE__, ##__VA_ARGS__);      \
    } while (0)
#endif

#define GLX_CONTEXT_MAJOR_VERSION_ARB       0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB       0x2092

// Context Attributes
#define GL_VER_MAJOR 4
#define GL_VER_MINOR 4

static Display *display = NULL;
static GLXContext ctx = 0;
static Window win;
static Colormap cmap;

typedef GLXContext(*glXCreateContextAttribsARBProc)(Display *, GLXFBConfig,
                   GLXContext, Bool, const int *);

// Helper to check for extension string presence.  Adapted from:
//   http://www.opengl.org/resources/features/OGLextensions/
static bool isExtensionSupported(const char *extList, const char *extension);

static int ctxErrorHandler(Display *dpy, XErrorEvent *ev);

void createGLContext();
void deleteGLContext();

// Helper to check for extension string presence.  Adapted from:
//   http://www.opengl.org/resources/features/OGLextensions/
static bool isExtensionSupported(const char *extList, const char *extension)
{
    const char *start;
    const char *where, *terminator;

    /* Extension names should not have spaces. */
    where = strchr(extension, ' ');

    if (where || *extension == '\0') {
        return false;
    }

    /* It takes a bit of care to be fool-proof about parsing the
       OpenGL extensions string. Don't be fooled by sub-strings,
       etc. */
    for (start = extList;;) {
        where = strstr(start, extension);

        if (!where) {
            break;
        }

        terminator = where + strlen(extension);

        if (where == start || *(where - 1) == ' ')
            if (*terminator == ' ' || *terminator == '\0') {
                return true;
            }

        start = terminator;
    }

    return false;
}

static bool ctxErrorOccurred = false;
static int ctxErrorHandler(Display *dpy, XErrorEvent *ev)
{
    ctxErrorOccurred = true;
    return 0;
}

void createGLContext()
{
    printf("Creating OpenGL Context...");
    display = XOpenDisplay(NULL);

    if (!display) {
        MSG("Fatal: Failed to open X display");
        exit(1);
    }

    // Get a matching FB config
    static int visual_attribs[] = {
        GLX_X_RENDERABLE    , True,
        GLX_DRAWABLE_TYPE   , GLX_WINDOW_BIT,
        GLX_RENDER_TYPE     , GLX_RGBA_BIT,
        GLX_X_VISUAL_TYPE   , GLX_TRUE_COLOR,
        GLX_RED_SIZE        , 8,
        GLX_GREEN_SIZE      , 8,
        GLX_BLUE_SIZE       , 8,
        GLX_ALPHA_SIZE      , 8,
        GLX_DEPTH_SIZE      , 24,
        GLX_STENCIL_SIZE    , 8,
        GLX_DOUBLEBUFFER    , True,
        //GLX_SAMPLE_BUFFERS  , 1,
        //GLX_SAMPLES         , 4,
        None
    };

    int glx_major, glx_minor;

    // FBConfigs were added in GLX version 1.3.
    if (!glXQueryVersion(display, &glx_major, &glx_minor) ||
            ((glx_major == 1) && (glx_minor < 3)) || (glx_major < 1)) {
        MSG("Fatal: Invalid GLX version");
        exit(1);
    }

    MSG("Getting matching framebuffer configs");
    int fbcount;
    GLXFBConfig *fbc = glXChooseFBConfig(display, DefaultScreen(display),
                                         visual_attribs, &fbcount);

    if (!fbc) {
        MSG("Fatal: Failed to retrieve a framebuffer config");
        exit(1);
    }

    MSG("Found %d matching FB configs.", fbcount);

    // Pick the FB config/visual with the most samples per pixel
    MSG("Getting XVisualInfos");
    int best_fbc = -1, worst_fbc = -1, best_num_samp = -1, worst_num_samp = 999;

    int i;

    for (i = 0; i < fbcount; ++i) {
        XVisualInfo *vi = glXGetVisualFromFBConfig(display, fbc[i]);

        if (vi) {
            int samp_buf, samples;
            glXGetFBConfigAttrib(display, fbc[i], GLX_SAMPLE_BUFFERS, &samp_buf);
            glXGetFBConfigAttrib(display, fbc[i], GLX_SAMPLES       , &samples);

            //Commented to reduce output
            //MSG("Matching fbconfig %d, visual ID 0x%2x:"
            //                "SAMPLE_BUFFERS = %d, SAMPLES = %d",
            //                 i, vi -> visualid, samp_buf, samples);

            if (best_fbc < 0 || samp_buf && samples > best_num_samp) {
                best_fbc = i, best_num_samp = samples;
            }

            if (worst_fbc < 0 || !samp_buf || samples < worst_num_samp) {
                worst_fbc = i, worst_num_samp = samples;
            }
        }

        XFree(vi);
    }

    GLXFBConfig bestFbc = fbc[best_fbc];

    // Be sure to free the FBConfig list allocated by glXChooseFBConfig()
    XFree(fbc);

    // Get a visual
    XVisualInfo *vi = glXGetVisualFromFBConfig(display, bestFbc);
    MSG("Chosen visual ID = 0x%x", vi->visualid);

    MSG("Creating colormap");
    XSetWindowAttributes swa;
    swa.colormap = cmap = XCreateColormap(display,
                                          RootWindow(display, vi->screen),
                                          vi->visual, AllocNone);
    swa.background_pixmap = None ;
    swa.border_pixel      = 0;
    swa.event_mask        = StructureNotifyMask;

    MSG("Creating window");
    win = XCreateWindow(display, RootWindow(display, vi->screen),
                        0, 0, 640, 480, 0, vi->depth, InputOutput,
                        vi->visual,
                        CWBorderPixel | CWColormap | CWEventMask, &swa);

    if (!win) {
        MSG("Fatal: Failed to create window.");
        exit(1);
    }

    // Done with the visual info data
    XFree(vi);

    XStoreName(display, win, "Window");

    MSG("Mapping window");
    XMapWindow(display, win);

    // Get the default screen's GLX extension list
    const char *glxExts = glXQueryExtensionsString(display,
                          DefaultScreen(display));

    // NOTE: It is not necessary to create or make current to a context before
    // calling glXGetProcAddressARB
    glXCreateContextAttribsARBProc glXCreateContextAttribsARB = 0;
    glXCreateContextAttribsARB = (glXCreateContextAttribsARBProc)
                                 glXGetProcAddressARB((const GLubyte *) "glXCreateContextAttribsARB");

    // Install an X error handler so the application won't exit if GL <VERSION>
    // context allocation fails.
    //
    // Note this error handler is global.  All display connections in all threads
    // of a process use the same error handler, so be sure to guard against other
    // threads issuing X commands while this code is running.
    ctxErrorOccurred = false;
    int (*oldHandler)(Display *, XErrorEvent *) = XSetErrorHandler(&ctxErrorHandler);

    // Check for the GLX_ARB_create_context extension string and the function.
    // If either is not present, use GLX 1.3 context creation method.
    if (!isExtensionSupported(glxExts, "GLX_ARB_create_context") ||
            !glXCreateContextAttribsARB) {
        MSG("glXCreateContextAttribsARB() not found"
                        "... using old-style GLX context");
        ctx = glXCreateNewContext(display, bestFbc, GLX_RGBA_TYPE, 0, True);
    }

    // If it does, try to get a GL <VERSION> context!
    // GL_<>_VERSION defined at top of file
    else {
        int context_attribs[] = {
            GLX_CONTEXT_MAJOR_VERSION_ARB, GL_VER_MAJOR,
            GLX_CONTEXT_MINOR_VERSION_ARB, GL_VER_MINOR,
            GLX_CONTEXT_FLAGS_ARB        , GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
            None
        };

        MSG("Creating context");
        ctx = glXCreateContextAttribsARB(display, bestFbc, 0,
                                         True, context_attribs);

        // Sync to ensure any errors generated are processed.
        XSync(display, False);

        if (!ctxErrorOccurred && ctx) {
            MSG("Created GL %d.%d context", GL_VER_MAJOR, GL_VER_MINOR);
        } else {
            MSG("Fatal: Failed to create GL %d.%d context",
                 GL_VER_MAJOR, GL_VER_MINOR);
            // Remove this exit to fall back to 2.1 context.
            //exit(1);

            // Couldn't create GL <VERSION> context.  Fall back to old-style 2.x context.
            // When a context version below <VERSION> is requested, implementations will
            // return the newest context version compatible with OpenGL versions less
            // than version <VERSION>.
            // GLX_CONTEXT_MAJOR_VERSION_ARB = 1
            context_attribs[1] = 1;
            // GLX_CONTEXT_MINOR_VERSION_ARB = 0
            context_attribs[3] = 0;

            ctxErrorOccurred = false;

            ctx = glXCreateContextAttribsARB(display, bestFbc, 0,
                                             True, context_attribs);
        }
    }

    // Sync to ensure any errors generated are processed.
    XSync(display, False);

    // Restore the original error handler
    XSetErrorHandler(oldHandler);

    if (ctxErrorOccurred || !ctx) {
        MSG("Fatal: Failed to create an OpenGL context");
        exit(1);
    }

    // Verifying that context is a direct context
    if (! glXIsDirect(display, ctx)) {
        MSG("Indirect GLX rendering context obtained");
    } else {
        MSG("Direct GLX rendering context obtained");
    }

    MSG("Making context current");
    glXMakeCurrent(display, win, ctx);

    MSG("Getting OpenGL Version information...");
    MSG("GL Version  = %s", glGetString(GL_VERSION));
    MSG("GL Vendor   = %s", glGetString(GL_VENDOR));
    MSG("GL Renderer = %s", glGetString(GL_RENDERER));
    MSG("GL Shader   = %s", glGetString(GL_SHADING_LANGUAGE_VERSION));

}

void deleteGLContext()
{
    glXMakeCurrent(display, 0, 0);
    glXDestroyContext(display, ctx);

    XDestroyWindow(display, win);
    XFreeColormap(display, cmap);
    XCloseDisplay(display);
}
