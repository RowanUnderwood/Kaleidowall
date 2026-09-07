#include "export_timeline.h"
#include "compositor.h"
#include <algorithm>

namespace kaleido {
ExportTimeline::ExportTimeline(Settings settings, QVector<Video> media, unsigned seed, double aspect)
    : config(settings), bag(seed), rng(seed), aspect(aspect) {
    config.normalize();
    QStringList ids;
    for (const auto& v : media)
        if (eligibilityReason(v, config).isEmpty()) {
            videos[v.id] = v;
            ids << v.id;
        }
    ids.sort();
    bag.reconcile(ids);
    if (ids.isEmpty())
        error = "No eligible videos. Add videos or adjust clip bounds before exporting.";
}
double ExportTimeline::progress() const {
    return easedProgress(now, transitionStart, transitionDuration);
}
bool ExportTimeline::replace(ExportSlot& slot) {
    QSet<QString> occupied;
    for (const auto& other : active)
        if (other.serial != slot.serial && other.stream)
            occupied.insert(other.video.id);
    const auto id = bag.reserve(occupied, config.duplicates);
    if (!id)
        return false;
    const auto clip = chooseClip(videos[*id], config, rng);
    if (!clip) {
        bag.cancel(*id);
        return false;
    }
    slot.video = videos[*id];
    slot.clip = *clip;
    slot.begins = now;
    slot.stream = nextStream++;
    const double skip = slot.video.skipEnd >= 0
                            ? slot.video.skipEnd
                            : config.skipEnd * (config.skipPercent ? slot.video.duration / 100.0 : 1.0);
    slot.usableEnd = slot.video.duration - skip;
    bag.commit(*id);
    return true;
}
void ExportTimeline::layout() {
    std::erase_if(active, [](const auto& s) { return s.retiring; });
    const int cap = config.duplicates ? config.maxSlots : std::min(config.maxSlots, int(videos.size()));
    if (!cap)
        return;
    const int count = std::uniform_int_distribution<int>(std::min(config.minSlots, cap), cap)(rng);
    oldMask = newMask;
    mode = pickMode(config, rng, mode);
    const auto resolved = resolveLayoutMode(mode, count, rng);
    newMask = count == 1 && mode != "Inset" ? 0 : maskKind(resolved);
    const auto rects = makeLayout(resolved, count, aspect, rng);
    for (auto& slot : active) {
        slot.from = slot.target;
        slot.opacityFrom = 1;
    }
    while (int(active.size()) < count) {
        ExportSlot slot;
        slot.serial = nextSerial++;
        slot.from = slot.target = rects[int(active.size())];
        active.push_back(slot);
    }
    for (int i = 0; i < int(active.size()); ++i) {
        auto& slot = active[i];
        slot.retiring = i >= count;
        slot.opacityTarget = slot.retiring ? 0 : 1;
        if (!slot.retiring)
            slot.target = rects[i];
    }
    transitionStart = now;
    transitionDuration = config.reducedMotion ? 0 : config.transition;
    nextLayout = now + randomRange(rng, config.layoutMin, config.layoutMax);
}
void ExportTimeline::advance(double time) {
    now = time;
    if (!error.isEmpty())
        return;
    if (now >= nextLayout)
        layout();
    if (progress() >= 1)
        std::erase_if(active, [](const auto& s) { return s.retiring; });
    for (auto& slot : active)
        if (!slot.retiring && (!slot.stream || now + 1e-9 >= slot.begins + slot.clip.length))
            replace(slot);
    routeAudio();
}
const ExportSlot* ExportTimeline::audioSlot() const {
    for (const auto& slot : active)
        if (slot.serial == audioSerial && !slot.retiring && slot.stream && slot.video.audio)
            return &slot;
    return nullptr;
}
void ExportTimeline::routeAudio() {
    if (audioSlot())
        return;
    QVector<int> candidates;
    for (const auto& slot : active)
        if (!slot.retiring && slot.stream && slot.video.audio)
            candidates << slot.serial;
    audioSerial = candidates.empty()
                      ? -1
                      : candidates[std::uniform_int_distribution<int>(0, int(candidates.size()) - 1)(rng)];
}
} // namespace kaleido
