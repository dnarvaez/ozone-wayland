// Copyright 2013 Intel Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ozone/wayland/dispatcher.h"
#include "ozone/impl/ozone_display.h"

#include "ozone/impl/desktop_screen_wayland.h"
#include "ozone/impl/event_factory_wayland.h"
#include "ozone/wayland/display.h"
#include "ozone/wayland/dispatcher.h"
#include "ozone/wayland/screen.h"
#include "ozone/wayland/window.h"
#include "ozone/wayland/egl/loader.h"

#include "ozone/impl/ipc/child_process_observer.h"
#include "ozone/impl/ipc/display_channel.h"
#include "ozone/impl/ipc/display_channel_host.h"

#include "base/command_line.h"
#include "base/stl_util.h"
#include "content/public/common/content_switches.h"

#include <EGL/egl.h>

namespace ozonewayland {

OzoneDisplay* OzoneDisplay::instance_ = NULL;

OzoneDisplay* OzoneDisplay::GetInstance()
{
  return instance_;
}

OzoneDisplay::OzoneDisplay() : launch_type_(None),
    process_type_(PreLaunch),
    initialized_(false),
    state_(UnInitialized),
    desktop_screen_(NULL),
    dispatcher_(NULL),
    display_(NULL),
    child_process_observer_(NULL),
    channel_(NULL),
    host_(NULL),
    e_factory_(NULL),
    spec_(NULL),
    kMaxDisplaySize_(20)
{
  instance_ = this;
}

OzoneDisplay::~OzoneDisplay()
{
  Terminate();
  instance_ = NULL;
}

const char* OzoneDisplay::DefaultDisplaySpec() {
  if (spec_[0] == '\0')
    NOTREACHED() << "OutputHandleMode should come from Wayland compositor first";

  return spec_;
}

gfx::Screen* OzoneDisplay::CreateDesktopScreen() {
  if (!desktop_screen_) {
    desktop_screen_ = new DesktopScreenWayland;
    LookAheadOutputGeometry();
  }

  return desktop_screen_;
}

gfx::SurfaceFactoryOzone::HardwareState OzoneDisplay::InitializeHardware()
{
  if (state_ & Initialized)
    return initialized_state_;

  DCHECK(base::MessageLoop::current());
  base::MessageLoop::current()->AddDestructionObserver(this);

  state_ |= Initialized;
  ValidateLaunchType();
  bool gpuProcess = (launch_type_ & MultiProcess) && (process_type_ & Gpu);
  bool singleProcess = launch_type_ & SingleProcess;
  bool browserProcess = (launch_type_ & MultiProcess) && (process_type_ & Browser);

  if (singleProcess || gpuProcess) {
    display_ = new WaylandDisplay(WaylandDisplay::RegisterAsNeeded);
    initialized_state_ = display_->display() ? gfx::SurfaceFactoryOzone::INITIALIZED
                                             : gfx::SurfaceFactoryOzone::FAILED;
  }

  if (singleProcess || browserProcess) {
    dispatcher_ = new WaylandDispatcher();
    spec_ = new char[kMaxDisplaySize_];
    spec_[0] = '\0';
  }

  if (singleProcess) {
    e_factory_ = new EventFactoryWayland();
    EventFactoryWayland::SetInstance(e_factory_);
  } else if (gpuProcess) {
    int fd = display_->GetDisplayFd();
    dispatcher_ = new WaylandDispatcher(fd);
    channel_ = new OzoneDisplayChannel(fd);
    dispatcher_->PostTask(WaylandDispatcher::Poll);
  } else if (browserProcess) {
    child_process_observer_ = new OzoneProcessObserver(this);
    initialized_state_ = gfx::SurfaceFactoryOzone::INITIALIZED;
    host_ = new OzoneDisplayChannelHost();
  }

  if (initialized_state_ != gfx::SurfaceFactoryOzone::INITIALIZED)
    LOG(ERROR) << "OzoneDisplay failed to initialize hardware";

  return initialized_state_;
}

intptr_t OzoneDisplay::GetNativeDisplay()
{
  return (intptr_t)display_->display();
}

void OzoneDisplay::ShutdownHardware()
{
  Terminate();
}

gfx::AcceleratedWidget OzoneDisplay::GetAcceleratedWidget()
{
  static int opaque_handle = 0;
  // Ensure display is initialized.
  InitializeHardware();
  opaque_handle++;
  CreateWidget(opaque_handle);

  return (gfx::AcceleratedWidget)opaque_handle;
}

gfx::AcceleratedWidget OzoneDisplay::RealizeAcceleratedWidget(
    gfx::AcceleratedWidget w) {
  // TODO(kalyan) The channel connection should be established as soon as
  // GPU thread is initialized.
  if (!(state_ & ChannelConnected) && channel_) {
    channel_->Register();
    return gfx::kNullAcceleratedWidget;
  }

  WaylandWindow* widget = GetWidget(w);
  DCHECK(widget);
  widget->RealizeAcceleratedWidget();
  return (gfx::AcceleratedWidget)widget->egl_window();
}

bool OzoneDisplay::LoadEGLGLES2Bindings() {
  return InitializeGLBindings();
}

bool OzoneDisplay::AttemptToResizeAcceleratedWidget(gfx::AcceleratedWidget w,
                                                    const gfx::Rect& bounds) {
  if (host_) {
    host_->SendWidgetState(w, Resize, bounds.width(), bounds.height());
    return true;
  }

  WaylandWindow* window = GetWidget(w);
  // TODO(kalyan): Handle may be a opaque handle or a realized widget.
  // Fix this properly once resizing support is added.
  if (!window) {
    std::map<unsigned, WaylandWindow*>::const_iterator it;
    for (it = widget_map_.begin(); it != widget_map_.end(); ++it) {
      if (w == (gfx::AcceleratedWidget)it->second->egl_window()) {
        window = it->second;
        break;
      }
    }
  }

  DCHECK(window);

  return window->SetBounds(bounds);
}

gfx::VSyncProvider* OzoneDisplay::GetVSyncProvider(gfx::AcceleratedWidget w) {
  return 0;
}

bool OzoneDisplay::SchedulePageFlip(gfx::AcceleratedWidget w) {
  return true;
}

const int32* OzoneDisplay::GetEGLSurfaceProperties(const int32* desired_list)
{
  static const EGLint kConfigAttribs[] = {
    EGL_BUFFER_SIZE, 32,
    EGL_ALPHA_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_RED_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE
  };

  return kConfigAttribs;
}

void OzoneDisplay::WillDestroyCurrentMessageLoop()
{
  DCHECK(base::MessageLoop::current());
  dispatcher_->MessageLoopDestroyed();

  if (child_process_observer_)
    child_process_observer_->WillDestroyCurrentMessageLoop();

  if (e_factory_)
    e_factory_->WillDestroyCurrentMessageLoop();

  base::MessageLoop::current()->RemoveDestructionObserver(this);
}

void OzoneDisplay::SetWidgetState(gfx::AcceleratedWidget w,
                                  WidgetState state,
                                  unsigned width,
                                  unsigned height)
{
  if (host_)
    host_->SendWidgetState(w, state, width, height);
  else
    OnWidgetStateChanged(w, state, width, height);
}

void OzoneDisplay::OnWidgetStateChanged(gfx::AcceleratedWidget w,
                                       WidgetState state,
                                       unsigned width,
                                       unsigned height)
{
  switch (state) {
    case Create:
      CreateWidget(w);
    case FullScreen:
      NOTIMPLEMENTED();
      break;
    case Maximized:
    {
      WaylandWindow* widget = GetWidget(w);
      widget->Maximize();
      break;
    }
    case Minimized:
    {
      WaylandWindow* widget = GetWidget(w);
      widget->Minimize();
      break;
    }
    case Restore:
    {
      WaylandWindow* widget = GetWidget(w);
      widget->Restore();
      break;
    }
    case Active:
      NOTIMPLEMENTED();
      break;
    case InActive:
      NOTIMPLEMENTED();
      break;
    case Show:
      NOTIMPLEMENTED();
      break;
    case Hide:
      NOTIMPLEMENTED();
      break;
    case Resize:
      AttemptToResizeAcceleratedWidget(w, gfx::Rect(0,0,width, height));
      break;
    default:
      break;
  }
}

void OzoneDisplay::EstablishChannel()
{
  if (!host_)
    return;

  host_->EstablishChannel();
}

void OzoneDisplay::OnChannelEstablished(unsigned id)
{
  state_ |= ChannelConnected;
}

void OzoneDisplay::OnChannelClosed()
{
  state_ &= ~ChannelConnected;
  if (!host_)
    return;

  host_->ChannelClosed();
}

void OzoneDisplay::OnChannelHostDestroyed()
{
  state_ &= ~ChannelConnected;
  host_ = NULL;
}

void OzoneDisplay::OnOutputSizeChanged(WaylandScreen* screen,
                                       int width,
                                       int height)
{
  if (!(state_ & Initialized))
    return;

  if (screen != display_->PrimaryScreen()) {
    NOTIMPLEMENTED () << "Multiple screens are not implemented";
    return;
  }

  if (launch_type_ & SingleProcess)
    OnOutputSizeChanged(width, height);
  else if (channel_ && (state_ & ChannelConnected))
    dispatcher_->OutputSizeChanged(width, height);
}

void OzoneDisplay::OnOutputSizeChanged(unsigned width, unsigned height)
{
  if (spec_)
    base::snprintf(spec_, kMaxDisplaySize_, "%dx%d*2", width, height);
  if (desktop_screen_)
    desktop_screen_->SetGeometry(gfx::Rect(0, 0, width, height));
}

WaylandWindow* OzoneDisplay::CreateWidget(unsigned w)
{
  WaylandWindow* window = NULL;
  if (!host_) {
    window = new WaylandWindow(display_ ? WaylandWindow::TOPLEVEL
                                        : WaylandWindow::None);
    widget_map_[w] = window;
  } else
    host_->SendWidgetState(w, Create, 0, 0);

  return window;
}

WaylandWindow* OzoneDisplay::GetWidget(gfx::AcceleratedWidget w)
{
  std::map<unsigned, WaylandWindow*>::const_iterator it = widget_map_.find(w);
  return it == widget_map_.end() ? NULL : it->second;
}

void OzoneDisplay::Terminate()
{
  if (!(state_ & Initialized))
    return;

  state_ &= ~Initialized;
  if (spec_) {
    delete[] spec_;
    spec_ = NULL;
  }

  if (widget_map_.size()) {
    STLDeleteValues(&widget_map_);
    widget_map_.clear();
  }

  if (channel_) {
    delete channel_;
    channel_ = NULL;
  }

  if (child_process_observer_) {
    delete child_process_observer_;
    child_process_observer_ = NULL;
  }

  if (dispatcher_) {
    delete dispatcher_;
    dispatcher_ = NULL;
  }

  if (desktop_screen_) {
    delete desktop_screen_;
    desktop_screen_ = NULL;
  }

  if (display_) {
    delete display_;
    display_ = NULL;
  }
}

void OzoneDisplay::ValidateLaunchType()
{
  const CommandLine& command_line = *CommandLine::ForCurrentProcess();
  bool singleProcess = command_line.HasSwitch(switches::kSingleProcess) ||
      command_line.HasSwitch(switches::kInProcessGPU) ||
      command_line.HasSwitch(switches::kTestCompositor);

  if (singleProcess) {
    launch_type_ = SingleProcess;
    process_type_ = Browser;
    return;
  }

  // We are not using single process mode, check if we are in browser process.
  if (!command_line.HasSwitch(switches::kProcessType)) {
    launch_type_ = MultiProcess;
    process_type_ = Browser;
    return;
  }

  bool gpuProcess = command_line.HasSwitch(switches::kProcessType) &&
      (command_line.HasSwitch(switches::kGpuDeviceID) ||
       command_line.HasSwitch(switches::kGpuDriverVendor) ||
       command_line.HasSwitch(switches::kGpuDriverVersion));

  if (gpuProcess) {
    launch_type_ = MultiProcess;
    process_type_ = Gpu;
  }
}

// TODO(vignatti): GPU process conceptually is the one that deals with hardware
// details and therefore we assume that the window system connection should
// happen in there only. There's a glitch with Chrome though, that creates its
// frame contents requiring access to the window system, before the GPU process
// even exists. In other words, Chrome runs
// BrowserMainLoop::PreMainMessageLoopRun before GpuProcessHost::Get. If the
// assumption of window system connection belongs to the GPU process is valid,
// then I believe this Chrome behavior needs to be addressed upstream.
//
// For now, we create another window system connection to look ahead the needed
// output properties that Chrome (among others) need and then close right after
// that. I haven't measured how long it takes to open a Wayland connection,
// listen all the interface the compositor sends and close it, but _for_ _sure_
// it slows down the overall initialization time of Chromium targets.
// Therefore, this is something that has to be solved in the future, moving all
// Chrome tasks after GPU process is created.
//
void OzoneDisplay::LookAheadOutputGeometry() {
  DCHECK(desktop_screen_);
  WaylandDisplay disp_(WaylandDisplay::RegisterOutputOnly);
  DCHECK(disp_.display());

  while (disp_.PrimaryScreen()->Geometry().IsEmpty())
    disp_.SyncDisplay();

  desktop_screen_->SetGeometry(disp_.PrimaryScreen()->Geometry());
}

}  // namespace ozonewayland
