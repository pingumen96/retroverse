#include <rv/audio/synth.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace rv::audio {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTau = 2.0 * kPi;

/// lowbias32: the noise's hash.
constexpr std::uint32_t hash(std::uint32_t x) noexcept {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/// A uniform value in [0, 1) from (seed, index).
double unit(std::uint32_t seed, std::uint32_t index) noexcept {
    return static_cast<double>(hash(seed ^ hash(index))) / 4294967296.0;
}

/// White noise in [-1, 1).
double white(std::uint32_t seed, std::uint32_t index) noexcept {
    return 2.0 * unit(seed, index) - 1.0;
}

/// A one-pole low-pass at `cutoff` hertz.
struct LowPass {
    double a = 1.0;
    double y = 0.0;

    LowPass(double cutoff, int rate) : a(1.0 - std::exp(-kTau * cutoff / static_cast<double>(rate))) {}
    double step(double x) noexcept {
        y += a * (x - y);
        return y;
    }
};

/// A band-pass biquad (RBJ, 0 dB at its centre) at `centre` hertz, quality `q`.
struct BandPass {
    double b0 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

    BandPass(double centre, double q, int rate) {
        const double w0 = kTau * centre / static_cast<double>(rate);
        const double alpha = std::sin(w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        b0 = alpha / a0;
        b2 = -alpha / a0;
        a1 = -2.0 * std::cos(w0) / a0;
        a2 = (1.0 - alpha) / a0;
    }
    double step(double x) noexcept {
        const double y = b0 * x + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};

std::size_t lengthOf(SoundId sound, int rate) {
    return static_cast<std::size_t>(std::llround(soundSeconds(sound) * static_cast<double>(rate)));
}

/// The frequency nearest `frequency` that completes a whole number of cycles in `length` samples (a loop's tones).
double wholeCycles(double frequency, std::size_t length, int rate) {
    const double cycles =
        std::max(1.0, std::round(frequency * static_cast<double>(length) / static_cast<double>(rate)));
    return cycles * static_cast<double>(rate) / static_cast<double>(length);
}

/// Runs `sample(i)` (a filtered noise, say) round the loop twice and keeps the second turn: its filters' state at the
/// first sample is the one the last left, so the loop flows on at its seam.
template <typename Sample>
std::vector<double> aroundTheLoop(std::size_t length, Sample&& sample) {
    std::vector<double> out(length, 0.0);
    for (int turn = 0; turn < 2; ++turn) {
        for (std::size_t i = 0; i < length; ++i) {
            out[i] = sample(i);
        }
    }
    return out;
}

/// An exponential decay of time constant `tau` seconds, `t` seconds after its start (0 before).
double decay(double t, double tau) noexcept {
    return t < 0.0 ? 0.0 : std::exp(-t / tau);
}

/// A short noise impulse into `bands` (centre, q, weight) and a low sine thump: what a click or a step is made of,
/// added into `out` from sample `start`.
struct Impact {
    double start = 0.0; ///< seconds
    double amplitude = 1.0;
    double noiseTau = 0.001;
    double lowPass = 8000.0;
    double bodyHz = 0.0; ///< 0: none
    double bodyTau = 0.02;
    double body = 0.0;
};

void addImpact(std::vector<double>& out, const Impact& impact, std::uint32_t seed, int rate,
               std::initializer_list<std::array<double, 3>> bands) {
    std::vector<BandPass> filters;
    std::vector<double> weights;
    for (const auto& band : bands) {
        filters.emplace_back(band[0], band[1], rate);
        weights.push_back(band[2]);
    }
    LowPass low(impact.lowPass, rate);
    const auto first = static_cast<std::size_t>(std::llround(impact.start * static_cast<double>(rate)));
    for (std::size_t i = first; i < out.size(); ++i) {
        const double t = static_cast<double>(i - first) / static_cast<double>(rate);
        const double attack = std::min(1.0, t / 0.0005);
        const double excitation = white(seed, static_cast<std::uint32_t>(i)) * attack * decay(t, impact.noiseTau);
        double value = filters.empty() ? low.step(excitation) : 0.0;
        if (!filters.empty()) {
            const double lowered = low.step(excitation);
            for (std::size_t f = 0; f < filters.size(); ++f) {
                value += weights[f] * filters[f].step(lowered);
            }
        }
        if (impact.bodyHz > 0.0) {
            value +=
                impact.body * std::sin(kTau * impact.bodyHz * t) * std::min(1.0, t / 0.001) * decay(t, impact.bodyTau);
        }
        out[i] += impact.amplitude * value;
    }
}

std::vector<double> crtHum(std::size_t n, std::uint32_t seed, int rate) {
    struct Partial {
        double hz;
        double gain;
    };
    constexpr Partial kPartials[] = {{50.0, 1.0}, {100.0, 0.55}, {150.0, 0.30}, {200.0, 0.15}, {250.0, 0.08}};
    const double beat = wholeCycles(0.5, n, rate);
    LowPass rumble(120.0, rate);
    return aroundTheLoop(n, [&](std::size_t i) {
        const double t = static_cast<double>(i) / static_cast<double>(rate);
        double value = 0.0;
        std::uint32_t k = 0;
        for (const Partial& partial : kPartials) {
            const double phase = kTau * unit(seed, 1000u + k++);
            value += partial.gain * std::sin(kTau * wholeCycles(partial.hz, n, rate) * t + phase);
        }
        value *= 1.0 + 0.15 * std::sin(kTau * beat * t);
        return value + 0.6 * rumble.step(white(seed, static_cast<std::uint32_t>(i)));
    });
}

std::vector<double> phosphorHiss(std::size_t n, std::uint32_t seed, int rate) {
    LowPass below(2000.0, rate);
    LowPass top(9000.0, rate);
    return aroundTheLoop(n, [&](std::size_t i) {
        const double x = white(seed, static_cast<std::uint32_t>(i));
        return top.step(x - below.step(x));
    });
}

std::vector<double> neonBuzz(std::size_t n, std::uint32_t seed, int rate) {
    const double hz = wholeCycles(100.0, n, rate);
    const double phase = kTau * unit(seed, 7u);
    LowPass soften(4000.0, rate);
    std::vector<double> out = aroundTheLoop(n, [&](std::size_t i) {
        const double t = static_cast<double>(i) / static_cast<double>(rate);
        const double tone =
            std::tanh(3.0 * std::sin(kTau * hz * t + phase)) * 0.6 + 0.25 * std::sin(2.0 * kTau * hz * t);
        return soften.step(tone);
    });
    // A few crackles, inside the loop and clear of its seam.
    for (std::uint32_t k = 0; k < 5; ++k) {
        Impact crackle;
        crackle.start = (0.05 + 0.85 * unit(seed, 100u + k)) * static_cast<double>(n) / static_cast<double>(rate);
        crackle.amplitude = 0.25 + 0.2 * unit(seed, 200u + k);
        crackle.noiseTau = 0.0006;
        crackle.lowPass = 6000.0;
        std::vector<double> burst(n, 0.0);
        addImpact(burst, crackle, seed + 300u + k, rate, {});
        const auto end = std::min(n, static_cast<std::size_t>((crackle.start + 0.03) * static_cast<double>(rate)));
        for (std::size_t i = 0; i < end; ++i) {
            out[i] += burst[i];
        }
    }
    return out;
}

std::vector<double> roomMurmur(std::size_t n, std::uint32_t seed, int rate) {
    LowPass low(250.0, rate);
    BandPass voices(600.0, 0.7, rate);
    const double swell = wholeCycles(0.25, n, rate);
    const double ripple = wholeCycles(0.75, n, rate);
    return aroundTheLoop(n, [&](std::size_t i) {
        const double t = static_cast<double>(i) / static_cast<double>(rate);
        const double x = white(seed, static_cast<std::uint32_t>(i));
        const double body = 2.5 * low.step(x) + 0.8 * voices.step(x);
        return body * (1.0 + 0.25 * std::sin(kTau * swell * t) + 0.12 * std::sin(kTau * ripple * t + 1.3));
    });
}

std::vector<double> clockTick(std::size_t n, std::uint32_t seed, int rate) {
    std::vector<double> out(n, 0.0);
    const double seconds = static_cast<double>(n) / static_cast<double>(rate);
    Impact tick;
    tick.start = 0.25 * seconds;
    tick.noiseTau = 0.0008;
    tick.bodyHz = 900.0;
    tick.bodyTau = 0.004;
    tick.body = 0.3;
    addImpact(out, tick, seed, rate, {{3200.0, 8.0, 1.0}});
    Impact tock = tick;
    tock.start = 0.75 * seconds;
    tock.amplitude = 0.85;
    addImpact(out, tock, seed + 1u, rate, {{2500.0, 8.0, 1.0}});
    return out;
}

std::vector<double> footstep(std::size_t n, std::uint32_t seed, int rate, double lowPass, double tau, double bodyHz,
                             double body, double knock) {
    std::vector<double> out(n, 0.0);
    Impact heel;
    heel.noiseTau = tau;
    heel.lowPass = lowPass;
    heel.bodyHz = bodyHz;
    heel.bodyTau = 2.0 * tau;
    heel.body = body;
    addImpact(out, heel, seed, rate, {{lowPass * 0.35, 1.5, 1.0}, {1200.0, 3.0, knock}});
    Impact toe = heel;
    toe.start = 0.045 + 0.025 * unit(seed, 11u);
    toe.amplitude = 0.55 + 0.15 * unit(seed, 12u);
    addImpact(out, toe, seed + 1u, rate, {{lowPass * 0.4, 1.5, 1.0}, {1200.0, 3.0, knock}});
    return out;
}

std::vector<double> itemClack(std::size_t n, std::uint32_t seed, int rate) {
    std::vector<double> out(n, 0.0);
    Impact click;
    click.noiseTau = 0.0004;
    addImpact(out, click, seed, rate, {{2400.0, 4.0, 1.0}, {5200.0, 6.0, 0.5}});
    Impact second = click;
    second.start = 0.028;
    second.amplitude = 0.6;
    addImpact(out, second, seed + 1u, rate, {{2200.0, 4.0, 1.0}, {4800.0, 6.0, 0.4}});
    return out;
}

std::vector<double> caseOpen(std::size_t n, std::uint32_t seed, int rate) {
    std::vector<double> out(n, 0.0);
    Impact latch;
    latch.noiseTau = 0.0005;
    addImpact(out, latch, seed, rate, {{3500.0, 5.0, 1.0}});
    // The hinge: soft low noise swelling and fading over the lid's swing.
    LowPass hinge(800.0, rate);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(rate);
        const double swing = t < 0.05 || t > 0.3 ? 0.0 : std::sin(kPi * (t - 0.05) / 0.25);
        out[i] += 0.6 * swing * hinge.step(white(seed + 5u, static_cast<std::uint32_t>(i)));
    }
    Impact stop;
    stop.start = 0.3;
    stop.amplitude = 0.7;
    stop.noiseTau = 0.0008;
    stop.bodyHz = 140.0;
    stop.bodyTau = 0.03;
    stop.body = 0.4;
    addImpact(out, stop, seed + 2u, rate, {{1800.0, 3.0, 1.0}});
    return out;
}

std::vector<double> insertionClick(std::size_t n, std::uint32_t seed, int rate) {
    std::vector<double> out(n, 0.0);
    BandPass slide(2000.0, 1.0, rate);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(rate);
        const double rise = t < 0.08 ? t / 0.08 : 0.0;
        out[i] += 0.3 * rise * slide.step(white(seed + 9u, static_cast<std::uint32_t>(i)));
    }
    Impact click;
    click.start = 0.09;
    click.noiseTau = 0.0005;
    click.bodyHz = 95.0;
    click.bodyTau = 0.04;
    click.body = 0.6;
    addImpact(out, click, seed, rate, {{2800.0, 5.0, 1.0}});
    return out;
}

std::vector<double> uiTick(std::size_t n, int rate) {
    std::vector<double> out(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(rate);
        const double envelope = std::min(1.0, t / 0.0005) * decay(t, 0.005);
        out[i] = envelope * (std::sin(kTau * 2600.0 * t) + 0.2 * std::sin(kTau * 5200.0 * t));
    }
    return out;
}

} // namespace

std::string_view toString(SoundId sound) noexcept {
    switch (sound) {
    case SoundId::CrtHum:
        return "crt-hum";
    case SoundId::PhosphorHiss:
        return "phosphor-hiss";
    case SoundId::NeonBuzz:
        return "neon-buzz";
    case SoundId::RoomMurmur:
        return "room-murmur";
    case SoundId::ClockTick:
        return "clock-tick";
    case SoundId::FootstepParquet:
        return "footstep-parquet";
    case SoundId::FootstepRug:
        return "footstep-rug";
    case SoundId::FootstepRunner:
        return "footstep-runner";
    case SoundId::ItemClack:
        return "item-clack";
    case SoundId::CaseOpen:
        return "case-open";
    case SoundId::InsertionClick:
        return "insertion-click";
    case SoundId::UiTick:
        return "ui-tick";
    }
    return "unknown";
}

bool isLoop(SoundId sound) noexcept {
    return sound == SoundId::CrtHum || sound == SoundId::PhosphorHiss || sound == SoundId::NeonBuzz ||
           sound == SoundId::RoomMurmur || sound == SoundId::ClockTick;
}

double soundSeconds(SoundId sound) noexcept {
    switch (sound) {
    case SoundId::CrtHum:
    case SoundId::PhosphorHiss:
    case SoundId::NeonBuzz:
    case SoundId::ClockTick:
        return 2.0;
    case SoundId::RoomMurmur:
        return 4.0;
    case SoundId::FootstepParquet:
    case SoundId::FootstepRug:
    case SoundId::FootstepRunner:
        return 0.22;
    case SoundId::ItemClack:
        return 0.12;
    case SoundId::CaseOpen:
        return 0.40;
    case SoundId::InsertionClick:
        return 0.25;
    case SoundId::UiTick:
        return 0.03;
    }
    return 0.0;
}

PcmBuffer synthesize(SoundId sound, std::uint32_t seed, int sampleRate) {
    const int rate = std::max(sampleRate, 8000);
    const std::size_t n = std::max<std::size_t>(lengthOf(sound, rate), 1);
    const std::uint32_t mixed = hash(seed ^ (0x9e3779b9u * (static_cast<std::uint32_t>(sound) + 1u)));
    std::vector<double> samples;
    switch (sound) {
    case SoundId::CrtHum:
        samples = crtHum(n, mixed, rate);
        break;
    case SoundId::PhosphorHiss:
        samples = phosphorHiss(n, mixed, rate);
        break;
    case SoundId::NeonBuzz:
        samples = neonBuzz(n, mixed, rate);
        break;
    case SoundId::RoomMurmur:
        samples = roomMurmur(n, mixed, rate);
        break;
    case SoundId::ClockTick:
        samples = clockTick(n, mixed, rate);
        break;
    case SoundId::FootstepParquet:
        samples = footstep(n, mixed, rate, 3500.0, 0.012, 160.0, 0.5, 0.35);
        break;
    case SoundId::FootstepRug:
        samples = footstep(n, mixed, rate, 500.0, 0.030, 90.0, 0.3, 0.0);
        break;
    case SoundId::FootstepRunner:
        samples = footstep(n, mixed, rate, 1500.0, 0.018, 120.0, 0.4, 0.1);
        break;
    case SoundId::ItemClack:
        samples = itemClack(n, mixed, rate);
        break;
    case SoundId::CaseOpen:
        samples = caseOpen(n, mixed, rate);
        break;
    case SoundId::InsertionClick:
        samples = insertionClick(n, mixed, rate);
        break;
    case SoundId::UiTick:
        samples = uiTick(n, rate);
        break;
    }
    samples.resize(n, 0.0);
    double peak = 0.0;
    for (const double value : samples) {
        peak = std::isfinite(value) ? std::max(peak, std::abs(value)) : peak;
    }
    PcmBuffer buffer;
    buffer.sampleRate = rate;
    buffer.loop = isLoop(sound);
    buffer.samples.resize(n, 0.0f);
    const double scale = peak > 0.0 ? kSynthPeak / peak : 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double value = std::isfinite(samples[i]) ? samples[i] * scale : 0.0;
        buffer.samples[i] = static_cast<float>(std::clamp(value, -kSynthPeak, kSynthPeak));
    }
    return buffer;
}

} // namespace rv::audio
