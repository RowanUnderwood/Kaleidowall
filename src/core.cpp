#include "core.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace kaleido {
double randomRange(std::mt19937& r, double a, double b) {
    return std::uniform_real_distribution<double>(a, std::max(a, b))(r);
}
void Settings::normalize() {
    minSlots = std::clamp(minSlots, 1, 32);
    maxSlots = std::clamp(maxSlots, minSlots, 32);
    clipMin = std::clamp(clipMin, 0.5, 3600.0);
    clipMax = std::clamp(clipMax, clipMin, 3600.0);
    layoutMin = std::clamp(layoutMin, 1.0, 3600.0);
    layoutMax = std::clamp(layoutMax, layoutMin, 3600.0);
    transition = std::clamp(transition, 0.0, std::min(10.0, layoutMin));
    skipStart = std::clamp(skipStart, 0.0, skipPercent ? 99.0 : 86400.0);
    skipEnd = std::clamp(skipEnd, 0.0, skipPercent ? 99.0 : 86400.0);
    volume = std::clamp(volume, 0, 100);
    fps = std::clamp(fps, 24, 144);
    bufferMiB = std::clamp(bufferMiB, 8, 1024);
    textureLimit = std::clamp(textureLimit, 320, 3840);
    static const QRegularExpression rgbHex("^#[0-9a-fA-F]{6}$");
    if (!rgbHex.match(backgroundColor).hasMatch())
        backgroundColor = "#06080c";
    backgroundColor = backgroundColor.toLower();
    const QStringList valid = {"Split", "Grid", "Hero", "Masonry", "Circles", "Hexagons", "Honeycomb", "Inset"};
    modes.removeIf([&](const QString& v) { return !valid.contains(v); });
    modes.removeDuplicates();
    if (modes.empty())
        modes = {"Grid"};
}
QJsonObject Settings::json() const {
    QJsonObject o;
#define SAVE(x) o[#x] = x
    SAVE(minSlots);
    SAVE(maxSlots);
    SAVE(clipMin);
    SAVE(clipMax);
    SAVE(layoutMin);
    SAVE(layoutMax);
    SAVE(transition);
    SAVE(skipStart);
    SAVE(skipEnd);
    SAVE(skipPercent);
    SAVE(muted);
    SAVE(reducedMotion);
    SAVE(duplicates);
    SAVE(crop);
    SAVE(hwdec);
    SAVE(volume);
    SAVE(fps);
    SAVE(bufferMiB);
    SAVE(textureLimit);
    SAVE(backgroundColor);
#undef SAVE
    o["modes"] = QJsonArray::fromStringList(modes);
    o["weights"] = weights;
    return o;
}
Settings Settings::fromJson(const QJsonObject& o) {
    Settings s;
#define INT(x)                                                                                               \
    if (o.contains(#x))                                                                                      \
    s.x = o[#x].toInt(s.x)
#define NUM(x)                                                                                               \
    if (o.contains(#x))                                                                                      \
    s.x = o[#x].toDouble(s.x)
#define BOOL(x)                                                                                              \
    if (o.contains(#x))                                                                                      \
    s.x = o[#x].toBool(s.x)
    INT(minSlots);
    INT(maxSlots);
    INT(volume);
    INT(fps);
    INT(bufferMiB);
    INT(textureLimit);
    NUM(clipMin);
    NUM(clipMax);
    NUM(layoutMin);
    NUM(layoutMax);
    NUM(transition);
    NUM(skipStart);
    NUM(skipEnd);
    BOOL(skipPercent);
    BOOL(muted);
    BOOL(reducedMotion);
    BOOL(duplicates);
    BOOL(crop);
    BOOL(hwdec);
#undef INT
#undef NUM
#undef BOOL
    if (o.contains("modes")) {
        s.modes.clear();
        for (auto v : o["modes"].toArray())
            s.modes << v.toString();
    }
    s.weights = o["weights"].toObject();
    s.backgroundColor = o["backgroundColor"].toString(s.backgroundColor);
    s.normalize();
    return s;
}
static std::pair<double, double> margins(const Video& v, const Settings& s) {
    const double unit = s.skipPercent ? v.duration / 100.0 : 1.0;
    return {v.skipStart >= 0 ? v.skipStart : s.skipStart * unit,
            v.skipEnd >= 0 ? v.skipEnd : s.skipEnd * unit};
}
QString eligibilityReason(const Video& v, const Settings& s) {
    if (!v.enabled)
        return "Excluded";
    if (!v.folderEnabled)
        return "Folder disabled";
    if (v.missing)
        return "File missing";
    if (!v.error.isEmpty())
        return v.error;
    if (!std::isfinite(v.duration) || v.duration <= 0)
        return "Unknown duration";
    auto [a, b] = margins(v, s);
    if (v.duration - a - b < s.clipMin)
        return "Too short after beginning/end exclusions";
    return {};
}
std::optional<Clip> chooseClip(const Video& v, const Settings& s, std::mt19937& rng) {
    if (!eligibilityReason(v, s).isEmpty())
        return {};
    auto [a, b] = margins(v, s);
    double len = randomRange(rng, s.clipMin, std::min(s.clipMax, v.duration - a - b));
    return Clip{randomRange(rng, a, std::max(a, v.duration - b - len)), len};
}
void ShuffleBag::reconcile(const QStringList& eligible) {
    QSet<QString> ids(eligible.begin(), eligible.end());
    remaining.removeIf([&](const QString& id) { return !ids.contains(id); });
    used.intersect(ids);
    reserved.intersect(ids);
    for (const auto& id : eligible)
        if (!pool.contains(id) && !remaining.contains(id) && !used.contains(id))
            remaining << id;
    pool = eligible;
    pool.removeDuplicates();
    // Random insertion order for new files; does not reintroduce consumed files.
    std::shuffle(remaining.begin(), remaining.end(), rng);
}
std::optional<QString> ShuffleBag::reserve(const QSet<QString>& active, bool duplicates) {
    if (pool.empty())
        return {};
    if (remaining.empty() && reserved.empty()) {
        remaining = pool;
        used.clear();
        ++cycleNumber;
        std::shuffle(remaining.begin(), remaining.end(), rng);
    }
    for (const auto& id : remaining)
        if (!reserved.contains(id) && (duplicates || !active.contains(id))) {
            reserved.insert(id);
            return id;
        }
    return {};
}
void ShuffleBag::commit(const QString& id) {
    reserved.remove(id);
    remaining.removeAll(id);
    used.insert(id);
}
void ShuffleBag::cancel(const QString& id) {
    reserved.remove(id);
}
QJsonObject ShuffleBag::json() const {
    return {{"pool", QJsonArray::fromStringList(pool)},
            {"remaining", QJsonArray::fromStringList(remaining)},
            {"used", QJsonArray::fromStringList(used.values())},
            {"cycle", cycleNumber}};
}
void ShuffleBag::restore(const QJsonObject& o) {
    pool.clear();
    remaining.clear();
    used.clear();
    reserved.clear();
    for (auto a : o["pool"].toArray())
        pool << a.toString();
    for (auto a : o["remaining"].toArray())
        remaining << a.toString();
    for (auto a : o["used"].toArray())
        used.insert(a.toString());
    cycleNumber = std::max(1, o["cycle"].toInt(1));
}
QString pickMode(const Settings& s, std::mt19937& rng, const QString& previous) {
    QStringList choices;
    std::vector<double> weights;
    for (const auto& mode : s.modes)
        if (s.modes.size() == 1 || mode != previous) {
            choices << mode;
            weights.push_back(std::max(0.01, s.weights.value(mode).toDouble(1)));
        }
    if (choices.empty())
        return "Grid";
    return choices[int(std::discrete_distribution<size_t>(weights.begin(), weights.end())(rng))];
}
QString resolveLayoutMode(const QString& mode, int count, std::mt19937& rng) {
    if (mode == "Inset") {
        const QStringList shapes = {"Circles", "Hexagons", "Honeycomb"};
        return "Inset " + shapes[std::uniform_int_distribution<int>(0, count >= 4 ? 2 : 1)(rng)];
    }
    return mode == "Honeycomb" && count < 3 ? QString("Hexagons") : mode;
}
QVector<QRectF> makeLayout(const QString& mode, int n, double aspect, std::mt19937& rng) {
    if (n <= 0)
        return {};
    if (n == 1)
        return {{0, 0, 1, 1}};
    QVector<QRectF> result;
    if (mode.startsWith("Inset ")) {
        result << QRectF(0, 0, 1, 1);
        result += makeLayout(mode.mid(6), n - 1, aspect, rng);
    } else if (mode == "Honeycomb" && n >= 3) {
        // Flat-top hexagons of radius 1: column spacing 1.5, row spacing sqrt(3).
        // Try balanced column counts and keep the arrangement with the largest tiles.
        const double h = std::sqrt(3.0);
        double bestScale = 0;
        for (int cols = 1; cols <= n; ++cols) {
            QVector<QRectF> candidate;
            QRectF bounds;
            for (int x = 0; x < cols; ++x) {
                const int rows = n / cols + (x < n % cols ? 1 : 0);
                for (int y = 0; y < rows; ++y) {
                    QRectF r(1.5 * x, h * (y + 0.5 * (x % 2)), 2, h);
                    candidate << r;
                    bounds = bounds.united(r);
                }
            }
            const double scale = std::min(aspect / bounds.width(), 1.0 / bounds.height());
            if (scale <= bestScale)
                continue;
            bestScale = scale;
            result.clear();
            const double left = (aspect - bounds.width() * scale) / 2;
            const double top = (1 - bounds.height() * scale) / 2;
            for (const auto& r : candidate)
                result << QRectF((left + r.x() * scale) / aspect, top + r.y() * scale,
                                 r.width() * scale / aspect, r.height() * scale);
        }
    } else if (mode == "Split") {
        bool vertical = aspect >= 1;
        for (int i = 0; i < n; ++i)
            result << (vertical ? QRectF(double(i) / n, 0, 1.0 / n, 1)
                                : QRectF(0, double(i) / n, 1, 1.0 / n));
    } else if (mode == "Hero") {
        result << QRectF(0, 0, 0.5, 1);
        auto rest = makeLayout("Grid", n - 1, aspect * 0.5, rng);
        for (auto r : rest)
            result << QRectF(0.5 + r.x() * 0.5, r.y(), r.width() * 0.5, r.height());
    } else if (mode == "Masonry") {
        result << QRectF(0, 0, 1, 1);
        while (result.size() < n) {
            auto it = std::max_element(result.begin(), result.end(), [](auto a, auto b) {
                return a.width() * a.height() < b.width() * b.height();
            });
            QRectF r = *it;
            result.erase(it);
            double f = randomRange(rng, 0.32, 0.68);
            if (r.width() * aspect > r.height()) {
                result << QRectF(r.x(), r.y(), r.width() * f, r.height())
                       << QRectF(r.x() + r.width() * f, r.y(), r.width() * (1 - f), r.height());
            } else {
                result << QRectF(r.x(), r.y(), r.width(), r.height() * f)
                       << QRectF(r.x(), r.y() + r.height() * f, r.width(), r.height() * (1 - f));
            }
        }
    } else {
        int cols = std::clamp(int(std::ceil(std::sqrt(n * aspect))), 1, n), rows = (n + cols - 1) / cols;
        int placed = 0;
        for (int y = 0; y < rows; ++y) {
            int inRow = std::min(cols, n - placed);
            for (int x = 0; x < inRow; ++x) {
                result << QRectF(double(x) / inRow, double(y) / rows, 1.0 / inRow, 1.0 / rows);
                ++placed;
            }
        }
    }
    return result;
}
} // namespace kaleido
