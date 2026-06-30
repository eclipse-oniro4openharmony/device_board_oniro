# Gesture Navigation → Launcher Migration Plan

**Status:** ✅ IMPLEMENTED + verified on the Volla X23 (2026-06-05) · **Drafted:** 2026-06-05 · **Target device:** Volla X23 (vidofnir)

---

## 0. Implementation status (2026-06-05) — DONE + FINALIZED

All phases shipped and verified on device. The migration is now **finalized**:
the launcher unconditionally owns the swipe-up gesture, the relocated swipe-up
code has been **deleted from systemui** (which keeps only the side-edge BACK),
and the `persist.oniro.gesturenav_owner` rollback param has been **removed**
(it was a migration-period safety switch — no longer needed once the launcher
path was proven). systemui's `phone_gestureNavigation` is now a BACK-only
service. Rollback is now a `git revert` of the cleanup commit, not a param flip.

What was removed from `systemui/product/phone/gestureNavigation/` in the
finalization pass: the `recognizer/` (except `VelocityTracker`, moved to
`back/` — the back controller's fling tracker) and `animation/` directories,
`pages/DragOverlay.ets`, the dead `pages/RecentsOverlay.ets`, the `swipeUpEnabled`
param gate, the `goHome`/drag-overlay/recents plumbing, and the now-unused
`CAPTURE_SCREEN`/`GET_WALLPAPER`/`RUNNING_STATE_OBSERVER` permissions. The
launcher's `maybeStartGestureNav` param read was replaced by an unconditional
`startGestureNavWhenDesktopReady`.

| Phase | Outcome |
|---|---|
| 0 — `findIconRect` | `launcher/product/phone/src/main/ets/gesturenav/findIconRect.ts` — reverse-lookup of an app's icon rect (vp) on the **current desktop page**; returns null (→ caller fallback) for off-page/dock/folder. Produces the identical rect to the forward `PageDesktopStartAppHandler` path (verified). |
| 1 — pop-from-icon OPEN | ✅ user-confirmed. `OniroRemoteWindowController` attaches an `OniroIconRect` (from the tap-time `startAppItemInfo`/`startAppIconInfo`); `OniroRemoteWindowHost` springs the window out of the icon (scale=icon/screen, translate=iconCentre−screenCentre, opaque, radius 90→0). |
| 2 — relocate gesture engine | ✅ recognizer + animation + `DragOverlay.ets` ported into `launcher/.../gesturenav/`; `GestureNavHost` (from `MainAbility.initLauncher`, once the desktop grid is ready) drives swipe-up; `goHome()`=`minimizeAllApps()`; nav-mode via launcher `SettingsModel`; foreground via desktop `WINDOW_ACTIVE/INACTIVE`. HOME + RECENTS/Overview verified. |
| 3 — icon-anchored go-home | ✅ user-confirmed. `DragController.commit(HOME)` shrinks the snapshot into `findIconRect(outgoing app)` (scale=iconW/cardW about the existing bottom-centre anchor); bottom-centre fallback otherwise. Outgoing bundle recorded by `RecentsLoader`. |
| §7 — BACK focus push | ✅ launcher publishes a **sticky** `com.ohos.oniro.desktop.focus_changed` CommonEvent on desktop focus change; systemui's gesture svc subscribes and sets `foregroundIsLauncher` from it (no more `getTopAbility`/observer). BACK suppressed on home, fires in apps. |
| 4 — flip default + retire | ✅ **finalized.** launcher owns swipe-up unconditionally; systemui `phone_gestureNavigation` KEPT as a **BACK-only** service — the relocated swipe-up code (recognizer/animation/DragOverlay/RecentsOverlay/`swipeUpEnabled` gate) is **deleted** and the `persist.oniro.gesturenav_owner` param is **removed**. systemui creates only the `OniroBackPanel` window. |

**Key deviations / hard-won findings (read before touching this code):**
- **Defer the host start.** Creating the `GestureNavHost`'s fullscreen
  `TYPE_VOLUME_OVERLAY` window from `MainAbility.initLauncher` *while the
  desktop's async icon-grid build is in flight* makes the **workspace render
  empty** (dock + wallpaper fine, 34 apps load, but no icons). `MainAbility.
  startGestureNavWhenDesktopReady` gates the start on the `loaded` AppStorage
  flag + an 800 ms grace. (Isolated by flipping the param → `systemui`: same
  HAP, host off → icons return. The `updateIconCache: No matching resource`
  log is a RED HERRING — present in working boots too.)
- **BACK and swipe-up shared systemui's one `dispatch()`/`startMonitor`.**
  During the param-gated phase the swipe-up recognizer was gated *inside*
  `dispatch()` **after** the back-edge block (NOT by bailing `startMonitor`,
  which would kill BACK too). In the finalization the swipe-up branches were
  removed outright, leaving `dispatch()` with only the side-edge BACK path —
  but the same lesson holds for anyone re-touching it: the input monitor and
  its DOWN/MOVE/UP routing are shared, so BACK must stay wired even when the
  bottom-edge gesture is absent.
- **§7 latency:** CommonEvent delivery is async (~1-2 s after a transition),
  so right after boot/home there's a brief window where BACK isn't yet
  suppressed on home — the accepted "harmless BACK on home" bias. The
  **sticky** publish seeds late subscribers; without it the first home view
  after boot never suppressed BACK.
- **Verify launcher logs via `hilog -x`** (buffer dump), not live
  `hdc shell hilog` (dropped the launcher domain over the Pi tunnel).

Rollback (post-finalization): the `persist.oniro.gesturenav_owner` param is
gone, so rollback is a `git revert` of the cleanup commit (which restores the
systemui swipe-up code + the param) followed by a reboot. During the migration
period the param was the instant rollback (`param set
persist.oniro.gesturenav_owner systemui` + reboot).

> This is a forward-looking refactor plan for the OHOS **system-app layer**
> (`applications/standard/systemui` + `applications/standard/launcher`), not a
> device-boot phase. It is filed here because this docs dir is the project's
> engineering-doc home (cf. `legacy_input_system.md`). The implemented gesture
> work it builds on is tracked in project memory:
> `project_systemui_quickstep_recognizer`, `project_systemui_swipe_up_anim`,
> `project_systemui_back_gesture`, `project_winanim_controller_native_boot`,
> `project_gesture_nav_double_recents`.

---

## 1. TL;DR

Today the swipe-up **gesture state machine**, the **recents/overview** UI, and
the **swipe-to-home animation** live in **systemui** and animate a **screenshot**
(no access to the live app window). The launcher, meanwhile, already holds the
one capability that matters — the `windowAnimationManager` controller slot — and
can render/transform the real app window leash via `RemoteWindow`.

**Plan:** move the swipe-up gesture machine + recents + home animation **into the
launcher**, so the home-shrink can target the real **icon rect** (launcher-only
knowledge) and the system becomes a single coherent owner. **Keep the back
gesture in systemui** (it needs nothing the launcher has, and must survive
launcher restarts) — but remove its cross-process stall hazard with a focus
**push** from the launcher.

**Hard platform constraint (shapes everything):** on this build the live leash is
usable **only for the OPEN direction** (an app *becoming* foreground). Rendering a
`RemoteWindow` proxy for a *minimizing/backgrounding* window re-promotes it to
foreground → re-launch → re-minimize **oscillation** (verified 2026-06-05). So the
**go-home shrink must animate a snapshot into the icon rect**, while **app-open
pops the real window from the icon rect**. AOSP-grade live-window-during-drag is a
deep native effort (fixing the WMS oscillation) and is kept off the main path.

---

## 2. Why the launcher (the decisive constraints)

The home/overview gesture is intrinsically a launcher concern; the back gesture is
not. The dividing line is **what each gesture needs**:

| Capability the gesture needs | Where it exists on OHOS |
|---|---|
| Live app-window leash transform | **Launcher only** — `windowAnimationManager.setController` is a **single-owner** slot; one `WindowAnimationController` in WMS. The launcher already holds it. |
| App **icon rect** to shrink into / pop from | **Launcher only** — desktop/hotseat layout; `PageDesktopStartAppHandler` already computes icon X/Y for launches. |
| Recents mission list + snapshots | Either (framework `missionManager`), but it pairs with the above. |
| Raw touch | Either (`inputMonitor` is global, multi-subscriber, observe-only). |

systemui is *structurally fenced off* from the leash and the icon rect, which is
why its swipe-up animation has always been a screenshot (the deferred "P4 live
foreground via RSNode bridge"). The launcher is the only process where the two
capabilities the "shrink into icon" animation needs actually co-exist.

This does **not** contradict the earlier "keep gesture nav in systemui" decision
(2026-05-28): that call was correct *for what was then possible* — the launcher
had **no** window-leash capability at all. The 2026-06-04/05 controller +
`RemoteWindow` work is exactly the capability that didn't exist then, and is what
tips the balance now.

---

## 3. AOSP reference (verified in-tree)

Reference trees (AOSP): `packages/apps/Launcher3/quickstep`,
`frameworks/base/packages/SystemUI`.

**Home / overview / swipe-to-home → Launcher (Quickstep).** SystemUI only binds
to the launcher's `TouchInteractionService` (via `OverviewProxyService` →
`IOverviewProxy`) and feeds it state flags. The launcher owns the swipe-up
`InputMonitor`, the `InputConsumer`/`GestureState` machine, `RecentsView`/`TaskView`,
the `RectFSpringAnim` home-shrink (`SwipeUpAnimationLogic`/`AbsSwipeUpHandler` +
`TaskViewSimulator`), and — crucially — **direct `SurfaceControl` leash control**
from the `RecentsAnimationController` (`RemoteAnimationTargets` → `TransformParams`
→ `SurfaceControl.Transaction`, RT-synced). The leash is what lets it shrink the
*real* app window into an icon found via `Launcher.getFirstMatchForAppClose`.

**Back gesture → SystemUI.** Verified: the runtime back gesture is entirely in
`packages/SystemUI/src/com/android/systemui/navigationbar/gestural/`
— `EdgeBackGestureHandler.java`, `BackPanel.kt`, `BackPanelController.kt`,
`EdgePanelParams.kt`. It owns a **dedicated** `InputMonitorCompat("edge-swipe")`
(separate from the swipe-up monitor), `pilferPointers()` on commit, and dispatches
either legacy `sendEvent(KEYCODE_BACK) → injectInputEvent` or modern predictive
back via WM Shell's `BackAnimation`. Quickstep's only "back" code is the gesture
**tutorial** mock (`quickstep/.../interaction/EdgeBackGestureHandler.java`), not
the runtime gesture.

Our OHOS port mirrors the AOSP SysUI gestural package 1:1 (`BackPanelController.ts`,
`Spring.ts`, `EdgePanelParams.ts`, `BackPanel.ets`).

**Why AOSP keeps back in SysUI:** back is a global navigation primitive bound to
the *focused* window (works in every app, on keyguard, on the launcher); "go home"
is only one possible back outcome. It belongs with the nav-bar / gesture-inset /
system-gesture-exclusion ownership, must be resilient to launcher death and 3p
launchers, and its predictive animation targets the *current* window via WM Shell
— never the launcher. **It needs nothing the launcher has.**

---

## 4. Decision: what moves, what stays

**Moves into the launcher:**
- Swipe-up gesture state machine (recognizer)
- In-gesture overlay + recents/overview UI
- Swipe-to-home animation (commit)

**Stays in systemui:**
- **Side-edge BACK gesture** — see §7 for the rationale and the decoupling task.
- Nav-bar pill / 24 vp gesture dock strip (a real `TYPE_NAVIGATION_BAR` window
  that reserves the bottom avoid inset).
- Dropdown / control / notification panels.

---

## 5. Target architecture (where code lands)

The launcher's `MainAbility` is already a `ServiceExtensionAbility` that owns
windows and is always-on. **No new ability is required** — a `GestureNavHost`
class instantiated from `MainAbility.initLauncher()` (param-gated) replicates what
systemui's `ServiceExtAbility.onCreate` does today. The controller registration
**stays** in `EntryView.aboutToAppear` (the napi-thread requirement is already
solved there). The two ends talk via process-local `AppStorage` (same
`OniroDrag*` / `OniroRemoteWindowList` keys, now both in one process).

| Concern | Today (systemui) | After (launcher) |
|---|---|---|
| Swipe recognizer + inputMonitor | `ServiceExtAbility` | `GestureNavHost` (from `MainAbility.initLauncher`) |
| Drag/Overview window + `DragOverlay.ets` | systemui | launcher (own top-level overlay window — the desktop window sits *behind* foreground apps, so the overlay must be its own `TYPE_VOLUME_OVERLAY`) |
| "Is launcher foreground" | `abilityForegroundState` observer + `getTopAbility` seed (the ~1.2 s stall source) | launcher's own desktop `WINDOW_ACTIVE`/inactive event — **observer + seed deleted** |
| Go-home structural commit | `startAbility(com.ohos.launcher)` (cross-process) | `windowManager.minimizeAllApps()` (native; already the launcher's go-home path) |
| Live-window leash control | none | launcher controller (already working) |
| App-open animation | n/a | `OniroRemoteWindowController` + `OniroRemoteWindowHost` (already working; gets pop-from-icon in Phase 1) |

The `RemoteWindow` host renders the leash proxy which composites at the app's
z-order, so it can stay in the desktop window (`EntryView`). The drag/recents
overlay cannot — it must float over the foreground app, hence its own window.

---

## 6. Phased plan

Each phase leaves a working, shippable device. Stop after any phase.

### Phase 0 — Probes + decision gate (≈½–1 day, low risk)
Two cheap probes everything depends on:
- **`findIconRect(bundle, ability)`** — reverse-lookup an app's on-screen icon
  rect from the desktop/dock view models, with an AOSP-style fallback (off-screen
  / in a folder / other page → hotseat-center). `PageDesktopStartAppHandler`
  already computes icon X/Y for launches; this is the reverse.
- **Confirm the leash constraint** — instrument `onMinimizeWindow` to render the
  proxy and verify the oscillation reproduces (expected yes per 2026-06-05). Locks
  in "snapshot for go-home." If it does *not* oscillate, the stretch track (§6.6)
  opens up.

**Exit:** a `findIconRect` helper + documented go/no-go on live-leash. No behavior
change.

### Phase 1 — Pop-from-icon OPEN animation (≈1 day, low risk, quick visible win)
Enhances only the **existing, working** open path — no gesture migration.
- `pages/OniroRemoteWindowHost.ets`: the `translateX/translateY` + scale-center
  hooks are already stubbed for this (members at lines ~73–79; reset at ~105–107).
  Start the zoom from the tapped icon rect instead of a centered 0.9→1.0.
- `common/OniroRemoteWindowController.ts` + the launch path
  (`AppItem.launchApp` → `PageDesktopViewModel.onAppDoubleClick` →
  `PageDesktopStartAppHandler`): stash the launching icon rect (bundle→rect) in
  `AppStorage` at tap time; `onStartAppFromLauncher` matches `target.bundleName`.
  `onStartAppFromRecent` → the Overview card rect; `onStartAppFromOther` → centered
  fallback.
- Param: reuse `persist.oniro.winanim_controller`.

**Exit:** apps visibly zoom out of their icon on launch; everything else
unchanged. Validates the icon-rect plumbing Phase 3 needs.

### Phase 2 — Relocate the gesture engine into the launcher (≈2–4 days, high risk, **parity only**)
Behind a new param `persist.oniro.gesturenav_owner` (`systemui` default | `launcher`).
- **Port** these from systemui into the launcher (only edit: swap
  `../../common/.../Log|Constants` imports for `@ohos/common`):
  - `recognizer/` — `SwipeRecognizer.ts`, `VelocityTracker.ts`, `MotionPauseDetector.ts` (zero systemui deps)
  - `animation/` — `DragController.ts`, `SnapshotCapture.ts`, `RecentsLoader.ts`, `WallpaperCache.ts`
  - `pages/DragOverlay.ets` (+ leave the unused `RecentsOverlay.ets` behind)
- **`GestureNavHost`** from `MainAbility.initLauncher()` (param-gated): mirror
  `ServiceExtAbility.onCreate` — build recognizer, create the drag overlay window
  from `this.context`, start inputMonitor, subscribe nav-mode. **Reuse the
  launcher's existing nav-mode plumbing** (`SettingsModel` /
  `EVENT_NAVIGATOR_BAR_STATUS_CHANGE`) instead of a fresh datashare helper.
- **Foreground tracking** → desktop `WINDOW_ACTIVE` (kills the observer + the
  `getTopAbility` block entirely; the launcher already registers `windowEvent` in
  `initLauncher`).
- **HOME commit** = `windowManager.minimizeAllApps()` + `DragController` snapshot
  shrink **to bottom-center (parity)**.
- systemui's `phone_gestureNavigation` reads the same param and `startMonitor()`
  bails when owner=launcher.
- The launcher's stock gesture nav stays disabled (`// this.startGestureNavigation()`
  in `MainAbility.ts`).
- **Permissions to add to the launcher module** (system app → auto-granted once
  declared): `ohos.permission.CAPTURE_SCREEN` (for `SnapshotCapture`). Wallpaper
  backdrop can reuse the launcher's `WallpaperModel` instead of a fresh
  `getImage` + `GET_WALLPAPER`. `INPUT_MONITORING` is already held (the stock
  launcher gesture nav used `inputMonitor`). Drop `RUNNING_STATE_OBSERVER` (not
  needed — local focus).

**Exit:** with `owner=launcher`, swipe-up home/recents behave **identically to
today**, driven from the launcher; flip the param back for instant rollback. No
visual change yet — this is the de-risked structural move.

### Phase 3 — Icon-anchored go-home shrink (≈1–2 days, medium risk — the headline)
Now the gesture lives where the icon rects live. Change `DragController`'s HOME
end-state from "scale 0.12 at bottom-center" to **shrink the snapshot into
`findIconRect(outgoing app)`** (fallback hotseat-center / bottom when off-screen).
Drag→commit pose handoff is now an in-process variable. RECENTS/CANCEL unchanged.

**Exit:** swipe-up-home shrinks the app into its launcher icon.

### Phase 4 — Retire systemui gesture service + flip default (≈½ day)
Disable/remove `phone_gestureNavigation` from the systemui product, make
`gesturenav_owner=launcher` the default, update docs + memory.

### Phase 6.6 — Stretch (optional, native): live window during drag
Only if Phase 0 surprises us, or as a separate graphic_2d/WMS effort to fix the
minimize-oscillation so the *real* window (not a snapshot) can be dragged
AOSP-style. High risk, deep, **off the main path.** Note: predictive-back (if ever
pursued) animating the outgoing window would hit the *same* oscillation, so this
track would benefit both.

---

## 7. Back gesture: keep in systemui, decouple the stall (companion task)

**Decision: keep the side-edge BACK gesture in systemui.** It needs nothing the
launcher has (no leash, no icons, no recents), it must survive launcher
restarts / 3p launchers, it shares the input layer with the 3-button BACK key
(also systemui), and our port is a faithful copy of AOSP's SysUI gestural package.
Moving it would buy only organizational tidiness, paid for in resilience.

**But remove its one real wart without moving it.** Today back suppresses itself
while the launcher is foreground via `getTopAbility` (a sync main-thread binder)
and the `abilityForegroundState` observer — the documented source of the ~1.2 s
stall, and brittle because the launcher is observer-silent on its own FOREGROUND.
The stall is intrinsic to the **pull**, not to back's location.

**Fix (companion to Phase 2):** the launcher is the authority on its own focus, so
have it **push** — emit a `desktopFocused` event on desktop `WINDOW_ACTIVE`/inactive
(reuse `navigationBarCommonEventManager` CommonEvent infra). systemui's back
handler reads that cached flag at touch-DOWN instead of `getTopAbility` / the
app-state observer. Removes the stall *and* the inference brittleness, keeps back
in the resilient process.

The two-monitor split is a non-issue: OHOS `inputMonitor` is multi-subscriber
observe-only, and edge-back vs swipe-up own disjoint hot zones — which is exactly
how AOSP runs them (`"edge-swipe"` vs the swipe-up monitor are separate).

**Flip condition:** only fold back into the launcher if you commit to
single-launcher-forever **and** the focus push proves flaky in practice — then a
single unified stack is defensible, accepting the launcher-restart gap.

---

## 8. Cross-cutting

- **Rollback / params.** Every risky change is param-gated; flip
  `persist.oniro.gesturenav_owner` / `persist.oniro.winanim_controller` and restart
  the two processes. No flash needed for app-side changes. Defaults live in
  `vendor/oniro/hybris_generic/etc/param/hybris_native.para`.
- **Build / deploy / verify** (see `ohos-system-dev` skill + memory):
  - Launcher: `oniro-app build --module phone_launcher` → `bm install -r -p` →
    **full reboot** (launcher is persistent; install ≠ reload; ACE cache).
  - systemui: same pattern (`--module phone_gestureNavigation`); only
    `param set ohos.startup.powerctrl reboot` reliably restarts the persistent
    process.
  - `bm install -r` can transiently hit BMS-died (9568391) — retry.
  - Verify on X23 with `oniro-app screenshot` / `oniro-app watch --log`. To land
    RECENTS vs HOME synthetically: slow `uitest uiInput drag 360 1545 360 380 350`
    → RECENTS; fast `…9000` flick → HOME (kernel `uinput` won't reach inputMonitor;
    use `uitest uiInput`).
- **Signing.** The launcher must remain signed apl=`system_core` (already required
  for `setController`). The gesture host inherits launcher signing.
- **Don't move:** nav-bar pill / dock strip, dropdown panels (see §4). The
  `OniroDropdownPanelOpen` gesture-suppression flag becomes a cross-process read
  (was same-process AppStorage) — route it via the same CommonEvent/datashare
  channel as the back-focus push.

---

## 9. Risks & open questions

1. **Phase 2 is a genuine refactor** of working, hard-won code. The
   param-coexistence + parity-first ordering is the safety net — do **not** fold
   Phase 3's visual change into Phase 2.
2. **Go-home stays a snapshot** (platform constraint, §1). Live-window drag is the
   native stretch track only.
3. **`minimizeAllApps()` vs `startAbility(launcher)`** may fire different
   WMS transitions/callbacks — verify the controller's no-op minimize still
   suppresses the default close zoom when the *gesture* (not the HOME key)
   triggers go-home.
4. **`findIconRect` coverage** — apps in folders / on other pages / not on the
   workspace need a defined fallback (hotseat center), mirroring AOSP
   `getFirstMatchForAppClose`'s fallback.
5. **Overview-from-launcher** must remain a top-level overlay window, not an
   `EntryView` component (desktop window is behind foreground apps).
6. **CommonEvent latency** for the back-focus push — verify it lands before a
   back-swipe immediately after go-home (optimistic local set on `goHome` covers
   the gap, as today).

---

## 10. File reference index

**systemui — gesture service** (`applications/standard/systemui/product/phone/gestureNavigation/src/main/ets/`):
- `ServiceExtAbility/ServiceExtAbility.ts` (907) — orchestrator (the thing that conceptually moves)
- `recognizer/SwipeRecognizer.ts` (346), `VelocityTracker.ts` (108), `MotionPauseDetector.ts` (111) — **move**
- `animation/DragController.ts` (376), `SnapshotCapture.ts` (89), `RecentsLoader.ts` (134), `WallpaperCache.ts` (100) — **move**
- `pages/DragOverlay.ets` (461) — **move**; `pages/RecentsOverlay.ets` (183) — legacy/unused, leave
- `back/BackPanelController.ts` (928), `Spring.ts` (273), `EdgePanelParams.ts` (360), `pages/BackPanel.ets` (189) — **stay** (§7)

**launcher** (`applications/standard/launcher/product/phone/src/main/ets/`):
- `MainAbility/MainAbility.ts` (209) — `ServiceExtensionAbility`; `initLauncher()` (host site), `onRequest` startId≠1 → `minimizeAllApps()` (go-home), stock gesture nav disabled
- `pages/EntryView.ets` (250) — `@Entry`; `registerWindowAnimationController()` (param-gated), mounts `OniroRemoteWindowHost()`, desktop `windowEvent`
- `common/OniroRemoteWindowController.ts` (141) — the controller; `show()` queues `RemoteWindowItem`; open=zoom, minimize/close=no-op
- `pages/OniroRemoteWindowHost.ets` (150) — renders `RemoteWindow(target)`, zoom spring, `translateX/Y` hooks for pop-from-icon
- `feature/gesturenavigation/` — stock gesture nav (disabled); `feature/recents/` — stock `RecentMissionsViewModel` / `RecentView` (button-mode recents, untouched)

**AOSP reference:**
- Launcher: `Launcher3/quickstep/src/com/android/quickstep/` — `TouchInteractionService`, `AbsSwipeUpHandler`, `SwipeUpAnimationLogic`, `util/RectFSpringAnim.java`, `util/TaskViewSimulator.java`, `views/RecentsView.java`/`TaskView.java`
- SystemUI back: `packages/SystemUI/src/com/android/systemui/navigationbar/gestural/` — `EdgeBackGestureHandler.java`, `BackPanel.kt`, `BackPanelController.kt`, `EdgePanelParams.kt`

---

## 11. Param / flag summary

| Param | Values | Meaning |
|---|---|---|
| `persist.oniro.winanim_controller` | `0` / `1` | Launcher registers the `windowAnimationManager` controller (needs the graphic_2d `rs_window_animation_controller` fix). Reused by Phase 1. |
| `persist.oniro.gesturenav_owner` | `systemui` / `launcher` | **New.** Who owns the swipe-up gesture: systemui (today) or launcher (this plan). Phase 2+. |
