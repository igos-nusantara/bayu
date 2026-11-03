// SPDX-License-Identifier: GPL-3.0-or-later
// The store's own building blocks (the design's components sheet): app
// icons, cards and rows, pill buttons, filter chips, the banner and the
// pieces of an app's page.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/timer.h"
#include "model.h"
#include "ui/widgets.h"

namespace bayu::ui {
class ImageView;
class ProgressBar;
}

namespace store {

std::string sourceName(Source source);
// Where the app comes from: its source, or "IGOS Nusantara" for that remote.
std::string sourceName(const App& app);
// "package" for Flathub, "shield-check" for F-Droid.
std::string sourceGlyph(Source source);
// The display name of a store category id.
std::string categoryName(const std::string& id);

// An app's icon: its picture, or its glyph on a gradient.
class AppIcon : public View
{
public:
    explicit AppIcon(double size);
    void setApp(const App& app);
    bayu::ui::SizeF sizeHint() const override { return {m_size, m_size}; }

protected:
    void layout() override;
    void paint(bayu::ui::Painter& painter) override;

private:
    double m_size;
    uint32_t m_tint = 0x3B82F6;
    bayu::ui::Icon* m_glyph;
    bayu::ui::ImageView* m_image;
};

// INSTALL, OPEN, UPDATE, GET: a pill, tonal (the accent's tint), neutral
// (grey) or filled (the accent).
class PillButton : public Pressable
{
public:
    enum class Kind {
        Tonal,
        Neutral,
        Filled,
        // On a coloured banner.
        White,
    };
    PillButton(std::string text, Kind kind = Kind::Tonal);
    void setText(std::string text);
    void setKind(Kind kind);
    void setGlyph(std::string glyph);
    // A job's progress fills the pill; negative: none.
    void setProgress(double progress);
    // Larger, mixed case: the app page's "Install".
    void setLarge(bool large);
    bayu::ui::SizeF sizeHint() const override;

protected:
    void layout() override;
    void paint(bayu::ui::Painter& painter) override;

private:
    void updateColors();
    Kind m_kind;
    bool m_large = false;
    double m_progress = -1;
    bayu::ui::Label* m_label;
    bayu::ui::Icon* m_icon;
};

// What an app's button says and does for its state: INSTALL, OPEN,
// UPDATE, a job's progress.
struct AppAction
{
    std::string text;
    PillButton::Kind kind = PillButton::Kind::Tonal;
    double progress = -1;
    enum class Does {
        Install,
        Open,
        Update,
        Pause,
        EnableAndroid,
        None,
    } does = Does::Install;
};

// "⬡ Flathub": the source, small and grey.
class SourceLabel : public Row
{
public:
    explicit SourceLabel(Source source, double size = 13, const std::string& name = {});
};

// The grid's tile: icon, name, summary, source and button.
class AppCard : public Pressable
{
public:
    AppCard();
    void setApp(const App& app, const AppAction& action);
    std::function<void()> onAction;
    bayu::ui::SizeF sizeHint() const override { return {200, 190}; }

protected:
    void layout() override;
    void paint(bayu::ui::Painter& painter) override;

private:
    AppIcon* m_icon;
    bayu::ui::Label* m_name;
    bayu::ui::Label* m_summary;
    SourceLabel* m_source = nullptr;
    PillButton* m_button;
};

// A list's line: rank, icon, name, "Category · ⬡ Source · 42 MB", button.
class AppRow : public Pressable
{
public:
    struct Options
    {
        int rank = 0;
        double iconSize = 40;
        // The summary between the name and the details: search results.
        bool summary = false;
        // "Editors' Choice", "Open source".
        std::string badge;
        bool badgeGreen = false;
        bool developer = false;
        bool license = false;
    };
    explicit AppRow(Options options);
    void setApp(const App& app, const AppAction& action);
    std::function<void()> onAction;
    bayu::ui::SizeF sizeHint() const override;

protected:
    void layout() override;
    void paint(bayu::ui::Painter& painter) override;

private:
    Options m_options;
    bayu::ui::Label* m_rank = nullptr;
    AppIcon* m_icon;
    bayu::ui::Label* m_name;
    bayu::ui::Label* m_badge = nullptr;
    bayu::ui::Label* m_summary = nullptr;
    bayu::ui::Label* m_meta;
    PillButton* m_button;
};

// A filter: "All", "Photo", "Flathub". Black when on.
class Chip : public Pressable
{
public:
    explicit Chip(std::string text);
    void setChecked(bool checked);
    bool isChecked() const { return m_checked; }
    bayu::ui::SizeF sizeHint() const override;

protected:
    void layout() override;
    void paint(bayu::ui::Painter& painter) override;

private:
    bool m_checked = false;
    bayu::ui::Label* m_label;
};

// "Popular on Flathub ........ See all".
class SectionHeader : public Row
{
public:
    SectionHeader(std::string title, std::string link = {});
    std::function<void()> onLink;
};

// A small label on a tint: "Editors' Choice", "Sandboxed".
class Tag : public View
{
public:
    enum class Kind {
        Neutral,
        Accent,
        Green,
        Dark,
    };
    Tag(std::string text, Kind kind = Kind::Neutral, std::string glyph = {});
    bayu::ui::SizeF sizeHint() const override;

protected:
    void layout() override;
    void paint(bayu::ui::Painter& painter) override;

private:
    Kind m_kind;
    bayu::ui::Icon* m_icon = nullptr;
    bayu::ui::Label* m_label;
};

// The app of the day: a gradient banner with the app's icon, cycling
// through the featured apps.
class HeroBanner : public Pressable
{
public:
    HeroBanner();
    void setApps(std::vector<App> apps, std::function<AppAction(const App&)> action);
    std::function<void(const App&)> onOpen;
    std::function<void(const App&)> onAction;
    bayu::ui::SizeF sizeHint() const override { return {600, 220}; }

protected:
    void layout() override;
    void paint(bayu::ui::Painter& painter) override;

private:
    void show(size_t index);
    std::vector<App> m_apps;
    std::function<AppAction(const App&)> m_action;
    size_t m_index = 0;
    bayu::base::Timer m_timer;
    bayu::ui::Label* m_eyebrow;
    bayu::ui::Label* m_title;
    bayu::ui::Label* m_summary;
    PillButton* m_button;
    bayu::ui::Label* m_meta;
    AppIcon* m_icon;
};

// A row of figures: "Flathub / verified publisher", "GPL-3.0 / free
// software"...
class StatStrip : public View
{
public:
    struct Stat
    {
        std::string value;
        std::string glyph;
        std::string caption;
    };
    void setStats(std::vector<Stat> stats);
    bayu::ui::SizeF sizeHint() const override { return {600, 84}; }

protected:
    void layout() override;
    void paint(bayu::ui::Painter& painter) override;

private:
    size_t m_count = 0;
};

// Screenshots, side by side, each with its caption.
class ScreenshotStrip : public View
{
public:
    void setScreenshots(const std::vector<Screenshot>& shots, uint32_t tint);
    bayu::ui::SizeF sizeHint() const override { return {600, m_height}; }
    double heightForWidth(double width) const override;

protected:
    void layout() override;

private:
    class Shot;
    std::vector<Shot*> m_shots;
    double m_height = 160;
};

// Numbered pages: 1 2 3 … 46.
class Pagination : public Row
{
public:
    Pagination();
    void setPages(int current, int count);
    std::function<void(int page)> onPage;
};

// The bar of where the space goes: apps, runtimes, cache, free.
class StorageBar : public View
{
public:
    void setUsage(const StorageUsage& usage);
    bayu::ui::SizeF sizeHint() const override { return {600, 26}; }

protected:
    void paint(bayu::ui::Painter& painter) override;

private:
    StorageUsage m_usage;
};

// A dot and text, for legends.
View* addLegend(View* parent, bayu::ui::Color color, std::string text);

// A thin progress bar in the accent colour.
class ThinProgress : public View
{
public:
    void setValue(double value);
    bayu::ui::SizeF sizeHint() const override { return {100, 6}; }

protected:
    void paint(bayu::ui::Painter& painter) override;

private:
    double m_value = 0;
};

// A square icon button: the heart, share, pause, more.
class IconButton : public Pressable
{
public:
    IconButton(std::string glyph, double size = 40);
    void setGlyph(std::string glyph);
    void setTint(std::optional<bayu::ui::ColorRecipe> tint);
    bayu::ui::SizeF sizeHint() const override { return {m_size, m_size}; }

protected:
    void layout() override;
    void paint(bayu::ui::Painter& painter) override;

private:
    double m_size;
    bayu::ui::Icon* m_icon;
};

// Colours from an RGB value.
bayu::ui::Color rgb(uint32_t value);
// A tint for an app without one, from its name.
uint32_t tintFor(const App& app);

} // namespace store
