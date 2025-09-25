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

    // Neue Member für Kontextmenü (damit lambdas / Opening handler sicher darauf zugreifen)
    MenuFlyoutItem m_addFavItem{ nullptr };
    MenuFlyoutItem m_remFavItem{ nullptr };

    void CopyItem(IInspectable const&, RoutedEventArgs const&);
    void DeleteItem(IInspectable const&, RoutedEventArgs const&);
    void RenameItem(IInspectable const&, RoutedEventArgs const&);
    void PasteItem(IInspectable const&, RoutedEventArgs const&);
    void SortByName(IInspectable const&, RoutedEventArgs const&);
    void SortBySize(IInspectable const&, RoutedEventArgs const&);
    void SortByDate(IInspectable const&, RoutedEventArgs const&);
    void SortByType(IInspectable const&, RoutedEventArgs const&);
    void SortAndRefresh();
    void AddToFavorites(IInspectable const&, RoutedEventArgs const&);
    void RemoveFromFavorites(IInspectable const&, RoutedEventArgs const&);

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
        Grid::SetRow(sortPanel, 2);
        mainGrid.Children().Append(sortPanel);

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
        PopulateSidebar();
        PopulateFiles(m_addressBar.Text());

        m_window.Content(m_navView);
        m_window.Activate();
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
            }
            else {
                ShellExecute(nullptr, L"open", it->fullPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
    }

    // Drag & Drop: mehrere Dateien
    void DragStart(IInspectable const&, DragItemsStartingEventArgs const& args) {
        std::vector<IStorageItem> itemsToDrag;
        for (auto const& item : args.Items()) {
            hstring name = unbox_value<hstring>(item);
            auto it = std::find_if(currentItems.begin(), currentItems.end(),
                [&name](FileItem const& fi) { return fi.name == name.c_str(); });

            if (it != currentItems.end()) {
                if (it->isFolder) {
                    itemsToDrag.push_back(StorageFolder::GetFolderFromPathAsync(it->fullPath).get());
                }
                else {
                    itemsToDrag.push_back(StorageFile::GetFileFromPathAsync(it->fullPath).get());
                }
            }
        }

        if (!itemsToDrag.empty()) {
            auto dataPackage = DataPackage();
            dataPackage.SetStorageItems(itemsToDrag);
            dataPackage.RequestedOperation(DataPackageOperation::Move);
            args.Data(dataPackage);
        }
    }

    fire_and_forget DropFiles(IInspectable const&, DragEventArgs const& args) {
        try {
            if (args.DataView().Contains(StandardDataFormats::StorageItems())) {
                auto items = co_await args.DataView().GetStorageItemsAsync();
                auto destPath = m_addressBar.Text();

                for (auto const& item : items) {
                    try {
                        if (item.IsOfType(StorageItemTypes::File)) {
                            auto file = item.as<StorageFile>();
                            auto destFolder = co_await StorageFolder::GetFolderFromPathAsync(destPath);
                            co_await file.MoveAsync(destFolder);
                        } else if (item.IsOfType(StorageItemTypes::Folder)) {
                            auto folder = item.as<StorageFolder>();
                            auto destFolder = co_await StorageFolder::GetFolderFromPathAsync(destPath);
                            co_await folder.MoveAsync(destFolder);
                        }
                    } catch (const winrt::hresult_error& e) {
                        // Log or handle the error for individual items
                        std::wstring msg = L"Error moving item: ";
                        msg += e.message().c_str();
                        msg += L"\n";
                        OutputDebugStringW(msg.c_str());
                    }
                }
                PopulateFiles(destPath);
            }
        } catch (const winrt::hresult_error& e) {
            // Log or handle the error for the entire operation
            std::wstring msg = L"Error during drag-and-drop: ";
            msg += e.message().c_str();
            msg += L"\n";
            OutputDebugStringW(msg.c_str());
        }
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
