#include <rv/ui/hint_bar_view.hpp>

#include <array>
#include <functional>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/StringUtilities.h>
#include <spdlog/spdlog.h>

#include <rv/ui/hint_bar.hpp>
#include <rv/ui/ui_view.hpp>

namespace rv::ui {
namespace {

constexpr std::array<const char*, 4> kColours = {"pad-a", "pad-b", "pad-x", "pad-y"};

[[nodiscard]] Rml::String attributeName() {
    return Rml::String(hint_bar::kActionAttribute);
}

void forEachHint(Rml::Element& root, const std::function<void(Rml::Element&)>& visit) {
    if (root.HasAttribute(attributeName())) {
        visit(root);
    }
    for (int index = 0; index < root.GetNumChildren(); ++index) {
        if (Rml::Element* child = root.GetChild(index)) {
            forEachHint(*child, visit);
        }
    }
}

/// What hides with the hint: its nearest `.hint` or `.hint-prose`, else the glyph itself.
[[nodiscard]] Rml::Element& hintOf(Rml::Element& glyph) {
    for (Rml::Element* at = &glyph; at != nullptr; at = at->GetParentNode()) {
        if (at->IsClassSet(Rml::String(hint_bar::kHintClass)) || at->IsClassSet(Rml::String(hint_bar::kProseClass))) {
            return *at;
        }
    }
    return glyph;
}

void fill(Rml::Element& element, const HintBar& bar) {
    const Rml::String actions = element.GetAttribute<Rml::String>(attributeName(), "");
    const HintGlyph glyph = bar.glyph(actions);
    hintOf(element).SetClass(Rml::String(hint_bar::kUnboundClass), !glyph.bound);
    element.SetInnerRML(Rml::StringUtilities::EncodeRml(Rml::String(glyph.text)));
    element.SetClass(Rml::String(hint_bar::kWideClass), glyph.wide);
    for (const char* colour : kColours) {
        element.SetClass(colour, glyph.colour == colour);
    }
}

} // namespace

void HintBarView::sync(const HintBar& bar) {
    Rml::Context& context = m_view.context();
    std::vector<Rml::ElementDocument*> documents;
    documents.reserve(static_cast<std::size_t>(context.GetNumDocuments()));
    for (int index = 0; index < context.GetNumDocuments(); ++index) {
        documents.push_back(context.GetDocument(index));
    }
    if (m_revision == bar.revision() && documents == m_documents) {
        return;
    }
    m_revision = bar.revision();
    m_documents = std::move(documents);
    for (Rml::ElementDocument* document : m_documents) {
        forEachHint(*document, [&](Rml::Element& element) { fill(element, bar); });
    }
}

std::vector<HintShown> HintBarView::shown() const {
    std::vector<HintShown> hints;
    for (Rml::ElementDocument* document : m_documents) {
        if (!document->IsVisible()) {
            continue;
        }
        forEachHint(*document, [&](Rml::Element& element) {
            if (element.IsVisible(true)) {
                hints.push_back(HintShown{element.GetAttribute<Rml::String>(attributeName(), ""),
                                          std::string(element.GetInnerRML())});
            }
        });
    }
    return hints;
}

std::uint32_t HintBarView::outside() const {
    std::uint32_t outside = 0;
    for (Rml::ElementDocument* document : m_documents) {
        if (!document->IsVisible()) {
            continue;
        }
        forEachHint(*document, [&](Rml::Element& element) {
            Rml::Element& hint = hintOf(element);
            Rml::Element* box = hint.GetParentNode();
            if (!element.IsVisible(true) || box == nullptr || &hint == &element) {
                return;
            }
            const float right = hint.GetAbsoluteLeft() + hint.GetOffsetWidth();
            const float bottom = hint.GetAbsoluteTop() + hint.GetOffsetHeight();
            if (right > box->GetAbsoluteLeft() + box->GetOffsetWidth() + 1.0f ||
                bottom > box->GetAbsoluteTop() + box->GetOffsetHeight() + 1.0f) {
                spdlog::warn("A hint runs past its box: {}", element.GetAttribute<Rml::String>(attributeName(), ""));
                ++outside;
            }
        });
    }
    return outside;
}

} // namespace rv::ui
