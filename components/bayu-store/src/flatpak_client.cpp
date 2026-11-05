// SPDX-License-Identifier: GPL-3.0-or-later

#include "flatpak_client.h"

#include <flatpak.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <thread>

namespace store::flatpak {

namespace {

constexpr const char* kRemote = "flathub";
constexpr const char* kRepoFile = "/usr/share/bayu-store/flathub.flatpakrepo";

template<typename T>
struct Unref
{
    void operator()(T* object) const
    {
        if (object)
            g_object_unref(object);
    }
};
template<typename T>
using Ref = std::unique_ptr<T, Unref<T>>;

struct ArrayUnref
{
    void operator()(GPtrArray* array) const
    {
        if (array)
            g_ptr_array_unref(array);
    }
};
using Array = std::unique_ptr<GPtrArray, ArrayUnref>;

std::string message(GError*& error)
{
    std::string text = error ? error->message : "Flatpak failed.";
    g_clear_error(&error);
    return text;
}

Ref<FlatpakInstallation> system(std::string* failure)
{
    GError* error = nullptr;
    FlatpakInstallation* installation = flatpak_installation_new_system(nullptr, &error);
    if (!installation && failure)
        *failure = message(error);
    else
        g_clear_error(&error);
    return Ref<FlatpakInstallation>(installation);
}

int64_t deployed(FlatpakInstalledRef* ref)
{
    const char* dir = flatpak_installed_ref_get_deploy_dir(ref);
    struct stat st{};
    return dir && ::stat(dir, &st) == 0 ? int64_t(st.st_mtime) : 0;
}

} // namespace

std::string ensureFlathub()
{
    return ensureRemote(kRemote, kRepoFile);
}

std::string ensureRemote(const char* name, const char* repoFile)
{
    // No installation at all while /var/lib/flatpak/repo is missing.
    GError* error = nullptr;
    if (Ref<FlatpakInstallation> installation = system(nullptr)) {
        Ref<FlatpakRemote> existing(flatpak_installation_get_remote_by_name(installation.get(), name, nullptr, &error));
        g_clear_error(&error);
        if (existing)
            return {};
    }
    if (::access(repoFile, R_OK) != 0)
        return std::string(name) + " is not set up, and its description is missing.";
    // The command line, not flatpak_installation_add_remote: only it asks the
    // system helper to create /var/lib/flatpak/repo when there is none yet.
    const char* argv[] = {"flatpak", "remote-add", "--system", "--if-not-exists", name, repoFile, nullptr};
    gchar* errors = nullptr;
    gint status = 0;
    if (!g_spawn_sync(nullptr, const_cast<gchar**>(argv), nullptr, GSpawnFlags(G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL),
                      nullptr, nullptr, nullptr, &errors, &status, &error))
        return message(error);
    std::string text = errors ? errors : "";
    g_free(errors);
    if (!g_spawn_check_wait_status(status, nullptr))
        return text.empty() ? std::string("Cannot add ") + name + "." : text;
    return {};
}

std::vector<Installed> installed(std::string* failure)
{
    std::vector<Installed> out;
    Ref<FlatpakInstallation> installation = system(failure);
    if (!installation)
        return out;
    GError* error = nullptr;
    Array refs(flatpak_installation_list_installed_refs_by_kind(installation.get(), FLATPAK_REF_KIND_APP, nullptr,
                                                                &error));
    if (!refs) {
        if (failure)
            *failure = message(error);
        return out;
    }
    for (guint i = 0; i < refs->len; ++i) {
        auto* ref = FLATPAK_INSTALLED_REF(g_ptr_array_index(refs.get(), i));
        Installed app;
        app.id = flatpak_ref_get_name(FLATPAK_REF(ref));
        const char* version = flatpak_installed_ref_get_appdata_version(ref);
        app.version = version ? version : "";
        app.size = flatpak_installed_ref_get_installed_size(ref);
        app.installed = deployed(ref);
        out.push_back(app);
    }
    return out;
}

std::vector<Update> updates(std::string* failure)
{
    std::vector<Update> out;
    Ref<FlatpakInstallation> installation = system(failure);
    if (!installation)
        return out;
    GError* error = nullptr;
    Array refs(flatpak_installation_list_installed_refs_for_update(installation.get(), nullptr, &error));
    if (!refs) {
        if (failure)
            *failure = message(error);
        return out;
    }
    for (guint i = 0; i < refs->len; ++i) {
        auto* ref = FLATPAK_INSTALLED_REF(g_ptr_array_index(refs.get(), i));
        if (flatpak_ref_get_kind(FLATPAK_REF(ref)) != FLATPAK_REF_KIND_APP)
            continue;
        out.push_back({flatpak_ref_get_name(FLATPAK_REF(ref)), 0});
    }
    return out;
}

Usage usage()
{
    Usage out;
    Ref<FlatpakInstallation> installation = system(nullptr);
    if (!installation)
        return out;
    GError* error = nullptr;
    Array refs(flatpak_installation_list_installed_refs(installation.get(), nullptr, &error));
    g_clear_error(&error);
    if (!refs)
        return out;
    for (guint i = 0; i < refs->len; ++i) {
        auto* ref = FLATPAK_INSTALLED_REF(g_ptr_array_index(refs.get(), i));
        const uint64_t size = flatpak_installed_ref_get_installed_size(ref);
        (flatpak_ref_get_kind(FLATPAK_REF(ref)) == FLATPAK_REF_KIND_APP ? out.apps : out.runtimes) += size;
    }
    return out;
}

namespace {

struct Run
{
    const Progress* progress;
    GCancellable* cancellable;
};

void progressChanged(FlatpakTransactionProgress* progress, gpointer data)
{
    auto* run = static_cast<Run*>(data);
    const int percent = flatpak_transaction_progress_get_progress(progress);
    const uint64_t done = flatpak_transaction_progress_get_bytes_transferred(progress);
    (*run->progress)(done, 0, percent);
}

void newOperation(FlatpakTransaction*, FlatpakTransactionOperation*, FlatpakTransactionProgress* progress,
                  gpointer data)
{
    flatpak_transaction_progress_set_update_frequency(progress, 300);
    g_signal_connect(progress, "changed", G_CALLBACK(progressChanged), data);
}

} // namespace

std::string run(Operation operation, const std::string& id, const std::string& remote, const Progress& progress,
                std::atomic<bool>& cancel)
{
    std::string failure;
    const char* from = remote.empty() ? kRemote : remote.c_str();
    if (operation == Operation::Install) {
        failure = remote.empty() || remote == kRemote ? ensureFlathub() : std::string();
        if (!failure.empty())
            return failure;
    }
    Ref<FlatpakInstallation> installation = system(&failure);
    if (!installation)
        return failure;
    // A thread of its own for the transaction's GLib context.
    GMainContext* context = g_main_context_new();
    g_main_context_push_thread_default(context);
    GError* error = nullptr;
    Ref<FlatpakTransaction> transaction(flatpak_transaction_new_for_installation(installation.get(), nullptr, &error));
    if (!transaction) {
        g_main_context_pop_thread_default(context);
        g_main_context_unref(context);
        return message(error);
    }
    const std::string ref = "app/" + id + "/" + flatpak_get_default_arch() + "/stable";
    gboolean added = FALSE;
    switch (operation) {
    case Operation::Install:
        added = flatpak_transaction_add_install(transaction.get(), from, ref.c_str(), nullptr, &error);
        break;
    case Operation::Update:
        added = flatpak_transaction_add_update(transaction.get(), ref.c_str(), nullptr, nullptr, &error);
        break;
    case Operation::Remove:
        added = flatpak_transaction_add_uninstall(transaction.get(), ref.c_str(), &error);
        break;
    }
    std::string result;
    if (!added) {
        result = message(error);
    } else {
        GCancellable* cancellable = g_cancellable_new();
        Run state{&progress, cancellable};
        g_signal_connect(transaction.get(), "new-operation", G_CALLBACK(newOperation), &state);
        // Cancelling comes from another thread: watch the flag meanwhile.
        std::atomic<bool> finished = false;
        std::thread watcher([&] {
            while (!finished) {
                if (cancel) {
                    g_cancellable_cancel(cancellable);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        });
        if (!flatpak_transaction_run(transaction.get(), cancellable, &error))
            result = g_cancellable_is_cancelled(cancellable) ? std::string("stopped") : message(error);
        g_clear_error(&error);
        finished = true;
        watcher.join();
        g_object_unref(cancellable);
    }
    transaction.reset();
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
    return result;
}

std::string launch(const std::string& id)
{
    std::string failure;
    Ref<FlatpakInstallation> installation = system(&failure);
    if (!installation)
        return failure;
    GError* error = nullptr;
    // Without a branch, launch looks for "master"; Flathub ships "stable".
    Ref<FlatpakInstalledRef> current(
        flatpak_installation_get_current_installed_app(installation.get(), id.c_str(), nullptr, &error));
    if (!current)
        return message(error);
    if (!flatpak_installation_launch(installation.get(), id.c_str(), nullptr,
                                     flatpak_ref_get_branch(FLATPAK_REF(current.get())), nullptr, nullptr, &error))
        return message(error);
    return {};
}

} // namespace store::flatpak
