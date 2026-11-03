// SPDX-License-Identifier: GPL-3.0-or-later
// The store's pages: Discover, Trending, Categories, a category or source,
// an app, the library, search results and the settings.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>

#include "base/event_loop.h"
#include "base/i18n.h"
#include "android_setup.h"
#include "catalog.h"
#include "ui/clipboard.h"
#include "ui/icon.h"
#include "ui/label.h"
#include "ui/menu.h"
#include "ui/painter.h"
#include "ui/page_util.h"
#include "ui/theme.h"

using namespace bayu::ui;
using bayu::i18n::format;
using bayu::i18n::tr;

namespace store {

using style::Role;

// --- Shared pieces --------------------------------------------------------

Grid::Grid(double minimumWidth, double spacing, int maximumColumns)
    : m_minimum(minimumWidth)
    , m_spacing(spacing)
    , m_maximum(maximumColumns)
{
    setLayoutHints({.fillWidth = true});
}

int Grid::columnsFor(double width) const
{
    if (m_fixed)
        return m_fixed;
    const int columns = int((width + m_spacing) / (m_minimum + m_spacing));
    return std::clamp(columns, 1, m_maximum);
}

double Grid::heightForWidth(double width) const
{
    const int columns = columnsFor(width);
    const double columnWidth = (width - m_spacing * (columns - 1)) / columns;
    double total = 0;
    double row = 0;
    int column = 0;
    for (const auto& child : children()) {
        if (!child->isVisible())
            continue;
        row = std::max(row, child->heightForWidth(columnWidth));
        if (++column == columns) {
            total += row + m_spacing;
            row = 0;
            column = 0;
        }
    }
    if (column)
        total += row + m_spacing;
    return std::max(0.0, total - m_spacing);
}

void Grid::layout()
{
    const double width = geometry().width;
    const int columns = columnsFor(width);
    const double columnWidth = (width - m_spacing * (columns - 1)) / columns;
    std::vector<View*> visible;
    for (const auto& child : children()) {
        if (child->isVisible())
            visible.push_back(child.get());
    }
    double y = 0;
    for (size_t start = 0; start < visible.size(); start += size_t(columns)) {
        double row = 0;
        for (size_t i = start; i < visible.size() && i < start + size_t(columns); ++i)
            row = std::max(row, visible[i]->heightForWidth(columnWidth));
        for (size_t i = start; i < visible.size() && i < start + size_t(columns); ++i)
            visible[i]->setGeometry({double(i - start) * (columnWidth + m_spacing), y, columnWidth, row});
        y += row + m_spacing;
    }
}

AppCard* addAppCard(StoreWindow& store, View* parent, const App& app)
{
    AppCard* card = parent->addChild<AppCard>();
    card->setApp(app, store.actionFor(app));
    card->onClicked = [&store, app] { store.openApp(app); };
    card->onAction = [&store, app] { store.act(app); };
    return card;
}

AppRow* addAppRow(StoreWindow& store, View* parent, const App& app, AppRow::Options options)
{
    AppRow* row = parent->addChild<AppRow>(options);
    row->setLayoutHints({.fillWidth = true});
    row->setApp(app, store.actionFor(app));
    row->onClicked = [&store, app] { store.openApp(app); };
    row->onAction = [&store, app] { store.act(app); };
    return row;
}

View* addBreadcrumb(View* parent, const std::vector<std::pair<std::string, std::function<void()>>>& parts)
{
    Row* row = parent->addChild<Row>();
    row->setSpacing(10);
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) {
            Icon* chevron = row->addChild<Icon>("chevron-right-sm", 14);
            chevron->setColor(style::color(Role::TertiaryText));
        }
        if (parts[i].second) {
            ActionButton* link = row->addChild<ActionButton>(parts[i].first, ActionButton::Kind::Link);
            link->onClicked = parts[i].second;
        } else {
            addText(row, parts[i].first, 15, 600);
        }
    }
    return row;
}

std::string ago(int64_t when)
{
    if (when <= 0)
        return tr("never");
    const int64_t seconds = std::max<int64_t>(0, int64_t(std::time(nullptr)) - when);
    if (seconds < 60)
        return tr("just now");
    if (seconds < 3600)
        return format(tr("{n} min ago"), {{"n", std::to_string(seconds / 60)}});
    if (seconds < 86400)
        return format(tr("{n} h ago"), {{"n", std::to_string(seconds / 3600)}});
    if (seconds < 2 * 86400)
        return tr("yesterday");
    if (seconds < 60 * 86400)
        return format(tr("{n} days ago"), {{"n", std::to_string(seconds / 86400)}});
    return format(tr("{n} months ago"), {{"n", std::to_string(seconds / (30 * 86400))}});
}

View* addDivider(View* parent)
{
    class Divider : public View
    {
    public:
        SizeF sizeHint() const override { return {10, 1}; }

    protected:
        void paint(Painter& painter) override { painter.fillRect(localRect(), resolve(Role::Divider)); }
    };
    View* divider = parent->addChild<Divider>();
    divider->setLayoutHints({.fillWidth = true});
    return divider;
}

namespace {

// Lays its children out in rows, wrapping as needed: tags, related searches.
class Flow : public View
{
public:
    explicit Flow(double spacing = 10)
        : m_spacing(spacing)
    {
        setLayoutHints({.fillWidth = true});
    }
    double heightForWidth(double width) const override { return place(width, false); }
    SizeF sizeHint() const override { return {300, place(300, false)}; }

protected:
    void layout() override { place(geometry().width, true); }

private:
    double place(double width, bool apply) const
    {
        double x = 0;
        double y = 0;
        double row = 0;
        for (const auto& child : children()) {
            const SizeF size = child->sizeHint();
            if (x > 0 && x + size.width > width) {
                x = 0;
                y += row + m_spacing;
                row = 0;
            }
            if (apply)
                child->setGeometry({x, y, size.width, size.height});
            x += size.width + m_spacing;
            row = std::max(row, size.height);
        }
        return y + row;
    }
    double m_spacing;
};

// "⇅ Sort: Most popular ⌄", a menu of orders.
class SortButton : public Pressable
{
public:
    SortButton(Sort current, std::vector<Sort> sorts)
        : m_sorts(std::move(sorts))
    {
        m_icon = addChild<Icon>("sort", 18);
        m_label = addText(this, label(current), 15, 600);
        m_chevron = addChild<Icon>("chevron-down-sm", 16);
        m_chevron->setColor(style::color(Role::SecondaryText));
        onClicked = [this] {
            auto* menu = new Menu;
            for (Sort sort : m_sorts)
                menu->addItem(name(sort), [this, sort] {
                    if (onSort)
                        onSort(sort);
                });
            menu->onClosed = [menu] { bayu::base::EventLoop::current()->post([menu] { delete menu; }); };
            menu->popupFor(this, 4);
        };
    }
    std::function<void(Sort)> onSort;
    SizeF sizeHint() const override { return {m_label->sizeHint().width + 90, 46}; }

    static std::string name(Sort sort)
    {
        switch (sort) {
        case Sort::Popular:
            return tr("Most popular");
        case Sort::Recent:
            return tr("Recently updated");
        case Sort::Name:
            return tr("Name");
        case Sort::Relevance:
            return tr("Relevance");
        }
        return {};
    }

protected:
    void layout() override
    {
        const double h = geometry().height;
        m_icon->setGeometry({18, (h - 18) / 2, 18, 18});
        const SizeF label = m_label->sizeHint();
        m_label->setGeometry({46, (h - label.height) / 2, label.width, label.height});
        m_chevron->setGeometry({geometry().width - 34, (h - 16) / 2, 16, 16});
    }
    void paint(Painter& painter) override
    {
        painter.drawRoundedRect(localRect(), {12, 0, Corners::Circular},
                                isHovered() ? resolve(Role::Tonal) : resolve(Role::Card), 1,
                                resolve(Role::CardBorder));
    }

private:
    static std::string label(Sort sort)
    {
        return sort == Sort::Relevance ? name(sort) : format(tr("Sort: {order}"), {{"order", name(sort)}});
    }
    std::vector<Sort> m_sorts;
    Icon* m_icon;
    Label* m_label;
    Icon* m_chevron;
};

// A title and a line under it, and something on the right.
Row* addPageHeading(View* parent, std::string title, std::string subtitle, double size = 38)
{
    Row* row = parent->addChild<Row>();
    row->setLayoutHints({.fillWidth = true});
    Column* text = row->addChild<Column>();
    text->setSpacing(6);
    text->setLayoutHints({.fillWidth = true});
    Label* heading = addText(text, std::move(title), size, 700);
    heading->setWrap(true);
    if (!subtitle.empty())
        addText(text, std::move(subtitle), 16, 400, Role::SecondaryText);
    return row;
}

// Chips in a row, one on.
Row* addChips(View* parent, const std::vector<std::pair<std::string, std::string>>& chips, const std::string& on,
              std::function<void(const std::string&)> choose)
{
    Row* row = parent->addChild<Row>();
    row->setSpacing(10);
    row->setLayoutHints({.fillWidth = true});
    for (const auto& [id, text] : chips) {
        Chip* chip = row->addChild<Chip>(text);
        chip->setChecked(id == on);
        const std::string value = id;
        chip->onClicked = [choose, value] { choose(value); };
    }
    return row;
}

// Applies a chip's id to a query.
void applyFilter(Query& query, const std::string& filter)
{
    if (filter == "flathub")
        query.sources = {Source::Flathub};
    else if (filter == "fdroid")
        query.sources = {Source::FDroid};
    else if (filter == "open-source")
        query.openSource = true;
    else if (filter == "verified")
        query.verified = true;
    else if (filter == "recent")
        query.recentlyUpdated = true;
    else if (!filter.empty())
        query.filter = filter;
}

Color categoryTint(const std::string& id)
{
    static const std::vector<std::pair<std::string, uint32_t>> tints{
        {"productivity", 0x3B82F6}, {"creative", 0xEC4899}, {"developer", 0x22C55E}, {"games", 0xA855F7},
        {"audio", 0xF97316},        {"video", 0x06B6D4},    {"education", 0xEAB308}, {"utilities", 0x6B7280},
    };
    for (const auto& [category, tint] : tints) {
        if (category == id)
            return rgb(tint);
    }
    return rgb(0x3B82F6);
}

std::string categoryBlurb(const std::string& id)
{
    if (id == "creative")
        return tr("apps for drawing, photo, audio, and video work");
    if (id == "games")
        return tr("games, from puzzles to racing");
    if (id == "developer")
        return tr("tools for writing, testing and shipping code");
    if (id == "productivity")
        return tr("apps for mail, notes, money and the office");
    if (id == "audio")
        return tr("players, podcasts and audio tools");
    if (id == "video")
        return tr("cameras, galleries and video players");
    if (id == "education")
        return tr("apps for learning, reading and science");
    return tr("tools that keep the computer tidy and safe");
}

// --- Discover -------------------------------------------------------------

class DiscoverPage : public StorePage
{
public:
    explicit DiscoverPage(StoreWindow& store)
        : StorePage(store)
    {
        Backend& backend = this->backend();
        if (backend.isLoading()) {
            addLoading();
            return;
        }
        if (!backend.syncError().empty())
            addChild<Banner>(Banner::Kind::Warning, tr("The catalog could not be refreshed"),
                             backend.syncError() + "\n" + tr("What the store had before is shown; it tries again later."))
                ->setLayoutHints({.fillWidth = true});
        HeroBanner* hero = addChild<HeroBanner>();
        hero->setLayoutHints({.fillWidth = true, .preferredHeight = 232});
        hero->setApps(backend.featured(), [&store](const App& app) { return store.actionFor(app); });
        hero->onOpen = [&store](const App& app) { store.openApp(app); };
        hero->onAction = [&store](const App& app) { store.act(app); };

        addSection(tr("Popular on Flathub"), tr("See all"), Source::Flathub, Sort::Popular);
        addSection(tr("Fresh on F-Droid"), tr("See all"), Source::FDroid, Sort::Recent);

        Column* section = addChild<Column>();
        section->setSpacing(16);
        section->setLayoutHints({.fillWidth = true});
        SectionHeader* recent = section->addChild<SectionHeader>(tr("Recently updated"), tr("Browse"));
        recent->onLink = [&store] { store.go({Route::Kind::Trending}); };
        Query query;
        query.sort = Sort::Recent;
        query.limit = 6;
        const std::vector<App> apps = backend.query(query).apps;
        Grid* grid = section->addChild<Grid>(380, 18, 2);
        for (size_t column = 0; column < 2; ++column) {
            Card* card = grid->addChild<Card>(14);
            card->setSpacing(4);
            for (size_t i = column * 3; i < apps.size() && i < column * 3 + 3; ++i)
                addAppRow(store, card, apps[i], {.iconSize = 48});
        }
    }
    std::string title() const override { return tr("Discover"); }

private:
    void addSection(std::string title, std::string link, Source source, Sort sort)
    {
        Query query;
        query.sources = {source};
        query.sort = sort;
        query.limit = 4;
        const std::vector<App> apps = backend().query(query).apps;
        if (apps.empty())
            return;
        Column* section = addChild<Column>();
        section->setSpacing(16);
        section->setLayoutHints({.fillWidth = true});
        SectionHeader* header = section->addChild<SectionHeader>(std::move(title), std::move(link));
        header->onLink = [this, source] {
            store().go({.kind = Route::Kind::Source, .source = source});
        };
        Grid* grid = section->addChild<Grid>(190, 18, 4);
        for (const App& app : apps)
            addAppCard(store(), grid, app);
    }
    void addLoading()
    {
        addStretch();
        Label* title = addText(this, tr("Getting the catalog"), 24, 700);
        title->setLayoutHints({.alignment = Alignment::Center, .margins = {0, 120, 0, 0}});
        Label* text = addText(this, tr("Apps from Flathub and F-Droid appear in a moment. This happens once; later "
                                       "the store opens instantly, offline too."),
                              15, 400, Role::SecondaryText);
        text->setWrap(true);
        text->setTextAlignment(Label::TextAlignment::Center);
        text->setLayoutHints({.fillWidth = true, .margins = {80, 0, 80, 0}});
    }
};

// --- Trending -------------------------------------------------------------

class TrendingPage : public StorePage
{
public:
    TrendingPage(StoreWindow& store, const Route& route)
        : StorePage(store)
    {
        const bool games = route.filter == "games";
        Row* heading = addPageHeading(this, tr("Trending this week"), tr("What people install, and what changed"));
        heading->addChild<Row>();
        addChips(this, {{"", tr("Apps")}, {"games", tr("Games")}}, route.filter, [&store, route](const std::string& id) {
            Route next = route;
            next.filter = id;
            store.replace(next);
        });
        Grid* grid = addChild<Grid>(300, 18, 3);
        addList(grid, tr("Most installed"), Sort::Popular, {Source::Flathub}, games, false);
        addList(grid, tr("Open source picks"), Sort::Recent, {Source::FDroid}, games, true);
        addList(grid, tr("Recently updated"), Sort::Recent, {}, games, false);
    }
    std::string title() const override { return tr("Trending"); }

private:
    void addList(View* parent, std::string title, Sort sort, std::vector<Source> sources, bool games, bool skip)
    {
        Card* card = parent->addChild<Card>(16);
        card->setSpacing(4);
        Row* header = card->addChild<Row>();
        header->setLayoutHints({.fillWidth = true, .margins = {6, 0, 0, 8}});
        addText(header, std::move(title), 16, 600);
        header->addStretch();
        Query query;
        query.sort = sort;
        query.sources = sources;
        query.games = games;
        // Open source picks: the ones after the freshest, so the lists differ.
        query.offset = skip ? 5 : 0;
        query.limit = 7;
        int rank = 0;
        for (const App& app : backend().query(query).apps)
            addAppRow(store(), card, app, {.rank = ++rank, .iconSize = 40});
    }
};

// --- Categories -----------------------------------------------------------

class CategoryTile : public Pressable
{
public:
    explicit CategoryTile(const Category& category)
        : m_tint(categoryTint(category.id))
    {
        m_icon = addChild<Icon>(category.glyph, 20);
        m_icon->setColor(ColorRecipe(m_tint));
        m_name = addText(this, category.name, 17, 600);
        m_count = addText(this, format(tr("{n} apps"), {{"n", style::formatCount(category.count)}}), 14, 400,
                          Role::SecondaryText);
    }
    SizeF sizeHint() const override { return {220, 128}; }

protected:
    void layout() override
    {
        m_icon->setGeometry({28, 28, 20, 20});
        const SizeF name = m_name->sizeHint();
        m_name->setGeometry({18, 74, geometry().width - 36, name.height});
        const SizeF count = m_count->sizeHint();
        m_count->setGeometry({18, 74 + name.height + 2, geometry().width - 36, count.height});
    }
    void paint(Painter& painter) override
    {
        painter.drawRoundedRect(localRect(), {16, 0, Corners::Circular}, resolve(Role::Card), 1,
                                resolve(Role::CardBorder));
        if (isHovered())
            painter.drawRoundedRect(localRect(), {16, 0, Corners::Circular}, Theme::current().text.withAlpha(0.025));
        painter.drawRoundedRect({18, 18, 40, 40}, {10, 0, Corners::Circular}, m_tint.withAlpha(0.12));
    }

private:
    Color m_tint;
    Icon* m_icon;
    Label* m_name;
    Label* m_count;
};

class CategoriesPage : public StorePage
{
public:
    explicit CategoriesPage(StoreWindow& store)
        : StorePage(store)
    {
        int total = backend().count(Source::Flathub) + backend().count(Source::FDroid);
        addPageHeading(this, tr("Find your next app by category"),
                       format(tr("{n} apps from Flathub and F-Droid"), {{"n", style::formatCount(total)}}), 32);
        Grid* grid = addChild<Grid>(200, 18, 4);
        for (const Category& category : backend().categories()) {
            CategoryTile* tile = grid->addChild<CategoryTile>(category);
            const std::string id = category.id;
            tile->onClicked = [&store, id] { store.go({Route::Kind::Category, id}); };
        }
    }
    std::string title() const override { return tr("Categories"); }
};

// --- A category or a source: a grid with filters and pages --------------

class ListingPage : public StorePage
{
public:
    ListingPage(StoreWindow& store, const Route& route)
        : StorePage(store)
        , m_route(route)
    {
        constexpr int perPage = 12;
        Query query;
        query.sort = route.sort;
        query.offset = (route.page - 1) * perPage;
        query.limit = perPage;
        std::vector<std::pair<std::string, std::string>> chips{{"", tr("All")}};
        std::string heading;
        std::string blurb;
        if (route.kind == Route::Kind::Source) {
            query.sources = {route.source};
            heading = sourceName(route.source);
            m_title = heading;
            chips.push_back({"open-source", tr("Open source")});
            if (route.source == Source::Flathub)
                chips.push_back({"verified", tr("Verified")});
            chips.push_back({"recent", tr("Recently updated")});
        } else {
            query.category = route.param;
            heading = categoryName(route.param);
            m_title = tr("Categories");
            for (const Category& category : storeCategories()) {
                if (category.id == route.param) {
                    for (const std::string& filter : category.filters)
                        chips.push_back({filter, filter});
                }
            }
            chips.push_back({"flathub", "Flathub"});
            chips.push_back({"fdroid", "F-Droid"});
            addBreadcrumb(this, {{tr("Categories"), [&store] { store.go({Route::Kind::Categories}); }},
                                 {heading, nullptr}});
        }
        applyFilter(query, route.filter);
        const Page page = backend().query(query);

        if (route.kind == Route::Kind::Source) {
            blurb = format(route.source == Source::Flathub ? tr("{n} apps, sandboxed with Flatpak")
                                                           : tr("{n} free Android apps, built from source"),
                           {{"n", style::formatCount(page.total)}});
        } else {
            blurb = format(tr("{n} {what}"), {{"n", style::formatCount(page.total)}, {"what", categoryBlurb(route.param)}});
        }
        Row* row = addPageHeading(this, heading, blurb, 40);
        SortButton* sort = row->addChild<SortButton>(route.sort, std::vector<Sort>{Sort::Popular, Sort::Recent, Sort::Name});
        sort->setLayoutHints({.alignment = Alignment::Start});
        sort->onSort = [this](Sort order) {
            Route next = m_route;
            next.sort = order;
            next.page = 1;
            this->store().replace(next);
        };
        addChips(this, chips, route.filter, [this](const std::string& id) {
            Route next = m_route;
            next.filter = id;
            next.page = 1;
            this->store().replace(next);
        });
        if (route.kind == Route::Kind::Source && route.source == Source::FDroid && !backend().androidAvailable())
            addChild<Banner>(Banner::Kind::Info, tr("Android apps are off"),
                             tr("F-Droid apps run in Android (lapis). Installing one offers to turn it on."))
                ->setLayoutHints({.fillWidth = true});
        Grid* grid = addChild<Grid>(190, 18, 4);
        for (const App& app : page.apps)
            addAppCard(store, grid, app);
        if (page.apps.empty())
            addText(this, tr("Nothing here yet."), 15, 400, Role::SecondaryText);
        Pagination* pages = addChild<Pagination>();
        pages->setLayoutHints({.alignment = Alignment::Center});
        pages->setPages(route.page, (page.total + perPage - 1) / perPage);
        pages->onPage = [this](int number) {
            Route next = m_route;
            next.page = number;
            this->store().replace(next);
        };
    }
    std::string title() const override { return m_title; }

private:
    Route m_route;
    std::string m_title;
};

// --- An app ---------------------------------------------------------------

class AppPage : public StorePage
{
public:
    AppPage(StoreWindow& store, const Route& route)
        : StorePage(store)
    {
        Backend& backend = this->backend();
        const std::optional<App> found = backend.app(route.param, route.source);
        if (!found) {
            m_title = tr("Not found");
            addText(this, tr("This app is no longer in the catalog."), 16, 400, Role::SecondaryText);
            return;
        }
        const App& app = *found;
        m_title = app.name;
        // Screenshots, counts and sizes arrive once; the page is rebuilt then.
        backend.details(app, [](const App&) {});

        Row* top = addChild<Row>();
        top->setSpacing(24);
        top->setLayoutHints({.fillWidth = true});
        AppIcon* icon = top->addChild<AppIcon>(112);
        icon->setApp(app);
        icon->setLayoutHints({.alignment = Alignment::Start});
        Column* info = top->addChild<Column>();
        info->setSpacing(8);
        info->setLayoutHints({.fillWidth = true});
        Label* name = addText(info, app.name, 36, 700);
        name->setWrap(true);
        Row* by = info->addChild<Row>();
        by->setSpacing(6);
        Label* developer = addText(by, app.developer.empty() ? tr("Unknown developer") : app.developer, 17, 500);
        developer->setColor(ColorRecipe(ColorId::Highlight));
        if (app.verified) {
            Icon* check = by->addChild<Icon>("circle-check", 17);
            check->setColor(ColorRecipe(ColorId::Highlight));
            addText(by, tr("· Verified publisher"), 17, 400, Role::SecondaryText);
        } else if (app.source == Source::FDroid) {
            addText(by, tr("· Built from source by F-Droid"), 17, 400, Role::SecondaryText);
        }
        Flow* tags = info->addChild<Flow>(10);
        for (const std::string& category : app.categories)
            tags->addChild<Tag>(categoryName(category));
        tags->addChild<Tag>(app.source == Source::Flathub ? "Flatpak" : "Android");
        tags->addChild<Tag>(tr("Sandboxed"));
        if (app.source == Source::FDroid
            && std::find(app.antiFeatures.begin(), app.antiFeatures.end(), "Ads") == app.antiFeatures.end())
            tags->addChild<Tag>(tr("No ads"));

        Column* actions = top->addChild<Column>();
        actions->setSpacing(10);
        actions->setLayoutHints({.alignment = Alignment::Start});
        Row* buttons = actions->addChild<Row>();
        buttons->setSpacing(12);
        buttons->setLayoutHints({.alignment = Alignment::End});
        const bool favorite = backend.isFavorite(app.id, app.source);
        IconButton* heart = buttons->addChild<IconButton>(favorite ? "heart-filled" : "heart", 46);
        if (favorite)
            heart->setTint(ColorRecipe(Color::rgb(0xF43F5E)));
        heart->onClicked = [&backend, app, favorite] { backend.setFavorite(app.id, app.source, !favorite); };
        IconButton* share = buttons->addChild<IconButton>("share", 46);
        share->onClicked = [this, app] {
            const std::string link = app.source == Source::Flathub ? "https://flathub.org/apps/" + app.id
                                                                   : "https://f-droid.org/packages/" + app.id + "/";
            Clipboard::setText(link);
            this->store().showError(tr("Link copied"), link);
        };
        if (backend.installedApp(app.id, app.source) && !backend.job(app.id, app.source)) {
            IconButton* remove = buttons->addChild<IconButton>("trash", 46);
            remove->onClicked = [this, app] { this->store().askToRemove(app.id, app.source); };
        }
        const AppAction action = store.actionFor(app);
        std::string text = action.text;
        for (size_t i = 1; i < text.size() && text[i] != '%'; ++i)
            text[i] = char(std::tolower(static_cast<unsigned char>(text[i])));
        PillButton* install = buttons->addChild<PillButton>(text, action.kind == PillButton::Kind::Neutral
                                                                      ? PillButton::Kind::Neutral
                                                                      : PillButton::Kind::Filled);
        install->setLarge(true);
        if (action.does == AppAction::Does::Install || action.does == AppAction::Does::EnableAndroid)
            install->setGlyph("download");
        install->setProgress(action.progress);
        install->onClicked = [&store, app] { store.act(app); };
        std::string sizes;
        if (app.downloadSize)
            sizes = style::formatSize(app.downloadSize);
        if (app.installedSize)
            sizes += (sizes.empty() ? "" : " · ")
                     + format(tr("needs {size} free"), {{"size", style::formatSize(app.installedSize)}});
        if (app.armOnly)
            sizes += (sizes.empty() ? "" : " · ") + std::string(tr("needs ARM translation"));
        if (!sizes.empty()) {
            Label* caption = addText(actions, sizes, 15, 400, Role::SecondaryText);
            caption->setLayoutHints({.alignment = Alignment::End});
        }

        std::vector<StatStrip::Stat> stats;
        stats.push_back({sourceName(app), sourceGlyph(app.source),
                         app.verified ? tr("verified publisher") : app.source == Source::FDroid ? tr("free software")
                                                                                                 : tr("community")});
        stats.push_back({app.license.empty() ? tr("Unknown") : app.license, "scale",
                         app.freeLicense ? tr("free software") : tr("proprietary")});
        if (app.installs > 0)
            stats.push_back({compact(app.installs), "download", tr("installs")});
        if (app.downloadSize)
            stats.push_back({style::formatSize(app.downloadSize), "hard-drive", tr("download size")});
        if (!app.version.empty())
            stats.push_back({"v" + app.version, "refresh", format(tr("updated {when}"), {{"when", ago(app.updated)}})});
        StatStrip* strip = addChild<StatStrip>();
        strip->setLayoutHints({.fillWidth = true});
        strip->setStats(stats);

        if (!app.screenshots.empty()) {
            ScreenshotStrip* shots = addChild<ScreenshotStrip>();
            shots->setLayoutHints({.fillWidth = true});
            shots->setScreenshots(app.screenshots, tintFor(app));
        }

        Row* columns = addChild<Row>();
        columns->setSpacing(28);
        columns->setLayoutHints({.fillWidth = true});
        Column* about = columns->addChild<Column>();
        about->setSpacing(14);
        about->setLayoutHints({.fillWidth = true, .stretch = 1.5, .alignment = Alignment::Start, .preferredWidth = 0});
        addText(about, tr("About this app"), 20, 700);
        Label* description = addText(about, app.description.empty() ? app.summary : app.description, 16, 400,
                                     Role::SecondaryText);
        description->setWrap(true);
        description->setLayoutHints({.fillWidth = true});
        if (!app.releaseNotes.empty()) {
            Card* notes = about->addChild<Card>(20);
            notes->setSpacing(10);
            notes->setLayoutHints({.fillWidth = true});
            Row* head = notes->addChild<Row>();
            head->setLayoutHints({.fillWidth = true});
            addText(head, format(tr("What's new in {version}"), {{"version", app.version}}), 17, 600);
            head->addStretch();
            addText(head, ago(app.updated), 14, 400, Role::SecondaryText);
            size_t start = 0;
            while (start < app.releaseNotes.size()) {
                size_t end = app.releaseNotes.find('\n', start);
                if (end == std::string::npos)
                    end = app.releaseNotes.size();
                const std::string line = app.releaseNotes.substr(start, end - start);
                start = end + 1;
                if (line.empty())
                    continue;
                Row* item = notes->addChild<Row>();
                item->setSpacing(12);
                item->setLayoutHints({.fillWidth = true});
                Icon* check = item->addChild<Icon>("check-sm", 16);
                check->setColor(style::color(Role::Success));
                check->setLayoutHints({.alignment = Alignment::Start, .margins = {0, 3, 0, 0}});
                Label* text = addText(item, line, 15, 400, Role::TonalText);
                text->setWrap(true);
                text->setLayoutHints({.fillWidth = true});
            }
        }

        Card* permissions = columns->addChild<Card>(22);
        permissions->setSpacing(14);
        permissions->setLayoutHints({.fillWidth = true, .alignment = Alignment::Start, .preferredWidth = 0});
        Row* head = permissions->addChild<Row>();
        head->setLayoutHints({.fillWidth = true});
        addText(head, tr("Source & permissions"), 18, 600);
        head->addStretch();
        Card* sourceBox = permissions->addChild<Card>(14);
        sourceBox->setFill(Role::InfoFill, Role::CardBorder);
        sourceBox->setLayoutHints({.fillWidth = true});
        Row* source = sourceBox->addChild<Row>();
        source->setSpacing(14);
        source->setLayoutHints({.fillWidth = true});
        class SourceTile : public View
        {
        public:
            explicit SourceTile(Source source)
            {
                m_icon = addChild<Icon>(sourceGlyph(source), 22);
                m_icon->setColor(ColorRecipe(Color::rgb(0xFFFFFF)));
            }
            SizeF sizeHint() const override { return {44, 44}; }

        protected:
            void layout() override { m_icon->setGeometry({11, 11, 22, 22}); }
            void paint(Painter& painter) override
            {
                painter.drawRoundedRect(localRect(), {11, 0, Corners::Circular}, Theme::current().highlight);
            }

        private:
            Icon* m_icon;
        };
        source->addChild<SourceTile>(app.source);
        Column* sourceText = source->addChild<Column>();
        sourceText->setLayoutHints({.fillWidth = true});
        addText(sourceText, sourceName(app), 16, 600);
        addText(sourceText, app.id, 14, 400, Role::SecondaryText);
        if (app.permissions.empty())
            addText(permissions, tr("Asks for no special access."), 15, 400, Role::SecondaryText);
        for (const Permission& permission : app.permissions) {
            Row* line = permissions->addChild<Row>();
            line->setSpacing(14);
            line->setLayoutHints({.fillWidth = true});
            Icon* glyph = line->addChild<Icon>(permission.glyph, 18);
            glyph->setColor(style::color(Role::SecondaryText));
            Label* label = addText(line, tr(permission.name.c_str()), 16, 500);
            label->setLayoutHints({.fillWidth = true});
            addText(line, tr(permission.detail.c_str()), 14, 400, Role::SecondaryText);
        }
        addDivider(permissions);
        Flow* badges = permissions->addChild<Flow>(10);
        auto missing = [&app](const char* feature) {
            return std::find(app.antiFeatures.begin(), app.antiFeatures.end(), feature) == app.antiFeatures.end();
        };
        if (app.source == Source::FDroid) {
            if (missing("Ads"))
                badges->addChild<Tag>(tr("No ads"), Tag::Kind::Green, "check-sm");
            if (missing("Tracking"))
                badges->addChild<Tag>(tr("No tracking"), Tag::Kind::Green, "check-sm");
            if (missing("NonFreeDep") && missing("NonFreeNet"))
                badges->addChild<Tag>(tr("No non-free deps"), Tag::Kind::Green, "check-sm");
            for (const std::string& feature : app.antiFeatures)
                badges->addChild<Tag>(feature, Tag::Kind::Neutral, "alert");
        } else {
            if (app.freeLicense)
                badges->addChild<Tag>(tr("Free software"), Tag::Kind::Green, "check-sm");
            if (app.verified)
                badges->addChild<Tag>(tr("Verified"), Tag::Kind::Green, "check-sm");
        }
    }
    std::string title() const override { return m_title; }

private:
    static std::string compact(int64_t count)
    {
        char text[32];
        if (count >= 1000000)
            snprintf(text, sizeof(text), "%.1fM", double(count) / 1e6);
        else if (count >= 1000)
            snprintf(text, sizeof(text), "%.0fK", double(count) / 1e3);
        else
            snprintf(text, sizeof(text), "%lld", static_cast<long long>(count));
        return text;
    }
    std::string m_title;
};

// --- The library ------------------------------------------------------------

// The library's tabs: a grey track, the current tab white.
class TabStrip : public View
{
public:
    TabStrip(const std::vector<std::tuple<std::string, std::string, int>>& tabs, const std::string& current,
             std::function<void(const std::string&)> choose)
    {
        for (const auto& [id, text, badge] : tabs) {
            Tab* tab = addChild<Tab>(text, badge, id == current);
            const std::string value = id;
            tab->onClicked = [choose, value] { choose(value); };
            m_tabs.push_back(tab);
        }
    }
    SizeF sizeHint() const override
    {
        double width = 12;
        for (Tab* tab : m_tabs)
            width += tab->sizeHint().width + 4;
        return {width, 54};
    }

protected:
    void layout() override
    {
        double x = 6;
        for (Tab* tab : m_tabs) {
            const double w = tab->sizeHint().width;
            tab->setGeometry({x, 6, w, geometry().height - 12});
            x += w + 4;
        }
    }
    void paint(Painter& painter) override
    {
        painter.drawRoundedRect(localRect(), {14, 0, Corners::Circular}, resolve(Role::Tonal));
    }

private:
    class Tab : public Pressable
    {
    public:
        Tab(std::string text, int badge, bool current)
            : m_current(current)
        {
            m_label = addText(this, std::move(text), 16, current ? 600 : 500,
                              current ? Role::TonalText : Role::SecondaryText);
            if (badge > 0) {
                m_badge = addText(this, std::to_string(badge), 12, 700);
                m_badge->setColor(ColorRecipe(Color::rgb(0xFFFFFF)));
                m_badge->setTextAlignment(Label::TextAlignment::Center);
            }
        }
        SizeF sizeHint() const override
        {
            return {m_label->sizeHint().width + 44 + (m_badge ? 34 : 0), 42};
        }

    protected:
        void layout() override
        {
            const double h = geometry().height;
            const SizeF label = m_label->sizeHint();
            m_label->setGeometry({22, (h - label.height) / 2, label.width, label.height});
            if (m_badge)
                m_badge->setGeometry({22 + label.width + 10, (h - 20) / 2 + 1, 26, 18});
        }
        void paint(Painter& painter) override
        {
            if (m_current)
                painter.drawRoundedRect(localRect(), {10, 0, Corners::Circular}, resolve(Role::Card), 1,
                                        resolve(Role::CardBorder));
            if (m_badge)
                painter.drawRoundedRect(m_badge->geometry().adjusted(0, -1, 0, 1), {10, 0, Corners::Circular},
                                        Theme::current().highlight);
        }

    private:
        bool m_current;
        Label* m_label;
        Label* m_badge = nullptr;
    };
    std::vector<Tab*> m_tabs;
};

// An update or download: its versions, notes and progress.
class UpdateRow : public Pressable
{
public:
    UpdateRow(StoreWindow& store, const App& app, const InstalledApp* installed, const std::optional<Job>& job)
    {
        Backend& backend = store.backend();
        m_active = job.has_value();
        AppIcon* icon = addChild<AppIcon>(56);
        icon->setApp(app);
        m_icon = icon;
        m_text = addChild<Column>();
        m_text->setSpacing(4);
        Row* name = m_text->addChild<Row>();
        name->setSpacing(12);
        addText(name, app.name, 18, 600);
        if (installed && !installed->newVersion.empty())
            name->addChild<Tag>(installed->version + "  ›  " + installed->newVersion);
        else if (!app.version.empty())
            name->addChild<Tag>(app.version);
        Label* notes = addText(m_text, firstLine(app), 15, 400, Role::SecondaryText);
        notes->setElide(Label::Elide::Right);
        if (job) {
            ThinProgress* bar = m_text->addChild<ThinProgress>();
            bar->setLayoutHints({.fillWidth = true, .margins = {0, 6, 0, 4}});
            bar->setValue(std::max(0.0, job->progress));
            std::string bytes = job->startingAndroid ? std::string(tr("Starting Android…"))
                                : job->bytesTotal  ? style::formatSize(job->bytesDone) + " " + tr("of") + " "
                                                         + style::formatSize(job->bytesTotal)
                                                   : std::string(tr("Waiting"));
            if (job->paused)
                bytes += " · " + std::string(tr("paused"));
            else if (job->bytesPerSecond)
                bytes += " · " + style::formatSize(job->bytesPerSecond) + "/s";
            addText(m_text, bytes, 14, 400, Role::SecondaryText);
            IconButton* pause = addChild<IconButton>(job->paused ? "play" : "pause", 44);
            pause->onClicked = [&backend, app, paused = job->paused] {
                if (paused)
                    backend.resume(app.id, app.source);
                else
                    backend.pause(app.id, app.source);
            };
            m_buttons.push_back(pause);
            PillButton* percent = addChild<PillButton>(
                job->progress >= 0 ? std::to_string(int(job->progress * 100)) + "%" : std::string(tr("QUEUED")));
            m_buttons.push_back(percent);
        } else {
            addText(m_text, installed && installed->updateSize ? style::formatSize(installed->updateSize)
                                                               : style::formatSize(app.downloadSize),
                    14, 400, Role::SecondaryText);
            PillButton* update = addChild<PillButton>(tr("UPDATE"));
            update->onClicked = [&backend, app] { backend.update(app.id, app.source); };
            m_buttons.push_back(update);
            IconButton* more = addChild<IconButton>("more", 36);
            more->onClicked = [&store, &backend, app, more] {
                auto* menu = new Menu;
                menu->addItem(tr("Open"), [&backend, app] { backend.launch(app.id, app.source); });
                menu->addItem(tr("About this app"), [&store, app] { store.openApp(app); });
                menu->addSeparator();
                menu->addItem(tr("Uninstall"), [&store, app] { store.askToRemove(app.id, app.source); });
                menu->onClosed = [menu] { bayu::base::EventLoop::current()->post([menu] { delete menu; }); };
                menu->popupFor(more, 4);
            };
            m_buttons.push_back(more);
        }
        onClicked = [&store, app] { store.openApp(app); };
    }
    SizeF sizeHint() const override { return {600, m_active ? 132 : 118}; }

protected:
    void layout() override
    {
        const double w = geometry().width;
        const double h = geometry().height;
        m_icon->setGeometry({22, (h - 56) / 2, 56, 56});
        double right = w - 22;
        for (auto it = m_buttons.rbegin(); it != m_buttons.rend(); ++it) {
            const SizeF size = (*it)->sizeHint();
            right -= size.width;
            (*it)->setGeometry({right, (h - size.height) / 2, size.width, size.height});
            right -= 12;
        }
        const double textX = 22 + 56 + 20;
        const double textW = right - 12 - textX;
        const double textH = m_text->heightForWidth(textW);
        m_text->setGeometry({textX, (h - textH) / 2, textW, textH});
    }
    void paint(Painter& painter) override
    {
        const Color accent = Theme::current().highlight;
        if (m_active)
            painter.drawRoundedRect(localRect(), {16, 0, Corners::Circular}, accent.withAlpha(0.06), 1,
                                    accent.withAlpha(0.35));
        else
            painter.drawRoundedRect(localRect(), {16, 0, Corners::Circular}, resolve(Role::Card), 1,
                                    resolve(Role::CardBorder));
    }

private:
    static std::string firstLine(const App& app)
    {
        const std::string& notes = app.releaseNotes.empty() ? app.summary : app.releaseNotes;
        return notes.substr(0, notes.find('\n'));
    }
    bool m_active;
    View* m_icon;
    Column* m_text;
    std::vector<View*> m_buttons;
};

class LibraryPage : public StorePage
{
public:
    LibraryPage(StoreWindow& store, const Route& route)
        : StorePage(store)
    {
        Backend& backend = this->backend();
        const std::string tab = route.param.empty() ? "updates" : route.param;
        const std::vector<InstalledApp> updates = backend.updates();
        const std::vector<InstalledApp> installed = backend.installed();

        Row* bar = addChild<Row>();
        bar->setSpacing(16);
        bar->setLayoutHints({.fillWidth = true});
        bar->addChild<TabStrip>(std::vector<std::tuple<std::string, std::string, int>>{
                                    {"updates", tr("Updates"), int(updates.size())},
                                    {"installed", tr("Installed"), 0},
                                    {"recent", tr("Recent"), 0},
                                    {"downloads", tr("Downloads"), int(backend.jobs().size())}},
                                tab, [&store](const std::string& id) { store.replace({Route::Kind::Library, id}); });
        bar->addStretch();
        addText(bar, format(tr("Last checked {when}"), {{"when", ago(backend.lastChecked())}}), 15, 400,
                Role::SecondaryText);
        ActionButton* check = bar->addChild<ActionButton>(tr("Check"));
        check->setLeadingGlyph("refresh");
        check->onClicked = [&backend] { backend.checkUpdates(); };

        if (tab == "updates")
            buildUpdates(updates, installed);
        else if (tab == "installed")
            buildInstalled(installed, false);
        else if (tab == "recent")
            buildInstalled(backend.recent(), true);
        else
            buildDownloads();
    }
    std::string title() const override { return tr("Library"); }

private:
    App appFor(const std::string& id, Source source) const
    {
        if (std::optional<App> app = backend().app(id, source))
            return *app;
        App app;
        app.id = id;
        app.source = source;
        app.name = id.substr(id.rfind('.') + 1);
        return app;
    }

    void buildUpdates(const std::vector<InstalledApp>& updates, const std::vector<InstalledApp>& installed)
    {
        uint64_t total = 0;
        bool flathub = false;
        bool fdroid = false;
        for (const InstalledApp& update : updates) {
            total += update.updateSize;
            (update.source == Source::Flathub ? flathub : fdroid) = true;
        }
        std::string from = flathub && fdroid ? tr("from Flathub and F-Droid")
                           : flathub         ? tr("from Flathub")
                                             : tr("from F-Droid");
        Row* heading = addPageHeading(
            this,
            updates.empty() ? std::string(tr("Everything is up to date"))
                            : bayu::i18n::trn("{n} update available", "{n} updates available", updates.size()),
            updates.empty() ? std::string(tr("Updates install in the background as they arrive."))
                            : style::formatSize(total) + " " + tr("total") + " · " + from + " · "
                                  + tr("installs in the background"),
            32);
        if (!updates.empty()) {
            ActionButton* all = heading->addChild<ActionButton>(tr("Update all"), ActionButton::Kind::Primary);
            all->setLeadingGlyph("download");
            all->setLayoutHints({.alignment = Alignment::Start});
            all->onClicked = [this] { backend().updateAll(); };
        }
        Column* list = addChild<Column>();
        list->setSpacing(14);
        list->setLayoutHints({.fillWidth = true});
        for (const InstalledApp& update : updates) {
            const App app = appFor(update.id, update.source);
            list->addChild<UpdateRow>(store(), app, &update, backend().job(update.id, update.source))
                ->setLayoutHints({.fillWidth = true});
        }

        Row* header = addChild<Row>();
        header->setSpacing(14);
        header->setLayoutHints({.fillWidth = true});
        addText(header, tr("Installed"), 21, 700);
        header->addChild<Tag>(format(tr("{n} apps"), {{"n", std::to_string(installed.size())}}));
        header->addStretch();
        ActionButton* manage = header->addChild<ActionButton>(tr("Manage"), ActionButton::Kind::Link);
        manage->onClicked = [this] { store().replace({Route::Kind::Library, "installed"}); };
        std::vector<InstalledApp> plain;
        for (const InstalledApp& app : installed) {
            if (app.newVersion.empty())
                plain.push_back(app);
        }
        Grid* grid = addChild<Grid>(380, 18, 2);
        for (size_t column = 0; column < 2; ++column) {
            Card* card = grid->addChild<Card>(14);
            card->setSpacing(4);
            for (size_t i = column * 2; i < plain.size() && i < column * 2 + 2; ++i)
                addAppRow(store(), card, appFor(plain[i].id, plain[i].source), {.iconSize = 48});
            if (card->children().empty())
                card->setVisible(false);
        }

        const StorageUsage usage = backend().storage();
        Card* storage = addChild<Card>(22);
        storage->setSpacing(14);
        storage->setLayoutHints({.fillWidth = true});
        Row* head = storage->addChild<Row>();
        head->setSpacing(12);
        head->setLayoutHints({.fillWidth = true});
        Icon* disk = head->addChild<Icon>("hard-drive", 20);
        disk->setColor(style::color(Role::SecondaryText));
        addText(head, tr("App storage"), 18, 600);
        head->addStretch();
        const uint64_t used = usage.apps + usage.runtimes + usage.cache;
        addText(head,
                format(tr("{used} of {total} used"),
                       {{"used", style::formatSize(used)}, {"total", style::formatSize(usage.total)}}),
                15, 400, Role::SecondaryText);
        StorageBar* barView = storage->addChild<StorageBar>();
        barView->setLayoutHints({.fillWidth = true});
        barView->setUsage(usage);
        Row* legend = storage->addChild<Row>();
        legend->setSpacing(26);
        addLegend(legend, Theme::current().highlight, tr("Apps") + std::string(" ") + style::formatSize(usage.apps));
        addLegend(legend, Color::rgb(0x8B5CF6), tr("Runtimes") + std::string(" ") + style::formatSize(usage.runtimes));
        addLegend(legend, Color::rgb(0xF5A524), tr("Cache") + std::string(" ") + style::formatSize(usage.cache));
        addLegend(legend, Color::rgb(0xD4D4D8), tr("Free") + std::string(" ") + style::formatSize(usage.free));
    }

    void buildInstalled(const std::vector<InstalledApp>& apps, bool recent)
    {
        addPageHeading(this,
                       recent ? std::string(tr("Recently installed"))
                              : bayu::i18n::trn("{n} app installed", "{n} apps installed", apps.size()),
                       recent ? std::string(tr("Installs and updates, newest first"))
                              : std::string(tr("From Flathub and F-Droid")),
                       32);
        Grid* grid = addChild<Grid>(380, 18, 2);
        for (const InstalledApp& installed : apps) {
            Card* card = grid->addChild<Card>(10);
            AppRow* row = addAppRow(store(), card, appFor(installed.id, installed.source), {.iconSize = 48});
            (void)row;
            Row* meta = card->addChild<Row>();
            meta->setSpacing(10);
            meta->setLayoutHints({.fillWidth = true, .margins = {8, 0, 8, 4}});
            std::string text = installed.version.empty() ? std::string() : "v" + installed.version + " · ";
            text += style::formatSize(installed.size);
            if (recent)
                text += " · " + format(tr("installed {when}"), {{"when", ago(installed.installed)}});
            Label* label = addText(meta, text, 13, 400, Role::TertiaryText);
            label->setLayoutHints({.fillWidth = true});
            ActionButton* remove = meta->addChild<ActionButton>(tr("Uninstall"), ActionButton::Kind::Link);
            const std::string id = installed.id;
            const Source source = installed.source;
            remove->onClicked = [this, id, source] { store().askToRemove(id, source); };
        }
        if (apps.empty())
            addText(this, tr("Nothing installed from the store yet."), 15, 400, Role::SecondaryText);
    }

    void buildDownloads()
    {
        const std::vector<Job> jobs = backend().jobs();
        addPageHeading(this, tr("Downloads"),
                       jobs.empty() ? std::string(tr("Nothing is downloading."))
                                    : std::string(tr("One at a time, in the background")),
                       32);
        Column* list = addChild<Column>();
        list->setSpacing(14);
        list->setLayoutHints({.fillWidth = true});
        for (const Job& job : jobs) {
            const App app = appFor(job.id, job.source);
            std::optional<InstalledApp> installed = backend().installedApp(job.id, job.source);
            list->addChild<UpdateRow>(store(), app, installed ? &*installed : nullptr, job)
                ->setLayoutHints({.fillWidth = true});
        }
    }
};

// --- Search -----------------------------------------------------------------

class SearchPage : public StorePage
{
public:
    SearchPage(StoreWindow& store, const Route& route)
        : StorePage(store)
        , m_route(route)
    {
        constexpr int perPage = 20;
        const auto started = std::chrono::steady_clock::now();
        Query query;
        query.text = route.param;
        query.sort = route.sort == Sort::Popular ? Sort::Relevance : route.sort;
        query.offset = (route.page - 1) * perPage;
        query.limit = perPage;
        applyFilter(query, route.filter);
        const Page page = backend().query(query);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        m_title = tr("Search");

        char elapsed[32];
        snprintf(elapsed, sizeof(elapsed), "%.2fs", seconds);
        Row* heading = addPageHeading(
            this, format(tr("Results for “{text}”"), {{"text", route.param}}),
            format(tr("{n} apps across Flathub and F-Droid · {time}"),
                   {{"n", style::formatCount(page.total)}, {"time", elapsed}}),
            34);
        SortButton* sort =
            heading->addChild<SortButton>(query.sort, std::vector<Sort>{Sort::Relevance, Sort::Popular, Sort::Recent});
        sort->setLayoutHints({.alignment = Alignment::Start});
        sort->onSort = [this](Sort order) {
            Route next = m_route;
            next.sort = order == Sort::Relevance ? Sort::Popular : order;
            next.page = 1;
            this->store().replace(next);
        };
        addChips(this,
                 {{"", tr("All")},
                  {"flathub", "Flathub"},
                  {"fdroid", "F-Droid"},
                  {"open-source", tr("Open source")},
                  {"verified", tr("Verified")},
                  {"recent", tr("Recently updated")}},
                 route.filter, [this](const std::string& id) {
                     Route next = m_route;
                     next.filter = id;
                     next.page = 1;
                     this->store().replace(next);
                 });

        Row* columns = addChild<Row>();
        columns->setSpacing(28);
        columns->setLayoutHints({.fillWidth = true});
        Column* results = columns->addChild<Column>();
        results->setSpacing(14);
        results->setLayoutHints({.fillWidth = true, .stretch = 2.4, .alignment = Alignment::Start, .preferredWidth = 0});
        int index = 0;
        for (const App& app : page.apps) {
            AppRow::Options options{.iconSize = 64, .summary = true, .developer = true, .license = true};
            if (app.verified) {
                options.badge = tr("Verified");
            } else if (app.source == Source::FDroid) {
                options.badge = tr("Open source");
                options.badgeGreen = true;
            }
            AppRow* row = addAppRow(store, results, app, options);
            (void)row;
            ++index;
        }
        if (page.apps.empty())
            addText(results, tr("No apps match. Try fewer or other words."), 16, 400, Role::SecondaryText);
        Pagination* pages = results->addChild<Pagination>();
        pages->setLayoutHints({.alignment = Alignment::Center});
        pages->setPages(route.page, (page.total + perPage - 1) / perPage);
        pages->onPage = [this](int number) {
            Route next = m_route;
            next.page = number;
            this->store().replace(next);
        };

        Column* side = columns->addChild<Column>();
        side->setSpacing(22);
        side->setLayoutHints({.fillWidth = true, .alignment = Alignment::Start, .preferredWidth = 0});
        const std::vector<std::string> related = backend().relatedSearches(route.param);
        if (!related.empty()) {
            Card* card = side->addChild<Card>(20);
            card->setSpacing(14);
            card->setLayoutHints({.fillWidth = true});
            addText(card, tr("Related searches"), 18, 600);
            Flow* flow = card->addChild<Flow>(10);
            for (const std::string& text : related) {
                Chip* chip = flow->addChild<Chip>(text);
                chip->onClicked = [&store, text] { store.go({Route::Kind::Search, text}); };
            }
        }
        const auto developers = backend().topDevelopers(query);
        if (!developers.empty()) {
            Card* card = side->addChild<Card>(20);
            card->setSpacing(12);
            card->setLayoutHints({.fillWidth = true});
            addText(card, tr("Top developers"), 18, 600);
            for (const auto& [name, count] : developers) {
                Row* row = card->addChild<Row>();
                row->setSpacing(14);
                row->setLayoutHints({.fillWidth = true});
                class Initial : public View
                {
                public:
                    explicit Initial(const std::string& name)
                    {
                        App fake;
                        fake.id = name;
                        m_tint = rgb(tintFor(fake));
                        m_label = addText(this, name.substr(0, 1), 17, 600);
                        m_label->setColor(ColorRecipe(Color::rgb(0xFFFFFF)));
                        m_label->setTextAlignment(Label::TextAlignment::Center);
                        m_label->setVerticallyCentered(true);
                    }
                    SizeF sizeHint() const override { return {44, 44}; }

                protected:
                    void layout() override { m_label->setGeometry(localRect()); }
                    void paint(Painter& painter) override
                    {
                        painter.drawRoundedRect(localRect(), {22, 0, Corners::Circular}, m_tint);
                    }

                private:
                    Color m_tint;
                    Label* m_label;
                };
                row->addChild<Initial>(name);
                Column* text = row->addChild<Column>();
                text->setLayoutHints({.fillWidth = true});
                addText(text, name, 16, 600);
                addText(text, bayu::i18n::trn("{n} app", "{n} apps", count), 14, 400, Role::SecondaryText);
                const std::string developer = name;
                ActionButton* open = row->addChild<ActionButton>(std::string(), ActionButton::Kind::Link);
                open->setTrailingGlyph("chevron-right-sm");
                open->onClicked = [&store, developer] { store.go({Route::Kind::Search, developer}); };
            }
        }
    }
    std::string title() const override { return m_title; }

private:
    Route m_route;
    std::string m_title;
};

// --- Settings ---------------------------------------------------------------

class SettingsPage : public StorePage
{
public:
    explicit SettingsPage(StoreWindow& store)
        : StorePage(store)
    {
        Backend& backend = this->backend();
        addPageHeading(this, tr("Store settings"), tr("Where apps come from and how they stay up to date"), 32);

        Column* group = section(tr("SOURCES"));
        Column* sources = group->addChild<Column>();
        sources->setSpacing(10);
        sources->setLayoutHints({.fillWidth = true});
        for (Source source : {Source::Flathub, Source::FDroid}) {
            ToggleCard* card = sources->addChild<ToggleCard>(
                sourceName(source),
                format(tr("{n} apps · refreshed {when}"),
                       {{"n", style::formatCount(backend.count(source))}, {"when", ago(backend.lastSynced(source))}}));
            card->setChecked(backend.sourceEnabled(source));
            card->onToggled = [&backend, source](bool on) { backend.setSourceEnabled(source, on); };
        }
        ActionButton* refresh = group->addChild<ActionButton>(tr("Refresh the catalog"));
        refresh->setLeadingGlyph("refresh");
        refresh->onClicked = [&backend] { backend.sync(); };

        group = section(tr("ANDROID APPS"));
        addAndroidCard(group, backend);
        ToggleCard* arm = group->addChild<ToggleCard>(tr("Show apps built only for ARM"),
                                               tr("They run through ARM translation (libhoudini), slower and not "
                                                  "always well"));
        arm->setChecked(backend.showArmOnly());
        arm->onToggled = [&backend](bool on) { backend.setShowArmOnly(on); };

        group = section(tr("UPDATES"));
        ToggleCard* automatic = group->addChild<ToggleCard>(tr("Install updates automatically"),
                                                     format(tr("In the background · last checked {when}"),
                                                            {{"when", ago(backend.lastChecked())}}));
        automatic->setChecked(backend.automaticUpdates());
        automatic->onToggled = [&backend](bool on) { backend.setAutomaticUpdates(on); };

        group = section(tr("STORAGE"));
        Card* storage = group->addChild<Card>(20);
        storage->setLayoutHints({.fillWidth = true});
        Row* row = storage->addChild<Row>();
        row->setSpacing(12);
        row->setLayoutHints({.fillWidth = true});
        Column* cache = row->addChild<Column>();
        cache->setLayoutHints({.fillWidth = true});
        addText(cache, tr("Downloaded pictures and packages"), 16, 600);
        addText(cache, style::formatSize(backend.cacheSize()), 14, 400, Role::SecondaryText);
        ActionButton* clear = row->addChild<ActionButton>(tr("Clear cache"));
        clear->onClicked = [&backend] { backend.clearCache(); };
        for (ToggleCard* card : {arm, automatic})
            card->setLayoutHints({.fillWidth = true});
    }
    std::string title() const override { return tr("Settings"); }

private:
    // Android: how its setup goes, or whether it runs, with the button
    // that turns it on or off.
    void addAndroidCard(Column* group, Backend& backend)
    {
        const AndroidSetup setup = backend.androidSetup();
        const AndroidState state = backend.androidState();
        Card* android = group->addChild<Card>(20);
        android->setSpacing(12);
        android->setLayoutHints({.fillWidth = true});
        Row* status = android->addChild<Row>();
        status->setSpacing(12);
        status->setLayoutHints({.fillWidth = true});

        std::string glyph = "smartphone-off";
        Role color = Role::SecondaryText;
        std::string title;
        std::string detail;
        if (setup.busy()) {
            glyph = setup.stage == AndroidSetup::Stage::Downloading ? "download" : "smartphone";
            color = Role::SecondaryText;
            describeSetup(setup, &title, &detail);
        } else if (setup.stage == AndroidSetup::Stage::Failed) {
            glyph = "alert";
            color = Role::Danger;
            title = tr("Android could not be turned on");
            detail = setup.error;
        } else if (state == AndroidState::Running) {
            glyph = "circle-check";
            color = Role::Success;
            title = tr("Android is running");
            detail = tr("F-Droid apps install into Android (lapis) and open in their own windows.");
        } else if (state == AndroidState::Off) {
            title = tr("Android is off");
            detail = tr("Opening an Android app starts it again.");
        } else {
            title = tr("Android apps are off");
            detail = tr("Turn on Android (lapis) to install apps from F-Droid. It downloads Android once, about 1.3 GB.");
        }

        Icon* icon = status->addChild<Icon>(glyph, 20);
        icon->setColor(style::color(color));
        Column* text = status->addChild<Column>();
        text->setSpacing(2);
        text->setLayoutHints({.fillWidth = true});
        addText(text, title, 16, 600);
        Label* note = addText(text, detail, 14, 400, setup.stage == AndroidSetup::Stage::Failed ? Role::Danger
                                                                                                  : Role::SecondaryText);
        note->setWrap(true);

        if (setup.busy()) {
            ActionButton* busy = status->addChild<ActionButton>(tr("Turning on"), ActionButton::Kind::Primary);
            busy->setBusy(true);
            ThinProgress* bar = android->addChild<ThinProgress>();
            bar->setLayoutHints({.fillWidth = true});
            bar->setValue(std::max(0.0, setup.progress));
            bar->setVisible(setup.progress >= 0);
            if (!setup.pendingApp.empty())
                addText(android, format(tr("{name} installs once Android runs."), {{"name", setup.pendingApp}}), 14,
                        400, Role::SecondaryText);
        } else if (state == AndroidState::Running) {
            ActionButton* off = status->addChild<ActionButton>(tr("Turn off"));
            off->setLeadingGlyph("power");
            off->onClicked = [this] { this->store().askToDisableAndroid(); };
        } else {
            ActionButton* on = status->addChild<ActionButton>(
                setup.stage == AndroidSetup::Stage::Failed ? tr("Try again") : tr("Turn on"), ActionButton::Kind::Primary);
            on->onClicked = [&backend] { backend.enableAndroid(); };
        }
    }

    Column* section(std::string title)
    {
        Column* group = addChild<Column>();
        group->setSpacing(10);
        group->setLayoutHints({.fillWidth = true});
        addSectionLabel(group, std::move(title));
        return group;
    }
};

} // namespace

std::unique_ptr<StorePage> createPage(StoreWindow& store, const Route& route)
{
    switch (route.kind) {
    case Route::Kind::Discover:
        return std::make_unique<DiscoverPage>(store);
    case Route::Kind::Trending:
        return std::make_unique<TrendingPage>(store, route);
    case Route::Kind::Categories:
        return std::make_unique<CategoriesPage>(store);
    case Route::Kind::Category:
    case Route::Kind::Source:
        return std::make_unique<ListingPage>(store, route);
    case Route::Kind::App:
        return std::make_unique<AppPage>(store, route);
    case Route::Kind::Library:
        return std::make_unique<LibraryPage>(store, route);
    case Route::Kind::Search:
        return std::make_unique<SearchPage>(store, route);
    case Route::Kind::Settings:
        return std::make_unique<SettingsPage>(store);
    }
    return std::make_unique<DiscoverPage>(store);
}

} // namespace store
