// streamvideo_entry.c
// The Playdate runtime calls a single exported `eventHandler`. This shim forwards
// it to the streaming engine so the `streamvideo.*` Lua functions get registered.
// If your project already defines `eventHandler` (e.g. you have other native code),
// don't compile this file — instead call eventHandler_streamvideo(pd, event, arg)
// from your own handler, the way the engine expects.
#include "pd_api.h"

extern int  eventHandler_streamvideo(PlaydateAPI* pd, PDSystemEvent event, uint32_t arg);
extern void streamvideo_setPD(PlaydateAPI* pd);

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI* pd, PDSystemEvent event, uint32_t arg)
{
	if (event == kEventInit) streamvideo_setPD(pd);
	return eventHandler_streamvideo(pd, event, arg);
}
