/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSStyleRulePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>

namespace Web::CSS {

JS_DEFINE_ALLOCATOR(CSSStyleRule);

JS::NonnullGCPtr<CSSStyleRule> CSSStyleRule::create(JS::Realm& realm, SelectorList&& selectors, PropertyOwningCSSStyleDeclaration& declaration, CSSRuleList& nested_rules)
{
    return realm.heap().allocate<CSSStyleRule>(realm, realm, move(selectors), declaration, nested_rules);
}

CSSStyleRule::CSSStyleRule(JS::Realm& realm, SelectorList&& selectors, PropertyOwningCSSStyleDeclaration& declaration, CSSRuleList& nested_rules)
    : CSSGroupingRule(realm, nested_rules)
    , m_selectors(move(selectors))
    , m_declaration(declaration)
{
    m_declaration->set_parent_rule(*this);
}

void CSSStyleRule::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSStyleRule);
}

void CSSStyleRule::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_declaration);
}

// https://drafts.csswg.org/cssom-1/#dom-cssstylerule-style
CSSStyleDeclaration* CSSStyleRule::style()
{
    return m_declaration;
}

// https://drafts.csswg.org/cssom-1/#serialize-a-css-rule
String CSSStyleRule::serialized() const
{
    StringBuilder builder;

    // 1. Let s initially be the result of performing serialize a group of selectors on the rule’s associated selectors,
    //    followed by the string " {", i.e., a single SPACE (U+0020), followed by LEFT CURLY BRACKET (U+007B).
    builder.append(serialize_a_group_of_selectors(selectors()));
    builder.append(" {"sv);

    // 2. Let decls be the result of performing serialize a CSS declaration block on the rule’s associated declarations,
    //    or null if there are no such declarations.
    auto decls = declaration().length() > 0 ? declaration().serialized() : Optional<String>();

    // 3. Let rules be the result of performing serialize a CSS rule on each rule in the rule’s cssRules list,
    //    or null if there are no such rules.
    Vector<String> rules;
    for (auto& rule : css_rules()) {
        rules.append(rule->serialized());
    }

    // 4. If decls and rules are both null, append " }" to s (i.e. a single SPACE (U+0020) followed by RIGHT CURLY BRACKET (U+007D)) and return s.
    if (!decls.has_value() && rules.is_empty()) {
        builder.append(" }"sv);
        return builder.to_string_without_validation();
    }

    // 5. If rules is null:
    if (rules.is_empty()) {
        // 1. Append a single SPACE (U+0020) to s
        builder.append(' ');
        // 2. Append decls to s
        builder.append(*decls);
        // 3. Append " }" to s (i.e. a single SPACE (U+0020) followed by RIGHT CURLY BRACKET (U+007D)).
        builder.append(" }"sv);
        // 4. Return s.
        return builder.to_string_without_validation();
    }

    // 6. Otherwise:
    else {
        // 1. If decls is not null, prepend it to rules.
        if (decls.has_value())
            rules.prepend(decls.value());

        // 2. For each rule in rules:
        for (auto& rule : rules) {
            // * If rule is the empty string, do nothing.
            if (rule.is_empty())
                continue;

            // * Otherwise:
            // 1. Append a newline followed by two spaces to s.
            // 2. Append rule to s.
            builder.appendff("\n  {}", rule);
        }

        // 3. Append a newline followed by RIGHT CURLY BRACKET (U+007D) to s.
        builder.append("\n}"sv);

        // 4. Return s.
        return builder.to_string_without_validation();
    }
}

// https://drafts.csswg.org/cssom-1/#dom-cssstylerule-selectortext
String CSSStyleRule::selector_text() const
{
    // The selectorText attribute, on getting, must return the result of serializing the associated group of selectors.
    return serialize_a_group_of_selectors(selectors());
}

// https://drafts.csswg.org/cssom-1/#dom-cssstylerule-selectortext
void CSSStyleRule::set_selector_text(StringView selector_text)
{
    clear_caches();

    // 1. Run the parse a group of selectors algorithm on the given value.
    auto parsed_selectors = parse_selector(Parser::ParsingContext { realm() }, selector_text);

    // 2. If the algorithm returns a non-null value replace the associated group of selectors with the returned value.
    if (parsed_selectors.has_value()) {
        m_selectors = parsed_selectors.release_value();
        if (auto* sheet = parent_style_sheet()) {
            if (auto style_sheet_list = sheet->style_sheet_list()) {
                style_sheet_list->document().style_computer().invalidate_rule_cache();
                style_sheet_list->document_or_shadow_root().invalidate_style(DOM::StyleInvalidationReason::SetSelectorText);
            }
        }
    }

    // 3. Otherwise, if the algorithm returns a null value, do nothing.
}

SelectorList const& CSSStyleRule::absolutized_selectors() const
{
    if (m_cached_absolutized_selectors.has_value())
        return m_cached_absolutized_selectors.value();

    // Replace all occurrences of `&` with the nearest ancestor style rule's selector list wrapped in `:is(...)`,
    // or if we have no such ancestor, with `:scope`.

    // If we don't have any nesting selectors, we can just use our selectors as they are.
    bool has_any_nesting = false;
    for (auto const& selector : selectors()) {
        if (selector->contains_the_nesting_selector()) {
            has_any_nesting = true;
            break;
        }
    }

    if (!has_any_nesting) {
        m_cached_absolutized_selectors = m_selectors;
        return m_cached_absolutized_selectors.value();
    }

    // Otherwise, build up a new list of selectors with the `&` replaced.

    // First, figure out what we should replace `&` with.
    // "When used in the selector of a nested style rule, the nesting selector represents the elements matched by the parent rule.
    // When used in any other context, it represents the same elements as :scope in that context (unless otherwise defined)."
    // https://drafts.csswg.org/css-nesting-1/#nest-selector
    CSSStyleRule const* parent_style_rule = nullptr;
    for (auto* parent = parent_rule(); parent; parent = parent->parent_rule()) {
        if (parent->type() == CSSStyleRule::Type::Style) {
            parent_style_rule = static_cast<CSSStyleRule const*>(parent);
            break;
        }
    }
    Selector::SimpleSelector parent_selector;
    if (parent_style_rule) {
        // TODO: If there's only 1, we don't have to use `:is()` for it
        parent_selector = {
            .type = Selector::SimpleSelector::Type::PseudoClass,
            .value = Selector::SimpleSelector::PseudoClassSelector {
                .type = PseudoClass::Is,
                .argument_selector_list = parent_style_rule->absolutized_selectors(),
            },
        };
    } else {
        parent_selector = {
            .type = Selector::SimpleSelector::Type::PseudoClass,
            .value = Selector::SimpleSelector::PseudoClassSelector { .type = PseudoClass::Scope },
        };
    }

    SelectorList absolutized_selectors;
    for (auto const& selector : selectors())
        absolutized_selectors.append(selector->absolutized(parent_selector));

    m_cached_absolutized_selectors = move(absolutized_selectors);
    return m_cached_absolutized_selectors.value();
}

void CSSStyleRule::clear_caches()
{
    Base::clear_caches();
    m_cached_absolutized_selectors.clear();
}

}
