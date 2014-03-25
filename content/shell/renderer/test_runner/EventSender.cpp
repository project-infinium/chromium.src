// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file contains the definition for EventSender.
//
// Some notes about drag and drop handling:
// Windows drag and drop goes through a system call to doDragDrop. At that
// point, program control is given to Windows which then periodically makes
// callbacks into the webview. This won't work for layout tests, so instead,
// we queue up all the mouse move and mouse up events. When the test tries to
// start a drag (by calling EvenSendingController::doDragDrop), we take the
// events in the queue and replay them.
// The behavior of queuing events and replaying them can be disabled by a
// layout test by setting eventSender.dragMode to false.

#include "content/shell/renderer/test_runner/EventSender.h"

#include <deque>

#include "content/shell/renderer/test_runner/KeyCodeMapping.h"
#include "content/shell/renderer/test_runner/MockSpellCheck.h"
#include "content/shell/renderer/test_runner/TestCommon.h"
#include "content/shell/renderer/test_runner/TestInterfaces.h"
#include "content/shell/renderer/test_runner/WebTestDelegate.h"
#include "content/shell/renderer/test_runner/WebTestProxy.h"
#include "third_party/WebKit/public/platform/WebDragData.h"
#include "third_party/WebKit/public/platform/WebString.h"
#include "third_party/WebKit/public/platform/WebVector.h"
#include "third_party/WebKit/public/web/WebContextMenuData.h"
#include "third_party/WebKit/public/web/WebTouchPoint.h"
#include "third_party/WebKit/public/web/WebView.h"

#ifdef WIN32
#include "third_party/WebKit/public/web/win/WebInputEventFactory.h"
#elif __APPLE__
#include "third_party/WebKit/public/web/mac/WebInputEventFactory.h"
#elif defined(ANDROID)
#include "third_party/WebKit/public/web/android/WebInputEventFactory.h"
#elif defined(TOOLKIT_GTK)
#include "third_party/WebKit/public/web/gtk/WebInputEventFactory.h"
#endif

// FIXME: layout before each event?

using namespace std;
using namespace blink;

namespace WebTestRunner {

WebPoint EventSender::lastMousePos;
WebMouseEvent::Button EventSender::pressedButton = WebMouseEvent::ButtonNone;
WebMouseEvent::Button EventSender::lastButtonType = WebMouseEvent::ButtonNone;

namespace {

struct SavedEvent {
    enum SavedEventType {
        Unspecified,
        MouseUp,
        MouseMove,
        LeapForward
    };

    SavedEventType type;
    WebMouseEvent::Button buttonType; // For MouseUp.
    WebPoint pos; // For MouseMove.
    int milliseconds; // For LeapForward.
    int modifiers;

    SavedEvent()
        : type(Unspecified)
        , buttonType(WebMouseEvent::ButtonNone)
        , milliseconds(0)
        , modifiers(0) { }
};

WebDragData currentDragData;
WebDragOperation currentDragEffect;
WebDragOperationsMask currentDragEffectsAllowed;
bool replayingSavedEvents = false;
deque<SavedEvent> mouseEventQueue;
int touchModifiers;
vector<WebTouchPoint> touchPoints;

// Time and place of the last mouse up event.
double lastClickTimeSec = 0;
WebPoint lastClickPos;
int clickCount = 0;

// maximum distance (in space and time) for a mouse click
// to register as a double or triple click
const double multipleClickTimeSec = 1;
const int multipleClickRadiusPixels = 5;

// How much we should scroll per event - the value here is chosen to
// match the WebKit impl and layout test results.
const float scrollbarPixelsPerTick = 40.0f;

inline bool outsideMultiClickRadius(const WebPoint& a, const WebPoint& b)
{
    return ((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)) >
        multipleClickRadiusPixels * multipleClickRadiusPixels;
}

// Used to offset the time the event hander things an event happened. This is
// done so tests can run without a delay, but bypass checks that are time
// dependent (e.g., dragging has a timeout vs selection).
uint32 timeOffsetMs = 0;

double getCurrentEventTimeSec(WebTestDelegate* delegate)
{
    return (delegate->getCurrentTimeInMillisecond() + timeOffsetMs) / 1000.0;
}

void advanceEventTime(int32_t deltaMs)
{
    timeOffsetMs += deltaMs;
}

void initMouseEvent(WebInputEvent::Type t, WebMouseEvent::Button b, const WebPoint& pos, WebMouseEvent* e, double ts, int modifiers)
{
    e->type = t;
    e->button = b;
    e->modifiers = modifiers;
    e->x = pos.x;
    e->y = pos.y;
    e->globalX = pos.x;
    e->globalY = pos.y;
    e->timeStampSeconds = ts;
    e->clickCount = clickCount;
}

int getKeyModifier(const string& modifierName)
{
    const char* characters = modifierName.c_str();
    if (!strcmp(characters, "ctrlKey")
#ifndef __APPLE__
        || !strcmp(characters, "addSelectionKey")
#endif
        ) {
        return WebInputEvent::ControlKey;
    } else if (!strcmp(characters, "shiftKey") || !strcmp(characters, "rangeSelectionKey")) {
        return WebInputEvent::ShiftKey;
    } else if (!strcmp(characters, "altKey")) {
        return WebInputEvent::AltKey;
#ifdef __APPLE__
    } else if (!strcmp(characters, "metaKey") || !strcmp(characters, "addSelectionKey")) {
        return WebInputEvent::MetaKey;
#else
    } else if (!strcmp(characters, "metaKey")) {
        return WebInputEvent::MetaKey;
#endif
    } else if (!strcmp(characters, "autoRepeat")) {
        return WebInputEvent::IsAutoRepeat;
    } else if (!strcmp(characters, "copyKey")) {
#ifdef __APPLE__
        return WebInputEvent::AltKey;
#else
        return WebInputEvent::ControlKey;
#endif
    }

    return 0;
}

int getKeyModifiers(const CppVariant* argument)
{
    int modifiers = 0;
    if (argument->isObject()) {
        vector<string> modifierNames = argument->toStringVector();
        for (vector<string>::const_iterator i = modifierNames.begin(); i != modifierNames.end(); ++i)
            modifiers |= getKeyModifier(*i);
    } else if (argument->isString()) {
        modifiers |= getKeyModifier(argument->toString());
    }
    return modifiers;
}

// Get the edit command corresponding to a keyboard event.
// Returns true if the specified event corresponds to an edit command, the name
// of the edit command will be stored in |*name|.
bool getEditCommand(const WebKeyboardEvent& event, string* name)
{
#ifdef __APPLE__
    // We only cares about Left,Right,Up,Down keys with Command or Command+Shift
    // modifiers. These key events correspond to some special movement and
    // selection editor commands, and was supposed to be handled in
    // WebKit/chromium/src/EditorClientImpl.cpp. But these keys will be marked
    // as system key, which prevents them from being handled. Thus they must be
    // handled specially.
    if ((event.modifiers & ~WebKeyboardEvent::ShiftKey) != WebKeyboardEvent::MetaKey)
        return false;

    switch (event.windowsKeyCode) {
    case VKEY_LEFT:
        *name = "MoveToBeginningOfLine";
        break;
    case VKEY_RIGHT:
        *name = "MoveToEndOfLine";
        break;
    case VKEY_UP:
        *name = "MoveToBeginningOfDocument";
        break;
    case VKEY_DOWN:
        *name = "MoveToEndOfDocument";
        break;
    default:
        return false;
    }

    if (event.modifiers & WebKeyboardEvent::ShiftKey)
        name->append("AndModifySelection");

    return true;
#else
    return false;
#endif
}

// Key event location code introduced in DOM Level 3.
// See also: http://www.w3.org/TR/DOM-Level-3-Events/#events-keyboardevents
enum KeyLocationCode {
    DOMKeyLocationStandard      = 0x00,
    DOMKeyLocationLeft          = 0x01,
    DOMKeyLocationRight         = 0x02,
    DOMKeyLocationNumpad        = 0x03
};

}

EventSender::EventSender(TestInterfaces* interfaces)
    : m_testInterfaces(interfaces)
    , m_delegate(0)
{
    // Initialize the map that associates methods of this class with the names
    // they will use when called by JavaScript. The actual binding of those
    // names to their methods will be done by calling bindToJavaScript() (defined
    // by CppBoundClass, the parent to EventSender).
    bindMethod("addTouchPoint", &EventSender::addTouchPoint);
    bindMethod("beginDragWithFiles", &EventSender::beginDragWithFiles);
    bindMethod("cancelTouchPoint", &EventSender::cancelTouchPoint);
    bindMethod("clearKillRing", &EventSender::clearKillRing);
    bindMethod("clearTouchPoints", &EventSender::clearTouchPoints);
    bindMethod("contextClick", &EventSender::contextClick);
    bindMethod("continuousMouseScrollBy", &EventSender::continuousMouseScrollBy);
    bindMethod("dispatchMessage", &EventSender::dispatchMessage);
    bindMethod("dumpFilenameBeingDragged", &EventSender::dumpFilenameBeingDragged);
    bindMethod("enableDOMUIEventLogging", &EventSender::enableDOMUIEventLogging);
    bindMethod("fireKeyboardEventsToElement", &EventSender::fireKeyboardEventsToElement);
    bindMethod("keyDown", &EventSender::keyDown);
    bindMethod("leapForward", &EventSender::leapForward);
    bindMethod("mouseDown", &EventSender::mouseDown);
    bindMethod("mouseMoveTo", &EventSender::mouseMoveTo);
    bindMethod("mouseScrollBy", &EventSender::mouseScrollBy);
    bindMethod("mouseUp", &EventSender::mouseUp);
    bindMethod("mouseDragBegin", &EventSender::mouseDragBegin);
    bindMethod("mouseDragEnd", &EventSender::mouseDragEnd);
    bindMethod("mouseMomentumBegin", &EventSender::mouseMomentumBegin);
    bindMethod("mouseMomentumScrollBy", &EventSender::mouseMomentumScrollBy);
    bindMethod("mouseMomentumEnd", &EventSender::mouseMomentumEnd);
    bindMethod("releaseTouchPoint", &EventSender::releaseTouchPoint);
    bindMethod("scheduleAsynchronousClick", &EventSender::scheduleAsynchronousClick);
    bindMethod("scheduleAsynchronousKeyDown", &EventSender::scheduleAsynchronousKeyDown);
    bindMethod("setTouchModifier", &EventSender::setTouchModifier);
    bindMethod("textZoomIn", &EventSender::textZoomIn);
    bindMethod("textZoomOut", &EventSender::textZoomOut);
    bindMethod("touchCancel", &EventSender::touchCancel);
    bindMethod("touchEnd", &EventSender::touchEnd);
    bindMethod("touchMove", &EventSender::touchMove);
    bindMethod("touchStart", &EventSender::touchStart);
    bindMethod("updateTouchPoint", &EventSender::updateTouchPoint);
    bindMethod("gestureFlingCancel", &EventSender::gestureFlingCancel);
    bindMethod("gestureFlingStart", &EventSender::gestureFlingStart);
    bindMethod("gestureScrollBegin", &EventSender::gestureScrollBegin);
    bindMethod("gestureScrollEnd", &EventSender::gestureScrollEnd);
    bindMethod("gestureScrollFirstPoint", &EventSender::gestureScrollFirstPoint);
    bindMethod("gestureScrollUpdate", &EventSender::gestureScrollUpdate);
    bindMethod("gestureScrollUpdateWithoutPropagation", &EventSender::gestureScrollUpdateWithoutPropagation);
    bindMethod("gestureTap", &EventSender::gestureTap);
    bindMethod("gestureTapDown", &EventSender::gestureTapDown);
    bindMethod("gestureShowPress", &EventSender::gestureShowPress);
    bindMethod("gestureTapCancel", &EventSender::gestureTapCancel);
    bindMethod("gestureLongPress", &EventSender::gestureLongPress);
    bindMethod("gestureLongTap", &EventSender::gestureLongTap);
    bindMethod("gestureTwoFingerTap", &EventSender::gestureTwoFingerTap);
    bindMethod("zoomPageIn", &EventSender::zoomPageIn);
    bindMethod("zoomPageOut", &EventSender::zoomPageOut);
    bindMethod("setPageScaleFactor", &EventSender::setPageScaleFactor);

    bindProperty("forceLayoutOnEvents", &forceLayoutOnEvents);

    // When set to true (the default value), we batch mouse move and mouse up
    // events so we can simulate drag & drop.
    bindProperty("dragMode", &dragMode);
#ifdef WIN32
    bindProperty("WM_KEYDOWN", &wmKeyDown);
    bindProperty("WM_KEYUP", &wmKeyUp);
    bindProperty("WM_CHAR", &wmChar);
    bindProperty("WM_DEADCHAR", &wmDeadChar);
    bindProperty("WM_SYSKEYDOWN", &wmSysKeyDown);
    bindProperty("WM_SYSKEYUP", &wmSysKeyUp);
    bindProperty("WM_SYSCHAR", &wmSysChar);
    bindProperty("WM_SYSDEADCHAR", &wmSysDeadChar);
#endif
}

EventSender::~EventSender()
{
}

void EventSender::setContextMenuData(const WebContextMenuData& contextMenuData)
{
    m_lastContextMenuData = scoped_ptr<WebContextMenuData>(new WebContextMenuData(contextMenuData));
}

void EventSender::reset()
{
    // The test should have finished a drag and the mouse button state.
    BLINK_ASSERT(currentDragData.isNull());
    currentDragData.reset();
    currentDragEffect = blink::WebDragOperationNone;
    currentDragEffectsAllowed = blink::WebDragOperationNone;
    if (webview() && pressedButton != WebMouseEvent::ButtonNone)
        webview()->mouseCaptureLost();
    pressedButton = WebMouseEvent::ButtonNone;
    dragMode.set(true);
    forceLayoutOnEvents.set(true);
#ifdef WIN32
    wmKeyDown.set(WM_KEYDOWN);
    wmKeyUp.set(WM_KEYUP);
    wmChar.set(WM_CHAR);
    wmDeadChar.set(WM_DEADCHAR);
    wmSysKeyDown.set(WM_SYSKEYDOWN);
    wmSysKeyUp.set(WM_SYSKEYUP);
    wmSysChar.set(WM_SYSCHAR);
    wmSysDeadChar.set(WM_SYSDEADCHAR);
#endif
    lastMousePos = WebPoint(0, 0);
    lastClickTimeSec = 0;
    lastClickPos = WebPoint(0, 0);
    clickCount = 0;
    lastButtonType = WebMouseEvent::ButtonNone;
    timeOffsetMs = 0;
    touchModifiers = 0;
    touchPoints.clear();
    m_taskList.revokeAll();
    m_currentGestureLocation = WebPoint(0, 0);
    mouseEventQueue.clear();
}

void EventSender::doDragDrop(const WebDragData& dragData, WebDragOperationsMask mask)
{
    WebMouseEvent event;
    initMouseEvent(WebInputEvent::MouseDown, pressedButton, lastMousePos, &event, getCurrentEventTimeSec(m_delegate), 0);
    WebPoint clientPoint(event.x, event.y);
    WebPoint screenPoint(event.globalX, event.globalY);
    currentDragData = dragData;
    currentDragEffectsAllowed = mask;
    currentDragEffect = webview()->dragTargetDragEnter(dragData, clientPoint, screenPoint, currentDragEffectsAllowed, 0);

    // Finish processing events.
    replaySavedEvents();
}

void EventSender::dumpFilenameBeingDragged(const CppArgumentList&, CppVariant*)
{
    WebString filename;
    WebVector<WebDragData::Item> items = currentDragData.items();
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].storageType == WebDragData::Item::StorageTypeBinaryData) {
            filename = items[i].title;
            break;
        }
    }
    m_delegate->printMessage(std::string("Filename being dragged: ") + filename.utf8().data() + "\n");
}

WebMouseEvent::Button EventSender::getButtonTypeFromButtonNumber(int buttonCode)
{
    if (!buttonCode)
        return WebMouseEvent::ButtonLeft;
    if (buttonCode == 2)
        return WebMouseEvent::ButtonRight;
    return WebMouseEvent::ButtonMiddle;
}

int EventSender::getButtonNumberFromSingleArg(const CppArgumentList& arguments)
{
    int buttonCode = 0;
    if (arguments.size() > 0 && arguments[0].isNumber())
        buttonCode = arguments[0].toInt32();
    return buttonCode;
}

void EventSender::updateClickCountForButton(WebMouseEvent::Button buttonType)
{
    if ((getCurrentEventTimeSec(m_delegate) - lastClickTimeSec < multipleClickTimeSec)
        && (!outsideMultiClickRadius(lastMousePos, lastClickPos))
        && (buttonType == lastButtonType))
        ++clickCount;
    else {
        clickCount = 1;
        lastButtonType = buttonType;
    }
}

//
// Implemented javascript methods.
//

void EventSender::mouseDown(const CppArgumentList& arguments, CppVariant* result)
{
    if (result) // Could be 0 if invoked asynchronously.
        result->setNull();

    if (shouldForceLayoutOnEvents())
        webview()->layout();

    int buttonNumber = getButtonNumberFromSingleArg(arguments);
    BLINK_ASSERT(buttonNumber != -1);

    WebMouseEvent::Button buttonType = getButtonTypeFromButtonNumber(buttonNumber);

    updateClickCountForButton(buttonType);

    WebMouseEvent event;
    pressedButton = buttonType;
    int modifiers = 0;
    if (arguments.size() >= 2 && (arguments[1].isObject() || arguments[1].isString()))
        modifiers = getKeyModifiers(&(arguments[1]));
    initMouseEvent(WebInputEvent::MouseDown, buttonType, lastMousePos, &event, getCurrentEventTimeSec(m_delegate), modifiers);
    webview()->handleInputEvent(event);
}

void EventSender::mouseUp(const CppArgumentList& arguments, CppVariant* result)
{
    if (result) // Could be 0 if invoked asynchronously.
        result->setNull();

    if (shouldForceLayoutOnEvents())
        webview()->layout();

    int buttonNumber = getButtonNumberFromSingleArg(arguments);
    BLINK_ASSERT(buttonNumber != -1);

    WebMouseEvent::Button buttonType = getButtonTypeFromButtonNumber(buttonNumber);

    int modifiers = 0;
    if (arguments.size() >= 2 && (arguments[1].isObject() || arguments[1].isString()))
        modifiers = getKeyModifiers(&(arguments[1]));

    if (isDragMode() && !replayingSavedEvents) {
        SavedEvent savedEvent;
        savedEvent.type = SavedEvent::MouseUp;
        savedEvent.buttonType = buttonType;
        savedEvent.modifiers = modifiers;
        mouseEventQueue.push_back(savedEvent);
        replaySavedEvents();
    } else {
        WebMouseEvent event;
        initMouseEvent(WebInputEvent::MouseUp, buttonType, lastMousePos, &event, getCurrentEventTimeSec(m_delegate), modifiers);
        doMouseUp(event);
    }
}

void EventSender::doMouseUp(const WebMouseEvent& e)
{
    webview()->handleInputEvent(e);

    pressedButton = WebMouseEvent::ButtonNone;
    lastClickTimeSec = e.timeStampSeconds;
    lastClickPos = lastMousePos;

    // If we're in a drag operation, complete it.
    if (currentDragData.isNull())
        return;

    WebPoint clientPoint(e.x, e.y);
    WebPoint screenPoint(e.globalX, e.globalY);
    finishDragAndDrop(e, webview()->dragTargetDragOver(clientPoint, screenPoint, currentDragEffectsAllowed, 0));
}

void EventSender::finishDragAndDrop(const WebMouseEvent& e, blink::WebDragOperation dragEffect)
{
    WebPoint clientPoint(e.x, e.y);
    WebPoint screenPoint(e.globalX, e.globalY);
    currentDragEffect = dragEffect;
    if (currentDragEffect) {
        // Specifically pass any keyboard modifiers to the drop
        // method. This allows tests to control the drop type
        // (i.e. copy or move).
        webview()->dragTargetDrop(clientPoint, screenPoint, e.modifiers);
    } else {
        webview()->dragTargetDragLeave();
    }
    webview()->dragSourceEndedAt(clientPoint, screenPoint, currentDragEffect);
    webview()->dragSourceSystemDragEnded();

    currentDragData.reset();
}

void EventSender::mouseMoveTo(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();

    if (arguments.size() < 2 || !arguments[0].isNumber() || !arguments[1].isNumber())
        return;
    if (shouldForceLayoutOnEvents())
        webview()->layout();

    WebPoint mousePos(arguments[0].toInt32(), arguments[1].toInt32());

    int modifiers = 0;
    if (arguments.size() >= 3 && (arguments[2].isObject() || arguments[2].isString()))
        modifiers = getKeyModifiers(&(arguments[2]));

    if (isDragMode() && pressedButton == WebMouseEvent::ButtonLeft && !replayingSavedEvents) {
        SavedEvent savedEvent;
        savedEvent.type = SavedEvent::MouseMove;
        savedEvent.pos = mousePos;
        savedEvent.modifiers = modifiers;
        mouseEventQueue.push_back(savedEvent);
    } else {
        WebMouseEvent event;
        initMouseEvent(WebInputEvent::MouseMove, pressedButton, mousePos, &event, getCurrentEventTimeSec(m_delegate), modifiers);
        doMouseMove(event);
    }
}

void EventSender::doMouseMove(const WebMouseEvent& e)
{
    lastMousePos = WebPoint(e.x, e.y);

    webview()->handleInputEvent(e);

    if (pressedButton == WebMouseEvent::ButtonNone || currentDragData.isNull())
        return;
    WebPoint clientPoint(e.x, e.y);
    WebPoint screenPoint(e.globalX, e.globalY);
    currentDragEffect = webview()->dragTargetDragOver(clientPoint, screenPoint, currentDragEffectsAllowed, 0);
}

void EventSender::keyDown(const CppArgumentList& arguments, CppVariant* result)
{
    if (result)
        result->setNull();
    if (arguments.size() < 1 || !arguments[0].isString())
        return;
    bool generateChar = false;

    // FIXME: I'm not exactly sure how we should convert the string to a key
    // event. This seems to work in the cases I tested.
    // FIXME: Should we also generate a KEY_UP?
    string codeStr = arguments[0].toString();

    // Convert \n -> VK_RETURN. Some layout tests use \n to mean "Enter", when
    // Windows uses \r for "Enter".
    int code = 0;
    int text = 0;
    bool needsShiftKeyModifier = false;
    if ("\n" == codeStr) {
        generateChar = true;
        text = code = VKEY_RETURN;
    } else if ("rightArrow" == codeStr)
        code = VKEY_RIGHT;
    else if ("downArrow" == codeStr)
        code = VKEY_DOWN;
    else if ("leftArrow" == codeStr)
        code = VKEY_LEFT;
    else if ("upArrow" == codeStr)
        code = VKEY_UP;
    else if ("insert" == codeStr)
        code = VKEY_INSERT;
    else if ("delete" == codeStr)
        code = VKEY_DELETE;
    else if ("pageUp" == codeStr)
        code = VKEY_PRIOR;
    else if ("pageDown" == codeStr)
        code = VKEY_NEXT;
    else if ("home" == codeStr)
        code = VKEY_HOME;
    else if ("end" == codeStr)
        code = VKEY_END;
    else if ("printScreen" == codeStr)
        code = VKEY_SNAPSHOT;
    else if ("menu" == codeStr)
        code = VKEY_APPS;
    else if ("leftControl" == codeStr)
        code = VKEY_LCONTROL;
    else if ("rightControl" == codeStr)
        code = VKEY_RCONTROL;
    else if ("leftShift" == codeStr)
        code = VKEY_LSHIFT;
    else if ("rightShift" == codeStr)
        code = VKEY_RSHIFT;
    else if ("leftAlt" == codeStr)
        code = VKEY_LMENU;
    else if ("rightAlt" == codeStr)
        code = VKEY_RMENU;
    else if ("numLock" == codeStr)
        code = VKEY_NUMLOCK;
    else {
        // Compare the input string with the function-key names defined by the
        // DOM spec (i.e. "F1",...,"F24"). If the input string is a function-key
        // name, set its key code.
        for (int i = 1; i <= 24; ++i) {
            char functionChars[10];
            snprintf(functionChars, 10, "F%d", i);
            string functionKeyName(functionChars);
            if (functionKeyName == codeStr) {
                code = VKEY_F1 + (i - 1);
                break;
            }
        }
        if (!code) {
            WebString webCodeStr = WebString::fromUTF8(codeStr.data(), codeStr.size());
            BLINK_ASSERT(webCodeStr.length() == 1);
            text = code = webCodeStr.at(0);
            needsShiftKeyModifier = needsShiftModifier(code);
            if ((code & 0xFF) >= 'a' && (code & 0xFF) <= 'z')
                code -= 'a' - 'A';
            generateChar = true;
        }

        if ("(" == codeStr) {
            code = '9';
            needsShiftKeyModifier = true;
        }
    }

    // For one generated keyboard event, we need to generate a keyDown/keyUp
    // pair; refer to EventSender.cpp in Tools/DumpRenderTree/win.
    // On Windows, we might also need to generate a char event to mimic the
    // Windows event flow; on other platforms we create a merged event and test
    // the event flow that that platform provides.
    WebKeyboardEvent eventDown, eventChar, eventUp;
    eventDown.type = WebInputEvent::RawKeyDown;
    eventDown.modifiers = 0;
    eventDown.windowsKeyCode = code;
#if defined(__linux__) && defined(TOOLKIT_GTK)
    eventDown.nativeKeyCode = NativeKeyCodeForWindowsKeyCode(code);
#endif

    if (generateChar) {
        eventDown.text[0] = text;
        eventDown.unmodifiedText[0] = text;
    }
    eventDown.setKeyIdentifierFromWindowsKeyCode();

    if (arguments.size() >= 2 && (arguments[1].isObject() || arguments[1].isString())) {
        eventDown.modifiers = getKeyModifiers(&(arguments[1]));
#if WIN32 || __APPLE__ || defined(ANDROID) || defined(TOOLKIT_GTK)
        eventDown.isSystemKey = WebInputEventFactory::isSystemKeyEvent(eventDown);
#endif
    }

    if (needsShiftKeyModifier)
        eventDown.modifiers |= WebInputEvent::ShiftKey;

    // See if KeyLocation argument is given.
    if (arguments.size() >= 3 && arguments[2].isNumber()) {
        int location = arguments[2].toInt32();
        if (location == DOMKeyLocationNumpad)
            eventDown.modifiers |= WebInputEvent::IsKeyPad;
    }

    eventChar = eventUp = eventDown;
    eventUp.type = WebInputEvent::KeyUp;
    // EventSender.m forces a layout here, with at least one
    // test (fast/forms/focus-control-to-page.html) relying on this.
    if (shouldForceLayoutOnEvents())
        webview()->layout();

    // In the browser, if a keyboard event corresponds to an editor command,
    // the command will be dispatched to the renderer just before dispatching
    // the keyboard event, and then it will be executed in the
    // RenderView::handleCurrentKeyboardEvent() method, which is called from
    // third_party/WebKit/Source/WebKit/chromium/src/EditorClientImpl.cpp.
    // We just simulate the same behavior here.
    string editCommand;
    if (getEditCommand(eventDown, &editCommand))
        m_delegate->setEditCommand(editCommand, "");

    webview()->handleInputEvent(eventDown);

    if (code == VKEY_ESCAPE && !currentDragData.isNull()) {
        WebMouseEvent event;
        initMouseEvent(WebInputEvent::MouseDown, pressedButton, lastMousePos, &event, getCurrentEventTimeSec(m_delegate), 0);
        finishDragAndDrop(event, blink::WebDragOperationNone);
    }

    m_delegate->clearEditCommand();

    if (generateChar) {
        eventChar.type = WebInputEvent::Char;
        eventChar.keyIdentifier[0] = '\0';
        webview()->handleInputEvent(eventChar);
    }

    webview()->handleInputEvent(eventUp);
}

void EventSender::dispatchMessage(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();

#ifdef WIN32
    if (arguments.size() == 3) {
        // Grab the message id to see if we need to dispatch it.
        int msg = arguments[0].toInt32();

        // WebKit's version of this function stuffs a MSG struct and uses
        // TranslateMessage and DispatchMessage. We use a WebKeyboardEvent, which
        // doesn't need to receive the DeadChar and SysDeadChar messages.
        if (msg == WM_DEADCHAR || msg == WM_SYSDEADCHAR)
            return;

        if (shouldForceLayoutOnEvents())
            webview()->layout();

        unsigned long lparam = static_cast<unsigned long>(arguments[2].toDouble());
        webview()->handleInputEvent(WebInputEventFactory::keyboardEvent(0, msg, arguments[1].toInt32(), lparam));
    } else
        BLINK_ASSERT_NOT_REACHED();
#endif
}

bool EventSender::needsShiftModifier(int keyCode)
{
    // If code is an uppercase letter, assign a SHIFT key to
    // eventDown.modifier, this logic comes from
    // Tools/DumpRenderTree/win/EventSender.cpp
    return (keyCode & 0xFF) >= 'A' && (keyCode & 0xFF) <= 'Z';
}

void EventSender::leapForward(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();

    if (arguments.size() < 1 || !arguments[0].isNumber())
        return;

    int milliseconds = arguments[0].toInt32();
    if (isDragMode() && pressedButton == WebMouseEvent::ButtonLeft && !replayingSavedEvents) {
        SavedEvent savedEvent;
        savedEvent.type = SavedEvent::LeapForward;
        savedEvent.milliseconds = milliseconds;
        mouseEventQueue.push_back(savedEvent);
    } else
        doLeapForward(milliseconds);
}

void EventSender::doLeapForward(int milliseconds)
{
    advanceEventTime(milliseconds);
}

// Apple's port of WebKit zooms by a factor of 1.2 (see
// WebKit/WebView/WebView.mm)
void EventSender::textZoomIn(const CppArgumentList&, CppVariant* result)
{
    webview()->setTextZoomFactor(webview()->textZoomFactor() * 1.2f);
    result->setNull();
}

void EventSender::textZoomOut(const CppArgumentList&, CppVariant* result)
{
    webview()->setTextZoomFactor(webview()->textZoomFactor() / 1.2f);
    result->setNull();
}

void EventSender::zoomPageIn(const CppArgumentList&, CppVariant* result)
{
    const vector<WebTestProxyBase*>& windowList = m_testInterfaces->windowList();

    for (size_t i = 0; i < windowList.size(); ++i)
        windowList.at(i)->webView()->setZoomLevel(windowList.at(i)->webView()->zoomLevel() + 1);
    result->setNull();
}

void EventSender::zoomPageOut(const CppArgumentList&, CppVariant* result)
{
    const vector<WebTestProxyBase*>& windowList = m_testInterfaces->windowList();

    for (size_t i = 0; i < windowList.size(); ++i)
        windowList.at(i)->webView()->setZoomLevel(windowList.at(i)->webView()->zoomLevel() - 1);
    result->setNull();
}

void EventSender::setPageScaleFactor(const CppArgumentList& arguments, CppVariant* result)
{
    if (arguments.size() < 3 || !arguments[0].isNumber() || !arguments[1].isNumber() || !arguments[2].isNumber())
        return;

    float scaleFactor = static_cast<float>(arguments[0].toDouble());
    int x = arguments[1].toInt32();
    int y = arguments[2].toInt32();
    webview()->setPageScaleFactorLimits(scaleFactor, scaleFactor);
    webview()->setPageScaleFactor(scaleFactor, WebPoint(x, y));
    result->setNull();
}

void EventSender::mouseScrollBy(const CppArgumentList& arguments, CppVariant* result)
{
    WebMouseWheelEvent event;
    initMouseWheelEvent(arguments, result, false, &event);
    webview()->handleInputEvent(event);
}

void EventSender::continuousMouseScrollBy(const CppArgumentList& arguments, CppVariant* result)
{
    WebMouseWheelEvent event;
    initMouseWheelEvent(arguments, result, true, &event);
    webview()->handleInputEvent(event);
}

void EventSender::replaySavedEvents()
{
    replayingSavedEvents = true;
    while (!mouseEventQueue.empty()) {
        SavedEvent e = mouseEventQueue.front();
        mouseEventQueue.pop_front();

        switch (e.type) {
        case SavedEvent::MouseMove: {
            WebMouseEvent event;
            initMouseEvent(WebInputEvent::MouseMove, pressedButton, e.pos, &event, getCurrentEventTimeSec(m_delegate), e.modifiers);
            doMouseMove(event);
            break;
        }
        case SavedEvent::LeapForward:
            doLeapForward(e.milliseconds);
            break;
        case SavedEvent::MouseUp: {
            WebMouseEvent event;
            initMouseEvent(WebInputEvent::MouseUp, e.buttonType, lastMousePos, &event, getCurrentEventTimeSec(m_delegate), e.modifiers);
            doMouseUp(event);
            break;
        }
        default:
            BLINK_ASSERT_NOT_REACHED();
        }
    }

    replayingSavedEvents = false;
}

// Because actual context menu is implemented by the browser side,
// this function does only what LayoutTests are expecting:
// - Many test checks the count of items. So returning non-zero value makes sense.
// - Some test compares the count before and after some action. So changing the count based on flags
//   also makes sense. This function is doing such for some flags.
// - Some test even checks actual string content. So providing it would be also helpful.
//
static vector<WebString> makeMenuItemStringsFor(WebContextMenuData* contextMenu, WebTestDelegate* delegate)
{
    // These constants are based on Safari's context menu because tests are made for it.
    static const char* nonEditableMenuStrings[] = { "Back", "Reload Page", "Open in Dashbaord", "<separator>", "View Source", "Save Page As", "Print Page", "Inspect Element", 0 };
    static const char* editableMenuStrings[] = { "Cut", "Copy", "<separator>", "Paste", "Spelling and Grammar", "Substitutions, Transformations", "Font", "Speech", "Paragraph Direction", "<separator>", 0 };

    // This is possible because mouse events are cancelleable.
    if (!contextMenu)
        return vector<WebString>();

    vector<WebString> strings;

    if (contextMenu->isEditable) {
        for (const char** item = editableMenuStrings; *item; ++item)
            strings.push_back(WebString::fromUTF8(*item));
        WebVector<WebString> suggestions;
        MockSpellCheck::fillSuggestionList(contextMenu->misspelledWord, &suggestions);
        for (size_t i = 0; i < suggestions.size(); ++i)
            strings.push_back(suggestions[i]);
    } else {
        for (const char** item = nonEditableMenuStrings; *item; ++item)
            strings.push_back(WebString::fromUTF8(*item));
    }

    return strings;
}

void EventSender::contextClick(const CppArgumentList& arguments, CppVariant* result)
{
    if (shouldForceLayoutOnEvents())
        webview()->layout();

    updateClickCountForButton(WebMouseEvent::ButtonRight);

    // Clears last context menu data because we need to know if the context menu be requested
    // after following mouse events.
    m_lastContextMenuData.reset();

    // Generate right mouse down and up.
    WebMouseEvent event;
    // This is a hack to work around only allowing a single pressed button since we want to
    // test the case where both the left and right mouse buttons are pressed.
    if (pressedButton == WebMouseEvent::ButtonNone)
        pressedButton = WebMouseEvent::ButtonRight;
    initMouseEvent(WebInputEvent::MouseDown, WebMouseEvent::ButtonRight, lastMousePos, &event, getCurrentEventTimeSec(m_delegate), 0);
    webview()->handleInputEvent(event);

#ifdef WIN32
    initMouseEvent(WebInputEvent::MouseUp, WebMouseEvent::ButtonRight, lastMousePos, &event, getCurrentEventTimeSec(m_delegate), 0);
    webview()->handleInputEvent(event);

    pressedButton = WebMouseEvent::ButtonNone;
#endif

    NPObject* resultArray = WebBindings::makeStringArray(makeMenuItemStringsFor(m_lastContextMenuData.get(), m_delegate));
    result->set(resultArray);
    WebBindings::releaseObject(resultArray);

    m_lastContextMenuData.reset();
}

class MouseDownTask: public WebMethodTask<EventSender> {
public:
    MouseDownTask(EventSender* obj, const CppArgumentList& arg)
        : WebMethodTask<EventSender>(obj), m_arguments(arg) { }
    virtual void runIfValid() OVERRIDE { m_object->mouseDown(m_arguments, 0); }

private:
    CppArgumentList m_arguments;
};

class MouseUpTask: public WebMethodTask<EventSender> {
public:
    MouseUpTask(EventSender* obj, const CppArgumentList& arg)
        : WebMethodTask<EventSender>(obj), m_arguments(arg) { }
    virtual void runIfValid() OVERRIDE { m_object->mouseUp(m_arguments, 0); }

private:
    CppArgumentList m_arguments;
};

void EventSender::scheduleAsynchronousClick(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();
    m_delegate->postTask(new MouseDownTask(this, arguments));
    m_delegate->postTask(new MouseUpTask(this, arguments));
}

class KeyDownTask : public WebMethodTask<EventSender> {
public:
    KeyDownTask(EventSender* obj, const CppArgumentList& arg)
        : WebMethodTask<EventSender>(obj), m_arguments(arg) { }
    virtual void runIfValid() OVERRIDE { m_object->keyDown(m_arguments, 0); }

private:
    CppArgumentList m_arguments;
};

void EventSender::scheduleAsynchronousKeyDown(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();
    m_delegate->postTask(new KeyDownTask(this, arguments));
}

void EventSender::beginDragWithFiles(const CppArgumentList& arguments, CppVariant* result)
{
    currentDragData.initialize();
    vector<string> files = arguments[0].toStringVector();
    WebVector<WebString> absoluteFilenames(files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        WebDragData::Item item;
        item.storageType = WebDragData::Item::StorageTypeFilename;
        item.filenameData = m_delegate->getAbsoluteWebStringFromUTF8Path(files[i]);
        currentDragData.addItem(item);
        absoluteFilenames[i] = item.filenameData;
    }
    currentDragData.setFilesystemId(m_delegate->registerIsolatedFileSystem(absoluteFilenames));
    currentDragEffectsAllowed = blink::WebDragOperationCopy;

    // Provide a drag source.
    webview()->dragTargetDragEnter(currentDragData, lastMousePos, lastMousePos, currentDragEffectsAllowed, 0);

    // dragMode saves events and then replays them later. We don't need/want that.
    dragMode.set(false);

    // Make the rest of eventSender think a drag is in progress.
    pressedButton = WebMouseEvent::ButtonLeft;

    result->setNull();
}

void EventSender::addTouchPoint(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();

    WebTouchPoint touchPoint;
    touchPoint.state = WebTouchPoint::StatePressed;
    touchPoint.position.x = arguments[0].toInt32();
    touchPoint.position.y = arguments[1].toInt32();
    touchPoint.screenPosition = touchPoint.position;

    if (arguments.size() > 2) {
        int radiusX = arguments[2].toInt32();
        int radiusY = radiusX;
        if (arguments.size() > 3)
            radiusY = arguments[3].toInt32();

        touchPoint.radiusX = radiusX;
        touchPoint.radiusY = radiusY;
    }

    int lowestId = 0;
    for (size_t i = 0; i < touchPoints.size(); i++) {
        if (touchPoints[i].id == lowestId)
            lowestId++;
    }
    touchPoint.id = lowestId;
    touchPoints.push_back(touchPoint);
}

void EventSender::clearTouchPoints(const CppArgumentList&, CppVariant* result)
{
    result->setNull();
    touchPoints.clear();
}

void EventSender::releaseTouchPoint(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();

    const unsigned index = arguments[0].toInt32();
    BLINK_ASSERT(index < touchPoints.size());

    WebTouchPoint* touchPoint = &touchPoints[index];
    touchPoint->state = WebTouchPoint::StateReleased;
}

void EventSender::setTouchModifier(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();

    int mask = 0;
    const string keyName = arguments[0].toString();
    if (keyName == "shift")
        mask = WebInputEvent::ShiftKey;
    else if (keyName == "alt")
        mask = WebInputEvent::AltKey;
    else if (keyName == "ctrl")
        mask = WebInputEvent::ControlKey;
    else if (keyName == "meta")
        mask = WebInputEvent::MetaKey;

    if (arguments[1].toBoolean())
        touchModifiers |= mask;
    else
        touchModifiers &= ~mask;
}

void EventSender::updateTouchPoint(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();

    const unsigned index = arguments[0].toInt32();
    BLINK_ASSERT(index < touchPoints.size());

    WebTouchPoint* touchPoint = &touchPoints[index];
    touchPoint->state = WebTouchPoint::StateMoved;
    touchPoint->position.x = arguments[1].toInt32();
    touchPoint->position.y = arguments[2].toInt32();
    touchPoint->screenPosition = touchPoint->position;
}

void EventSender::cancelTouchPoint(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();

    const unsigned index = arguments[0].toInt32();
    BLINK_ASSERT(index < touchPoints.size());

    WebTouchPoint* touchPoint = &touchPoints[index];
    touchPoint->state = WebTouchPoint::StateCancelled;
}

void EventSender::sendCurrentTouchEvent(const WebInputEvent::Type type)
{
    BLINK_ASSERT(static_cast<unsigned>(WebTouchEvent::touchesLengthCap) > touchPoints.size());
    if (shouldForceLayoutOnEvents())
        webview()->layout();

    WebTouchEvent touchEvent;
    touchEvent.type = type;
    touchEvent.modifiers = touchModifiers;
    touchEvent.timeStampSeconds = getCurrentEventTimeSec(m_delegate);
    touchEvent.touchesLength = touchPoints.size();
    for (unsigned i = 0; i < touchPoints.size(); ++i)
        touchEvent.touches[i] = touchPoints[i];
    webview()->handleInputEvent(touchEvent);

    for (unsigned i = 0; i < touchPoints.size(); ++i) {
        WebTouchPoint* touchPoint = &touchPoints[i];
        if (touchPoint->state == WebTouchPoint::StateReleased) {
            touchPoints.erase(touchPoints.begin() + i);
            --i;
        } else
            touchPoint->state = WebTouchPoint::StateStationary;
    }
}

void EventSender::mouseDragBegin(const CppArgumentList& arguments, CppVariant* result)
{
    WebMouseWheelEvent event;
    initMouseEvent(WebInputEvent::MouseWheel, WebMouseEvent::ButtonNone, lastMousePos, &event, getCurrentEventTimeSec(m_delegate), 0);
    event.phase = WebMouseWheelEvent::PhaseBegan;
    event.hasPreciseScrollingDeltas = true;
    webview()->handleInputEvent(event);
}

void EventSender::mouseDragEnd(const CppArgumentList& arguments, CppVariant* result)
{
    WebMouseWheelEvent event;
    initMouseEvent(WebInputEvent::MouseWheel, WebMouseEvent::ButtonNone, lastMousePos, &event, getCurrentEventTimeSec(m_delegate), 0);
    event.phase = WebMouseWheelEvent::PhaseEnded;
    event.hasPreciseScrollingDeltas = true;
    webview()->handleInputEvent(event);
}

void EventSender::mouseMomentumBegin(const CppArgumentList& arguments, CppVariant* result)
{
    WebMouseWheelEvent event;
    initMouseEvent(WebInputEvent::MouseWheel, WebMouseEvent::ButtonNone, lastMousePos, &event, getCurrentEventTimeSec(m_delegate), 0);
    event.momentumPhase = WebMouseWheelEvent::PhaseBegan;
    event.hasPreciseScrollingDeltas = true;
    webview()->handleInputEvent(event);
}

void EventSender::mouseMomentumScrollBy(const CppArgumentList& arguments, CppVariant* result)
{
    WebMouseWheelEvent event;
    initMouseWheelEvent(arguments, result, true, &event);
    event.momentumPhase = WebMouseWheelEvent::PhaseChanged;
    event.hasPreciseScrollingDeltas = true;
    webview()->handleInputEvent(event);
}

void EventSender::mouseMomentumEnd(const CppArgumentList& arguments, CppVariant* result)
{
    WebMouseWheelEvent event;
    initMouseEvent(WebInputEvent::MouseWheel, WebMouseEvent::ButtonNone, lastMousePos, &event, getCurrentEventTimeSec(m_delegate), 0);
    event.momentumPhase = WebMouseWheelEvent::PhaseEnded;
    event.hasPreciseScrollingDeltas = true;
    webview()->handleInputEvent(event);
}

void EventSender::initMouseWheelEvent(const CppArgumentList& arguments, CppVariant* result, bool continuous, WebMouseWheelEvent* event)
{
    result->setNull();

    if (arguments.size() < 2 || !arguments[0].isNumber() || !arguments[1].isNumber())
        return;

    // Force a layout here just to make sure every position has been
    // determined before we send events (as well as all the other methods
    // that send an event do).
    if (shouldForceLayoutOnEvents())
        webview()->layout();

    int horizontal = arguments[0].toInt32();
    int vertical = arguments[1].toInt32();
    int paged = false;
    int hasPreciseScrollingDeltas = false;
    int modifiers = 0;

    if (arguments.size() > 2 && arguments[2].isBool())
        paged = arguments[2].toBoolean();

    if (arguments.size() > 3 && arguments[3].isBool())
        hasPreciseScrollingDeltas = arguments[3].toBoolean();

    if (arguments.size() > 4 && (arguments[4].isObject() || arguments[4].isString()))
        modifiers = getKeyModifiers(&(arguments[4]));

    initMouseEvent(WebInputEvent::MouseWheel, pressedButton, lastMousePos, event, getCurrentEventTimeSec(m_delegate), modifiers);
    event->wheelTicksX = static_cast<float>(horizontal);
    event->wheelTicksY = static_cast<float>(vertical);
    event->deltaX = event->wheelTicksX;
    event->deltaY = event->wheelTicksY;
    event->scrollByPage = paged;
    event->hasPreciseScrollingDeltas = hasPreciseScrollingDeltas;

    if (continuous) {
        event->wheelTicksX /= scrollbarPixelsPerTick;
        event->wheelTicksY /= scrollbarPixelsPerTick;
    } else {
        event->deltaX *= scrollbarPixelsPerTick;
        event->deltaY *= scrollbarPixelsPerTick;
    }
}

void EventSender::touchEnd(const CppArgumentList&, CppVariant* result)
{
    result->setNull();
    sendCurrentTouchEvent(WebInputEvent::TouchEnd);
}

void EventSender::touchMove(const CppArgumentList&, CppVariant* result)
{
    result->setNull();
    sendCurrentTouchEvent(WebInputEvent::TouchMove);
}

void EventSender::touchStart(const CppArgumentList&, CppVariant* result)
{
    result->setNull();
    sendCurrentTouchEvent(WebInputEvent::TouchStart);
}

void EventSender::touchCancel(const CppArgumentList&, CppVariant* result)
{
    result->setNull();
    sendCurrentTouchEvent(WebInputEvent::TouchCancel);
}

void EventSender::gestureScrollBegin(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();
    gestureEvent(WebInputEvent::GestureScrollBegin, arguments);
}

void EventSender::gestureScrollEnd(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();
    gestureEvent(WebInputEvent::GestureScrollEnd, arguments);
}

void EventSender::gestureScrollUpdate(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();
    gestureEvent(WebInputEvent::GestureScrollUpdate, arguments);
}

void EventSender::gestureScrollUpdateWithoutPropagation(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();
    gestureEvent(WebInputEvent::GestureScrollUpdateWithoutPropagation, arguments);
}

void EventSender::gestureTap(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();
    gestureEvent(WebInputEvent::GestureTap, arguments);
}

void EventSender::gestureTapDown(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();
    gestureEvent(WebInputEvent::GestureTapDown, arguments);
}

void EventSender::gestureShowPress(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();
    gestureEvent(WebInputEvent::GestureShowPress, arguments);
}

void EventSender::gestureTapCancel(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();
    gestureEvent(WebInputEvent::GestureTapCancel, arguments);
}

void EventSender::gestureLongPress(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();
    gestureEvent(WebInputEvent::GestureLongPress, arguments);
}

void EventSender::gestureLongTap(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();
    gestureEvent(WebInputEvent::GestureLongTap, arguments);
}

void EventSender::gestureTwoFingerTap(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();
    gestureEvent(WebInputEvent::GestureTwoFingerTap, arguments);
}

void EventSender::gestureScrollFirstPoint(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();
    if (arguments.size() < 2 || !arguments[0].isNumber() || !arguments[1].isNumber())
        return;

    WebPoint point(arguments[0].toInt32(), arguments[1].toInt32());
    m_currentGestureLocation = point;
}

void EventSender::gestureEvent(WebInputEvent::Type type, const CppArgumentList& arguments)
{
    if (arguments.size() < 2 || !arguments[0].isNumber() || !arguments[1].isNumber())
        return;

    WebPoint point(arguments[0].toInt32(), arguments[1].toInt32());

    WebGestureEvent event;
    event.type = type;

    switch (type) {
    case WebInputEvent::GestureScrollUpdate:
    case WebInputEvent::GestureScrollUpdateWithoutPropagation:
        event.data.scrollUpdate.deltaX = static_cast<float>(arguments[0].toDouble());
        event.data.scrollUpdate.deltaY = static_cast<float>(arguments[1].toDouble());
        event.x = m_currentGestureLocation.x;
        event.y = m_currentGestureLocation.y;
        m_currentGestureLocation.x = m_currentGestureLocation.x + event.data.scrollUpdate.deltaX;
        m_currentGestureLocation.y = m_currentGestureLocation.y + event.data.scrollUpdate.deltaY;
        break;

    case WebInputEvent::GestureScrollBegin:
        m_currentGestureLocation = WebPoint(point.x, point.y);
        event.x = m_currentGestureLocation.x;
        event.y = m_currentGestureLocation.y;
        break;
    case WebInputEvent::GestureScrollEnd:
    case WebInputEvent::GestureFlingStart:
        event.x = m_currentGestureLocation.x;
        event.y = m_currentGestureLocation.y;
        break;
    case WebInputEvent::GestureTap:
        if (arguments.size() >= 3)
            event.data.tap.tapCount = static_cast<float>(arguments[2].toDouble());
        else
          event.data.tap.tapCount = 1;
        event.x = point.x;
        event.y = point.y;
        break;
    case WebInputEvent::GestureTapUnconfirmed:
        if (arguments.size() >= 3)
            event.data.tap.tapCount = static_cast<float>(arguments[2].toDouble());
        else
          event.data.tap.tapCount = 1;
        event.x = point.x;
        event.y = point.y;
        break;
    case WebInputEvent::GestureTapDown:
        event.x = point.x;
        event.y = point.y;
        if (arguments.size() >= 4) {
            event.data.tapDown.width = static_cast<float>(arguments[2].toDouble());
            event.data.tapDown.height = static_cast<float>(arguments[3].toDouble());
        }
        break;
    case WebInputEvent::GestureShowPress:
        event.x = point.x;
        event.y = point.y;
        if (arguments.size() >= 4) {
            event.data.showPress.width = static_cast<float>(arguments[2].toDouble());
            event.data.showPress.height = static_cast<float>(arguments[3].toDouble());
        }
        break;
    case WebInputEvent::GestureTapCancel:
        event.x = point.x;
        event.y = point.y;
        break;
    case WebInputEvent::GestureLongPress:
        event.x = point.x;
        event.y = point.y;
        if (arguments.size() >= 4) {
            event.data.longPress.width = static_cast<float>(arguments[2].toDouble());
            event.data.longPress.height = static_cast<float>(arguments[3].toDouble());
        }
        break;
    case WebInputEvent::GestureLongTap:
        event.x = point.x;
        event.y = point.y;
        if (arguments.size() >= 4) {
            event.data.longPress.width = static_cast<float>(arguments[2].toDouble());
            event.data.longPress.height = static_cast<float>(arguments[3].toDouble());
        }
        break;
    case WebInputEvent::GestureTwoFingerTap:
        event.x = point.x;
        event.y = point.y;
        if (arguments.size() >= 4) {
            event.data.twoFingerTap.firstFingerWidth = static_cast<float>(arguments[2].toDouble());
            event.data.twoFingerTap.firstFingerHeight = static_cast<float>(arguments[3].toDouble());
        }
        break;
    default:
        BLINK_ASSERT_NOT_REACHED();
    }

    event.globalX = event.x;
    event.globalY = event.y;
    event.timeStampSeconds = getCurrentEventTimeSec(m_delegate);

    if (shouldForceLayoutOnEvents())
        webview()->layout();

    webview()->handleInputEvent(event);

    // Long press might start a drag drop session. Complete it if so.
    if (type == WebInputEvent::GestureLongPress && !currentDragData.isNull()) {
        WebMouseEvent mouseEvent;
        initMouseEvent(WebInputEvent::MouseDown, pressedButton, point, &mouseEvent, getCurrentEventTimeSec(m_delegate), 0);
        finishDragAndDrop(mouseEvent, blink::WebDragOperationNone);
    }
}

void EventSender::gestureFlingCancel(const CppArgumentList&, CppVariant* result)
{
    result->setNull();

    WebGestureEvent event;
    event.type = WebInputEvent::GestureFlingCancel;
    event.timeStampSeconds = getCurrentEventTimeSec(m_delegate);

    if (shouldForceLayoutOnEvents())
        webview()->layout();

    webview()->handleInputEvent(event);
}

void EventSender::gestureFlingStart(const CppArgumentList& arguments, CppVariant* result)
{
    result->setNull();
    if (arguments.size() < 4)
        return;

    for (int i = 0; i < 4; i++)
        if (!arguments[i].isNumber())
            return;

    WebGestureEvent event;
    event.type = WebInputEvent::GestureFlingStart;

    event.x = static_cast<float>(arguments[0].toDouble());
    event.y = static_cast<float>(arguments[1].toDouble());
    event.globalX = event.x;
    event.globalY = event.y;

    event.data.flingStart.velocityX = static_cast<float>(arguments[2].toDouble());
    event.data.flingStart.velocityY = static_cast<float>(arguments[3].toDouble());
    event.timeStampSeconds = getCurrentEventTimeSec(m_delegate);

    if (shouldForceLayoutOnEvents())
        webview()->layout();

    webview()->handleInputEvent(event);
}

//
// Unimplemented stubs
//

void EventSender::enableDOMUIEventLogging(const CppArgumentList&, CppVariant* result)
{
    result->setNull();
}

void EventSender::fireKeyboardEventsToElement(const CppArgumentList&, CppVariant* result)
{
    result->setNull();
}

void EventSender::clearKillRing(const CppArgumentList&, CppVariant* result)
{
    result->setNull();
}

}
