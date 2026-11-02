// Tessera multi-display: which window and stream the input handler's mouse and keyboard work with
// (docs/phase-3-design.md, component 6). Members of SdlInputHandler, kept out of the upstream input files.
#include "streaming/input/input.h"

#include "multidisplay.h"
#include "streaming/session.h"

void SdlInputHandler::getTesseraMouseTarget(Uint32 windowId, SDL_Window*& window, int& stream,
                                            int& streamWidth, int& streamHeight)
{
    window = m_Window;
    stream = 0;
    streamWidth = m_StreamWidth;
    streamHeight = m_StreamHeight;

    Session* session = Session::get();
    if (session != nullptr && session->m_Tessera != nullptr) {
        session->m_Tessera->getMouseTarget(windowId, window, stream, streamWidth, streamHeight);
    }
}

SDL_Window* SdlInputHandler::getTesseraWindow(SDL_Window* candidate)
{
    Session* session = Session::get();
    if (candidate != nullptr && session != nullptr && session->m_Tessera != nullptr &&
            session->m_Tessera->ownsWindow(SDL_GetWindowID(candidate))) {
        return candidate;
    }
    return m_Window;
}
