/*
    clip2output - a KWin effect that confines each window to the output that owns it, both for
    painting and for input.

    Why not just use EffectWindow::screen()? Because that is KWin's CENTRE-BASED guess, and a
    scrolling tiler deliberately parks columns partly (or wholly) outside their own screen. Once
    such a column's centre crosses the seam, KWin re-homes it to the neighbour and the column
    reappears there - the very spill we are trying to remove.

    So ownership here is STICKY: it exists only because a client claimed the window over D-Bus,
    and it changes only on another claim or an explicit user drag. That makes a column scrolled
    off its screen fade out at the screen edge and stay gone, while dragging a window between
    monitors still works normally.

    Windows nobody claimed are never clipped. Guessing an owner for them (as this effect
    once did, centre-based, at first paint) turns every PROGRAMMATIC cross-screen move - KWin
    placing a dialog, an app repositioning itself, a "Window to Next Screen" shortcut - into an
    invisible window: the guess never follows the move, so the window is painted (and, with input
    clipping, clicked) only on the screen it left. A modal dialog in that state locks its whole
    application. Unclaimed windows therefore keep stock KWin behaviour, bugs included, rather
    than gaining new ones.

    Clipping the PAINT alone is not enough. The part of a window that hangs over the seam is
    invisible but still hit-testable, so it swallows clicks meant for the windows that really live
    on the neighbouring screen. The input half of the fix is in the "Input clipping" section below.
*/
#include "config-kwin.h"

#include "core/output.h"
#include "core/renderviewport.h"
#include "effect/effect.h"
#include "effect/effecthandler.h"
#include "effect/effectwindow.h"
#include "wayland/plasmawindowmanagement.h"
#include "window.h"

#include <QDBusConnection>
#include <QTimer>

/*
    One source, both KWin API generations. 6.7 renamed Output to LogicalOutput, introduced its own
    Region type, moved the paint region into device coordinates, and takes that region by const
    reference; 6.3/6.5 use Output and QRegion, hand the region over in logical coordinates, and take
    it by value.

    The switch is a feature test rather than a version test on purpose: 6.3.6's config-kwin.h does
    not define PROJECT_VERSION_MAJOR/MINOR at all, so a version comparison silently evaluates to
    zero and picks a branch by accident. The presence of core/region.h IS the API difference.
*/
#if __has_include("core/region.h")
#  define CLIP2OUTPUT_DEVICE_REGION 1
#  include "core/region.h"
#  define CLIP2OUTPUT_REGION_ARG const ClipRegion &
// 6.7 also gained a RenderView parameter on prePaintWindow and dropped its presentTime.
#  define CLIP2OUTPUT_PREPAINT_ARGS RenderView *view, EffectWindow *w, WindowPrePaintData &data
#  define CLIP2OUTPUT_PREPAINT_FORWARD view, w, data
#else
#  define CLIP2OUTPUT_DEVICE_REGION 0
#  define CLIP2OUTPUT_REGION_ARG ClipRegion
#  define CLIP2OUTPUT_PREPAINT_ARGS EffectWindow *w, WindowPrePaintData &data, \
                                    std::chrono::milliseconds presentTime
#  define CLIP2OUTPUT_PREPAINT_FORWARD w, data, presentTime
#endif

#include <QDebug>
#include <QHash>
#include <QSet>
#include <QString>

#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>

#include <algorithm>

namespace KWin
{

#if CLIP2OUTPUT_DEVICE_REGION
using ClipOutput = LogicalOutput;
using ClipRegion = Region;
using ClipRect = Rect;
#else
using ClipOutput = Output;
using ClipRegion = QRegion;
using ClipRect = QRect;
#endif

namespace
{
/*
    Input clipping
    --------------

    KWin funnels every "which window is under this point" decision through the one virtual
    Window::hitTest() - InputRedirection::findToplevel() (pointer, touch and tablet targeting),
    the drag-and-drop filter, and the Workspace.windowAt() script call all end in it. Teaching
    hitTest() the same output boundary the painter already honours therefore fixes every input
    path at once, and nothing else: the window keeps its geometry, its buffer and its stacking.

    An effect cannot subclass Window, so the slot is redirected in the vtable instead. That is
    blunt, but it is contained: the original is kept and called for every window we have no
    opinion about, and the destructor puts every slot back before this .so is unmapped.
*/
using HitTestFn = bool (*)(const Window *, const QPointF &);

constexpr std::size_t NoIndex = static_cast<std::size_t>(-1);

// vtable -> the hitTest slot it held before we touched it. Keyed by vtable, not by window, so
// each Window subclass (XdgToplevelWindow, X11Window, InternalWindow, ...) is patched once.
QHash<void **, HitTestFn> s_originalHitTest;

/*
    The Itanium C++ ABI stores a pointer to a virtual member function as {ptr, adj} where an odd
    ptr is "1 + byte offset into the vtable". Reading the index out of it is far safer than
    hard-coding a slot number that shifts with every KWin release.
*/
std::size_t hitTestVtableIndex()
{
    struct ItaniumMemberPtr
    {
        std::uintptr_t ptr;
        std::ptrdiff_t adjustment;
    };

    bool (Window::*hitTest)(const QPointF &) const = &Window::hitTest;
    if (sizeof(hitTest) != sizeof(ItaniumMemberPtr)) {
        return NoIndex;
    }
    ItaniumMemberPtr raw{};
    std::memcpy(&raw, &hitTest, sizeof(raw));
    if ((raw.ptr & 1) == 0 || raw.adjustment != 0) {
        return NoIndex; // not virtual, or not at offset zero - do not guess
    }
    return (raw.ptr - 1) / sizeof(void *);
}

// Vtables live in .data.rel.ro, which full RELRO has already turned read-only. Look the current
// protection up rather than assuming it, so it can be restored exactly as it was.
//
// Read with stdio, not QFile: procfs reports these files as zero length, and QFile::atEnd() then
// says "empty" before a single line has been read.
bool pageProtection(std::uintptr_t address, int *protection)
{
    std::FILE *maps = std::fopen("/proc/self/maps", "r");
    if (!maps) {
        return false;
    }
    bool found = false;
    char line[512];
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long start = 0;
        unsigned long long end = 0;
        char perms[8] = {};
        if (std::sscanf(line, "%llx-%llx %7s", &start, &end, perms) != 3) {
            continue;
        }
        if (address < start || address >= end) {
            continue;
        }
        int prot = PROT_NONE;
        if (perms[0] == 'r') {
            prot |= PROT_READ;
        }
        if (perms[1] == 'w') {
            prot |= PROT_WRITE;
        }
        if (perms[2] == 'x') {
            prot |= PROT_EXEC;
        }
        *protection = prot;
        found = true;
        break;
    }
    std::fclose(maps);
    return found;
}

// Swap one vtable slot, taking the page writable for exactly as long as the store needs.
bool writeVtableSlot(void **slot, void *value)
{
    const auto address = reinterpret_cast<std::uintptr_t>(slot);
    int protection = PROT_READ;
    if (!pageProtection(address, &protection)) {
        return false;
    }
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) {
        return false;
    }
    void *page = reinterpret_cast<void *>(address & ~static_cast<std::uintptr_t>(pageSize - 1));
    if (mprotect(page, pageSize, PROT_READ | PROT_WRITE) != 0) {
        return false;
    }
    *slot = value;
    mprotect(page, pageSize, protection);
    return true;
}
/*
    Screen attribution
    ------------------

    Plasma decides which screen a window is on by taking the centre of the geometry KWin
    publishes over org_kde_plasma_window_management:

        libtaskmanager/waylandtasksmodel.cpp:  screenGeometry(window->geometry.center())

    and a task manager set to "show only tasks from current screen" then keeps a task only if
    that screen rect equals its own panel's. That is the same centre-based guess this effect
    exists to override, made a second time in another process - so a window scrolled across the
    seam, drawn and clicked on one screen only, has its button jump to the other one.

    The fix is to publish a rectangle whose centre lands on the owning output. The top-left
    corner is left alone wherever that is possible and only the size is changed, because the
    published x is what a position-ordered task manager sorts on: clamping it would make
    scrolled-off columns tie at the screen edge and lose their column order.
*/

// Qt rebuilds a QRect from the four ints sent over the wire and calls QRect::center(), which is
// (x1 + x2) / 2 with x2 = x + width - 1 and the division truncating toward zero. Replicating it
// exactly is cheaper than reasoning about where that rounding lands, especially for the negative
// coordinates a scrolled-off column really does have.
int centreOf(int pos, int len)
{
    return int((qint64(pos) + (pos + len - 1)) / 2);
}

// Position and length whose centre falls within [lo, hi), keeping pos untouched whenever the
// centre can be brought there by changing the length alone.
void fitCentre(int pos, int len, int lo, int hi, int *outPos, int *outLen)
{
    const int last = hi - 1; // last coordinate still on the output
    const int centre = centreOf(pos, len);
    if (centre >= lo && centre <= last) {
        *outPos = pos; // already attributed correctly; publish the honest rectangle
        *outLen = len;
        return;
    }

    const int target = centre < lo ? lo : last;
    if (target >= pos) {
        // Growing (or shrinking) the length walks the centre toward the output.
        const int candidate = 2 * (target - pos) + 1;
        for (const int length : {candidate, candidate + 1, candidate - 1}) {
            if (length >= 1 && centreOf(pos, length) >= lo && centreOf(pos, length) <= last) {
                *outPos = pos;
                *outLen = length;
                return;
            }
        }
    }

    // The output lies entirely before this edge, so length alone cannot help and the corner has
    // to give. Only reachable by a window wholly off its own screen - one nobody can see - and
    // the cost is that several such windows share an x and tie in a position-ordered task manager.
    *outPos = target;
    *outLen = 1;
}
} // namespace

class ClipToOutputEffect : public Effect
{
    Q_OBJECT
public:
    ClipToOutputEffect()
        : m_hitTestIndex(hitTestVtableIndex())
    {
        connect(effects, &EffectsHandler::windowClosed, this, [this](EffectWindow *w) {
            m_owner.remove(w);
            m_moving.remove(w);
        });
        // Every Window subclass has its own vtable, so keep hooking as new kinds of window show
        // up rather than assuming the ones present at load time are all there will ever be.
        connect(effects, &EffectsHandler::windowAdded, this, &ClipToOutputEffect::adopt);
        for (EffectWindow *w : effects->stackingOrder()) {
            adopt(w);
        }
        s_effect = this;

        // Exposed as org.kde.KWin /ClipToOutput so a KWin script (a tiler, typically) can hand
        // ownership over explicitly, instead of relying on drag detection alone.
        QDBusConnection::sessionBus().registerObject(QStringLiteral("/ClipToOutput"), this,
                                                     QDBusConnection::ExportScriptableSlots);
    }

    ~ClipToOutputEffect() override
    {
        // Plasma keeps whatever it was last told, so give every corrected window its real
        // rectangle back. Unclaimed windows were never told anything but the truth.
        m_attributeScreen = false;
        const auto owned = m_owner.keys();
        for (EffectWindow *w : owned) {
            publishGeometry(w);
        }

        // Put every slot back before this .so is unmapped, or the next hit test jumps into a hole.
        for (auto it = s_originalHitTest.constBegin(); it != s_originalHitTest.constEnd(); ++it) {
            writeVtableSlot(it.key() + m_hitTestIndex, reinterpret_cast<void *>(it.value()));
        }
        s_originalHitTest.clear();
        s_effect = nullptr;
    }

    /*
        The scene culls both damage and painting underneath an opaque window. A window that
        straddles a seam is opaque over the overhang as far as the scene is concerned, so
        everything below it there is dropped from the repaint - and then this effect declines to
        draw the overhang, leaving whatever the buffer happened to hold. That is the band of stale,
        striped content at the seam.

        Declaring the window translucent for the frame withdraws it from the occlusion cull, so the
        windows that really live on that screen are repainted normally. Verified in both scene
        implementations: 6.3 zeroes `data.opaque` in setTranslucent() and skips the window in its
        cull, and 6.7 skips computing an opaque region at all when the mask says translucent.

        Only windows that actually overhang another screen pay for it; anything else is left opaque
        and keeps culling as before.
    */
    void prePaintWindow(CLIP2OUTPUT_PREPAINT_ARGS) override
    {
        if (w && shouldClip(w) && !w->isUserMove() && !w->isUserResize()) {
            if (ClipOutput *owner = ownerOf(w)) {
                if (overhangsAnotherOutput(w, owner)) {
                    data.setTranslucent();
                }
            }
        }
        effects->prePaintWindow(CLIP2OUTPUT_PREPAINT_FORWARD);
    }

    void paintWindow(const RenderTarget &renderTarget, const RenderViewport &viewport,
                     EffectWindow *w, int mask, CLIP2OUTPUT_REGION_ARG paintRegion,
                     WindowPaintData &data) override
    {
        if (!w || !shouldClip(w)) {
            effects->paintWindow(renderTarget, viewport, w, mask, paintRegion, data);
            return;
        }

        // While the user is dragging or resizing, show the whole window wherever it lies -
        // otherwise you could not see what you are dragging across the seam.
        if (w->isUserMove() || w->isUserResize()) {
            if (m_owner.contains(w)) {
                m_moving.insert(w); // unclaimed windows have no owner for the drop to correct
            }
            effects->paintWindow(renderTarget, viewport, w, mask, paintRegion, data);
            return;
        }

        // A drag just ended: the drop decides the new owner.
        if (m_moving.remove(w)) {
            assignOwnerFromCentre(w);
        }

        ClipOutput *owner = ownerOf(w);
        if (!owner) {
            effects->paintWindow(renderTarget, viewport, w, mask, paintRegion, data);
            return;
        }

#if CLIP2OUTPUT_DEVICE_REGION
        const Rect ownerRect = viewport.mapToDeviceCoordinatesAligned(owner->geometry());
#else
        const QRect ownerRect = owner->geometry();   // pre-6.7: the region is already logical
#endif
        const ClipRegion clipped = paintRegion.intersected(ownerRect);
        if (clipped.isEmpty()) {
            return; // none of this window belongs on the output being painted
        }
        effects->paintWindow(renderTarget, viewport, w, mask, clipped, data);
    }

    int requestedEffectChainPosition() const override
    {
        return 99;
    }

    // The replacement hitTest slot. Runs for every window of every hooked class, so it has to be
    // cheap and has to fail open for anything this effect has no ownership opinion about.
    static bool hitTest(const Window *self, const QPointF &point)
    {
        void **vtable = *reinterpret_cast<void **const *>(self);
        const HitTestFn original = s_originalHitTest.value(vtable, nullptr);
        if (!original) {
            return false; // unreachable: a slot is recorded before it is ever replaced
        }
        if (!original(self, point)) {
            return false;
        }
        return !s_effect || s_effect->acceptsInputAt(self, point);
    }

public Q_SLOTS:
    Q_SCRIPTABLE void setOwner(const QString &windowId, const QString &outputName)
    {
        EffectWindow *w = effects->findWindow(QUuid(windowId));
        if (!w) {
            return;
        }
        if (outputName.isEmpty()) {
            m_owner.remove(w);
            publishGeometry(w); // no longer corrected, so put the honest rectangle back
            // The claim may have pinned the window's attribution to the owner; hand it back to
            // the output that actually holds it, the same way KWin itself would attribute it.
            if (Window *window = w->window()) {
                if (ClipOutput *honest = effects->screenAt(window->frameGeometry().center().toPoint())) {
                    if (window->output() != honest) {
                        window->setOutput(honest);
                    }
                }
            }
        } else if (effects->findScreen(outputName)) {
            m_owner[w] = outputName;
            enforceAttribution(w);
        }
        schedulePublishAll();
        effects->addRepaintFull();
    }

    // Which binary is actually running. Replacing an installed .so does NOT change the code in a
    // running KWin - the library stays mapped, and even unloadEffect/loadEffect re-runs the OLD
    // code while still reporting "loaded: true". Every previous way of telling the two apart
    // relied on a feature that happened to be new at the time, which stops working as soon as
    // both builds have it. A stamp baked in at compile time always answers honestly.
    Q_SCRIPTABLE QString buildStamp()
    {
        return QStringLiteral(CLIP2OUTPUT_BUILD_STAMP);
    }

    Q_SCRIPTABLE QString ownerOf(const QString &windowId)
    {
        EffectWindow *w = effects->findWindow(QUuid(windowId));
        if (!w) {
            return QStringLiteral("(no such window)");
        }
        auto it = m_owner.constFind(w);
        return it == m_owner.constEnd() ? QStringLiteral("(unassigned)") : *it;
    }

    // Painting is always clipped; input clipping can be switched off at runtime, which makes it
    // possible to tell "the effect is misbehaving" from "the window really is over there".
    Q_SCRIPTABLE void setInputClipping(bool enabled)
    {
        m_clipInput = enabled;
    }

    // The screen a window is reported to be on, like the clipping itself, can be switched off at
    // runtime - useful for telling "the effect is lying to Plasma" from "Plasma is confused".
    Q_SCRIPTABLE void setScreenAttribution(bool enabled)
    {
        if (m_attributeScreen == enabled) {
            return;
        }
        m_attributeScreen = enabled;
        schedulePublishAll();
    }

    // What each clipped window is being reported as, so the attribution can be checked without
    // reading Plasma's model.
    Q_SCRIPTABLE QString screenAttributionStatus()
    {
        QString out = m_attributeScreen ? QStringLiteral("on\n") : QStringLiteral("off\n");
        for (EffectWindow *w : effects->stackingOrder()) {
            if (!w || !shouldClip(w)) {
                continue;
            }
            auto it = m_owner.constFind(w);
            if (it == m_owner.constEnd()) {
                continue;
            }
            Window *window = w->window();
            if (!window || !effects->findScreen(*it)) {
                continue;
            }
            const ClipRect honest = window->frameGeometry().toRect();
            const ClipRect shown = attributedGeometry(w);
            out += QStringLiteral("  %1 | owner %2 | real x=%3 w=%4 centre %5 | published x=%6 w=%7 centre %8%9\n")
                       .arg(w->caption().left(28), *it)
                       .arg(honest.x()).arg(honest.width()).arg(centreOf(honest.x(), honest.width()))
                       .arg(shown.x()).arg(shown.width()).arg(centreOf(shown.x(), shown.width()))
                       .arg(honest.x() == shown.x() ? QString() : QStringLiteral("  [corner moved]"));
        }
        return out;
    }

    Q_SCRIPTABLE QString inputClippingStatus()
    {
        if (m_hitTestIndex == NoIndex) {
            return QStringLiteral("unavailable (could not read the hitTest vtable slot)");
        }
        return QStringLiteral("%1, slot %2, %3 window class(es) hooked")
            .arg(m_clipInput ? QStringLiteral("on") : QStringLiteral("off"))
            .arg(m_hitTestIndex)
            .arg(s_originalHitTest.size());
    }

private:
    static bool shouldClip(EffectWindow *w)
    {
        return w->isNormalWindow()
            && !w->isDesktop() && !w->isDock()
            && !w->isPopupWindow() && !w->isTooltip()
            && !w->isOnScreenDisplay() && !w->isNotification();
    }

    /*
        Everything this effect wants to do to a window, done once when it appears.

        KWin publishes the geometry itself, from a lambda it connects to frameGeometryChanged when
        the window is created. Connecting here - necessarily later than that - means Qt calls this
        one second and the corrected rectangle is the one Plasma ends up with.
    */
    void adopt(EffectWindow *w)
    {
        hookHitTest(w);
        if (!w || !shouldClip(w)) {
            return; // can never be claimed, so its geometry cannot affect what is published
        }
        if (Window *window = w->window()) {
            // A claimed window's move changes its own attribution and can re-spread its parked
            // neighbours; an unclaimed window's move changes nothing that is published here
            // (KWin publishes its honest rectangle itself), so it is not worth a timer.
            connect(window, &Window::frameGeometryChanged, this, [this, w]() {
                if (m_owner.contains(w)) {
                    schedulePublishAll();
                }
            });
            // KWin re-derives a window's output from its geometry, and with per-output virtual
            // desktops that attribution decides which screen's desktop switch shows or hides the
            // window. A column parked past the seam must not change hands like that: it belongs
            // to its grid's screen, so the neighbour's desktop switch has to leave it alone and
            // its own screen's switch has to take it along. Re-assert the claimed owner whenever
            // KWin re-attributes.
            connect(window, &Window::outputChanged, this, [this, w]() {
                enforceAttribution(w);
            });
        }
    }

    void publishGeometry(EffectWindow *w)
    {
        if (!w) {
            return;
        }
        Window *window = w->window();
        if (!window) {
            return;
        }
        PlasmaWindowInterface *published = window->windowManagementInterface();
        if (!published) {
            return; // nothing is listening for this window
        }

        const ClipRect honest = window->frameGeometry().toRect();
        // A window being dragged is drawn whole wherever it lies, so report it honestly too.
        if (!m_attributeScreen || !shouldClip(w) || w->isUserMove() || w->isUserResize()) {
            published->setGeometry(honest);
            return;
        }
        published->setGeometry(attributedGeometry(w));
    }

    /*
        A tiler's scroll moves every window in a grid one at a time, each through its
        own synchronous setGeometry() call - not as one atomic update. Republishing immediately on
        every one of those, as this used to, meant a 4-window scroll made 4 separate trips to
        Plasma, each showing a mix of windows that had already been moved and windows that had
        not. A position-ordered task manager reorders on every trip, which is the flicker: several
        wrong intermediate orderings flashing past before the loop finishes and the last trip
        finally shows the real order.

        QTimer::singleShot(0, ...) defers the publish to the next turn of the event loop, which a
        synchronous script loop cannot reach until it returns. So every geometry change in one
        script invocation collapses into a single publish of the settled state - proven by
        counting invocations of publishAllGeometry() before and after: a 4-window scroll dropped
        from 4 calls (one per window, each a different incomplete mix) to 1.
    */
    void schedulePublishAll()
    {
        if (m_publishScheduled) {
            return;
        }
        m_publishScheduled = true;
        QTimer::singleShot(0, this, [this]() {
            m_publishScheduled = false;
            publishAllGeometry();
        });
    }

    // Every claimed window's geometry, not just the one that moved: the corners of parked
    // columns are spread relative to each other, so one column moving can change where its
    // neighbours belong. Unclaimed windows are skipped - KWin itself keeps publishing their
    // honest rectangle, and honest is exactly what this effect would say about them anyway.
    void publishAllGeometry()
    {
        const auto owned = m_owner.keys();
        for (EffectWindow *w : owned) {
            publishGeometry(w);
        }
    }

    ClipRect attributedGeometry(EffectWindow *w) const
    {
        Window *window = w->window();
        const ClipRect frame = window->frameGeometry().toRect();
        auto it = m_owner.constFind(w);
        ClipOutput *output = it == m_owner.constEnd() ? nullptr : effects->findScreen(*it);
        if (!output) {
            return frame;
        }
        const ClipRect owner = output->geometry();

        int x = frame.x();
        int y = frame.y();
        int width = frame.width();
        int height = frame.height();
        fitCentre(frame.x(), frame.width(), owner.x(), owner.x() + owner.width(), &x, &width);
        fitCentre(frame.y(), frame.height(), owner.y(), owner.y() + owner.height(), &y, &height);

        /*
            A column parked beyond its own screen's right edge is the one case width cannot solve,
            so fitCentre pins its corner to that edge - and every such column lands on the same
            pixel. A task manager ordered by window position then sees them tie and falls back to
            model order, which is what scrambles the panel once more than one column is scrolled
            away. At 100% column width that is every column but the visible one.

            Pinning them one pixel apart instead, in the order they really sit in, costs a single
            pass and keeps the column order intact. Columns parked off the LEFT edge need none of
            this: their real x is kept, so they are already distinct and already in order.
        */
        const int edge = owner.x() + owner.width() - 1;
        if (frame.x() > edge) {
            x = std::max(owner.x(), edge - parkedFurtherRight(w, *it, frame.x()));
        }
        return ClipRect(x, y, width, height);
    }

    // How many columns of the same grid are parked further right than this one.
    int parkedFurtherRight(EffectWindow *w, const QString &ownerName, int honestX) const
    {
        int later = 0;
        for (EffectWindow *other : effects->stackingOrder()) {
            if (other == w || !other || !shouldClip(other)) {
                continue;
            }
            if (m_owner.value(other) != ownerName) {
                continue;
            }
            if (Window *window = other->window()) {
                if (window->frameGeometry().toRect().x() > honestX) {
                    ++later;
                }
            }
        }
        return later;
    }

    void hookHitTest(EffectWindow *w)
    {
        if (m_hitTestIndex == NoIndex || !w) {
            return;
        }
        Window *window = w->window();
        if (!window) {
            return;
        }
        // Single inheritance all the way down from Window, so the vptr sits at offset zero.
        void **vtable = *reinterpret_cast<void ***>(window);
        if (!vtable || s_originalHitTest.contains(vtable)) {
            return;
        }
        void **slot = vtable + m_hitTestIndex;
        const auto original = reinterpret_cast<HitTestFn>(*slot);
        if (!original) {
            return;
        }
        // Record before replacing, so the hook can never run against an unrecorded vtable.
        s_originalHitTest.insert(vtable, original);
        if (!writeVtableSlot(slot, reinterpret_cast<void *>(&ClipToOutputEffect::hitTest))) {
            s_originalHitTest.remove(vtable);
            qWarning("clip2output: could not make the hitTest vtable slot %p writable; input "
                     "clipping is off for this window class", (void *)slot);
        }
    }

    /*
        The cull only has to be relaxed where this effect's clip leaves a hole that some other
        output would otherwise have filled. A frame that pokes past its own screen into dead space
        - off the outer edge of the desktop - covers nothing on any output, so leaving it opaque is
        both safe and cheaper.

        The distinction is not academic: a column can miss the far edge of the desktop by a single
        pixel (seen live at x=2531..3841 against an edge at 3840), and a plain "is it contained"
        test would put that window on the blended path for its whole life for no reason at all.
    */
    bool overhangsAnotherOutput(EffectWindow *w, ClipOutput *owner) const
    {
        const QRect frame = w->frameGeometry().toAlignedRect();
        if (owner->geometry().contains(frame)) {
            return false;
        }
        const auto outputs = effects->screens();
        for (ClipOutput *other : outputs) {
            if (other != owner && other->geometry().intersects(frame)) {
                return true;
            }
        }
        return false;
    }

    // The input-side twin of the paint clip: a window only answers for points that fall on the
    // output that owns it.
    bool acceptsInputAt(const Window *window, const QPointF &point) const
    {
        if (!m_clipInput) {
            return true;
        }
        // A window being dragged is drawn whole wherever it lies, so it must be grabbable whole.
        if (window->isInteractiveMove() || window->isInteractiveResize()) {
            return true;
        }
        EffectWindow *w = const_cast<EffectWindow *>(window->effectWindow());
        if (!w || !shouldClip(w)) {
            return true;
        }
        auto it = m_owner.constFind(w);
        if (it == m_owner.constEnd()) {
            return true; // never claimed by a grid; not ours to restrict
        }
        ClipOutput *owner = effects->findScreen(*it);
        if (!owner) {
            return true;
        }
        // Exclusive on the far edges, matching KWin's own idea of which output a point is on:
        // the seam pixel belongs to the screen on its right, not to both.
        const QRect g = owner->geometry();
        return point.x() >= g.x() && point.x() < g.x() + g.width()
            && point.y() >= g.y() && point.y() < g.y() + g.height();
    }

    void assignOwnerFromCentre(EffectWindow *w)
    {
        if (ClipOutput *o = effects->screenAt(w->frameGeometry().center().toPoint())) {
            m_owner[w] = o->name();
        }
    }

    // Pins the window's output attribution to its claimed owner. Skipped while the user is
    // dragging or resizing: KWin's interactive move tracks the cursor's output, and fighting it
    // mid-drag would break per-output scale handling; the drop reassigns the owner from the
    // window's centre anyway. Loop-safe: setOutput fires outputChanged, but the re-entered
    // handler sees the attribution already matching and does nothing.
    void enforceAttribution(EffectWindow *w)
    {
        if (!w || w->isUserMove() || w->isUserResize()) {
            return;
        }
        auto it = m_owner.constFind(w);
        if (it == m_owner.constEnd()) {
            return;
        }
        ClipOutput *owner = effects->findScreen(*it);
        Window *window = w->window();
        if (owner && window && window->output() != owner) {
            window->setOutput(owner);
        }
    }

    ClipOutput *ownerOf(EffectWindow *w)
    {
        auto it = m_owner.constFind(w);
        if (it == m_owner.constEnd()) {
            return nullptr; // never claimed by a grid; not ours to clip
        }
        ClipOutput *o = effects->findScreen(*it);
        if (!o) {
            // The owning output went away. Fail open rather than guess: KWin is about to move
            // the window onto a live screen (republishing its honest geometry as it does), and
            // The claiming client evacuates the grid and announces the new owner itself.
            m_owner.remove(w);
        }
        return o;
    }

    static ClipToOutputEffect *s_effect;

    const std::size_t m_hitTestIndex;
    bool m_clipInput = true;
    bool m_attributeScreen = true;
    bool m_publishScheduled = false;
    QHash<EffectWindow *, QString> m_owner;
    QSet<EffectWindow *> m_moving;
};

ClipToOutputEffect *ClipToOutputEffect::s_effect = nullptr;

KWIN_EFFECT_FACTORY_ENABLED(ClipToOutputEffect, "metadata.json", return false;)

} // namespace KWin

#include "clip2output.moc"
