// Copyright (c) 2016 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/osr/osr_render_widget_host_view.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "base/callback_helpers.h"
#include "base/location.h"
#include "base/memory/ptr_util.h"
#include "base/single_thread_task_runner.h"
#include "base/time/time.h"
#include "components/viz/common/features.h"
#include "components/viz/common/frame_sinks/copy_output_request.h"
#include "components/viz/common/frame_sinks/delay_based_time_source.h"
#include "components/viz/common/gl_helper.h"
#include "components/viz/common/quads/render_pass.h"
#include "content/browser/renderer_host/compositor_resize_lock.h"
#include "content/browser/renderer_host/render_widget_host_delegate.h"
#include "content/browser/renderer_host/render_widget_host_impl.h"
#include "content/common/view_messages.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/context_factory.h"
#include "content/public/browser/render_process_host.h"
#include "media/base/video_frame.h"
#include "third_party/blink/public/platform/web_input_event.h"
#include "ui/compositor/compositor.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/layer_type.h"
#include "ui/events/base_event_utils.h"
#include "ui/events/event_constants.h"
#include "ui/gfx/geometry/dip_util.h"
#include "ui/gfx/native_widget_types.h"
#include "ui/gfx/skbitmap_operations.h"
#include "ui/latency/latency_info.h"

namespace atom {

namespace {

const float kDefaultScaleFactor = 1.0;
const int kFrameRetryLimit = 2;

ui::MouseEvent UiMouseEventFromWebMouseEvent(blink::WebMouseEvent event) {
  ui::EventType type = ui::EventType::ET_UNKNOWN;
  switch (event.GetType()) {
    case blink::WebInputEvent::kMouseDown:
      type = ui::EventType::ET_MOUSE_PRESSED;
      break;
    case blink::WebInputEvent::kMouseUp:
      type = ui::EventType::ET_MOUSE_RELEASED;
      break;
    case blink::WebInputEvent::kMouseMove:
      type = ui::EventType::ET_MOUSE_MOVED;
      break;
    case blink::WebInputEvent::kMouseEnter:
      type = ui::EventType::ET_MOUSE_ENTERED;
      break;
    case blink::WebInputEvent::kMouseLeave:
      type = ui::EventType::ET_MOUSE_EXITED;
      break;
    case blink::WebInputEvent::kMouseWheel:
      type = ui::EventType::ET_MOUSEWHEEL;
      break;
    default:
      type = ui::EventType::ET_UNKNOWN;
      break;
  }

  int button_flags = 0;
  switch (event.button) {
    case blink::WebMouseEvent::Button::kBack:
      button_flags |= ui::EventFlags::EF_BACK_MOUSE_BUTTON;
      break;
    case blink::WebMouseEvent::Button::kForward:
      button_flags |= ui::EventFlags::EF_FORWARD_MOUSE_BUTTON;
      break;
    case blink::WebMouseEvent::Button::kLeft:
      button_flags |= ui::EventFlags::EF_LEFT_MOUSE_BUTTON;
      break;
    case blink::WebMouseEvent::Button::kMiddle:
      button_flags |= ui::EventFlags::EF_MIDDLE_MOUSE_BUTTON;
      break;
    case blink::WebMouseEvent::Button::kRight:
      button_flags |= ui::EventFlags::EF_RIGHT_MOUSE_BUTTON;
      break;
    default:
      button_flags = 0;
      break;
  }

  ui::MouseEvent ui_event(type,
                          gfx::Point(std::floor(event.PositionInWidget().x),
                                     std::floor(event.PositionInWidget().y)),
                          gfx::Point(std::floor(event.PositionInWidget().x),
                                     std::floor(event.PositionInWidget().y)),
                          ui::EventTimeForNow(), button_flags, button_flags);
  ui_event.SetClickCount(event.click_count);

  return ui_event;
}

ui::MouseWheelEvent UiMouseWheelEventFromWebMouseEvent(
    blink::WebMouseWheelEvent event) {
  return ui::MouseWheelEvent(UiMouseEventFromWebMouseEvent(event),
                             std::floor(event.delta_x),
                             std::floor(event.delta_y));
}

}  // namespace

class AtomCopyFrameGenerator {
 public:
  AtomCopyFrameGenerator(OffScreenRenderWidgetHostView* view,
                         int frame_rate_threshold_us)
      : view_(view),
        frame_duration_(
            base::TimeDelta::FromMicroseconds(frame_rate_threshold_us)),
        weak_ptr_factory_(this) {
    last_time_ = base::Time::Now();
  }

  void GenerateCopyFrame(const gfx::Rect& damage_rect) {
    if (!view_->render_widget_host() || !view_->IsPainting())
      return;

    auto request = std::make_unique<viz::CopyOutputRequest>(
        viz::CopyOutputRequest::ResultFormat::RGBA_BITMAP,
        base::BindOnce(
            &AtomCopyFrameGenerator::CopyFromCompositingSurfaceHasResult,
            weak_ptr_factory_.GetWeakPtr(), damage_rect));

    request->set_area(gfx::Rect(view_->GetCompositorViewportPixelSize()));
    view_->GetRootLayer()->RequestCopyOfOutput(std::move(request));
  }

  void set_frame_rate_threshold_us(int frame_rate_threshold_us) {
    frame_duration_ =
        base::TimeDelta::FromMicroseconds(frame_rate_threshold_us);
  }

 private:
  void CopyFromCompositingSurfaceHasResult(
      const gfx::Rect& damage_rect,
      std::unique_ptr<viz::CopyOutputResult> result) {
    if (result->IsEmpty() || result->size().IsEmpty() ||
        !view_->render_widget_host()) {
      OnCopyFrameCaptureFailure(damage_rect);
      return;
    }

    DCHECK(!result->IsEmpty());
    auto source = std::make_unique<SkBitmap>(result->AsSkBitmap());
    DCHECK(source->readyToDraw());
    if (source) {
      base::AutoLock autolock(lock_);
      std::shared_ptr<SkBitmap> bitmap(source.release());

      base::TimeTicks now = base::TimeTicks::Now();
      base::TimeDelta next_frame_in = next_frame_time_ - now;
      if (next_frame_in > frame_duration_ / 4) {
        next_frame_time_ += frame_duration_;
        content::BrowserThread::PostDelayedTask(
            content::BrowserThread::UI, FROM_HERE,
            base::BindOnce(&AtomCopyFrameGenerator::OnCopyFrameCaptureSuccess,
                           weak_ptr_factory_.GetWeakPtr(), damage_rect, bitmap),
            next_frame_in);
      } else {
        next_frame_time_ = now + frame_duration_;
        OnCopyFrameCaptureSuccess(damage_rect, bitmap);
      }

      frame_retry_count_ = 0;
    } else {
      OnCopyFrameCaptureFailure(damage_rect);
    }
  }

  void OnCopyFrameCaptureFailure(const gfx::Rect& damage_rect) {
    const bool force_frame = (++frame_retry_count_ <= kFrameRetryLimit);
    if (force_frame) {
      // Retry with the same |damage_rect|.
      content::BrowserThread::PostTask(
          content::BrowserThread::UI, FROM_HERE,
          base::BindOnce(&AtomCopyFrameGenerator::GenerateCopyFrame,
                         weak_ptr_factory_.GetWeakPtr(), damage_rect));
    }
  }

  void OnCopyFrameCaptureSuccess(const gfx::Rect& damage_rect,
                                 const std::shared_ptr<SkBitmap>& bitmap) {
    base::AutoLock lock(onPaintLock_);
    view_->OnPaint(damage_rect, *bitmap);
  }

  base::Lock lock_;
  base::Lock onPaintLock_;
  OffScreenRenderWidgetHostView* view_;

  base::Time last_time_;

  int frame_retry_count_ = 0;
  base::TimeTicks next_frame_time_ = base::TimeTicks::Now();
  base::TimeDelta frame_duration_;

  base::WeakPtrFactory<AtomCopyFrameGenerator> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(AtomCopyFrameGenerator);
};

class AtomBeginFrameTimer : public viz::DelayBasedTimeSourceClient {
 public:
  AtomBeginFrameTimer(int frame_rate_threshold_us,
                      const base::Closure& callback)
      : callback_(callback) {
    time_source_.reset(new viz::DelayBasedTimeSource(
        content::BrowserThread::GetTaskRunnerForThread(
            content::BrowserThread::UI)
            .get()));
    time_source_->SetTimebaseAndInterval(
        base::TimeTicks(),
        base::TimeDelta::FromMicroseconds(frame_rate_threshold_us));
    time_source_->SetClient(this);
  }

  void SetActive(bool active) { time_source_->SetActive(active); }

  bool IsActive() const { return time_source_->Active(); }

  void SetFrameRateThresholdUs(int frame_rate_threshold_us) {
    time_source_->SetTimebaseAndInterval(
        base::TimeTicks::Now(),
        base::TimeDelta::FromMicroseconds(frame_rate_threshold_us));
  }

 private:
  void OnTimerTick() override { callback_.Run(); }

  const base::Closure callback_;
  std::unique_ptr<viz::DelayBasedTimeSource> time_source_;

  DISALLOW_COPY_AND_ASSIGN(AtomBeginFrameTimer);
};

OffScreenRenderWidgetHostView::OffScreenRenderWidgetHostView(
    bool transparent,
    bool painting,
    int frame_rate,
    const OnPaintCallback& callback,
    content::RenderWidgetHost* host,
    OffScreenRenderWidgetHostView* parent_host_view,
    NativeWindow* native_window)
    : content::RenderWidgetHostViewBase(host),
      render_widget_host_(content::RenderWidgetHostImpl::From(host)),
      parent_host_view_(parent_host_view),
      native_window_(native_window),
      transparent_(transparent),
      callback_(callback),
      frame_rate_(frame_rate),
      scale_factor_(kDefaultScaleFactor),
      size_(native_window->GetSize()),
      painting_(painting),
      is_showing_(!render_widget_host_->is_hidden()),
      mouse_wheel_phase_handler_(this),
      weak_ptr_factory_(this) {
  DCHECK(render_widget_host_);
  bool is_guest_view_hack = parent_host_view_ != nullptr;
#if !defined(OS_MACOSX)
  delegated_frame_host_ = std::make_unique<content::DelegatedFrameHost>(
      AllocateFrameSinkId(is_guest_view_hack), this,
      features::IsSurfaceSynchronizationEnabled(),
      base::FeatureList::IsEnabled(features::kVizDisplayCompositor),
      true /* should_register_frame_sink_id */);

  root_layer_.reset(new ui::Layer(ui::LAYER_SOLID_COLOR));
#endif

  local_surface_id_ = local_surface_id_allocator_.GenerateId();

#if defined(OS_MACOSX)
  last_frame_root_background_color_ = SK_ColorTRANSPARENT;
  CreatePlatformWidget(is_guest_view_hack);
#endif

  bool opaque = SkColorGetA(background_color()) == SK_AlphaOPAQUE;
  GetRootLayer()->SetFillsBoundsOpaquely(opaque);
  GetRootLayer()->SetColor(background_color());

#if !defined(OS_MACOSX)
  // On macOS the ui::Compositor is created/owned by the platform view.
  content::ImageTransportFactory* factory =
      content::ImageTransportFactory::GetInstance();
  ui::ContextFactoryPrivate* context_factory_private =
      factory->GetContextFactoryPrivate();
  compositor_.reset(
      new ui::Compositor(context_factory_private->AllocateFrameSinkId(),
                         content::GetContextFactory(), context_factory_private,
                         base::ThreadTaskRunnerHandle::Get(),
                         features::IsSurfaceSynchronizationEnabled(),
                         false /* enable_pixel_canvas */));
  compositor_->SetAcceleratedWidget(gfx::kNullAcceleratedWidget);
  compositor_->SetRootLayer(root_layer_.get());
#endif
  GetCompositor()->SetDelegate(this);

  native_window_->AddObserver(this);

  ResizeRootLayer(false);
  render_widget_host_->SetView(this);
  InstallTransparency();
}

OffScreenRenderWidgetHostView::~OffScreenRenderWidgetHostView() {
  if (native_window_)
    native_window_->RemoveObserver(this);

#if defined(OS_MACOSX)
  if (is_showing_)
    browser_compositor_->SetRenderWidgetHostIsHidden(true);
#else
  // Marking the DelegatedFrameHost as removed from the window hierarchy is
  // necessary to remove all connections to its old ui::Compositor.
  if (is_showing_)
    delegated_frame_host_->WasHidden();
  delegated_frame_host_->ResetCompositor();
#endif

  if (copy_frame_generator_.get())
    copy_frame_generator_.reset(NULL);

#if defined(OS_MACOSX)
  DestroyPlatformWidget();
#else
  delegated_frame_host_.reset(NULL);
  compositor_.reset(NULL);
  root_layer_.reset(NULL);
#endif
}

void OffScreenRenderWidgetHostView::OnWindowResize() {
  // In offscreen mode call RenderWidgetHostView's SetSize explicitly
  auto size = native_window_->GetSize();
  SetSize(size);
}

void OffScreenRenderWidgetHostView::OnWindowClosed() {
  native_window_->RemoveObserver(this);
  native_window_ = nullptr;
}

void OffScreenRenderWidgetHostView::OnBeginFrameTimerTick() {
  const base::TimeTicks frame_time = base::TimeTicks::Now();
  const base::TimeDelta vsync_period =
      base::TimeDelta::FromMicroseconds(frame_rate_threshold_us_);
  SendBeginFrame(frame_time, vsync_period);
}

void OffScreenRenderWidgetHostView::SendBeginFrame(
    base::TimeTicks frame_time,
    base::TimeDelta vsync_period) {
  base::TimeTicks display_time = frame_time + vsync_period;

  base::TimeDelta estimated_browser_composite_time =
      base::TimeDelta::FromMicroseconds(
          (1.0f * base::Time::kMicrosecondsPerSecond) / (3.0f * 60));

  base::TimeTicks deadline = display_time - estimated_browser_composite_time;

  const viz::BeginFrameArgs& begin_frame_args = viz::BeginFrameArgs::Create(
      BEGINFRAME_FROM_HERE, begin_frame_source_.source_id(),
      begin_frame_number_, frame_time, deadline, vsync_period,
      viz::BeginFrameArgs::NORMAL);
  DCHECK(begin_frame_args.IsValid());
  begin_frame_number_++;

  if (renderer_compositor_frame_sink_)
    renderer_compositor_frame_sink_->OnBeginFrame(begin_frame_args);
}

void OffScreenRenderWidgetHostView::InitAsChild(gfx::NativeView) {
  DCHECK(parent_host_view_);

  if (parent_host_view_->child_host_view_) {
    parent_host_view_->child_host_view_->CancelWidget();
  }

  parent_host_view_->set_child_host_view(this);
  parent_host_view_->Hide();

  ResizeRootLayer(false);
  Show();
}

void OffScreenRenderWidgetHostView::SetSize(const gfx::Size& size) {
  size_ = size;
  WasResized();
}

void OffScreenRenderWidgetHostView::SetBounds(const gfx::Rect& new_bounds) {
  SetSize(new_bounds.size());
}

gfx::NativeView OffScreenRenderWidgetHostView::GetNativeView() const {
  return gfx::NativeView();
}

gfx::NativeViewAccessible
OffScreenRenderWidgetHostView::GetNativeViewAccessible() {
  return gfx::NativeViewAccessible();
}

ui::TextInputClient* OffScreenRenderWidgetHostView::GetTextInputClient() {
  return nullptr;
}

void OffScreenRenderWidgetHostView::Focus() {}

bool OffScreenRenderWidgetHostView::HasFocus() const {
  return false;
}

bool OffScreenRenderWidgetHostView::IsSurfaceAvailableForCopy() const {
  return GetDelegatedFrameHost()->CanCopyFromCompositingSurface();
}

void OffScreenRenderWidgetHostView::Show() {
  if (is_showing_)
    return;

  is_showing_ = true;

#if defined(OS_MACOSX)
  browser_compositor_->SetRenderWidgetHostIsHidden(false);
#else
  delegated_frame_host_->SetCompositor(compositor_.get());
  delegated_frame_host_->WasShown(
      GetLocalSurfaceId(), GetRootLayer()->bounds().size(), ui::LatencyInfo());
#endif

  if (render_widget_host_)
    render_widget_host_->WasShown(ui::LatencyInfo());
}

void OffScreenRenderWidgetHostView::Hide() {
  if (!is_showing_)
    return;

  if (render_widget_host_)
    render_widget_host_->WasHidden();

#if defined(OS_MACOSX)
  browser_compositor_->SetRenderWidgetHostIsHidden(true);
#else
  GetDelegatedFrameHost()->WasHidden();
  GetDelegatedFrameHost()->ResetCompositor();
#endif

  is_showing_ = false;
}

bool OffScreenRenderWidgetHostView::IsShowing() {
  return is_showing_;
}

gfx::Rect OffScreenRenderWidgetHostView::GetViewBounds() const {
  if (IsPopupWidget())
    return popup_position_;

  return gfx::Rect(size_);
}

void OffScreenRenderWidgetHostView::SetBackgroundColor(SkColor color) {
  // The renderer will feed its color back to us with the first CompositorFrame.
  // We short-cut here to show a sensible color before that happens.
  UpdateBackgroundColorFromRenderer(color);

  if (render_widget_host_) {
    render_widget_host_->SetBackgroundOpaque(SkColorGetA(color) ==
                                             SK_AlphaOPAQUE);
  }
}

SkColor OffScreenRenderWidgetHostView::background_color() const {
  return background_color_;
}

gfx::Size OffScreenRenderWidgetHostView::GetVisibleViewportSize() const {
  return size_;
}

void OffScreenRenderWidgetHostView::SetInsets(const gfx::Insets& insets) {}

bool OffScreenRenderWidgetHostView::LockMouse() {
  return false;
}

void OffScreenRenderWidgetHostView::UnlockMouse() {}

void OffScreenRenderWidgetHostView::TakeFallbackContentFrom(
    content::RenderWidgetHostView* view) {
  DCHECK(!static_cast<content::RenderWidgetHostViewBase*>(view)
              ->IsRenderWidgetHostViewChildFrame());
  DCHECK(!static_cast<content::RenderWidgetHostViewBase*>(view)
              ->IsRenderWidgetHostViewGuest());
  OffScreenRenderWidgetHostView* view_osr =
      static_cast<OffScreenRenderWidgetHostView*>(view);
  SetBackgroundColor(view_osr->background_color());
  if (GetDelegatedFrameHost() && view_osr->GetDelegatedFrameHost()) {
    GetDelegatedFrameHost()->TakeFallbackContentFrom(
        view_osr->GetDelegatedFrameHost());
  }
  host()->GetContentRenderingTimeoutFrom(view_osr->host());
}

void OffScreenRenderWidgetHostView::DidCreateNewRendererCompositorFrameSink(
    viz::mojom::CompositorFrameSinkClient* renderer_compositor_frame_sink) {
  renderer_compositor_frame_sink_ = renderer_compositor_frame_sink;
  if (GetDelegatedFrameHost()) {
    GetDelegatedFrameHost()->DidCreateNewRendererCompositorFrameSink(
        renderer_compositor_frame_sink_);
  }
}

void OffScreenRenderWidgetHostView::SubmitCompositorFrame(
    const viz::LocalSurfaceId& local_surface_id,
    viz::CompositorFrame frame,
    viz::mojom::HitTestRegionListPtr hit_test_region_list) {
  TRACE_EVENT0("electron",
               "OffScreenRenderWidgetHostView::SubmitCompositorFrame");

#if defined(OS_MACOSX)
  last_frame_root_background_color_ = frame.metadata.root_background_color;
#endif

  if (frame.metadata.root_scroll_offset != last_scroll_offset_) {
    last_scroll_offset_ = frame.metadata.root_scroll_offset;
  }

  if (!frame.render_pass_list.empty()) {
    if (software_output_device_) {
      if (!begin_frame_timer_.get() || IsPopupWidget()) {
        software_output_device_->SetActive(painting_, false);
      }

      // The compositor will draw directly to the SoftwareOutputDevice which
      // then calls OnPaint.
      // We would normally call BrowserCompositorMac::SubmitCompositorFrame on
      // macOS, however it contains compositor resize logic that we don't want.
      // Consequently we instead call the SubmitCompositorFrame method directly.
      GetDelegatedFrameHost()->SubmitCompositorFrame(
          local_surface_id, std::move(frame), std::move(hit_test_region_list));
    } else {
      if (!copy_frame_generator_.get()) {
        copy_frame_generator_.reset(
            new AtomCopyFrameGenerator(this, frame_rate_threshold_us_));
      }

      // Determine the damage rectangle for the current frame. This is the same
      // calculation that SwapDelegatedFrame uses.
      viz::RenderPass* root_pass = frame.render_pass_list.back().get();
      gfx::Size frame_size = root_pass->output_rect.size();
      gfx::Rect damage_rect =
          gfx::ToEnclosingRect(gfx::RectF(root_pass->damage_rect));
      damage_rect.Intersect(gfx::Rect(frame_size));

      // We would normally call BrowserCompositorMac::SubmitCompositorFrame on
      // macOS, however it contains compositor resize logic that we don't want.
      // Consequently we instead call the SubmitCompositorFrame method directly.
      GetDelegatedFrameHost()->SubmitCompositorFrame(
          local_surface_id, std::move(frame), std::move(hit_test_region_list));

      // Request a copy of the last compositor frame which will eventually call
      // OnPaint asynchronously.
      copy_frame_generator_->GenerateCopyFrame(damage_rect);
    }
  }
}

void OffScreenRenderWidgetHostView::ClearCompositorFrame() {
  GetDelegatedFrameHost()->ClearDelegatedFrame();
}

void OffScreenRenderWidgetHostView::InitAsPopup(
    content::RenderWidgetHostView* parent_host_view,
    const gfx::Rect& pos) {
  DCHECK_EQ(parent_host_view_, parent_host_view);

  if (parent_host_view_->popup_host_view_) {
    parent_host_view_->popup_host_view_->CancelWidget();
  }

  parent_host_view_->set_popup_host_view(this);
  parent_host_view_->popup_bitmap_.reset(new SkBitmap);
  parent_callback_ =
      base::Bind(&OffScreenRenderWidgetHostView::OnPopupPaint,
                 parent_host_view_->weak_ptr_factory_.GetWeakPtr());

  popup_position_ = pos;

  ResizeRootLayer(false);
  Show();
}

void OffScreenRenderWidgetHostView::InitAsFullscreen(
    content::RenderWidgetHostView*) {}

void OffScreenRenderWidgetHostView::UpdateCursor(const content::WebCursor&) {}

void OffScreenRenderWidgetHostView::SetIsLoading(bool loading) {}

void OffScreenRenderWidgetHostView::TextInputStateChanged(
    const content::TextInputState& params) {}

void OffScreenRenderWidgetHostView::ImeCancelComposition() {}

void OffScreenRenderWidgetHostView::RenderProcessGone(base::TerminationStatus,
                                                      int) {
  Destroy();
}

void OffScreenRenderWidgetHostView::Destroy() {
  if (!is_destroyed_) {
    is_destroyed_ = true;

    if (parent_host_view_ != NULL) {
      CancelWidget();
    } else {
      if (popup_host_view_)
        popup_host_view_->CancelWidget();
      popup_bitmap_.reset();
      if (child_host_view_)
        child_host_view_->CancelWidget();
      if (!guest_host_views_.empty()) {
        // Guest RWHVs will be destroyed when the associated RWHVGuest is
        // destroyed. This parent RWHV may be destroyed first, so disassociate
        // the guest RWHVs here without destroying them.
        for (auto* guest_host_view : guest_host_views_)
          guest_host_view->parent_host_view_ = nullptr;
        guest_host_views_.clear();
      }
      for (auto* proxy_view : proxy_views_)
        proxy_view->RemoveObserver();
      Hide();
    }
  }

  delete this;
}

void OffScreenRenderWidgetHostView::SetTooltipText(const base::string16&) {}

void OffScreenRenderWidgetHostView::SelectionBoundsChanged(
    const ViewHostMsg_SelectionBounds_Params&) {}

void OffScreenRenderWidgetHostView::CopyFromSurface(
    const gfx::Rect& src_rect,
    const gfx::Size& output_size,
    base::OnceCallback<void(const SkBitmap&)> callback) {
  GetDelegatedFrameHost()->CopyFromCompositingSurface(src_rect, output_size,
                                                      std::move(callback));
}

void OffScreenRenderWidgetHostView::GetScreenInfo(
    content::ScreenInfo* screen_info) const {
  screen_info->depth = 24;
  screen_info->depth_per_component = 8;
  screen_info->orientation_angle = 0;
  screen_info->device_scale_factor = 1.0;
  screen_info->orientation_type =
      content::SCREEN_ORIENTATION_VALUES_LANDSCAPE_PRIMARY;
  screen_info->rect = gfx::Rect(size_);
  screen_info->available_rect = gfx::Rect(size_);
}

void OffScreenRenderWidgetHostView::InitAsGuest(
    content::RenderWidgetHostView* parent_host_view,
    content::RenderWidgetHostViewGuest* guest_view) {
  parent_host_view_->AddGuestHostView(this);
  parent_host_view_->RegisterGuestViewFrameSwappedCallback(guest_view);
}

gfx::Vector2d OffScreenRenderWidgetHostView::GetOffsetFromRootSurface() {
  return gfx::Vector2d();
}

gfx::Rect OffScreenRenderWidgetHostView::GetBoundsInRootWindow() {
  return gfx::Rect(size_);
}

viz::SurfaceId OffScreenRenderWidgetHostView::GetCurrentSurfaceId() const {
  return GetDelegatedFrameHost()
             ? GetDelegatedFrameHost()->GetCurrentSurfaceId()
             : viz::SurfaceId();
}

void OffScreenRenderWidgetHostView::ImeCompositionRangeChanged(
    const gfx::Range&,
    const std::vector<gfx::Rect>&) {}

gfx::Size OffScreenRenderWidgetHostView::GetCompositorViewportPixelSize()
    const {
  return gfx::ScaleToCeiledSize(GetRequestedRendererSize(), scale_factor_);
}

gfx::Size OffScreenRenderWidgetHostView::GetRequestedRendererSize() const {
  return GetDelegatedFrameHost()->GetRequestedRendererSize();
}

content::RenderWidgetHostViewBase*
OffScreenRenderWidgetHostView::CreateViewForWidget(
    content::RenderWidgetHost* render_widget_host,
    content::RenderWidgetHost* embedder_render_widget_host,
    content::WebContentsView* web_contents_view) {
  if (render_widget_host->GetView()) {
    return static_cast<content::RenderWidgetHostViewBase*>(
        render_widget_host->GetView());
  }

  OffScreenRenderWidgetHostView* embedder_host_view = nullptr;
  if (embedder_render_widget_host) {
    embedder_host_view = static_cast<OffScreenRenderWidgetHostView*>(
        embedder_render_widget_host->GetView());
  }

  return new OffScreenRenderWidgetHostView(
      transparent_, true, embedder_host_view->GetFrameRate(), callback_,
      render_widget_host, embedder_host_view, native_window_);
}

#if !defined(OS_MACOSX)
ui::Layer* OffScreenRenderWidgetHostView::DelegatedFrameHostGetLayer() const {
  return const_cast<ui::Layer*>(root_layer_.get());
}

bool OffScreenRenderWidgetHostView::DelegatedFrameHostIsVisible() const {
  return !render_widget_host_->is_hidden();
}

SkColor OffScreenRenderWidgetHostView::DelegatedFrameHostGetGutterColor()
    const {
  if (render_widget_host_->delegate() &&
      render_widget_host_->delegate()->IsFullscreenForCurrentTab()) {
    return SK_ColorWHITE;
  }
  return background_color_;
}

bool OffScreenRenderWidgetHostView::DelegatedFrameCanCreateResizeLock() const {
  return !render_widget_host_->auto_resize_enabled();
}

std::unique_ptr<content::CompositorResizeLock>
OffScreenRenderWidgetHostView::DelegatedFrameHostCreateResizeLock() {
  HoldResize();
  const gfx::Size& desired_size = GetRootLayer()->bounds().size();
  return std::make_unique<content::CompositorResizeLock>(this, desired_size);
}

void OffScreenRenderWidgetHostView::OnFirstSurfaceActivation(
    const viz::SurfaceInfo& surface_info) {}

void OffScreenRenderWidgetHostView::OnBeginFrame(base::TimeTicks frame_time) {}

void OffScreenRenderWidgetHostView::OnFrameTokenChanged(uint32_t frame_token) {
  render_widget_host_->DidProcessFrame(frame_token);
}

void OffScreenRenderWidgetHostView::DidReceiveFirstFrameAfterNavigation() {
  render_widget_host_->DidReceiveFirstFrameAfterNavigation();
}

std::unique_ptr<ui::CompositorLock>
OffScreenRenderWidgetHostView::GetCompositorLock(
    ui::CompositorLockClient* client) {
  return GetCompositor()->GetCompositorLock(client);
}

void OffScreenRenderWidgetHostView::CompositorResizeLockEnded() {
  ReleaseResize();
}

bool OffScreenRenderWidgetHostView::IsAutoResizeEnabled() const {
  return render_widget_host_->auto_resize_enabled();
}

viz::LocalSurfaceId OffScreenRenderWidgetHostView::GetLocalSurfaceId() const {
  return local_surface_id_;
}

#endif  // !defined(OS_MACOSX)

viz::FrameSinkId OffScreenRenderWidgetHostView::GetFrameSinkId() {
  return GetDelegatedFrameHost()->frame_sink_id();
}

void OffScreenRenderWidgetHostView::DidNavigate() {
  ResizeRootLayer(true);
#if defined(OS_MACOSX)
  browser_compositor_->DidNavigate();
#else
  if (delegated_frame_host_)
    delegated_frame_host_->DidNavigate();
#endif
}

bool OffScreenRenderWidgetHostView::TransformPointToLocalCoordSpace(
    const gfx::PointF& point,
    const viz::SurfaceId& original_surface,
    gfx::PointF* transformed_point) {
  // Transformations use physical pixels rather than DIP, so conversion
  // is necessary.
  gfx::PointF point_in_pixels = gfx::ConvertPointToPixel(scale_factor_, point);
  if (!GetDelegatedFrameHost()->TransformPointToLocalCoordSpace(
          point_in_pixels, original_surface, transformed_point)) {
    return false;
  }

  *transformed_point =
      gfx::ConvertPointToDIP(scale_factor_, *transformed_point);
  return true;
}

bool OffScreenRenderWidgetHostView::TransformPointToCoordSpaceForView(
    const gfx::PointF& point,
    RenderWidgetHostViewBase* target_view,
    gfx::PointF* transformed_point) {
  if (target_view == this) {
    *transformed_point = point;
    return true;
  }

  // In TransformPointToLocalCoordSpace() there is a Point-to-Pixel conversion,
  // but it is not necessary here because the final target view is responsible
  // for converting before computing the final transform.
  return GetDelegatedFrameHost()->TransformPointToCoordSpaceForView(
      point, target_view, transformed_point);
}

void OffScreenRenderWidgetHostView::CancelWidget() {
  if (render_widget_host_)
    render_widget_host_->LostCapture();
  Hide();

  if (parent_host_view_) {
    if (parent_host_view_->popup_host_view_ == this) {
      parent_host_view_->set_popup_host_view(NULL);
      parent_host_view_->popup_bitmap_.reset();
    } else if (parent_host_view_->child_host_view_ == this) {
      parent_host_view_->set_child_host_view(NULL);
      parent_host_view_->Show();
    } else {
      parent_host_view_->RemoveGuestHostView(this);
    }
    parent_host_view_ = NULL;
  }

  if (render_widget_host_ && !is_destroyed_) {
    is_destroyed_ = true;
    // Results in a call to Destroy().
    render_widget_host_->ShutdownAndDestroyWidget(true);
  }
}

void OffScreenRenderWidgetHostView::AddGuestHostView(
    OffScreenRenderWidgetHostView* guest_host) {
  guest_host_views_.insert(guest_host);
}

void OffScreenRenderWidgetHostView::RemoveGuestHostView(
    OffScreenRenderWidgetHostView* guest_host) {
  guest_host_views_.erase(guest_host);
}

void OffScreenRenderWidgetHostView::AddViewProxy(OffscreenViewProxy* proxy) {
  proxy->SetObserver(this);
  proxy_views_.insert(proxy);
}

void OffScreenRenderWidgetHostView::RemoveViewProxy(OffscreenViewProxy* proxy) {
  proxy->RemoveObserver();
  proxy_views_.erase(proxy);
}

void OffScreenRenderWidgetHostView::ProxyViewDestroyed(
    OffscreenViewProxy* proxy) {
  proxy_views_.erase(proxy);
  Invalidate();
}

void OffScreenRenderWidgetHostView::RegisterGuestViewFrameSwappedCallback(
    content::RenderWidgetHostViewGuest* guest_host_view) {
  guest_host_view->RegisterFrameSwappedCallback(base::BindOnce(
      &OffScreenRenderWidgetHostView::OnGuestViewFrameSwapped,
      weak_ptr_factory_.GetWeakPtr(), base::Unretained(guest_host_view)));
}

void OffScreenRenderWidgetHostView::OnGuestViewFrameSwapped(
    content::RenderWidgetHostViewGuest* guest_host_view) {
  InvalidateBounds(
      gfx::ConvertRectToPixel(scale_factor_, guest_host_view->GetViewBounds()));

  RegisterGuestViewFrameSwappedCallback(guest_host_view);
}

std::unique_ptr<viz::SoftwareOutputDevice>
OffScreenRenderWidgetHostView::CreateSoftwareOutputDevice(
    ui::Compositor* compositor) {
  DCHECK_EQ(GetCompositor(), compositor);
  DCHECK(!copy_frame_generator_);
  DCHECK(!software_output_device_);

  ResizeRootLayer(false);

  software_output_device_ = new OffScreenOutputDevice(
      transparent_, base::Bind(&OffScreenRenderWidgetHostView::OnPaint,
                               weak_ptr_factory_.GetWeakPtr()));
  return base::WrapUnique(software_output_device_);
}

bool OffScreenRenderWidgetHostView::InstallTransparency() {
  if (transparent_) {
    SetBackgroundColor(SkColor());
#if defined(OS_MACOSX)
    browser_compositor_->SetBackgroundColor(SK_ColorTRANSPARENT);
#else
    compositor_->SetBackgroundColor(SK_ColorTRANSPARENT);
#endif
    return true;
  }
  return false;
}

void OffScreenRenderWidgetHostView::SetNeedsBeginFrames(
    bool needs_begin_frames) {
  SetupFrameRate(true);

  begin_frame_timer_->SetActive(needs_begin_frames);

  if (software_output_device_) {
    software_output_device_->SetActive(needs_begin_frames && painting_, false);
  }
}

void OffScreenRenderWidgetHostView::SetWantsAnimateOnlyBeginFrames() {
  if (GetDelegatedFrameHost()) {
    GetDelegatedFrameHost()->SetWantsAnimateOnlyBeginFrames();
  }
}

void CopyBitmapTo(const SkBitmap& destination,
                  const SkBitmap& source,
                  const gfx::Rect& pos) {
  char* src = static_cast<char*>(source.getPixels());
  char* dest = static_cast<char*>(destination.getPixels());
  int pixelsize = source.bytesPerPixel();

  int width =
      pos.x() + pos.width() <= destination.width()
          ? pos.width()
          : pos.width() - ((pos.x() + pos.width()) - destination.width());
  int height =
      pos.y() + pos.height() <= destination.height()
          ? pos.height()
          : pos.height() - ((pos.y() + pos.height()) - destination.height());

  if (width > 0 && height > 0) {
    for (int i = 0; i < height; i++) {
      memcpy(dest + ((pos.y() + i) * destination.width() + pos.x()) * pixelsize,
             src + (i * source.width()) * pixelsize, width * pixelsize);
    }
  }

  destination.notifyPixelsChanged();
}

void OffScreenRenderWidgetHostView::OnPaint(const gfx::Rect& damage_rect,
                                            const SkBitmap& bitmap) {
  TRACE_EVENT0("electron", "OffScreenRenderWidgetHostView::OnPaint");

  HoldResize();

  if (parent_callback_) {
    parent_callback_.Run(damage_rect, bitmap);
  } else {
    gfx::Rect damage(damage_rect);

    std::vector<gfx::Rect> damages;
    std::vector<const SkBitmap*> bitmaps;
    std::vector<SkBitmap> originals;

    if (popup_host_view_ && popup_bitmap_.get()) {
      gfx::Rect pos = popup_host_view_->popup_position_;
      damage.Union(pos);
      damages.push_back(pos);
      bitmaps.push_back(popup_bitmap_.get());
      originals.push_back(SkBitmapOperations::CreateTiledBitmap(
          bitmap, pos.x(), pos.y(), pos.width(), pos.height()));
    }

    for (auto* proxy_view : proxy_views_) {
      gfx::Rect pos = proxy_view->GetBounds();
      damage.Union(pos);
      damages.push_back(pos);
      bitmaps.push_back(proxy_view->GetBitmap());
      originals.push_back(SkBitmapOperations::CreateTiledBitmap(
          bitmap, pos.x(), pos.y(), pos.width(), pos.height()));
    }

    for (size_t i = 0; i < damages.size(); i++) {
      CopyBitmapTo(bitmap, *(bitmaps[i]), damages[i]);
    }

    damage.Intersect(GetViewBounds());
    paint_callback_running_ = true;
    callback_.Run(damage, bitmap);
    paint_callback_running_ = false;

    for (size_t i = 0; i < damages.size(); i++) {
      CopyBitmapTo(bitmap, originals[i], damages[i]);
    }
  }

  ReleaseResize();
}

void OffScreenRenderWidgetHostView::OnPopupPaint(const gfx::Rect& damage_rect,
                                                 const SkBitmap& bitmap) {
  if (popup_host_view_ && popup_bitmap_.get())
    popup_bitmap_.reset(new SkBitmap(bitmap));
  InvalidateBounds(popup_host_view_->popup_position_);
}

void OffScreenRenderWidgetHostView::OnProxyViewPaint(
    const gfx::Rect& damage_rect) {
  InvalidateBounds(damage_rect);
}

void OffScreenRenderWidgetHostView::HoldResize() {
  if (!hold_resize_)
    hold_resize_ = true;
}

void OffScreenRenderWidgetHostView::ReleaseResize() {
  if (!hold_resize_)
    return;

  hold_resize_ = false;
  if (pending_resize_) {
    pending_resize_ = false;
    content::BrowserThread::PostTask(
        content::BrowserThread::UI, FROM_HERE,
        base::BindOnce(&OffScreenRenderWidgetHostView::WasResized,
                       weak_ptr_factory_.GetWeakPtr()));
  }
}

void OffScreenRenderWidgetHostView::WasResized() {
  if (hold_resize_) {
    if (!pending_resize_)
      pending_resize_ = true;
    return;
  }

  ResizeRootLayer(false);
  if (render_widget_host_)
    render_widget_host_->WasResized();
  GetDelegatedFrameHost()->WasResized(local_surface_id_, size_,
                                      cc::DeadlinePolicy::UseDefaultDeadline());
}

void OffScreenRenderWidgetHostView::SendMouseEvent(
    const blink::WebMouseEvent& event) {
  for (auto* proxy_view : proxy_views_) {
    gfx::Rect bounds = proxy_view->GetBounds();
    if (bounds.Contains(event.PositionInWidget().x,
                        event.PositionInWidget().y)) {
      blink::WebMouseEvent proxy_event(event);
      proxy_event.SetPositionInWidget(
          proxy_event.PositionInWidget().x - bounds.x(),
          proxy_event.PositionInWidget().y - bounds.y());

      ui::MouseEvent ui_event = UiMouseEventFromWebMouseEvent(proxy_event);
      proxy_view->OnEvent(&ui_event);
      return;
    }
  }

  if (!IsPopupWidget()) {
    if (popup_host_view_ &&
        popup_host_view_->popup_position_.Contains(
            event.PositionInWidget().x, event.PositionInWidget().y)) {
      blink::WebMouseEvent popup_event(event);
      popup_event.SetPositionInWidget(
          popup_event.PositionInWidget().x -
              popup_host_view_->popup_position_.x(),
          popup_event.PositionInWidget().y -
              popup_host_view_->popup_position_.y());

      popup_host_view_->ProcessMouseEvent(popup_event, ui::LatencyInfo());
      return;
    }
  }

  if (!render_widget_host_)
    return;
  render_widget_host_->ForwardMouseEvent(event);
}

void OffScreenRenderWidgetHostView::SendMouseWheelEvent(
    const blink::WebMouseWheelEvent& event) {
  for (auto* proxy_view : proxy_views_) {
    gfx::Rect bounds = proxy_view->GetBounds();
    if (bounds.Contains(event.PositionInWidget().x,
                        event.PositionInWidget().y)) {
      blink::WebMouseWheelEvent proxy_event(event);
      proxy_event.SetPositionInWidget(
          proxy_event.PositionInWidget().x - bounds.x(),
          proxy_event.PositionInWidget().y - bounds.y());

      ui::MouseWheelEvent ui_event =
          UiMouseWheelEventFromWebMouseEvent(proxy_event);
      proxy_view->OnEvent(&ui_event);
      return;
    }
  }

  blink::WebMouseWheelEvent mouse_wheel_event(event);

  mouse_wheel_phase_handler_.SendWheelEndForTouchpadScrollingIfNeeded();
  mouse_wheel_phase_handler_.AddPhaseIfNeededAndScheduleEndEvent(
      mouse_wheel_event, false);

  if (!IsPopupWidget()) {
    if (popup_host_view_) {
      if (popup_host_view_->popup_position_.Contains(
              mouse_wheel_event.PositionInWidget().x,
              mouse_wheel_event.PositionInWidget().y)) {
        blink::WebMouseWheelEvent popup_mouse_wheel_event(mouse_wheel_event);
        popup_mouse_wheel_event.SetPositionInWidget(
            mouse_wheel_event.PositionInWidget().x -
                popup_host_view_->popup_position_.x(),
            mouse_wheel_event.PositionInWidget().y -
                popup_host_view_->popup_position_.y());
        popup_mouse_wheel_event.SetPositionInScreen(
            popup_mouse_wheel_event.PositionInWidget().x,
            popup_mouse_wheel_event.PositionInWidget().y);

        popup_host_view_->SendMouseWheelEvent(popup_mouse_wheel_event);
        return;
      } else {
        // Scrolling outside of the popup widget so destroy it.
        // Execute asynchronously to avoid deleting the widget from inside some
        // other callback.
        content::BrowserThread::PostTask(
            content::BrowserThread::UI, FROM_HERE,
            base::BindOnce(&OffScreenRenderWidgetHostView::CancelWidget,
                           popup_host_view_->weak_ptr_factory_.GetWeakPtr()));
      }
    } else if (!guest_host_views_.empty()) {
      for (auto* guest_host_view : guest_host_views_) {
        if (!guest_host_view->render_widget_host_ ||
            !guest_host_view->render_widget_host_->GetView()) {
          continue;
        }
        const gfx::Rect& guest_bounds =
            guest_host_view->render_widget_host_->GetView()->GetViewBounds();
        if (guest_bounds.Contains(mouse_wheel_event.PositionInWidget().x,
                                  mouse_wheel_event.PositionInWidget().y)) {
          blink::WebMouseWheelEvent guest_mouse_wheel_event(mouse_wheel_event);
          guest_mouse_wheel_event.SetPositionInWidget(
              mouse_wheel_event.PositionInWidget().x - guest_bounds.x(),
              mouse_wheel_event.PositionInWidget().y - guest_bounds.y());
          guest_mouse_wheel_event.SetPositionInScreen(
              guest_mouse_wheel_event.PositionInWidget().x,
              guest_mouse_wheel_event.PositionInWidget().y);

          guest_host_view->SendMouseWheelEvent(guest_mouse_wheel_event);
          return;
        }
      }
    }
  }
  if (!render_widget_host_)
    return;
  render_widget_host_->ForwardWheelEvent(event);
}

void OffScreenRenderWidgetHostView::SetPainting(bool painting) {
  painting_ = painting;

  if (software_output_device_) {
    software_output_device_->SetActive(painting_, !paint_callback_running_);
  }
}

bool OffScreenRenderWidgetHostView::IsPainting() const {
  return painting_;
}

void OffScreenRenderWidgetHostView::SetFrameRate(int frame_rate) {
  if (parent_host_view_) {
    if (parent_host_view_->GetFrameRate() == GetFrameRate())
      return;

    frame_rate_ = parent_host_view_->GetFrameRate();
  } else {
    if (frame_rate <= 0)
      frame_rate = 1;
    if (frame_rate > 240)
      frame_rate = 240;

    frame_rate_ = frame_rate;
  }

  SetupFrameRate(true);

  for (auto* guest_host_view : guest_host_views_)
    guest_host_view->SetFrameRate(frame_rate);
}

int OffScreenRenderWidgetHostView::GetFrameRate() const {
  return frame_rate_;
}

#if !defined(OS_MACOSX)
ui::Compositor* OffScreenRenderWidgetHostView::GetCompositor() const {
  return compositor_.get();
}

ui::Layer* OffScreenRenderWidgetHostView::GetRootLayer() const {
  return root_layer_.get();
}

content::DelegatedFrameHost*
OffScreenRenderWidgetHostView::GetDelegatedFrameHost() const {
  return delegated_frame_host_.get();
}
#endif

void OffScreenRenderWidgetHostView::SetupFrameRate(bool force) {
  if (!force && frame_rate_threshold_us_ != 0)
    return;

  frame_rate_threshold_us_ = 1000000 / frame_rate_;

  if (GetCompositor()) {
    GetCompositor()->SetAuthoritativeVSyncInterval(
        base::TimeDelta::FromMicroseconds(frame_rate_threshold_us_));
  }

  if (copy_frame_generator_.get()) {
    copy_frame_generator_->set_frame_rate_threshold_us(
        frame_rate_threshold_us_);
  }

  if (begin_frame_timer_.get()) {
    begin_frame_timer_->SetFrameRateThresholdUs(frame_rate_threshold_us_);
  } else {
    begin_frame_timer_.reset(new AtomBeginFrameTimer(
        frame_rate_threshold_us_,
        base::Bind(&OffScreenRenderWidgetHostView::OnBeginFrameTimerTick,
                   weak_ptr_factory_.GetWeakPtr())));
  }
}

void OffScreenRenderWidgetHostView::Invalidate() {
  InvalidateBounds(GetViewBounds());
}

void OffScreenRenderWidgetHostView::InvalidateBounds(const gfx::Rect& bounds) {
  if (software_output_device_) {
    software_output_device_->OnPaint(bounds);
  } else if (copy_frame_generator_) {
    copy_frame_generator_->GenerateCopyFrame(bounds);
  }
}

void OffScreenRenderWidgetHostView::ResizeRootLayer(bool force) {
  SetupFrameRate(false);

  const float compositorScaleFactor = GetCompositor()->device_scale_factor();
  const bool scaleFactorDidChange = (compositorScaleFactor != scale_factor_);

  gfx::Size size;
  if (!IsPopupWidget())
    size = GetViewBounds().size();
  else
    size = popup_position_.size();

  if (!force && !scaleFactorDidChange &&
      size == GetRootLayer()->bounds().size())
    return;

  const gfx::Size& size_in_pixels =
      gfx::ConvertSizeToPixel(scale_factor_, size);

  local_surface_id_ = local_surface_id_allocator_.GenerateId();

  GetRootLayer()->SetBounds(gfx::Rect(size));
  GetCompositor()->SetScaleAndSize(scale_factor_, size_in_pixels,
                                   local_surface_id_);

#if defined(OS_MACOSX)
  bool resized = UpdateNSViewAndDisplay();
#else
  bool resized = true;
  GetDelegatedFrameHost()->WasResized(local_surface_id_, size,
                                      cc::DeadlinePolicy::UseDefaultDeadline());
#endif

  // Note that |render_widget_host_| will retrieve resize parameters from the
  // DelegatedFrameHost, so it must have WasResized called after.
  if (resized && render_widget_host_)
    render_widget_host_->WasResized();
}

viz::FrameSinkId OffScreenRenderWidgetHostView::AllocateFrameSinkId(
    bool is_guest_view_hack) {
  // GuestViews have two RenderWidgetHostViews and so we need to make sure
  // we don't have FrameSinkId collisions.
  // The FrameSinkId generated here must be unique with FrameSinkId allocated
  // in ContextFactoryPrivate.
  content::ImageTransportFactory* factory =
      content::ImageTransportFactory::GetInstance();
  return is_guest_view_hack
             ? factory->GetContextFactoryPrivate()->AllocateFrameSinkId()
             : viz::FrameSinkId(base::checked_cast<uint32_t>(
                                    render_widget_host_->GetProcess()->GetID()),
                                base::checked_cast<uint32_t>(
                                    render_widget_host_->GetRoutingID()));
}

void OffScreenRenderWidgetHostView::UpdateBackgroundColorFromRenderer(
    SkColor color) {
  if (color == background_color())
    return;
  background_color_ = color;

  bool opaque = SkColorGetA(color) == SK_AlphaOPAQUE;
  GetRootLayer()->SetFillsBoundsOpaquely(opaque);
  GetRootLayer()->SetColor(color);
}

}  // namespace atom
