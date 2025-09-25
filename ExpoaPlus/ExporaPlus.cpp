#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <shlobj.h>
#include <shobjidl_core.h>
#include <shellapi.h>
#include <ole2.h>
#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <json/json.h>
#include <winrt/Windows.System.h> // <-- neu: VirtualKey / hresult handling support
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <sstream>
#include <iomanip>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Windows::ApplicationModel::DataTransfer;
using namespace Windows::Storage;

struct FileItem {
    std::wstring name;
    std::wstring fullPath;
    bool isFolder;
    ULONGLONG size;
    FILETIME modifiedTime;
    winrt::com_ptr<IShellItemImageFactory> imageFactory;
};

struct ExplorerFinal : ApplicationT<ExplorerFinal>
{
    Window m_window{ nullptr };
    NavigationView m_navView{ nullptr };
    GridView m_fileGrid{ nullptr };
    TextBox m_addressBar{ nullptr };
    TextBox m_searchBox{ nullptr };
    std::vector<FileItem> currentItems;
    std::vector<FileItem> filteredItems;
    bool sortAscending = true;
    int sortColumn = 0; // 0=Name,1=Type,2=Size,3=Date
    std::vector<std::wstring> favorites;

    // --- Neue Members ---
    StackPanel m_breadcrumb{ nullptr };
    Image m_previewImage{ nullptr };
    TextBlock m_previewText{ nullptr };
    Border m_previewBorder{ nullptr };
    ToggleSwitch m_recursiveToggle{ nullptr };
    Button m_analyzeButton{ nullptr };
    Button m_batchRenameButton{ nullptr };
    std::vector<std::wstring> recentFiles;

    // Neue UI / Funktionalitäts-Erweiterungen
    ComboBox m_searchHistoryCombo{ nullptr };
    Button m_searchButton{ nullptr };
    Button m_compressButton{ nullptr };
    Button m_extractButton{ nullptr };
    Button m_propertiesButton{ nullptr };
    Button m_copyPathButton{ nullptr };
    Button m_openTerminalButton{ nullptr };
    ToggleButton m_toggleHiddenButton{ nullptr };
    bool m_showHidden = false;
    std::vector<std::wstring> searchHistory;

    // Neue Methoden/Prototypen
    void UpdateBreadcrumb(std::wstring const& path);
    fire_and_forget ShowPreview(FileItem const& fi);
    void FileSelectionChanged(IInspectable const&, SelectionChangedEventArgs const&);
    void RecursiveSearch(std::wstring const& path, std::wstring const& filter, std::vector<FileItem>& outItems);
    fire_and_forget BatchRename(IInspectable const&, RoutedEventArgs const&);
    fire_and_forget AnalyzeSizes(IInspectable const&, RoutedEventArgs const&);
    void LoadRecent();
    void SaveRecent();
    void AddToRecent(std::wstring const& path);

    // Neue Prototypen
    void PerformSearch();
    void ToggleShowHidden(IInspectable const&, RoutedEventArgs const&);
    void CompressSelected(IInspectable const&, RoutedEventArgs const&);
    void ExtractSelected(IInspectable const&, RoutedEventArgs const&);
    void CopyPathToClipboard(IInspectable const&, RoutedEventArgs const&);
    void OpenInTerminal(IInspectable const&, RoutedEventArgs const&);
    void ShowProperties(IInspectable const&, RoutedEventArgs const&);

    void OnLaunched(LaunchActivatedEventArgs const&)
    {
        m_window = Window();
        m_window.Title(L"Ultimate Explorer Final");

        m_navView = NavigationView();
        Grid mainGrid;
        mainGrid.RowDefinitions().Append(RowDefinition{ GridLengthHelper::Auto() }); // Adressleiste
        mainGrid.RowDefinitions().Append(RowDefinition{ GridLengthHelper::Auto() }); // Suchleiste
        mainGrid.RowDefinitions().Append(RowDefinition{ GridLengthHelper::Auto() }); // Sortierleiste
        mainGrid.RowDefinitions().Append(RowDefinition{ GridLengthHelper::Star() }); // FileGrid

        // Adressleiste
        m_addressBar = TextBox();
        m_addressBar.Text(L"C:\\");
        m_addressBar.KeyUp({ this, &ExplorerFinal::AddressBarKeyUp });
        mainGrid.Children().Append(m_addressBar);

        // Suchleiste
        m_searchBox = TextBox();
        m_searchBox.PlaceholderText(L"Suchen...");
        m_searchBox.TextChanged({ this, &ExplorerFinal::SearchTextChanged });
        Grid::SetRow(m_searchBox, 1);
        mainGrid.Children().Append(m_searchBox);

        // Sortierleiste: zusätzlicher "Typ"-Button
        StackPanel sortPanel;
        sortPanel.Orientation(Orientation::Horizontal);
        sortPanel.Spacing(5);
        Button sortName; sortName.Content(box_value(winrt::hstring(L"Name"))); sortName.Click({ this, &ExplorerFinal::SortByName });
        Button sortSize; sortSize.Content(box_value(winrt::hstring(L"Größe"))); sortSize.Click({ this, &ExplorerFinal::SortBySize });
        Button sortDate; sortDate.Content(box_value(winrt::hstring(L"Datum"))); sortDate.Click({ this, &ExplorerFinal::SortByDate });
        Button sortType; sortType.Content(box_value(winrt::hstring(L"Typ"))); sortType.Click({ this, &ExplorerFinal::SortByType });
        sortPanel.Children().Append(sortName);
        sortPanel.Children().Append(sortSize);
        sortPanel.Children().Append(sortType); // <-- neu
        sortPanel.Children().Append(sortDate);

        // Füge rekursiven Suche-Toggle + Analyse + Batch-Rename Buttons in SortPanel
        m_recursiveToggle = ToggleSwitch();
        m_recursiveToggle.IsOn(false);
        m_recursiveToggle.OnContent(winrt::box_value(winrt::hstring(L"Rekursiv")));
        sortPanel.Children().Append(m_recursiveToggle);

        m_analyzeButton = Button();
        m_analyzeButton.Content(winrt::box_value(winrt::hstring(L"Analyze Sizes")));
        m_analyzeButton.Click({ this, &ExplorerFinal::AnalyzeSizes });
        sortPanel.Children().Append(m_analyzeButton);

        m_batchRenameButton = Button();
        m_batchRenameButton.Content(winrt::box_value(winrt::hstring(L"Batch-Rename")));
        m_batchRenameButton.Click({ this, &ExplorerFinal::BatchRename });
        sortPanel.Children().Append(m_batchRenameButton);

        // Neue Buttons: Compress / Extract / Properties / CopyPath / OpenTerminal / ToggleHidden
        m_compressButton = Button(); m_compressButton.Content(winrt::box_value(winrt::hstring(L"Compress"))); m_compressButton.Click({ this, &ExplorerFinal::CompressSelected });
        m_extractButton = Button(); m_extractButton.Content(winrt::box_value(winrt::hstring(L"Extract"))); m_extractButton.Click({ this, &ExplorerFinal::ExtractSelected });
        m_propertiesButton = Button(); m_propertiesButton.Content(winrt::box_value(winrt::hstring(L"Properties"))); m_propertiesButton.Click({ this, &ExplorerFinal::ShowProperties });
        m_copyPathButton = Button(); m_copyPathButton.Content(winrt::box_value(winrt::hstring(L"Copy Path"))); m_copyPathButton.Click({ this, &ExplorerFinal::CopyPathToClipboard });
        m_openTerminalButton = Button(); m_openTerminalButton.Content(winrt::box_value(winrt::hstring(L"Open Terminal"))); m_openTerminalButton.Click({ this, &ExplorerFinal::OpenInTerminal });
        m_toggleHiddenButton = ToggleButton(); m_toggleHiddenButton.Content(winrt::box_value(winrt::hstring(L"Show Hidden"))); m_toggleHiddenButton.Checked({ this, &ExplorerFinal::ToggleShowHidden }); m_toggleHiddenButton.Unchecked({ this, &ExplorerFinal::ToggleShowHidden });

        sortPanel.Children().Append(m_compressButton);
        sortPanel.Children().Append(m_extractButton);
        sortPanel.Children().Append(m_propertiesButton);
        sortPanel.Children().Append(m_copyPathButton);
        sortPanel.Children().Append(m_openTerminalButton);
        sortPanel.Children().Append(m_toggleHiddenButton);

        // Search UI: Button + History ComboBox
        StackPanel searchStack;
        searchStack.Orientation(Orientation::Horizontal);
        m_searchHistoryCombo = ComboBox();
        m_searchHistoryCombo.PlaceholderText(L"History");
        m_searchHistoryCombo.SelectionChanged([this](auto const&, auto const&) {
            auto sel = m_searchHistoryCombo.SelectedItem();
            if (sel) m_searchBox.Text(unbox_value<hstring>(sel));
        });
        m_searchButton = Button();
        m_searchButton.Content(winrt::box_value(winrt::hstring(L"Search")));
        m_searchButton.Click([this](auto const&, auto const&) { PerformSearch(); });

        searchStack.Children().Append(m_searchBox);
        searchStack.Children().Append(m_searchButton);
        searchStack.Children().Append(m_searchHistoryCombo);
        Grid::SetRow(searchStack, 1);
        // replace previous single-searchBox append with the stack
        mainGrid.Children().Append(searchStack);

        // Datei-GridView
        m_fileGrid = GridView();
        Grid::SetRow(m_fileGrid, 3);
        m_fileGrid.IsItemClickEnabled(true);
        m_fileGrid.ItemClick({ this, &ExplorerFinal::FileItemClick });
        m_fileGrid.AllowDrop(true);
        m_fileGrid.DragItemsStarting({ this, &ExplorerFinal::DragStart });
        m_fileGrid.Drop({ this, &ExplorerFinal::DropFiles });
        mainGrid.Children().Append(m_fileGrid);

        // Kontextmenü: now use member items so Opening-handler can update them
        MenuFlyout flyout;
        MenuFlyoutItem copyItem; copyItem.Text(L"Kopieren"); copyItem.Click({ this, &ExplorerFinal::CopyItem });
        MenuFlyoutItem pasteItem; pasteItem.Text(L"Einfügen"); pasteItem.Click({ this, &ExplorerFinal::PasteItem });
        MenuFlyoutItem deleteItem; deleteItem.Text(L"Löschen"); deleteItem.Click({ this, &ExplorerFinal::DeleteItem });
        MenuFlyoutItem renameItem; renameItem.Text(L"Umbenennen"); renameItem.Click({ this, &ExplorerFinal::RenameItem });

        m_addFavItem = MenuFlyoutItem(); m_addFavItem.Text(L"Zu Favoriten hinzufügen"); m_addFavItem.Click({ this, &ExplorerFinal::AddToFavorites });
        m_remFavItem = MenuFlyoutItem(); m_remFavItem.Text(L"Aus Favoriten entfernen"); m_remFavItem.Click({ this, &ExplorerFinal::RemoveFromFavorites });

        flyout.Items().Append(copyItem);
        flyout.Items().Append(pasteItem);
        flyout.Items().Append(deleteItem);
        flyout.Items().Append(renameItem);
        flyout.Items().Append(m_addFavItem);
        flyout.Items().Append(m_remFavItem);

        // Opening-Handler: aktivieren/deaktivieren je nach Auswahl / Favoriten-Status
        flyout.Opening([this](auto const&, auto const&) {
            auto sel = m_fileGrid.SelectedItem();
            bool enableAdd = false;
            bool enableRem = false;
            if (sel) {
                hstring name = unbox_value<hstring>(sel);
                auto it = FindItemByName(name);
                if (it != currentItems.end()) {
                    std::wstring path = it->fullPath;
                    bool isFav = (std::find(favorites.begin(), favorites.end(), path) != favorites.end());
                    enableAdd = !isFav;
                    enableRem = isFav;
                }
            }
            m_addFavItem.IsEnabled(enableAdd);
            m_remFavItem.IsEnabled(enableRem);
        });

        m_fileGrid.ContextFlyout(flyout);

        // F5 Refresh: Window KeyDown handler registrieren
        m_window.KeyDown({ this, &ExplorerFinal::WindowKeyDown });

        m_navView.Content(mainGrid);
        LoadFavorites();
        LoadRecent();
        PopulateSidebar();
        PopulateFiles(m_addressBar.Text());

        m_window.Content(m_navView);
        m_window.Activate();

        // initiale Breadcrumb aktualisieren
        UpdateBreadcrumb(m_addressBar.Text().c_str());
    }

    // Sidebar: Laufwerke + Favoriten
    void PopulateSidebar() {
        m_navView.MenuItems().Clear(); // Verhindert doppelte Einträge
        DWORD drives = GetLogicalDrives();
        for (int i = 0; i < 26; i++) {
            if (drives & (1 << i)) {
                std::wstring drive = { wchar_t(L'A' + i), L':' };
                NavigationViewItem item;
                item.Content(winrt::box_value(winrt::hstring(drive))); // <-- sicherer box_value(hstring)
                item.Tag(winrt::box_value(winrt::hstring(drive)));
                item.Tapped([this, drive](auto&&, auto&&) {
                    m_addressBar.Text(drive + L"\\");
                    PopulateFiles(m_addressBar.Text());
                    });
                m_navView.MenuItems().Append(item);
            }
        }

        // Favoriten hinzufügen
        for (auto& fav : favorites) {
            NavigationViewItem favItem;
            favItem.Content(winrt::box_value(winrt::hstring(fav))); // <-- sicherer box_value(hstring)
            favItem.Tag(winrt::box_value(winrt::hstring(fav)));
            favItem.Tapped([this, fav](auto&&, auto&&) {
                m_addressBar.Text(fav);
                PopulateFiles(m_addressBar.Text());
                });
            m_navView.MenuItems().Append(favItem);
        }
    }

    // Dateien laden + Miniaturansichten
    void PopulateFiles(hstring path) {
        currentItems.clear();
        m_fileGrid.Items().Clear();
        std::wstring dir = path.c_str();
        if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) return;
        WIN32_FIND_DATA fd;
        HANDLE hFind = FindFirstFile((dir + L"\\*").c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return;

        do {
            if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
                // Skip hidden files unless enabled
                bool isHidden = (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
                if (isHidden && !m_showHidden) continue;

                FileItem fi;
                fi.name = fd.cFileName;
                fi.fullPath = dir + L"\\" + fd.cFileName;
                fi.isFolder = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                fi.size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                fi.modifiedTime = fd.ftLastWriteTime;

                if (!fi.isFolder) {
                    IShellItem* psi = nullptr;
                    SHCreateItemFromParsingName(fi.fullPath.c_str(), nullptr, IID_PPV_ARGS(&psi));
                    if (psi) {
                        winrt::com_ptr<IShellItemImageFactory> factory;
                        psi->QueryInterface(IID_PPV_ARGS(&factory));
                        fi.imageFactory = factory;
                        psi->Release();
                    }
                }

                currentItems.push_back(fi);
                m_fileGrid.Items().Append(box_value(winrt::hstring(fi.name))); // <-- store hstring-backed items
            }
        } while (FindNextFile(hFind, &fd));
        FindClose(hFind);
        filteredItems = currentItems;
        SortAndRefresh();
    }

    // Suchleiste filter
    void SearchTextChanged(IInspectable const&, TextChangedEventArgs const&) {
        std::wstring filter = m_searchBox.Text().c_str();
        filteredItems.clear();
        for (auto& fi : currentItems) {
            if (fi.name.find(filter) != std::wstring::npos) {
                filteredItems.push_back(fi);
            }
        }
        SortAndRefresh();
    }

    void AddressBarKeyUp(IInspectable const&, KeyRoutedEventArgs const& args) {
        if (args.Key() == Windows::System::VirtualKey::Enter)
            PopulateFiles(m_addressBar.Text());
    }

    void FileItemClick(IInspectable const&, ItemClickEventArgs const& args) {
        hstring clicked = unbox_value<hstring>(args.ClickedItem());
        auto it = std::find_if(filteredItems.begin(), filteredItems.end(),
            [&clicked](FileItem& fi) { return fi.name == clicked.c_str(); });
        if (it != filteredItems.end()) {
            if (it->isFolder) {
                m_addressBar.Text(it->fullPath);
                PopulateFiles(it->fullPath);
                UpdateBreadcrumb(it->fullPath);
            }
            else {
                ShellExecute(nullptr, L"open", it->fullPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                AddToRecent(it->fullPath);
            }
        }
    }

    void FileSelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
        auto sel = m_fileGrid.SelectedItem();
        if (!sel) {
            m_previewImage.Source(nullptr);
            m_previewText.Text(L"");
            return;
        }
        hstring name = unbox_value<hstring>(sel);
        auto it = std::find_if(currentItems.begin(), currentItems.end(),
            [&name](FileItem const& fi) { return fi.name == name.c_str(); });
        if (it != currentItems.end()) {
            // Show preview asynchronously
            ShowPreview(*it);
        }
    }

    fire_and_forget ShowPreview(FileItem const& fi) {
        try {
            if (fi.isFolder) {
                m_previewImage.Source(nullptr);
                m_previewText.Text(winrt::hstring(L"Folder: " + fi.name));
                co_return;
            }
            auto path = fi.fullPath;
            auto ext = std::filesystem::path(path).extension().wstring();
            // simple image preview by extension
            std::wstring extLower = ext;
            std::transform(extLower.begin(), extLower.end(), extLower.begin(), ::towlower);
            if (extLower == L".png" || extLower == L".jpg" || extLower == L".jpeg" || extLower == L".bmp" || extLower == L".gif") {
                try {
                    auto file = co_await StorageFile::GetFileFromPathAsync(hstring(path));
                    auto stream = co_await file.OpenReadAsync();
                    auto bitmap = winrt::Windows::UI::Xaml::Media::Imaging::BitmapImage();
                    bitmap.SetSource(stream);
                    m_previewImage.Source(bitmap);
                    m_previewText.Text(winrt::hstring(L"Image: " + fi.name));
                } catch (...) {
                    m_previewImage.Source(nullptr);
                    m_previewText.Text(winrt::hstring(L"Unable to preview image."));
                }
                co_return;
            }

            // text preview for small text files
            if (extLower == L".txt" || extLower == L".csv" || extLower == L".log" || extLower == L".md") {
                try {
                    auto file = co_await StorageFile::GetFileFromPathAsync(hstring(path));
                    auto text = co_await FileIO::ReadTextAsync(file);
                    std::wstring preview;
                    auto s = text.c_str();
                    preview = s;
                    if (preview.size() > 4000) preview = preview.substr(0, 4000) + L"...";
                    m_previewImage.Source(nullptr);
                    m_previewText.Text(winrt::hstring(preview));
                } catch (...) {
                    m_previewText.Text(winrt::hstring(L"Unable to load text preview."));
                }
                co_return;
            }

            // fallback: basic properties
            std::wstringstream ss;
            ss << L"Name: " << fi.name << L"\n";
            ss << L"Path: " << fi.fullPath << L"\n";
            ss << L"Size: " << fi.size << L" bytes\n";
            // convert sys time to readable
            SYSTEMTIME stUTC, stLocal;
            FileTimeToSystemTime(&fi.modifiedTime, &stUTC);
            SystemTimeToTzSpecificLocalTime(NULL, &stUTC, &stLocal);
            ss << L"Modified: " << stLocal.wDay << L"." << stLocal.wMonth << L"." << stLocal.wYear;
            m_previewImage.Source(nullptr);
            m_previewText.Text(winrt::hstring(ss.str()));
        } catch (...) {
            m_previewText.Text(winrt::hstring(L"Preview error."));
        }
    }

    // Breadcrumb helper
    void UpdateBreadcrumb(std::wstring const& path) {
        m_breadcrumb.Children().Clear();
        if (path.empty()) return;
        std::wstring p = path;
        if (p.back() == L'\\') p.pop_back();
        std::vector<std::wstring> parts;
        std::wstring temp;
        for (size_t i = 0; i < p.size(); ++i) {
            temp.push_back(p[i]);
            if (p[i] == L'\\') {
                parts.push_back(temp);
            }
        }
        // if final part not added (no trailing backslash)
        if (parts.empty()) parts.push_back(p + L"\\");
        // build breadcrumb buttons
        for (auto const& part : parts) {
            Button b;
            b.Content(winrt::box_value(winrt::hstring(part)));
            std::wstring tag = part;
            b.Click([this, tag](auto&&, auto&&) {
                m_addressBar.Text(tag);
                PopulateFiles(m_addressBar.Text());
                UpdateBreadcrumb(tag);
            });
            m_breadcrumb.Children().Append(b);
        }
    }

    // Rekursive Suche
    void RecursiveSearch(std::wstring const& path, std::wstring const& filter, std::vector<FileItem>& outItems) {
        try {
            for (auto const& entry : std::filesystem::directory_iterator(path)) {
                try {
                    auto name = entry.path().filename().wstring();
                    WIN32_FIND_DATA fd;
                    FileItem fi;
                    fi.name = name;
                    fi.fullPath = entry.path().wstring();
                    fi.isFolder = entry.is_directory();
                    if (!fi.isFolder) {
                        auto f = std::filesystem::file_size(entry.path());
                        fi.size = f;
                    } else fi.size = 0;
                    if (fi.name.find(filter) != std::wstring::npos) outItems.push_back(fi);
                    if (entry.is_directory()) {
                        RecursiveSearch(entry.path().wstring(), filter, outItems);
                    }
                } catch (...) { /* ignore individual errors */ }
            }
        } catch (...) { /* ignore access errors */ }
    }

    // Batch-Rename: Pattern example: "File_{n}" where {n} replaced with index
    fire_and_forget BatchRename(IInspectable const&, RoutedEventArgs const&) {
        auto sel = m_fileGrid.SelectedItem();
        if (!sel) co_return;
        TextBox input;
        input.PlaceholderText(L"Pattern, use {n} for index, e.g. Photo_{n}");
        ContentDialog dlg;
        dlg.Title(winrt::box_value(winrt::hstring(L"Batch-Rename")));
        dlg.Content(input);
        dlg.PrimaryButtonText(L"OK");
        dlg.SecondaryButtonText(L"Cancel");
        dlg.XamlRoot(m_fileGrid.XamlRoot());
        auto res = co_await dlg.ShowAsync();
        if (res != ContentDialogResult::Primary) co_return;
        hstring pattern = input.Text();
        std::vector<std::wstring> selectedNames;
        for (auto const& s : m_fileGrid.SelectedItems()) {
            selectedNames.push_back(std::wstring(unbox_value<hstring>(s).c_str()));
        }
        int idx = 1;
        for (auto const& nm : selectedNames) {
            auto it = std::find_if(currentItems.begin(), currentItems.end(),
                [&nm](FileItem const& fi) { return fi.name == nm; });
            if (it == currentItems.end()) continue;
            std::wstring pat = pattern.c_str();
            // replace {n}
            size_t pos = pat.find(L"{n}");
            std::wstringstream ss;
            ss << idx++;
            if (pos != std::wstring::npos) pat.replace(pos, 3, ss.str());
            std::filesystem::path oldp(it->fullPath);
            std::filesystem::path newp = oldp.parent_path() / pat;
            try {
                std::filesystem::rename(oldp, newp);
            } catch (...) {}
        }
        PopulateFiles(m_addressBar.Text());
    }

    // Größenanalyse: berechnet rekursiv Größen für Items in current directory
    static ULONGLONG ComputeSizeRecursive(std::wstring const& path) {
        ULONGLONG total = 0;
        try {
            for (auto const& e : std::filesystem::recursive_directory_iterator(path)) {
                try {
                    if (e.is_regular_file()) total += (ULONGLONG)std::filesystem::file_size(e.path());
                } catch (...) {}
            }
        } catch (...) {}
        return total;
    }
    fire_and_forget AnalyzeSizes(IInspectable const&, RoutedEventArgs const&) {
        std::vector<std::pair<std::wstring, ULONGLONG>> sizes;
        for (auto& fi : currentItems) {
            if (fi.isFolder) {
                ULONGLONG s = ComputeSizeRecursive(fi.fullPath);
                sizes.push_back({ fi.name, s });
            } else {
                sizes.push_back({ fi.name, fi.size });
            }
        }
        std::sort(sizes.begin(), sizes.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
        std::wstringstream ss;
        ss << L"Top items by size:\n\n";
        int count = 0;
        for (auto const& p : sizes) {
            ss << p.first << L" - " << p.second << L" bytes\n";
            if (++count >= 15) break;
        }
        ContentDialog dlg;
        dlg.Title(winrt::box_value(winrt::hstring(L"Size Analysis")));
        TextBlock tb;
        tb.Text(winrt::hstring(ss.str()));
        tb.TextWrapping(TextWrapping::Wrap);
        dlg.Content(tb);
        dlg.PrimaryButtonText(L"OK");
        dlg.XamlRoot(m_fileGrid.XamlRoot());
        co_await dlg.ShowAsync();
    }

    // Recent handling (simple)
    void AddToRecent(std::wstring const& path) {
        if (path.empty()) return;
        recentFiles.erase(std::remove(recentFiles.begin(), recentFiles.end(), path), recentFiles.end());
        recentFiles.insert(recentFiles.begin(), path);
        if (recentFiles.size() > 50) recentFiles.resize(50);
        SaveRecent();
    }
    void LoadRecent() {
        recentFiles.clear();
        std::ifstream f(GetFavoritesPath() + L".recent", std::ios::binary);
        if (!f.is_open()) return;
        try {
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            Json::CharReaderBuilder rbuilder;
            std::unique_ptr<Json::CharReader> reader(rbuilder.newCharReader());
            Json::Value root; std::string errs;
            if (reader->parse(content.c_str(), content.c_str()+content.size(), &root, &errs)) {
                if (root.isMember("recent") && root["recent"].isArray()) {
                    for (auto& v : root["recent"]) recentFiles.push_back(WStringFromUtf8(v.asString()));
                }
            }
        } catch (...) {}
    }
    void SaveRecent() {
        try {
            Json::Value root;
            for (auto const& r : recentFiles) root["recent"].append(Utf8FromWString(r));
            Json::StreamWriterBuilder wbuilder; wbuilder["indentation"] = "  ";
            auto out = Json::writeString(wbuilder, root);
            std::ofstream f(GetFavoritesPath() + L".recent", std::ios::binary);
            if (f.is_open()) f << out;
        } catch (...) {}
    }

    // Copy/Delete/Rename / Sortierung / Favoriten speichern + laden hier implementieren
    void SortByName(IInspectable const&, RoutedEventArgs const&) {
        if (sortColumn == 0) sortAscending = !sortAscending;
        else sortAscending = true;
        sortColumn = 0;
        SortAndRefresh();
    }
    void SortBySize(IInspectable const&, RoutedEventArgs const&) {
        if (sortColumn == 2) sortAscending = !sortAscending;
        else sortAscending = true;
        sortColumn = 2;
        SortAndRefresh();
    }
    void SortByDate(IInspectable const&, RoutedEventArgs const&) {
        if (sortColumn == 3) sortAscending = !sortAscending;
        else sortAscending = true;
        sortColumn = 3;
        SortAndRefresh();
    }

    // Sortierung nach Typ (Extension)
    void SortByType(IInspectable const&, RoutedEventArgs const&) {
        if (sortColumn == 1) sortAscending = !sortAscending;
        else sortAscending = true;
        sortColumn = 1;
        SortAndRefresh();
    }

    void SortAndRefresh() {
        std::sort(filteredItems.begin(), filteredItems.end(), [this](const FileItem& a, const FileItem& b) {
            int comparison = 0;
            switch (sortColumn) {
            case 0: // Name
                comparison = _wcsicmp(a.name.c_str(), b.name.c_str());
                break;
            case 1: { // Type (extension)
                auto extA = std::filesystem::path(a.name).extension().wstring();
                auto extB = std::filesystem::path(b.name).extension().wstring();
                // case-insensitive
                comparison = _wcsicmp(extA.c_str(), extB.c_str());
                if (comparison == 0) comparison = _wcsicmp(a.name.c_str(), b.name.c_str());
                break;
            }
            case 2: // Size
                if (a.size < b.size) comparison = -1;
                else if (a.size > b.size) comparison = 1;
                else comparison = 0;
                break;
            case 3: // Date
                comparison = CompareFileTime(&a.modifiedTime, &b.modifiedTime);
                break;
            }
            return sortAscending ? (comparison < 0) : (comparison > 0);
        });

        m_fileGrid.Items().Clear();
        for (auto const& fi : filteredItems) {
            m_fileGrid.Items().Append(box_value(winrt::hstring(fi.name)));
        }
    }

    void CopyItem(IInspectable const&, RoutedEventArgs const&) {
        auto selectedItem = m_fileGrid.SelectedItem();
        if (!selectedItem) return;

        hstring name = unbox_value<hstring>(selectedItem);
        auto it = std::find_if(currentItems.begin(), currentItems.end(),
            [&name](FileItem const& fi) { return fi.name == name.c_str(); });

        if (it != currentItems.end()) {
            std::wstring path = it->fullPath;
            size_t len = sizeof(DROPFILES) + (path.length() + 2) * sizeof(wchar_t); // +2 for double null terminator
            HGLOBAL hg = GlobalAlloc(GHND, len);
            if (!hg) return;

            DROPFILES* df = (DROPFILES*)GlobalLock(hg);
            if (!df) {
                GlobalFree(hg);
                return;
            }
            df->pFiles = sizeof(DROPFILES);
            df->fWide = TRUE;
            wchar_t* dst = (wchar_t*)(df + 1);
            wcscpy_s(dst, path.length() + 1, path.c_str());
            dst[path.length() + 1] = L'\0'; // Double null terminate
            GlobalUnlock(hg);

            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                SetClipboardData(CF_HDROP, hg);
                CloseClipboard();
            }
            else {
                GlobalFree(hg);
            }
            m_fileGrid.SelectedItem(nullptr); // Auswahl zurücksetzen
        }
    }

    void PasteItem(IInspectable const&, RoutedEventArgs const&) {
        if (OpenClipboard(nullptr)) {
            HANDLE hData = GetClipboardData(CF_HDROP);
            if (hData) {
                HDROP hDrop = (HDROP)GlobalLock(hData);
                if (hDrop) {
                    UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
                    if (fileCount > 0) {
                        UINT totalLen = 0;
                        for (UINT i = 0; i < fileCount; ++i) {
                            totalLen += DragQueryFileW(hDrop, i, nullptr, 0);
                        }
                        totalLen += fileCount + 1;

                        std::vector<wchar_t> fromPath(totalLen);
                        wchar_t* current = fromPath.data();
                        for (UINT i = 0; i < fileCount; ++i) {
                            UINT len = DragQueryFileW(hDrop, i, current, totalLen - (current - fromPath.data()));
                            current += len + 1;
                        }
                        *current = L'\0';

                        std::wstring toPathStr = m_addressBar.Text().c_str();
                        std::vector<wchar_t> toPath(toPathStr.length() + 2, 0);
                        wcscpy_s(toPath.data(), toPathStr.length() + 1, toPathStr.c_str());

                        SHFILEOPSTRUCTW fileOp = { 0 };
                        fileOp.wFunc = FO_COPY;
                        fileOp.pFrom = fromPath.data();
                        fileOp.pTo = toPath.data();
                        fileOp.fFlags = FOF_ALLOWUNDO;

                        if (SHFileOperationW(&fileOp) == 0 && !fileOp.fAnyOperationsAborted) {
                            PopulateFiles(m_addressBar.Text());
                        }
                    }
                    GlobalUnlock(hData);
                }
            }
            CloseClipboard();
        }
        m_fileGrid.SelectedItem(nullptr); // Auswahl zurücksetzen
    }

    void DeleteItem(IInspectable const&, RoutedEventArgs const&) {
        auto selectedItem = m_fileGrid.SelectedItem();
        if (!selectedItem) return;

        hstring name = unbox_value<hstring>(selectedItem);
        auto it = std::find_if(currentItems.begin(), currentItems.end(),
            [&name](FileItem const& fi) { return fi.name == name.c_str(); });

        if (it != currentItems.end()) {
            std::wstring path = it->fullPath;
            std::vector<wchar_t> doubleNullPath(path.length() + 2, 0);
            wcscpy_s(doubleNullPath.data(), path.length() + 1, path.c_str());

            SHFILEOPSTRUCTW fileOp = { 0 };
            fileOp.wFunc = FO_DELETE;
            fileOp.pFrom = doubleNullPath.data();
            fileOp.fFlags = FOF_ALLOWUNDO;

            int result = SHFileOperationW(&fileOp);
            if (result == 0 && !fileOp.fAnyOperationsAborted) {
                PopulateFiles(m_addressBar.Text());
            }
            m_fileGrid.SelectedItem(nullptr); // Auswahl zurücksetzen
        }
    }

    fire_and_forget RenameItem(IInspectable const&, RoutedEventArgs const&) {
        auto selectedItem = m_fileGrid.SelectedItem();
        if (!selectedItem) co_return;

        hstring name = unbox_value<hstring>(selectedItem);
        auto it = std::find_if(currentItems.begin(), currentItems.end(),
            [&name](FileItem const& fi) { return fi.name == name.c_str(); });

        if (it != currentItems.end()) {
            TextBox input;
            input.Text(name);

            ContentDialog dialog;
            dialog.Title(box_value(L"Umbenennen"));
            dialog.Content(input);
            dialog.PrimaryButtonText(L"OK");
            dialog.SecondaryButtonText(L"Abbrechen");
            dialog.XamlRoot(m_fileGrid.XamlRoot());

            auto result = co_await dialog.ShowAsync();
            std::wstring newName = input.Text().c_str();
            if (result == ContentDialogResult::Primary && !newName.empty() && newName != it->name) {
                std::wstring oldPath = it->fullPath;
                std::filesystem::path p(oldPath);
                std::wstring newPath = p.parent_path().wstring() + L"\\" + newName;

                if (MoveFile(oldPath.c_str(), newPath.c_str())) {
                    PopulateFiles(m_addressBar.Text());
                }
            }
            m_fileGrid.SelectedItem(nullptr); // Auswahl zurücksetzen
        }
    }

    std::wstring GetFavoritesPath() {
        PWSTR path = NULL;
        HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &path);
        if (SUCCEEDED(hr)) {
            std::wstring appDataPath(path);
            CoTaskMemFree(path);
            std::filesystem::path p = appDataPath;
            p /= L"UltimateExplorer";
            std::filesystem::create_directories(p);
            p /= L"favorites.json";
            return p.wstring();
        }
        return L"favorites.json"; // Fallback
    }

    // Hilfsfunktionen: Suche selektiertes Element
    std::vector<FileItem>::iterator FindItemByName(hstring const& name) {
        std::wstring n = name.c_str();
        return std::find_if(currentItems.begin(), currentItems.end(),
            [&n](FileItem const& fi) { return fi.name == n; });
    }

    // Window KeyDown: F5 refresh
    void WindowKeyDown(IInspectable const&, KeyRoutedEventArgs const& args) {
        if (args.Key() == Windows::System::VirtualKey::F5) {
            PopulateFiles(m_addressBar.Text());
        }
    }

    void LoadFavorites() {
        favorites.clear();
        std::ifstream f(GetFavoritesPath(), std::ios::binary);
        if (f.is_open()) {
            try {
                std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                Json::CharReaderBuilder rbuilder;
                std::unique_ptr<Json::CharReader> reader(rbuilder.newCharReader());
                Json::Value root;
                std::string errs;
                if (reader->parse(content.c_str(), content.c_str() + content.size(), &root, &errs)) {
                    if (root.isMember("favorites") && root["favorites"].isArray()) {
                        for (auto& val : root["favorites"]) {
                            std::string s = val.asString();
                            favorites.push_back(WStringFromUtf8(s));
                        }
                    }
                }
            } catch (const std::exception& e) {
                OutputDebugStringA(("Error loading favorites: " + std::string(e.what()) + "\n").c_str());
            }
        } else {
            SaveFavorites();
        }
        // dedupe just in case
        std::sort(favorites.begin(), favorites.end());
        favorites.erase(std::unique(favorites.begin(), favorites.end()), favorites.end());
    }

    void SaveFavorites() {
        try {
            Json::Value root;
            for (auto& fav : favorites) {
                root["favorites"].append(Utf8FromWString(fav));
            }
            Json::StreamWriterBuilder wbuilder;
            wbuilder["indentation"] = "  ";
            const std::string output = Json::writeString(wbuilder, root);
            std::ofstream f(GetFavoritesPath(), std::ios::binary);
            if (f.is_open()) {
                f << output;
            } else {
                OutputDebugStringA("Error: Unable to open favorites.json for writing.\n");
            }
        } catch (const std::exception& e) {
            OutputDebugStringA(("Error saving favorites: " + std::string(e.what()) + "\n").c_str());
        }
    }

    // Neue Methoden: Favoriten hinzufügen / entfernen
    void AddToFavorites(IInspectable const&, RoutedEventArgs const&) {
        auto selectedItem = m_fileGrid.SelectedItem();
        if (!selectedItem) return;

        hstring name = unbox_value<hstring>(selectedItem);
        auto it = std::find_if(currentItems.begin(), currentItems.end(),
            [&name](FileItem const& fi) { return fi.name == name.c_str(); });

        if (it != currentItems.end()) {
            std::wstring pathToAdd;
            if (it->isFolder) {
                pathToAdd = it->fullPath;
            } else {
                // für Dateien: Ordner als Favorit speichern
                std::filesystem::path p(it->fullPath);
                pathToAdd = p.parent_path().wstring();
            }

            if (!pathToAdd.empty() &&
                std::find(favorites.begin(), favorites.end(), pathToAdd) == favorites.end()) {
                favorites.push_back(pathToAdd);
                SaveFavorites();
                PopulateSidebar();
            }
        }
        m_fileGrid.SelectedItem(nullptr);
    }

    void RemoveFromFavorites(IInspectable const&, RoutedEventArgs const&) {
        auto selectedItem = m_fileGrid.SelectedItem();
        if (!selectedItem) return;

        hstring name = unbox_value<hstring>(selectedItem);
        auto it = std::find_if(currentItems.begin(), currentItems.end(),
            [&name](FileItem const& fi) { return fi.name == name.c_str(); });

        if (it != currentItems.end()) {
            std::wstring pathToRemove;
            if (it->isFolder) {
                pathToRemove = it->fullPath;
            } else {
                std::filesystem::path p(it->fullPath);
                pathToRemove = p.parent_path().wstring();
            }

            if (!pathToRemove.empty()) {
                favorites.erase(std::remove(favorites.begin(), favorites.end(), pathToRemove), favorites.end());
                SaveFavorites();
                PopulateSidebar();
            }
        }
        m_fileGrid.SelectedItem(nullptr);
    }

    // Hilfsfunktionen: UTF-8 Konvertierung
    std::wstring WStringFromUtf8(std::string const& s) {
        if (s.empty()) return {};
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
        std::wstring w(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], size_needed);
        return w;
    }
    std::string Utf8FromWString(std::wstring const& w) {
        if (w.empty()) return {};
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
        std::string s(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], size_needed, NULL, NULL);
        return s;
    }
};

int main()
{
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    Application::Start([](auto&&) { make<ExplorerFinal>(); });
}
