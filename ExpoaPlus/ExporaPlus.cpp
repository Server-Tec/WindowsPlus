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

    void CopyItem(IInspectable const&, RoutedEventArgs const&);
    void DeleteItem(IInspectable const&, RoutedEventArgs const&);
    void RenameItem(IInspectable const&, RoutedEventArgs const&);
    void PasteItem(IInspectable const&, RoutedEventArgs const&);
    void SortByName(IInspectable const&, RoutedEventArgs const&);
    void SortBySize(IInspectable const&, RoutedEventArgs const&);
    void SortByDate(IInspectable const&, RoutedEventArgs const&);
    void SortAndRefresh();

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

        // Sortierleiste
        StackPanel sortPanel;
        sortPanel.Orientation(Orientation::Horizontal);
        sortPanel.Spacing(5);
        Button sortName; sortName.Content(box_value(L"Name")); sortName.Click({ this, &ExplorerFinal::SortByName });
        Button sortSize; sortSize.Content(box_value(L"Größe")); sortSize.Click({ this, &ExplorerFinal::SortBySize });
        Button sortDate; sortDate.Content(box_value(L"Datum")); sortDate.Click({ this, &ExplorerFinal::SortByDate });
        sortPanel.Children().Append(sortName);
        sortPanel.Children().Append(sortSize);
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

        // Kontextmenü
        MenuFlyout flyout;
        MenuFlyoutItem copyItem; copyItem.Text(L"Kopieren"); copyItem.Click({ this, &ExplorerFinal::CopyItem });
        MenuFlyoutItem pasteItem; pasteItem.Text(L"Einfügen"); pasteItem.Click({ this, &ExplorerFinal::PasteItem });
        MenuFlyoutItem deleteItem; deleteItem.Text(L"Löschen"); deleteItem.Click({ this, &ExplorerFinal::DeleteItem });
        MenuFlyoutItem renameItem; renameItem.Text(L"Umbenennen"); renameItem.Click({ this, &ExplorerFinal::RenameItem });
        flyout.Items().Append(copyItem); flyout.Items().Append(pasteItem); flyout.Items().Append(deleteItem); flyout.Items().Append(renameItem);
        m_fileGrid.ContextFlyout(flyout);

        m_navView.Content(mainGrid);
        LoadFavorites();
        PopulateSidebar();
        PopulateFiles(m_addressBar.Text());

        m_window.Content(m_navView);
        m_window.Activate();
    }

    // Sidebar: Laufwerke + Favoriten
    void PopulateSidebar() {
        DWORD drives = GetLogicalDrives();
        for (int i = 0; i < 26; i++) {
            if (drives & (1 << i)) {
                std::wstring drive = { wchar_t(L'A' + i), L':' };
                NavigationViewItem item;
                item.Content(winrt::box_value(drive));
                item.Tag(winrt::box_value(drive));
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
            favItem.Content(winrt::box_value(fav));
            favItem.Tag(winrt::box_value(fav));
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
        WIN32_FIND_DATA fd;
        HANDLE hFind = FindFirstFile((std::wstring(path) + L"\\*").c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return;

        do {
            if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
                FileItem fi;
                fi.name = fd.cFileName;
                fi.fullPath = std::wstring(path) + L"\\" + fd.cFileName;
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
                m_fileGrid.Items().Append(box_value(fi.name));
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
        if (args.DataView().Contains(StandardDataFormats::StorageItems())) {
            auto items = co_await args.DataView().GetStorageItemsAsync();
            auto destPath = m_addressBar.Text();

            for (auto const& item : items) {
                if (item.IsOfType(StorageItemTypes::File)) {
                    auto file = item.as<StorageFile>();
                    auto destFolder = co_await StorageFolder::GetFolderFromPathAsync(destPath);
                    co_await file.MoveAsync(destFolder);
                }
                else if (item.IsOfType(StorageItemTypes::Folder)) {
                    auto folder = item.as<StorageFolder>();
                    auto destFolder = co_await StorageFolder::GetFolderFromPathAsync(destPath);
                    co_await folder.MoveAsync(destFolder);
                }
            }
            PopulateFiles(destPath);
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

    void SortAndRefresh() {
        std::sort(filteredItems.begin(), filteredItems.end(), [this](const FileItem& a, const FileItem& b) {
            int comparison = 0;
            switch (sortColumn) {
            case 0: // Name
                comparison = _wcsicmp(a.name.c_str(), b.name.c_str());
                break;
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
            m_fileGrid.Items().Append(box_value(fi.name));
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
            size_t len = sizeof(DROPFILES) + (path.length() + 2) * sizeof(wchar_t);
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
            GlobalUnlock(hg);

            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                SetClipboardData(CF_HDROP, hg);
                CloseClipboard();
            }
            else {
                GlobalFree(hg);
            }
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
                        std::wstring fromPath;
                        for (UINT i = 0; i < fileCount; ++i) {
                            UINT len = DragQueryFileW(hDrop, i, nullptr, 0) + 1;
                            std::vector<wchar_t> buffer(len);
                            DragQueryFileW(hDrop, i, buffer.data(), len);
                            fromPath += buffer.data();
                            fromPath += L'\0';
                        }
                        fromPath += L'\0';

                        std::wstring toPath = m_addressBar.Text().c_str();
                        toPath += L'\0';

                        SHFILEOPSTRUCTW fileOp = { 0 };
                        fileOp.wFunc = FO_COPY;
                        fileOp.pFrom = fromPath.c_str();
                        fileOp.pTo = toPath.c_str();
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
    }

    void DeleteItem(IInspectable const&, RoutedEventArgs const&) {
        auto selectedItem = m_fileGrid.SelectedItem();
        if (!selectedItem) return;

        hstring name = unbox_value<hstring>(selectedItem);
        auto it = std::find_if(currentItems.begin(), currentItems.end(),
            [&name](FileItem const& fi) { return fi.name == name.c_str(); });

        if (it != currentItems.end()) {
            std::wstring path = it->fullPath;
            path.push_back(L'\0'); // Double-null terminate for SHFileOperation

            SHFILEOPSTRUCTW fileOp = { 0 };
            fileOp.wFunc = FO_DELETE;
            fileOp.pFrom = path.c_str();
            fileOp.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION; // Use FOF_ALLOWUNDO for recycle bin

            int result = SHFileOperationW(&fileOp);
            if (result == 0 && !fileOp.fAnyOperationsAborted) {
                PopulateFiles(m_addressBar.Text());
            }
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
            if (result == ContentDialogResult::Primary) {
                std::wstring oldPath = it->fullPath;
                std::wstring newName = input.Text().c_str();
                std::filesystem::path p(oldPath);
                std::wstring newPath = p.parent_path().wstring() + L"\\" + newName;

                if (MoveFile(oldPath.c_str(), newPath.c_str())) {
                    PopulateFiles(m_addressBar.Text());
                }
            }
        }
    }

    void LoadFavorites() {
        std::ifstream f("favorites.json");
        if (f.is_open()) {
            Json::Value root;
            f >> root;
            for (auto& val : root["favorites"]) {
                std::string s = val.asString();
                favorites.push_back(std::wstring(s.begin(), s.end()));
            }
        }
    }

    void SaveFavorites() {
        Json::Value root;
        for (auto& fav : favorites) {
            std::string s(fav.begin(), fav.end());
            root["favorites"].append(s);
        }
        std::ofstream f("favorites.json");
        f << root;
    }
};

int main()
{
    winrt::init_apartment();
    Application::Start([](auto&&) { make<ExplorerFinal>(); });
}
