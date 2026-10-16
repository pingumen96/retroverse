#include <rv/esde/find_rules.hpp>

#include <algorithm>
#include <array>
#include <utility>

#include <windows.h>

#include <fmt/format.h>

#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/discovery.hpp>

#include "xml.hpp"

namespace rv::esde {
namespace {

constexpr std::string_view kBundledFindRules = "systems/windows/es_find_rules.xml";

constexpr std::array<std::pair<std::string_view, FindRuleType>, 5> kRuleTypes{{
    {"winregistrypath", FindRuleType::WinRegistryPath},
    {"winregistryvalue", FindRuleType::WinRegistryValue},
    {"systempath", FindRuleType::SystemPath},
    {"staticpath", FindRuleType::StaticPath},
    {"corepath", FindRuleType::CorePath},
}};

catalog::Issue issueOf(catalog::IssueSeverity severity, std::string code, std::string file, std::string reason) {
    return catalog::Issue{severity, std::move(code), {}, std::move(file), std::move(reason)};
}

/// The <emulator> or <core> elements of a <ruleList> into `sets`, ES-DE's way: a name already defined is skipped (by
/// the custom file: replaced; by this file: repeated), rules of another type than the kind reads are skipped.
void readSets(const pugi::xml_node ruleList, bool cores, ConfigOrigin origin, const std::string& file,
              std::vector<FindRuleSet>& sets, std::vector<catalog::Issue>& issues) {
    const char* const element = cores ? "core" : "emulator";
    for (const pugi::xml_node node : ruleList.children(element)) {
        const std::string name = node.attribute("name").value();
        if (name.empty()) {
            issues.push_back(issueOf(catalog::IssueSeverity::Warning, "find-rule-skipped", file,
                                     fmt::format("an <{}> has no name: ES-DE skips it", element)));
            continue;
        }
        const auto existing =
            std::find_if(sets.begin(), sets.end(), [&](const FindRuleSet& set) { return set.name == name; });
        if (existing != sets.end()) {
            if (existing->origin == origin) {
                issues.push_back(issueOf(catalog::IssueSeverity::Warning, "find-rule-repeated", file,
                                         fmt::format("the {} {} is defined again: ES-DE keeps the first definition",
                                                     element, name)));
            } else {
                issues.push_back(issueOf(catalog::IssueSeverity::Info, "find-rule-replaced", file,
                                         fmt::format("the custom {} {} replaces the bundled one", element, name)));
            }
            continue;
        }
        FindRuleSet set;
        set.name = name;
        set.origin = origin;
        for (const pugi::xml_node rule : node.children("rule")) {
            const std::string_view typeText = rule.attribute("type").value();
            const auto type = parseFindRuleType(typeText);
            if (!type || (*type == FindRuleType::CorePath) != cores) {
                issues.push_back(issueOf(
                    catalog::IssueSeverity::Warning, "find-rule-skipped", file,
                    typeText.empty()
                        ? fmt::format("a rule of the {} {} has no type: ES-DE skips it", element, name)
                        : fmt::format("the rule type \"{}\" is none ES-DE reads for the {} {} on Windows: it skips it",
                                      typeText, element, name)));
                continue;
            }
            FindRule read;
            read.type = *type;
            for (const pugi::xml_node entry : rule.children("entry")) {
                read.entries.emplace_back(entry.text().get());
            }
            set.rules.push_back(std::move(read));
        }
        sets.push_back(std::move(set));
    }
}

void readFindRulesFile(const std::filesystem::path& path, ConfigOrigin origin, FindRules& rules) {
    const std::string file = core::pathToUtf8Lossy(path);
    auto document = detail::loadXmlFile(path, kMaxFindRulesBytes);
    if (!document) {
        rules.issues.push_back(issueOf(catalog::IssueSeverity::Error, "find-rules-unreadable", file,
                                       fmt::format("es_find_rules.xml cannot be read ({}): ES-DE skips it",
                                                   document.error().message)));
        return;
    }
    const pugi::xml_node ruleList = document->topLevel("ruleList");
    if (!ruleList) {
        rules.issues.push_back(issueOf(catalog::IssueSeverity::Error, "find-rules-unreadable", file,
                                       "es_find_rules.xml has no <ruleList> element: ES-DE skips it"));
        return;
    }
    rules.files.push_back(path);
    readSets(ruleList, false, origin, file, rules.emulators, rules.issues);
    readSets(ruleList, true, origin, file, rules.cores, rules.issues);
}

const FindRuleSet* findByName(const std::vector<FindRuleSet>& sets, std::string_view name) noexcept {
    const auto found = std::lower_bound(sets.begin(), sets.end(), name, [](const FindRuleSet& set,
                                                                          std::string_view wanted) {
        return std::string_view(set.name) < wanted;
    });
    return found != sets.end() && found->name == name ? &*found : nullptr;
}

} // namespace

std::string_view toString(FindRuleType type) noexcept {
    for (const auto& [name, value] : kRuleTypes) {
        if (value == type) {
            return name;
        }
    }
    return "staticpath";
}

std::optional<FindRuleType> parseFindRuleType(std::string_view text) noexcept {
    for (const auto& [name, value] : kRuleTypes) {
        if (name == text) {
            return value;
        }
    }
    return std::nullopt;
}

std::vector<std::string> FindRuleSet::entries(FindRuleType type) const {
    std::vector<std::string> all;
    for (const FindRule& rule : rules) {
        if (rule.type == type) {
            all.insert(all.end(), rule.entries.begin(), rule.entries.end());
        }
    }
    return all;
}

const FindRuleSet* FindRules::findEmulator(std::string_view name) const noexcept {
    return findByName(emulators, name);
}

const FindRuleSet* FindRules::findCore(std::string_view name) const noexcept {
    return findByName(cores, name);
}

std::size_t FindRules::entryCount() const noexcept {
    std::size_t count = 0;
    for (const auto* sets : {&emulators, &cores}) {
        for (const FindRuleSet& set : *sets) {
            for (const FindRule& rule : set.rules) {
                count += rule.entries.size();
            }
        }
    }
    return count;
}

FindRules loadFindRulesFiles(const std::optional<std::filesystem::path>& customFile,
                             const std::optional<std::filesystem::path>& bundledFile) {
    FindRules rules;
    if (customFile) {
        readFindRulesFile(*customFile, ConfigOrigin::Custom, rules);
    }
    if (bundledFile) {
        readFindRulesFile(*bundledFile, ConfigOrigin::Bundled, rules);
    } else {
        rules.issues.push_back(issueOf(catalog::IssueSeverity::Warning, "find-rules-missing", {},
                                       "ES-DE's bundled es_find_rules.xml was not found: only custom find rules "
                                       "apply"));
    }
    const auto byName = [](const FindRuleSet& a, const FindRuleSet& b) { return a.name < b.name; };
    std::sort(rules.emulators.begin(), rules.emulators.end(), byName);
    std::sort(rules.cores.begin(), rules.cores.end(), byName);
    return rules;
}

FindRules loadFindRules(const catalog::LibraryProfile& profile) {
    std::optional<std::filesystem::path> custom;
    if (profile.esdeHome.is_absolute()) { // never a file of the current folder
        custom = profile.esdeHome / "custom_systems" / "es_find_rules.xml";
        if (GetFileAttributesW(core::extendedLengthPath(*custom).c_str()) == INVALID_FILE_ATTRIBUTES) {
            custom.reset();
        }
    }
    return loadFindRulesFiles(custom, bundledResource(profile, kBundledFindRules));
}

} // namespace rv::esde
