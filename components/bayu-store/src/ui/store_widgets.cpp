// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/store_widgets.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "base/i18n.h"
#include "catalog.h"
#include "ui/icon.h"
#include "ui/image_view.h"
#include "ui/label.h"
#include "ui/painter.h"
#include "ui/path.h"
#include "ui/theme.h"

using namespace bayu::ui;
using bayu::i18n::format;
using bayu::i18n::tr;

namespace store {

using style::Role;

Color rgb(uint32_t value)
{
    return Color::rgb(value);
}

static Color accentColor()
{
    return Theme::current().highlight;
}

uint32_t tintFor(const App& app)
{
    if (app.tint)
        return app.tint;
    static const uint32_t tints[] = {0x3B82F6, 0x8B5CF6, 0xF43F5E, 0x14B8A6, 0xF97316, 0x22C55E, 0x06B6D4, 0xA855F7};
    size_t hash = 0;
    for (char c : app.id)
        hash = hash * 31 + static_cast<unsigned char>(c);
    return tints[hash % (sizeof(tints) / sizeof(tints[0]))];
}

std::string sourceName(Source source)
{
    return source == Source::Flathub ? "Flathub" : "F-Droid";
}

std::string sourceName(const App& app)
{
    return app.remote == "igos" ? "IGOS Nusantara" : sourceName(app.source);
}

std::string sourceGlyph(Source source)
{
    return source == Source::Flathub ? "package" : "shield-check";
}

std::string categoryName(const std::string& id)
{
    for (const Category& category : storeCategories()) {
        if (category.id == id)
            return category.name;
    }
    return id;
}

// --- AppIcon ------------------------------------------------------------

AppIcon::AppIcon(double size)
    : m_size(size)
{
    m_glyph = addChild<Icon>("package", size * 0.5);
    m_glyph->setColor(ColorRecipe(Color::rgb(0xFFFFFF)));
    m_image = addChild<ImageView>();
    m_image->setFillMode(ImageView::FillMode::PreserveAspectFit);
    m_image->setVisible(false);
}

void AppIcon::setApp(const App& app)
{
    m_tint = tintFor(app);
    m_glyph->setIconName(app.glyph.empty() ? "package" : app.glyph);
    const bool picture = !app.iconPath.empty();
    m_image->setVisible(picture);
    m_glyph->setVisible(!picture);
    if (picture)
        m_image->setSource(app.iconPath);
    requestPaint();
}

void AppIcon::layout()
{
    const double glyph = m_size * 0.5;
    m_glyph->setGeometry({(m_size - glyph) / 2, (m_size - glyph) / 2, glyph, glyph});
    m_image->setGeometry(localRect());
}

void AppIcon::paint(Painter& painter)
{
    if (m_image->isVisible())
        return;
    const Color tint = rgb(m_tint);
    Path tile;
    tile.addRoundedRect(localRect(), m_size * 0.24, m_size * 0.24);
    painter.fillPath(tile, LinearGradient{{0, 0}, {m_size, m_size}, {{0, tint.lighter(1.18)}, {1, tint.darker(1.12)}}});
}

// --- PillButton ---------------------------------------------------------

PillButton::PillButton(std::string text, Kind kind)
    : m_kind(kind)
{
    m_icon = addChild<Icon>(std::string(), 16);
    m_icon->setVisible(false);
    m_label = addText(this, std::move(text), 13, 700);
    m_label->setLetterSpacing(0.6);
    updateColors();
}

void PillButton::setText(std::string text)
{
    m_label->setText(std::move(text));
    requestLayout();
}

void PillButton::setKind(Kind kind)
{
    m_kind = kind;
    updateColors();
    requestPaint();
}

void PillButton::setGlyph(std::string glyph)
{
    m_icon->setIconName(glyph);
    m_icon->setVisible(!glyph.empty());
    requestLayout();
}

void PillButton::setProgress(double progress)
{
    m_progress = progress;
    requestPaint();
}

void PillButton::setLarge(bool large)
{
    m_large = large;
    style::setFont(m_label, large ? 16 : 13, large ? 600 : 700);
    m_label->setLetterSpacing(large ? 0 : 0.6);
    m_icon->setSize(large ? 18 : 16);
    requestLayout();
}

void PillButton::updateColors()
{
    const ColorRecipe text = m_kind == Kind::Filled  ? ColorRecipe(Color::rgb(0xFFFFFF))
                             : m_kind == Kind::Tonal ? ColorRecipe(ColorId::Highlight)
                             : m_kind == Kind::White ? ColorRecipe(Color::rgb(0x111114))
                                                     : style::color(Role::TonalText);
    m_label->setColor(text);
    m_icon->setColor(text);
}

SizeF PillButton::sizeHint() const
{
    const SizeF text = m_label->sizeHint();
    const double icon = m_icon->isVisible() ? (m_large ? 26 : 22) : 0;
    return {std::ceil(text.width) + icon + (m_large ? 44 : 40), m_large ? 46 : 34};
}

void PillButton::layout()
{
    const SizeF text = m_label->sizeHint();
    const double icon = m_icon->isVisible() ? (m_large ? 26 : 22) : 0;
    const double x = (geometry().width - text.width - icon) / 2;
    if (m_icon->isVisible()) {
        const double s = m_large ? 18 : 16;
        m_icon->setGeometry({x, (geometry().height - s) / 2, s, s});
    }
    m_label->setGeometry({x + icon, (geometry().height - text.height) / 2, text.width, text.height});
}

void PillButton::paint(Painter& painter)
{
    const RectF rect = localRect();
    const RoundedShape shape{m_large ? 10 : rect.height / 2, 0, Corners::Circular};
    const Color accent = accentColor();
    Color fill = m_kind == Kind::Filled  ? accent
                 : m_kind == Kind::Tonal ? accent.withAlpha(Theme::current().darkMode ? 0.22 : 0.12)
                 : m_kind == Kind::White ? Color::rgb(0xFFFFFF)
                                         : resolve(Role::Tonal);
    // A filled pill with progress: pale, filling with the accent.
    if (m_kind == Kind::Filled && m_progress >= 0)
        fill = accent.withAlpha(0.45);
    if (isPressed())
        fill = fill.darker(1.1);
    else if (isHovered())
        fill = m_kind == Kind::Filled ? fill.lighter(1.06) : fill.darker(1.04);
    painter.drawRoundedRect(rect, shape, fill);
    if (m_progress >= 0) {
        painter.save();
        painter.clipRoundedRect(rect, shape.radius);
        painter.fillRect({0, 0, rect.width * std::clamp(m_progress, 0.0, 1.0), rect.height},
                         m_kind == Kind::Filled ? accent : accent.withAlpha(0.22));
        painter.restore();
    }
    if (hasVisibleFocus())
        painter.drawRoundedRect(rect.adjusted(-2, -2, 2, 2), {shape.radius + 2, 0, Corners::Circular}, Color{}, 2,
                                accent.withAlpha(0.5));
}

// --- SourceLabel --------------------------------------------------------

SourceLabel::SourceLabel(Source source, double size, const std::string& name)
{
    setSpacing(5);
    Icon* icon = addChild<Icon>(sourceGlyph(source), size);
    icon->setColor(style::color(Role::TertiaryText));
    addText(this, name.empty() ? sourceName(source) : name, size, 400, Role::SecondaryText);
}

// --- AppCard ------------------------------------------------------------

AppCard::AppCard()
{
    m_icon = addChild<AppIcon>(48);
    m_name = addText(this, {}, 16, 600);
    m_name->setElide(Label::Elide::Right);
    m_summary = addText(this, {}, 13, 400, Role::SecondaryText);
    m_summary->setElide(Label::Elide::Right);
    m_button = addChild<PillButton>(tr("INSTALL"));
    m_button->onClicked = [this] {
        if (onAction)
            onAction();
    };
}

void AppCard::setApp(const App& app, const AppAction& action)
{
    m_icon->setApp(app);
    m_name->setText(app.name);
    m_summary->setText(app.summary);
    if (m_source)
        takeChild(m_source);
    m_source = addChild<SourceLabel>(app.source, 13, sourceName(app));
    m_button->setText(action.text);
    m_button->setKind(action.kind);
    m_button->setProgress(action.progress);
    m_button->setVisible(action.does != AppAction::Does::None || !action.text.empty());
    requestLayout();
}

void AppCard::layout()
{
    const double w = geometry().width;
    const double h = geometry().height;
    const double pad = 14;
    m_icon->setGeometry({pad, pad, 48, 48});
    const double nameH = m_name->sizeHint().height;
    m_name->setGeometry({pad, pad + 48 + 14, w - 2 * pad, nameH});
    const double summaryH = m_summary->sizeHint().height;
    m_summary->setGeometry({pad, pad + 48 + 14 + nameH + 2, w - 2 * pad, summaryH});
    const SizeF button = m_button->sizeHint();
    const double bottom = h - pad;
    m_button->setGeometry({w - pad - button.width, bottom - button.height, button.width, button.height});
    if (m_source) {
        const SizeF source = m_source->sizeHint();
        m_source->setGeometry({pad, bottom - button.height / 2 - source.height / 2, source.width, source.height});
    }
}

void AppCard::paint(Painter& painter)
{
    painter.drawRoundedRect(localRect(), {style::cardRadius + 2, 0, Corners::Circular}, resolve(Role::Card), 1,
                            resolve(Role::CardBorder));
    if (isHovered())
        painter.drawRoundedRect(localRect(), {style::cardRadius + 2, 0, Corners::Circular},
                                Theme::current().text.withAlpha(isPressed() ? 0.05 : 0.025));
}

// --- AppRow -------------------------------------------------------------

AppRow::AppRow(Options options)
    : m_options(options)
{
    if (options.rank > 0) {
        m_rank = addText(this, std::to_string(options.rank), 15, 500, Role::TertiaryText);
        m_rank->setTextAlignment(Label::TextAlignment::Center);
    }
    m_icon = addChild<AppIcon>(options.iconSize);
    m_name = addText(this, {}, 16, 600);
    m_name->setElide(Label::Elide::Right);
    if (!options.badge.empty()) {
        m_badge = addText(this, options.badge, 12, 600);
        m_badge->setColor(options.badgeGreen ? style::color(Role::Success) : ColorRecipe(ColorId::Highlight));
    }
    if (options.summary) {
        m_summary = addText(this, {}, 14, 400, Role::SecondaryText);
        m_summary->setElide(Label::Elide::Right);
    }
    m_meta = addText(this, {}, 13, 400, Role::SecondaryText);
    m_meta->setElide(Label::Elide::Right);
    m_button = addChild<PillButton>(tr("INSTALL"));
    m_button->onClicked = [this] {
        if (onAction)
            onAction();
    };
}

void AppRow::setApp(const App& app, const AppAction& action)
{
    m_icon->setApp(app);
    m_name->setText(app.name);
    if (m_summary)
        m_summary->setText(app.summary);
    std::string meta;
    auto add = [&meta](const std::string& part) {
        if (part.empty())
            return;
        if (!meta.empty())
            meta += "  ·  ";
        meta += part;
    };
    if (m_options.developer)
        add(app.developer);
    else if (!app.categories.empty())
        add(categoryName(app.categories.front()));
    add(sourceName(app));
    if (m_options.license)
        add(app.license);
    if (app.downloadSize)
        add(style::formatSize(app.downloadSize));
    m_meta->setText(meta);
    m_button->setText(action.text);
    m_button->setKind(action.kind);
    m_button->setProgress(action.progress);
    requestLayout();
}

SizeF AppRow::sizeHint() const
{
    const double text = m_summary ? 66 : 46;
    return {400, std::max(m_options.iconSize, text) + (m_summary ? 28 : 20)};
}

void AppRow::layout()
{
    const double w = geometry().width;
    const double h = geometry().height;
    double x = m_summary ? 18 : 6;
    if (m_rank) {
        m_rank->setGeometry({x, (h - 20) / 2, 18, 20});
        x += 28;
    }
    const double icon = m_options.iconSize;
    m_icon->setGeometry({x, (h - icon) / 2, icon, icon});
    x += icon + 16;
    const SizeF button = m_button->sizeHint();
    const double right = w - (m_summary ? 18 : 6);
    m_button->setGeometry({right - button.width, (h - button.height) / 2, button.width, button.height});
    const double textW = right - button.width - 16 - x;
    const double nameH = m_name->sizeHint().height;
    const double metaH = m_meta->sizeHint().height;
    const double summaryH = m_summary ? m_summary->sizeHint().height + 2 : 0;
    double y = (h - nameH - summaryH - metaH - 2) / 2;
    double nameW = std::min(textW, std::ceil(m_name->sizeHint().width));
    if (m_badge) {
        const SizeF badge = m_badge->sizeHint();
        nameW = std::min(nameW, textW - badge.width - 30);
        m_badge->setGeometry({x + nameW + 18, y + (nameH - badge.height) / 2, badge.width, badge.height});
    }
    m_name->setGeometry({x, y, nameW, nameH});
    y += nameH + 2;
    if (m_summary) {
        m_summary->setGeometry({x, y, textW, summaryH - 2});
        y += summaryH;
    }
    m_meta->setGeometry({x, y, textW, metaH});
}

void AppRow::paint(Painter& painter)
{
    if (m_summary)
        painter.drawRoundedRect(localRect(), {style::cardRadius + 2, 0, Corners::Circular}, resolve(Role::Card), 1,
                                resolve(Role::CardBorder));
    if (m_badge) {
        const RectF badge = m_badge->geometry().adjusted(-8, -3, 8, 3);
        const Color tint = m_options.badgeGreen ? resolve(Role::Success) : accentColor();
        painter.drawRoundedRect(badge, {6, 0, Corners::Circular}, tint.withAlpha(0.12));
    }
    if (isHovered())
        painter.drawRoundedRect(localRect(), {style::cardRadius, 0, Corners::Circular},
                                Theme::current().text.withAlpha(isPressed() ? 0.05 : 0.025));
}

// --- Chip ---------------------------------------------------------------

Chip::Chip(std::string text)
{
    m_label = addText(this, std::move(text), 15, 500);
    setChecked(false);
}

void Chip::setChecked(bool checked)
{
    m_checked = checked;
    m_label->setColor(checked ? ColorRecipe(Theme::current().darkMode ? Color::rgb(0x111111) : Color::rgb(0xFFFFFF))
                              : style::color(Role::TonalText));
    requestPaint();
}

SizeF Chip::sizeHint() const
{
    const SizeF text = m_label->sizeHint();
    return {std::ceil(text.width) + 40, 42};
}

void Chip::layout()
{
    const SizeF text = m_label->sizeHint();
    m_label->setGeometry({20, (geometry().height - text.height) / 2, text.width, text.height});
}

void Chip::paint(Painter& painter)
{
    const RectF rect = localRect();
    const RoundedShape shape{rect.height / 2, 0, Corners::Circular};
    if (m_checked) {
        painter.drawRoundedRect(rect, shape, Theme::current().darkMode ? Color::rgb(0xF2F2F4) : Color::rgb(0x111114));
        return;
    }
    painter.drawRoundedRect(rect, shape, resolve(Role::Card), 1, resolve(Role::CardBorder));
    if (isHovered())
        painter.drawRoundedRect(rect, shape, Theme::current().text.withAlpha(0.04));
}

// --- SectionHeader ------------------------------------------------------

SectionHeader::SectionHeader(std::string title, std::string link)
{
    setLayoutHints({.fillWidth = true});
    addText(this, std::move(title), 21, 700);
    addStretch();
    if (!link.empty()) {
        ActionButton* button = addChild<ActionButton>(std::move(link), ActionButton::Kind::Link);
        button->onClicked = [this] {
            if (onLink)
                onLink();
        };
    }
}

// --- Tag ----------------------------------------------------------------

Tag::Tag(std::string text, Kind kind, std::string glyph)
    : m_kind(kind)
{
    if (!glyph.empty()) {
        m_icon = addChild<Icon>(std::move(glyph), 14);
    }
    m_label = addText(this, std::move(text), 13, 500);
    ColorRecipe color = style::color(Role::TonalText);
    if (kind == Kind::Accent)
        color = ColorRecipe(ColorId::Highlight);
    else if (kind == Kind::Green)
        color = style::color(Role::Success);
    else if (kind == Kind::Dark)
        color = ColorRecipe(Color::rgb(0xFFFFFF));
    m_label->setColor(color);
    if (m_icon)
        m_icon->setColor(color);
}

SizeF Tag::sizeHint() const
{
    const SizeF text = m_label->sizeHint();
    return {std::ceil(text.width) + 24 + (m_icon ? 20 : 0), 30};
}

void Tag::layout()
{
    const SizeF text = m_label->sizeHint();
    double x = 12;
    if (m_icon) {
        m_icon->setGeometry({x, (geometry().height - 14) / 2, 14, 14});
        x += 20;
    }
    m_label->setGeometry({x, (geometry().height - text.height) / 2, text.width, text.height});
}

void Tag::paint(Painter& painter)
{
    const RectF rect = localRect();
    Color fill = resolve(Role::Tonal);
    if (m_kind == Kind::Accent)
        fill = accentColor().withAlpha(0.12);
    else if (m_kind == Kind::Green)
        fill = resolve(Role::Success).withAlpha(0.12);
    else if (m_kind == Kind::Dark)
        fill = Color::rgb(0x000000).withAlpha(0.45);
    painter.drawRoundedRect(rect, {rect.height / 2, 0, Corners::Circular}, fill, m_kind == Kind::Neutral ? 1 : 0,
                            resolve(Role::CardBorder));
}

// --- HeroBanner ---------------------------------------------------------

HeroBanner::HeroBanner()
{
    m_eyebrow = addText(this, tr("APP OF THE DAY"), 13, 600);
    m_eyebrow->setLetterSpacing(1.8);
    m_eyebrow->setColor(ColorRecipe(Color::rgb(0xFFFFFF).withAlpha(0.85)));
    m_title = addText(this, {}, 34, 700);
    m_title->setColor(ColorRecipe(Color::rgb(0xFFFFFF)));
    m_title->setElide(Label::Elide::Right);
    m_summary = addText(this, {}, 16, 400);
    m_summary->setColor(ColorRecipe(Color::rgb(0xFFFFFF).withAlpha(0.88)));
    m_summary->setElide(Label::Elide::Right);
    m_button = addChild<PillButton>(tr("Install"), PillButton::Kind::White);
    m_button->setLarge(true);
    m_button->onClicked = [this] {
        if (onAction && m_index < m_apps.size())
            onAction(m_apps[m_index]);
    };
    m_meta = addText(this, {}, 14, 400);
    m_meta->setColor(ColorRecipe(Color::rgb(0xFFFFFF).withAlpha(0.75)));
    m_icon = addChild<AppIcon>(120);
    onClicked = [this] {
        if (onOpen && m_index < m_apps.size())
            onOpen(m_apps[m_index]);
    };
    m_timer.onTimeout = [this] {
        if (m_apps.size() > 1)
            show((m_index + 1) % m_apps.size());
    };
}

void HeroBanner::setApps(std::vector<App> apps, std::function<AppAction(const App&)> action)
{
    m_apps = std::move(apps);
    m_action = std::move(action);
    show(std::min(m_index, m_apps.empty() ? size_t(0) : m_apps.size() - 1));
    m_timer.start(std::chrono::seconds(8));
}

void HeroBanner::show(size_t index)
{
    m_index = index;
    if (m_apps.empty())
        return;
    const App& app = m_apps[index];
    m_title->setText(app.name);
    m_summary->setText(app.summary);
    std::string meta = sourceName(app);
    if (app.downloadSize)
        meta += " · " + style::formatSize(app.downloadSize);
    m_meta->setText(meta);
    m_icon->setApp(app);
    if (m_action) {
        const AppAction action = m_action(app);
        // Mixed case on the banner: "Install", "Open".
        std::string text = action.text;
        for (size_t i = 1; i < text.size(); ++i)
            text[i] = char(std::tolower(static_cast<unsigned char>(text[i])));
        m_button->setText(text);
    }
    requestLayout();
    requestPaint();
}

void HeroBanner::layout()
{
    const double w = geometry().width;
    const double h = geometry().height;
    const double left = 34;
    const double icon = std::min(130.0, h - 70);
    m_icon->setGeometry({w - 34 - icon, (h - icon) / 2, icon, icon});
    const double textW = w - left - icon - 80;
    double y = 26;
    const SizeF eyebrow = m_eyebrow->sizeHint();
    m_eyebrow->setGeometry({left, y, eyebrow.width, eyebrow.height});
    y += eyebrow.height + 8;
    const double titleH = m_title->sizeHint().height;
    m_title->setGeometry({left, y, textW, titleH});
    y += titleH + 8;
    const double summaryH = m_summary->sizeHint().height;
    m_summary->setGeometry({left, y, textW, summaryH});
    y += summaryH + 18;
    const SizeF button = m_button->sizeHint();
    m_button->setGeometry({left, y, button.width + 16, button.height});
    const SizeF meta = m_meta->sizeHint();
    m_meta->setGeometry({left + button.width + 32, y + (button.height - meta.height) / 2, meta.width, meta.height});
}

void HeroBanner::paint(Painter& painter)
{
    const RectF rect = localRect();
    Path shape;
    shape.addRoundedRect(rect, 20, 20);
    painter.fillPath(shape, LinearGradient{{0, rect.height}, {rect.width, 0},
                                           {{0, Color::rgb(0xB65CF6)}, {0.55, Color::rgb(0x7C5CF8)}, {1, Color::rgb(0x2E8BFF)}}});
    // The icon sits on a frosted tile.
    const RectF icon = m_icon->geometry().adjusted(-6, -6, 6, 6);
    painter.drawRoundedRect(icon, {icon.width * 0.26, 0, Corners::Circular}, Color::rgb(0xFFFFFF).withAlpha(0.16), 1,
                            Color::rgb(0xFFFFFF).withAlpha(0.35));
    // The pager.
    const double y = rect.height - 22;
    double x = 34;
    for (size_t i = 0; i < m_apps.size(); ++i) {
        const double width = i == m_index ? 22 : 8;
        painter.drawRoundedRect({x, y, width, 8}, {4, 0, Corners::Circular},
                                Color::rgb(0xFFFFFF).withAlpha(i == m_index ? 1 : 0.5));
        x += width + 8;
    }
}

// --- StatStrip ----------------------------------------------------------

void StatStrip::setStats(std::vector<Stat> stats)
{
    while (!children().empty())
        takeChild(children().back().get());
    m_count = stats.size();
    for (const Stat& stat : stats) {
        Column* column = addChild<Column>();
        column->setSpacing(4);
        Row* top = column->addChild<Row>();
        top->setSpacing(6);
        top->setLayoutHints({.alignment = Alignment::Center});
        addText(top, stat.value, 21, 700);
        if (!stat.glyph.empty()) {
            Icon* icon = top->addChild<Icon>(stat.glyph, 15);
            icon->setColor(style::color(Role::TertiaryText));
        }
        Label* caption = addText(column, stat.caption, 14, 400, Role::SecondaryText);
        caption->setLayoutHints({.alignment = Alignment::Center});
    }
    requestLayout();
}

void StatStrip::layout()
{
    if (!m_count)
        return;
    const double w = geometry().width / double(m_count);
    for (size_t i = 0; i < children().size(); ++i) {
        View* column = children()[i].get();
        const double h = column->sizeHint().height;
        column->setGeometry({w * double(i), (geometry().height - h) / 2, w, h});
    }
}

void StatStrip::paint(Painter& painter)
{
    painter.drawRoundedRect(localRect(), {style::cardRadius + 2, 0, Corners::Circular}, resolve(Role::Card), 1,
                            resolve(Role::CardBorder));
    const double w = geometry().width / double(std::max<size_t>(1, m_count));
    for (size_t i = 1; i < m_count; ++i)
        painter.fillRect({std::round(w * double(i)), 14, 1, geometry().height - 28}, resolve(Role::Divider));
}

// --- ScreenshotStrip ----------------------------------------------------

class ScreenshotStrip::Shot : public View
{
public:
    Shot(const Screenshot& shot, uint32_t tint, int index)
        : m_tint(tint)
        , m_index(index)
    {
        m_image = addChild<ImageView>();
        m_image->setFillMode(shot.portrait ? ImageView::FillMode::PreserveAspectFit
                                           : ImageView::FillMode::PreserveAspectCrop);
        m_image->setCornerRadius(14);
        if (!shot.url.empty())
            m_image->setSource(shot.url);
        m_image->setVisible(!shot.url.empty());
        if (!shot.caption.empty())
            m_caption = addChild<Tag>(shot.caption, Tag::Kind::Dark);
    }

protected:
    void layout() override
    {
        m_image->setGeometry(localRect());
        if (m_caption) {
            const SizeF caption = m_caption->sizeHint();
            m_caption->setGeometry({14, geometry().height - caption.height - 14, caption.width, caption.height});
        }
    }
    void paint(Painter& painter) override
    {
        // Until (or without) the picture: a gradient in the app's colour.
        static const uint32_t shades[] = {0x2E4A7A, 0x6A3373, 0x1F6B5A, 0x8A4A2A};
        const Color base = m_tint ? rgb(m_tint).darker(1.6) : rgb(shades[m_index % 4]);
        const Color from = rgb(shades[m_index % 4]);
        Path shape;
        shape.addRoundedRect(localRect(), 14, 14);
        painter.fillPath(shape, LinearGradient{{0, 0}, {geometry().width, geometry().height},
                                               {{0, from.lighter(1.15)}, {1, base.darker(1.3)}}});
    }

private:
    uint32_t m_tint;
    int m_index;
    ImageView* m_image;
    Tag* m_caption = nullptr;
};

void ScreenshotStrip::setScreenshots(const std::vector<Screenshot>& shots, uint32_t)
{
    while (!children().empty())
        takeChild(children().back().get());
    m_shots.clear();
    for (size_t i = 0; i < shots.size() && i < 4; ++i)
        m_shots.push_back(addChild<Shot>(shots[i], 0, int(i)));
    requestLayout();
}

double ScreenshotStrip::heightForWidth(double width) const
{
    if (m_shots.empty())
        return 0;
    const double gap = 18;
    const double w = (width - gap * 3) / 4;
    return std::round(w * 0.86);
}

void ScreenshotStrip::layout()
{
    const double gap = 18;
    const double w = (geometry().width - gap * 3) / 4;
    for (size_t i = 0; i < m_shots.size(); ++i)
        m_shots[i]->setGeometry({double(i) * (w + gap), 0, w, geometry().height});
}

// --- Pagination ---------------------------------------------------------

namespace {

class PageButton : public Pressable
{
public:
    PageButton(std::string text, bool current)
        : m_current(current)
    {
        m_label = addText(this, std::move(text), 15, 500);
        m_label->setTextAlignment(Label::TextAlignment::Center);
        m_label->setVerticallyCentered(true);
        if (current)
            m_label->setColor(ColorRecipe(Color::rgb(0xFFFFFF)));
    }
    SizeF sizeHint() const override { return {std::max(44.0, m_label->sizeHint().width + 24), 44}; }

protected:
    void layout() override { m_label->setGeometry(localRect()); }
    void paint(Painter& painter) override
    {
        const RoundedShape shape{10, 0, Corners::Circular};
        if (m_current)
            painter.drawRoundedRect(localRect(), shape, accentColor());
        else
            painter.drawRoundedRect(localRect(), shape, isHovered() ? resolve(Role::Tonal) : resolve(Role::Card), 1,
                                    resolve(Role::CardBorder));
    }

private:
    bool m_current;
    Label* m_label;
};

} // namespace

Pagination::Pagination()
{
    setSpacing(12);
}

void Pagination::setPages(int current, int count)
{
    while (!children().empty())
        takeChild(children().back().get());
    if (count <= 1)
        return;
    std::vector<int> pages;
    for (int page = 1; page <= count; ++page) {
        if (page <= 3 || page == count || std::abs(page - current) <= 1)
            pages.push_back(page);
    }
    int last = 0;
    for (int page : pages) {
        if (page - last > 1)
            addChild<PageButton>("…", false)->setEnabled(false);
        PageButton* button = addChild<PageButton>(std::to_string(page), page == current);
        button->onClicked = [this, page] {
            if (onPage)
                onPage(page);
        };
        last = page;
    }
}

// --- StorageBar ---------------------------------------------------------

void StorageBar::setUsage(const StorageUsage& usage)
{
    m_usage = usage;
    requestPaint();
}

void StorageBar::paint(Painter& painter)
{
    const RectF rect{0, (geometry().height - 10) / 2, geometry().width, 10};
    painter.drawRoundedRect(rect, {5, 0, Corners::Circular}, resolve(Role::Tonal));
    const double total = double(std::max<uint64_t>(1, m_usage.total));
    const Color colors[] = {accentColor(), Color::rgb(0x8B5CF6), Color::rgb(0xF5A524)};
    const uint64_t parts[] = {m_usage.apps, m_usage.runtimes, m_usage.cache};
    double x = 0;
    painter.save();
    painter.clipRoundedRect(rect, 5);
    for (int i = 0; i < 3; ++i) {
        const double w = rect.width * double(parts[i]) / total;
        painter.fillRect({x, rect.y, w, rect.height}, colors[i]);
        x += w;
    }
    painter.restore();
}

View* addLegend(View* parent, Color color, std::string text)
{
    class Dot : public View
    {
    public:
        explicit Dot(Color color)
            : m_color(color)
        {
        }
        SizeF sizeHint() const override { return {9, 9}; }

    protected:
        void paint(Painter& painter) override { painter.drawRoundedRect(localRect(), {4.5, 0, Corners::Circular}, m_color); }

    private:
        Color m_color;
    };
    Row* row = parent->addChild<Row>();
    row->setSpacing(8);
    row->addChild<Dot>(color);
    addText(row, std::move(text), 14, 400, Role::SecondaryText);
    return row;
}

// --- ThinProgress -------------------------------------------------------

void ThinProgress::setValue(double value)
{
    m_value = std::clamp(value, 0.0, 1.0);
    requestPaint();
}

void ThinProgress::paint(Painter& painter)
{
    const RectF rect = localRect();
    painter.drawRoundedRect(rect, {rect.height / 2, 0, Corners::Circular}, accentColor().withAlpha(0.16));
    if (m_value > 0)
        painter.drawRoundedRect({0, 0, rect.width * m_value, rect.height}, {rect.height / 2, 0, Corners::Circular},
                                accentColor());
}

// --- IconButton ---------------------------------------------------------

IconButton::IconButton(std::string glyph, double size)
    : m_size(size)
{
    m_icon = addChild<Icon>(std::move(glyph), size * 0.45);
}

void IconButton::setGlyph(std::string glyph)
{
    m_icon->setIconName(std::move(glyph));
}

void IconButton::setTint(std::optional<ColorRecipe> tint)
{
    m_icon->setColor(std::move(tint));
}

void IconButton::layout()
{
    const double s = m_size * 0.45;
    m_icon->setGeometry({(geometry().width - s) / 2, (geometry().height - s) / 2, s, s});
}

void IconButton::paint(Painter& painter)
{
    const RoundedShape shape{10, 0, Corners::Circular};
    painter.drawRoundedRect(localRect(), shape, isHovered() ? resolve(Role::Tonal) : resolve(Role::Card), 1,
                            resolve(Role::CardBorder));
}

} // namespace store
