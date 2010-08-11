//
// Deus Hax (working title)
// Copyright (c) 2010 Sam Hocevar <sam@hocevar.net>
//

//
// The SdlVideo class
// ------------------
//

#if !defined __DH_SDLVIDEO_H__
#define __DH_SDLVIDEO_H__

#include "video.h"

class SdlVideoData;

class SdlVideo : public Video
{
public:
    SdlVideo(char const *title, int width, int height);
    virtual ~SdlVideo();

    virtual int GetWidth() const;
    virtual int GetHeight() const;
    virtual void PreRender();
    virtual void PostRender(float milliseconds);
    virtual void FullScreen();

private:
    SdlVideoData *data;
};

#endif // __DH_SDLVIDEO_H__

