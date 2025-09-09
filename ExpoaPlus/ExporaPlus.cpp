#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <shlobj.h>
#include <shobjidl_core.h>
#include <shellapi.h>
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

struct FileItem {
    d
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

    void OnLaunched(LaunchActivatedEventArgs const&)
    {
        m_window = Window();
        m_window.Title(L"Ultimate Explorer Final");

        m_navView = NavigationView();
        Grid mainGrid;
        mainGrid.RowDefinitions().Append(RowDefinition{ GridLengthHelper::Auto() }); // Adressleiste
        mainGrid.RowDefinitions().Append(RowDefinition{ GridLengthHelper::Auto() }); // Suchleiste
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

        // Datei-GridView
        m_fileGrid = GridView();
        Grid::SetRow(m_fileGrid, 2);
        m_fileGrid.IsItemClickEnabled(true);
        m_fileGrid.ItemClick({ this, &ExplorerFinal::FileItemClick });
        m_fileGrid.AllowDrop(true);
        m_fileGrid.DragItemsStarting({ this, &ExplorerFinal::DragStart });
        m_fileGrid.Drop({ this, &ExplorerFinal::DropFiles });
        mainGrid.Children().Append(m_fileGrid);

        // Kontextmenü
        MenuFlyout flyout;
        MenuFlyoutItem copyItem; copyItem.Text(L"Kopieren"); copyItem.Click({ this, &ExplorerFinal::CopyItem });
        MenuFlyoutItem deleteItem; deleteItem.Text(L"Löschen"); deleteItem.Click({ this, &ExplorerFinal::DeleteItem });
        MenuFlyoutItem renameItem; renameItem.Text(L"Umbenennen"); renameItem.Click({ this, &ExplorerFinal::RenameItem });
        flyout.Items().Append(copyItem); flyout.Items().Append(deleteItem); flyout.Items().Append(renameItem);
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
    }

    // Suchleiste filter
    void SearchTextChanged(IInspectable const&, TextChangedEventArgs const&) {
        std::wstring filter = m_searchBox.Text().c_str();
        filteredItems.clear();
        m_fileGrid.Items().Clear();
        for (auto& fi : currentItems) {
            if (fi.name.find(filter) != std::wstring::npos) {
                filteredItems.push_back(fi);
                m_fileGrid.Items().Append(box_value(fi.name));
            }
        }
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
        com_array<hstring> files;
        for (auto item : args.Items()) {
            hstring name = unbox_value<hstring>(item);
            auto it = std::find_if(currentItems.begin(), currentItems.end(), [&name](FileItem& fi) { return fi.name == name.c_str(); });
            if (it != currentItems.end()) files.push_back(it->fullPath);
        }
        // Set DataPackage
    }

    void DropFiles(IInspectable const&, DragEventArgs const& args) {
        // Zielpfad = m_addressBar.Text()
        // Ctrl prüfen für Copy, sonst Move
    }

    // Copy/Delete/Rename / Sortierung / Favoriten speichern + laden hier implementieren

    void LoadFavorites() {
        std::ifstream f("favorites.json");
        if (f.is_open()) {
            Json::Value root;
            f >> root;
            for (auto& val : root["favorites"]) favorites.push_back(val.asString());
        }
    }

    void SaveFavorites() {
        Json::Value root;
        for (auto& fav : favorites) root["favorites"].append(fav);
        std::ofstream f("favorites.json");
        f << root;
    }
};

int main()
{
    winrt::init_apartment();
    Application::Start([](auto&&) { make<ExplorerFinal>(); });
}
