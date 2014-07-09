// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/services/view_manager/root_node_manager.h"
#include "mojo/services/view_manager/window_tree_host_impl.h"
#include "mojo/public/c/gles2/gles2.h"
#include "mojo/services/public/cpp/geometry/geometry_type_converters.h"
#include "mojo/services/view_manager/context_factory_impl.h"
#include "ui/aura/env.h"
#include "ui/aura/layout_manager.h"
#include "ui/aura/window.h"
#include "ui/aura/window_event_dispatcher.h"
#include "ui/compositor/compositor.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/rect.h"

namespace mojo {
namespace view_manager {
namespace service {

// TODO(sky): nuke this. It shouldn't be static.
// static
ContextFactoryImpl* WindowTreeHostImpl::context_factory_ = NULL;

////////////////////////////////////////////////////////////////////////////////
// RootLayoutManager, layout management for the root window's (one) child

class RootLayoutManager : public aura::LayoutManager {
 public:
  RootLayoutManager() : child_(NULL) { }

  // Overridden from aura::LayoutManager
  virtual void OnWindowResized() OVERRIDE;
  virtual void OnWindowAddedToLayout(aura::Window* child) OVERRIDE;
  virtual void OnWillRemoveWindowFromLayout(aura::Window* child) OVERRIDE {}
  virtual void OnWindowRemovedFromLayout(aura::Window* child) OVERRIDE {}
  virtual void OnChildWindowVisibilityChanged(aura::Window* child,
                                              bool visible) OVERRIDE {}
  virtual void SetChildBounds(aura::Window* child,
                              const gfx::Rect& requested_bounds) OVERRIDE;
 private:
  aura::Window* child_;

  DISALLOW_COPY_AND_ASSIGN(RootLayoutManager);
};

void RootLayoutManager::OnWindowResized() {
  if (child_) {
    gfx::Rect bounds = child_->parent()->bounds();
    child_->SetBounds(gfx::Rect(0, 0, bounds.width(), bounds.height()));
  }
}

void RootLayoutManager::OnWindowAddedToLayout(aura::Window* child) {
  DCHECK(!child_);
  child_ = child;
}

void RootLayoutManager::SetChildBounds(aura::Window* child,
                                       const gfx::Rect& requested_bounds) {
  SetChildBoundsDirect(child, gfx::Rect(
      requested_bounds.width(),
      requested_bounds.height()));
}

////////////////////////////////////////////////////////////////////////////////
// WindowTreeHostImpl, public:

WindowTreeHostImpl::WindowTreeHostImpl(
    NativeViewportPtr viewport,
    const gfx::Rect& bounds,
    const base::Callback<void()>& compositor_created_callback)
    : native_viewport_(viewport.Pass()),
      compositor_created_callback_(compositor_created_callback),
      bounds_(bounds) {
  native_viewport_.set_client(this);
  native_viewport_->Create(Rect::From(bounds));

  MessagePipe pipe;
  native_viewport_->CreateGLES2Context(
      MakeRequest<CommandBuffer>(pipe.handle0.Pass()));

  // The ContextFactory must exist before any Compositors are created.
  if (context_factory_) {
    delete context_factory_;
    context_factory_ = NULL;
  }
  context_factory_ = new ContextFactoryImpl(pipe.handle1.Pass());
  aura::Env::GetInstance()->set_context_factory(context_factory_);

  window()->SetLayoutManager(new RootLayoutManager());
}

WindowTreeHostImpl::~WindowTreeHostImpl() {
  DestroyCompositor();
  DestroyDispatcher();
}

////////////////////////////////////////////////////////////////////////////////
// WindowTreeHostImpl, aura::WindowTreeHost implementation:

ui::EventSource* WindowTreeHostImpl::GetEventSource() {
  return this;
}

gfx::AcceleratedWidget WindowTreeHostImpl::GetAcceleratedWidget() {
  NOTIMPLEMENTED() << "GetAcceleratedWidget";
  return gfx::kNullAcceleratedWidget;
}

void WindowTreeHostImpl::Show() {
  window()->Show();
  native_viewport_->Show();
}

void WindowTreeHostImpl::Hide() {
  native_viewport_->Hide();
  window()->Hide();
}

gfx::Rect WindowTreeHostImpl::GetBounds() const {
  return bounds_;
}

void WindowTreeHostImpl::SetBounds(const gfx::Rect& bounds) {
  native_viewport_->SetBounds(Rect::From(bounds));
}

gfx::Point WindowTreeHostImpl::GetLocationOnNativeScreen() const {
  return gfx::Point(0, 0);
}

void WindowTreeHostImpl::SetCapture() {
  NOTIMPLEMENTED();
}

void WindowTreeHostImpl::ReleaseCapture() {
  NOTIMPLEMENTED();
}

void WindowTreeHostImpl::PostNativeEvent(
    const base::NativeEvent& native_event) {
  NOTIMPLEMENTED();
}

void WindowTreeHostImpl::OnDeviceScaleFactorChanged(float device_scale_factor) {
  NOTIMPLEMENTED();
}

void WindowTreeHostImpl::SetCursorNative(gfx::NativeCursor cursor) {
  NOTIMPLEMENTED();
}

void WindowTreeHostImpl::MoveCursorToNative(const gfx::Point& location) {
  NOTIMPLEMENTED();
}

void WindowTreeHostImpl::OnCursorVisibilityChangedNative(bool show) {
  NOTIMPLEMENTED();
}

////////////////////////////////////////////////////////////////////////////////
// WindowTreeHostImpl, ui::EventSource implementation:

ui::EventProcessor* WindowTreeHostImpl::GetEventProcessor() {
  return dispatcher();
}

////////////////////////////////////////////////////////////////////////////////
// WindowTreeHostImpl, NativeViewportClient implementation:

void WindowTreeHostImpl::OnCreated() {
  CreateCompositor(GetAcceleratedWidget());
  compositor_created_callback_.Run();
}

void WindowTreeHostImpl::OnBoundsChanged(RectPtr bounds) {
  bounds_ = bounds.To<gfx::Rect>();
  OnHostResized(bounds_.size());
}

void WindowTreeHostImpl::OnDestroyed() {
  base::MessageLoop::current()->Quit();
}

void WindowTreeHostImpl::OnEvent(EventPtr event,
                                 const mojo::Callback<void()>& callback) {
  switch (event->action) {
    case ui::ET_MOUSE_PRESSED:
    case ui::ET_MOUSE_DRAGGED:
    case ui::ET_MOUSE_RELEASED:
    case ui::ET_MOUSE_MOVED:
    case ui::ET_MOUSE_ENTERED:
    case ui::ET_MOUSE_EXITED: {
      gfx::Point location(event->location->x, event->location->y);
      ui::MouseEvent ev(static_cast<ui::EventType>(event->action), location,
                        location, event->flags, 0);
      SendEventToProcessor(&ev);
      break;
    }
    case ui::ET_MOUSEWHEEL: {
      gfx::Vector2d offset(event->wheel_data->x_offset,
                           event->wheel_data->y_offset);
      gfx::Point location(event->location->x, event->location->y);
      ui::MouseWheelEvent ev(offset, location, location, event->flags, 0);
      SendEventToProcessor(&ev);
      break;
    }
    case ui::ET_KEY_PRESSED:
    case ui::ET_KEY_RELEASED: {
      ui::KeyEvent ev(
          static_cast<ui::EventType>(event->action),
          static_cast<ui::KeyboardCode>(event->key_data->key_code),
          event->flags, event->key_data->is_char);
      SendEventToProcessor(&ev);
      break;
    }
    // TODO(beng): touch, etc.
  }
  callback.Run();
};

}  // namespace service
}  // namespace view_manager
}  // namespace mojo
