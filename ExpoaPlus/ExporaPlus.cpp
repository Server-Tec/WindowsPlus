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
#include <wincrypt.h>
#include <unordered_map>
#include <set>
#include <locale>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <map>

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

    // Neue UI / Funktionalitäts-Erweiterungen (zusätzlich)
    Button m_duplicateButton{ nullptr };
    Button m_selectAllButton{ nullptr };
    Button m_invertSelectionButton{ nullptr };
    ToggleButton m_toggleDetailsButton{ nullptr };
    bool m_showDetails = false;

    // AI features UI
    Button m_aiSuggestButton{ nullptr };
    Button m_findDuplicatesButton{ nullptr };
    Button m_summarizeButton{ nullptr };
    ToggleButton m_useFuzzyToggle{ nullptr };

    // Indexer / AI infrastructure
    std::thread m_indexThread;
    std::atomic<bool> m_indexRunning{ false };
    std::mutex m_indexMutex;
    std::condition_variable m_indexCv;
    std::map<std::wstring, Json::Value> m_index; // path -> metadata
    ProgressBar m_progressBar{ nullptr };
    Button m_indexButton{ nullptr };
    Button m_semanticSearchButton{ nullptr };
    Button m_autoCategorizeButton{ nullptr };
    Button m_autoRenameAllButton{ nullptr };
    Button m_undoButton{ nullptr };
    std::vector<std::tuple<std::wstring,std::wstring,std::wstring>> m_undoStack; // (type, src, dst): type="rename" etc.

    std::wstring m_settingsPath;
    Json::Value m_settings;

    // Zusätzliche Member: Thumbnail-Cache, Settings, Buttons
    std::unordered_map<std::wstring, winrt::Windows::UI::Xaml::Media::Imaging::BitmapImage> m_thumbnailCache;
    unsigned int m_thumbnailSize = 128;
    int m_fuzzyThreshold = 3;
    Button m_settingsButton{ nullptr };

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
    void DuplicateSelected(IInspectable const&, RoutedEventArgs const&);
    void SelectAllItems(IInspectable const&, RoutedEventArgs const&);
    void InvertSelection(IInspectable const&, RoutedEventArgs const&);
    void ToggleDetails(IInspectable const&, RoutedEventArgs const&);
    void SuggestRename(IInspectable const&, RoutedEventArgs const&);
    void FindDuplicates(IInspectable const&, RoutedEventArgs const&);
    fire_and_forget SummarizeSelected(IInspectable const&, RoutedEventArgs const&);
    std::vector<std::wstring> ExtractTagsFromTextFile(std::wstring const& path, size_t topN = 5);
    int LevenshteinDistance(std::wstring const& a, std::wstring const& b);
    bool FuzzyMatch(std::wstring const& hay, std::wstring const& needle, int maxDistance = 3);
    std::string ComputeFileSHA1(std::wstring const& path);

    // OnLaunched: Toolbar - AI buttons + Fuzzy toggle
    void OnLaunched(LaunchActivatedEventArgs const&)
    {
        m_window = Window();
        m_window.Title(L"Ultimate Explorer Final");

        // --- Theme: Dark gray palette ---
        auto darkBackgroundBrush = SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 30, 30, 30));   // main background
        auto panelBrush = SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 40, 40, 40));             // panels
        auto cardBrush = SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 48, 48, 48));              // item cards
        auto accentBrush = SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 70, 70, 70));            // buttons / accents
        auto textBrush = SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 230, 230, 230));           // primary text
        auto subTextBrush = SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 190, 190, 190));        // secondary text
        auto previewBrush = SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 36, 36, 36));           // preview background

        m_navView = NavigationView();
        // Apply dark background to nav view
        m_navView.Background(darkBackgroundBrush);

        Grid mainGrid;
        mainGrid.Background(panelBrush); // darker panel background
        mainGrid.RowDefinitions().Append(RowDefinition{ GridLengthHelper::Auto() }); // Adressleiste
        mainGrid.RowDefinitions().Append(RowDefinition{ GridLengthHelper::Auto() }); // Suchleiste
        mainGrid.RowDefinitions().Append(RowDefinition{ GridLengthHelper::Auto() }); // Sortierleiste
        mainGrid.RowDefinitions().Append(RowDefinition{ GridLengthHelper::Star() }); // FileGrid

        // Adressleiste
        m_addressBar = TextBox();
        m_addressBar.Text(L"C:\\");
        m_addressBar.KeyUp({ this, &ExplorerFinal::AddressBarKeyUp });
        // Breadcrumb + Address in a vertical stack
        m_breadcrumb = StackPanel();
        m_breadcrumb.Orientation(Orientation::Horizontal);
        StackPanel addressStack;
        addressStack.Orientation(Orientation::Vertical);
        addressStack.Children().Append(m_addressBar);
        addressStack.Children().Append(m_breadcrumb);
        mainGrid.Children().Append(addressStack);

        // Suchleiste
        m_searchBox = TextBox();
        m_searchBox.PlaceholderText(L"Suchen...");
        m_searchBox.TextChanged({ this, &ExplorerFinal::SearchTextChanged });
        Grid::SetRow(m_searchBox, 1);
        // NOTE: actual search UI stack appended later (searchStack). Do not append m_searchBox directly here.

        // Sortierleiste: zusätzlicher "Typ"-Button
        StackPanel sortPanel;
        sortPanel.Orientation(Orientation::Horizontal);
        sortPanel.Spacing(5);
        sortPanel.Background(winrt::box_value(nullptr)); // keep transparent, uses mainGrid bg

        Button sortName; sortName.Content(box_value(winrt::hstring(L"Name"))); sortName.Click({ this, &ExplorerFinal::SortByName });
        Button sortSize; sortSize.Content(box_value(winrt::hstring(L"Größe"))); sortSize.Click({ this, &ExplorerFinal::SortBySize });
        Button sortDate; sortDate.Content(box_value(winrt::hstring(L"Datum"))); sortDate.Click({ this, &ExplorerFinal::SortByDate });
        Button sortType; sortType.Content(box_value(winrt::hstring(L"Typ"))); sortType.Click({ this, &ExplorerFinal::SortByType });

        // Style buttons: dark accent + light text
        auto styleBtn = [&](Button& b) {
            b.Background(accentBrush);
            b.Foreground(textBrush);
            b.BorderBrush(nullptr);
            b.CornerRadius(Microsoft::UI::Xaml::CornerRadius{ 6 });
            b.Padding(ThicknessHelper::FromLengths(8, 6, 8, 6));
        };
        styleBtn(sortName); styleBtn(sortSize); styleBtn(sortDate); styleBtn(sortType);

        sortPanel.Children().Append(sortName);
        sortPanel.Children().Append(sortSize);
        sortPanel.Children().Append(sortType);
        sortPanel.Children().Append(sortDate);

        // Füge rekursiven Suche-Toggle + Analyse + Batch-Rename Buttons in SortPanel
        m_recursiveToggle = ToggleSwitch();
        m_recursiveToggle.IsOn(false);
        m_recursiveToggle.OnContent(winrt::box_value(winrt::hstring(L"Rekursiv")));
        m_recursiveToggle.OffContent(winrt::box_value(winrt::hstring(L"Rekursiv")));
        // Toggle background/foreground
        m_recursiveToggle.Background(panelBrush);
        m_recursiveToggle.Foreground(textBrush);
        sortPanel.Children().Append(m_recursiveToggle);

        m_analyzeButton = Button();
        m_analyzeButton.Content(winrt::box_value(winrt::hstring(L"Analyze Sizes")));
        m_analyzeButton.Click({ this, &ExplorerFinal::AnalyzeSizes });
        styleBtn(m_analyzeButton);
        sortPanel.Children().Append(m_analyzeButton);

        m_batchRenameButton = Button();
        m_batchRenameButton.Content(winrt::box_value(winrt::hstring(L"Batch-Rename")));
        m_batchRenameButton.Click({ this, &ExplorerFinal::BatchRename });
        styleBtn(m_batchRenameButton);
        sortPanel.Children().Append(m_batchRenameButton);

        // Selection and UI enhancement buttons
        m_selectAllButton = Button(); m_selectAllButton.Content(box_value(winrt::hstring(L"Select All"))); m_selectAllButton.Click({ this, &ExplorerFinal::SelectAllItems }); styleBtn(m_selectAllButton);
        m_invertSelectionButton = Button(); m_invertSelectionButton.Content(box_value(winrt::hstring(L"Invert Sel"))); m_invertSelectionButton.Click({ this, &ExplorerFinal::InvertSelection }); styleBtn(m_invertSelectionButton);
        m_duplicateButton = Button(); m_duplicateButton.Content(box_value(winrt::hstring(L"Duplicate"))); m_duplicateButton.Click({ this, &ExplorerFinal::DuplicateSelected }); styleBtn(m_duplicateButton);
        m_toggleDetailsButton = ToggleButton(); m_toggleDetailsButton.Content(box_value(winrt::hstring(L"Details"))); m_toggleDetailsButton.Checked({ this, &ExplorerFinal::ToggleDetails }); m_toggleDetailsButton.Unchecked({ this, &ExplorerFinal::ToggleDetails }); m_toggleDetailsButton.Background(accentBrush); m_toggleDetailsButton.Foreground(textBrush);
        sortPanel.Children().Append(m_selectAllButton);
        sortPanel.Children().Append(m_invertSelectionButton);
        sortPanel.Children().Append(m_duplicateButton);
        sortPanel.Children().Append(m_toggleDetailsButton);

        // AI toolbar buttons
        m_aiSuggestButton = Button(); m_aiSuggestButton.Content(winrt::box_value(winrt::hstring(L"AI Rename"))); m_aiSuggestButton.Click({ this, &ExplorerFinal::SuggestRename }); styleBtn(m_aiSuggestButton);
        m_findDuplicatesButton = Button(); m_findDuplicatesButton.Content(winrt::box_value(winrt::hstring(L"Find Dups"))); m_findDuplicatesButton.Click({ this, &ExplorerFinal::FindDuplicates }); styleBtn(m_findDuplicatesButton);
        m_summarizeButton = Button(); m_summarizeButton.Content(winrt::box_value(winrt::hstring(L"Summarize"))); m_summarizeButton.Click({ this, &ExplorerFinal::SummarizeSelected }); styleBtn(m_summarizeButton);
        m_useFuzzyToggle = ToggleButton(); m_useFuzzyToggle.Content(winrt::box_value(winrt::hstring(L"Fuzzy Search"))); m_useFuzzyToggle.Background(accentBrush); m_useFuzzyToggle.Foreground(textBrush); m_useFuzzyToggle.CornerRadius(Microsoft::UI::Xaml::CornerRadius{6});
        sortPanel.Children().Append(m_aiSuggestButton);
        sortPanel.Children().Append(m_findDuplicatesButton);
        sortPanel.Children().Append(m_summarizeButton);
        sortPanel.Children().Append(m_useFuzzyToggle);

        // Neue Buttons: Compress / Extract / Properties / CopyPath / OpenTerminal / ToggleHidden
        m_compressButton = Button(); m_compressButton.Content(winrt::box_value(winrt::hstring(L"Compress"))); m_compressButton.Click({ this, &ExplorerFinal::CompressSelected });
        m_extractButton = Button(); m_extractButton.Content(winrt::box_value(winrt::hstring(L"Extract"))); m_extractButton.Click({ this, &ExplorerFinal::ExtractSelected });
        m_propertiesButton = Button(); m_propertiesButton.Content(winrt::box_value(winrt::hstring(L"Properties"))); m_propertiesButton.Click({ this, &ExplorerFinal::ShowProperties });
        m_copyPathButton = Button(); m_copyPathButton.Content(winrt::box_value(winrt::hstring(L"Copy Path"))); m_copyPathButton.Click({ this, &ExplorerFinal::CopyPathToClipboard });
        m_openTerminalButton = Button(); m_openTerminalButton.Content(winrt::box_value(winrt::hstring(L"Open Terminal"))); m_openTerminalButton.Click({ this, &ExplorerFinal::OpenInTerminal });
        m_toggleHiddenButton = ToggleButton(); m_toggleHiddenButton.Content(winrt::box_value(winrt::hstring(L"Show Hidden"))); m_toggleHiddenButton.Checked({ this, &ExplorerFinal::ToggleShowHidden }); m_toggleHiddenButton.Unchecked({ this, &ExplorerFinal::ToggleShowHidden });

        // Style the new buttons consistently
        styleBtn(m_compressButton); styleBtn(m_extractButton); styleBtn(m_propertiesButton);
        styleBtn(m_copyPathButton); styleBtn(m_openTerminalButton);
        // ToggleButton styling (use same visuals as Button)
        m_toggleHiddenButton.Background(accentBrush); m_toggleHiddenButton.Foreground(textBrush); m_toggleHiddenButton.CornerRadius(Microsoft::UI::Xaml::CornerRadius{6});

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
        styleBtn(m_searchButton);
        // ComboBox style
        m_searchHistoryCombo.Background(panelBrush);
        m_searchHistoryCombo.Foreground(textBrush);

        searchStack.Children().Append(m_searchBox);
        searchStack.Children().Append(m_searchButton);
        searchStack.Children().Append(m_searchHistoryCombo);
        Grid::SetRow(searchStack, 1);
        // replace previous single-searchBox append with the stack
        mainGrid.Children().Append(searchStack);

        // Content grid: left = files, right = preview
        Grid contentGrid;
        ColumnDefinition colFiles; colFiles.Width(GridLengthHelper::Star());
        ColumnDefinition colPreview; colPreview.Width(GridLengthHelper::Auto());
        contentGrid.ColumnDefinitions().Append(colFiles);
        contentGrid.ColumnDefinitions().Append(colPreview);
        Grid::SetRow(contentGrid, 3);

        // Datei-GridView (left)
        m_fileGrid = GridView();
        m_fileGrid.IsItemClickEnabled(true);
        m_fileGrid.SelectionMode(Microsoft::UI::Xaml::Controls::ListViewSelectionMode::Multiple);
        m_fileGrid.IsMultiSelectCheckBoxEnabled(true);
        m_fileGrid.ItemClick({ this, &ExplorerFinal::FileItemClick });
        m_fileGrid.SelectionChanged({ this, &ExplorerFinal::FileSelectionChanged });
        m_fileGrid.AllowDrop(true);
        m_fileGrid.DragItemsStarting({ this, &ExplorerFinal::DragStart });
        m_fileGrid.Drop({ this, &ExplorerFinal::DropFiles });
        m_fileGrid.Background(panelBrush);
        Grid::SetColumn(m_fileGrid, 0);
        contentGrid.Children().Append(m_fileGrid);

        // Preview pane (right)
        m_previewBorder = Border();
        m_previewBorder.Padding(ThicknessHelper::FromUniformLength(8));
        m_previewBorder.Width(360);
        m_previewBorder.Background(previewBrush);
        m_previewBorder.BorderBrush(accentBrush);
        m_previewBorder.CornerRadius(Microsoft::UI::Xaml::CornerRadius{ 8 });
        StackPanel previewStack;
        previewStack.Orientation(Orientation::Vertical);
        m_previewImage = Image();
        m_previewImage.MaxHeight(240);
        m_previewImage.MaxWidth(320);
        previewStack.Children().Append(m_previewImage);
        m_previewText = TextBlock();
        m_previewText.TextWrapping(TextWrapping::Wrap);
        m_previewText.Foreground(textBrush);
        previewStack.Children().Append(m_previewText);
        m_previewBorder.Child(previewStack);
        Grid::SetColumn(m_previewBorder, 1);
        contentGrid.Children().Append(m_previewBorder);

        mainGrid.Children().Append(contentGrid);

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
            std::wstring selPath = GetSelectedFullPath();
            bool enableAdd = false;
            bool enableRem = false;
            if (!selPath.empty()) {
                auto it = FindItemByPath(selPath);
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

                // create image factory (if possible) for future thumbnail retrieval
                IShellItem* psi = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(fi.fullPath.c_str(), nullptr, IID_PPV_ARGS(&psi)))) {
                    winrt::com_ptr<IShellItemImageFactory> factory;
                    if (SUCCEEDED(psi->QueryInterface(IID_PPV_ARGS(&factory)))) {
                        fi.imageFactory = factory;
                    }
                    psi->Release();
                }

                currentItems.push_back(fi);

                // Build a nicer rounded card for the GridView item (dark theme)
                Border itemCard;
                itemCard.CornerRadius(Microsoft::UI::Xaml::CornerRadius{ 8 });
                itemCard.Padding(ThicknessHelper::FromUniformLength(8));
                itemCard.Margin(ThicknessHelper::FromLengths(6, 6, 6, 6));
                // dark card
                itemCard.Background(SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 48, 48, 48)));

                StackPanel cardStack;
                cardStack.Orientation(Orientation::Vertical);

                // Thumbnail image (async loaded)
                Image thumbImg;
                thumbImg.Width( (double) m_thumbnailSize );
                thumbImg.Height( (double) (m_thumbnailSize * 0.75) );
                thumbImg.Margin(ThicknessHelper::FromLengths(0, 0, 0, 6));
                cardStack.Children().Append(thumbImg);
                // start async thumbnail load (fire-and-forget)
                LoadThumbnailAsync(fi, thumbImg);

                TextBlock iconTb;
                iconTb.FontSize(28);
                iconTb.Margin(ThicknessHelper::FromLengths(0, 0, 0, 4));
                iconTb.Text(winrt::hstring(fi.isFolder ? L"📁" : L"📄"));
                iconTb.Foreground(SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 230, 230, 230)));

                TextBlock nameTb;
                nameTb.Text(winrt::hstring(fi.name));
                nameTb.TextWrapping(TextWrapping::NoWrap);
                nameTb.MaxWidth(220);
                nameTb.FontSize(14);
                nameTb.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
                nameTb.Foreground(SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 235, 235, 235)));

                cardStack.Children().Append(iconTb);
                cardStack.Children().Append(nameTb);

                if (m_showDetails) {
                    std::wstringstream ss;
                    ss << L"Size: " << fi.size << L" bytes\n";
                    SYSTEMTIME stUTC, stLocal;
                    FileTimeToSystemTime(&fi.modifiedTime, &stUTC);
                    SystemTimeToTzSpecificLocalTime(NULL, &stUTC, &stLocal);
                    ss << L"Modified: " << stLocal.wDay << L"." << stLocal.wMonth << L"." << stLocal.wYear;
                    TextBlock detailsTb;
                    detailsTb.Text(winrt::hstring(ss.str()));
                    detailsTb.FontSize(11);
                    detailsTb.Opacity(0.9);
                    detailsTb.TextWrapping(TextWrapping::Wrap);
                    detailsTb.Margin(ThicknessHelper::FromLengths(0, 4, 0, 0));
                    detailsTb.Foreground(SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 190, 190, 190)));
                    cardStack.Children().Append(detailsTb);
                }

                itemCard.Child(cardStack);

                // attach the full path in Tag for later retrieval on clicks
                itemCard.Tag(winrt::box_value(winrt::hstring(fi.fullPath)));

                m_fileGrid.Items().Append(itemCard);
            }
        } while (FindNextFile(hFind, &fd));
        FindClose(hFind);
        filteredItems = currentItems;
        SortAndRefresh();
    }

    // Suchleiste filter: delegiere an PerformSearch (unterstützt Fuzzy + Rekursion)
    void SearchTextChanged(IInspectable const&, TextChangedEventArgs const&) {
        PerformSearch();
    }

    void PerformSearch() {
        std::wstring filter = m_searchBox.Text().c_str();
        if (!filter.empty()) {
            auto it = std::find(searchHistory.begin(), searchHistory.end(), filter);
            if (it != searchHistory.end()) searchHistory.erase(it);
            searchHistory.insert(searchHistory.begin(), filter);
            if (searchHistory.size() > 25) searchHistory.resize(25);
            m_searchHistoryCombo.Items().Clear();
            for (auto const& s : searchHistory) m_searchHistoryCombo.Items().Append(box_value(winrt::hstring(s)));
        }

        filteredItems.clear();
        bool useFuzzy = m_useFuzzyToggle && m_useFuzzyToggle.IsChecked().HasValue() && m_useFuzzyToggle.IsChecked().Value();
        if (m_recursiveToggle && m_recursiveToggle.IsOn()) {
            RecursiveSearch(m_addressBar.Text().c_str(), filter, filteredItems);
        } else {
            for (auto& fi : currentItems) {
                if (useFuzzy) {
                    if (FuzzyMatch(fi.name, filter)) filteredItems.push_back(fi);
                } else {
                    if (fi.name.find(filter) != std::wstring::npos) filteredItems.push_back(fi);
                }
            }
        }
        SortAndRefresh();
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

    // New helper: get full path of first selected item (works with cards or legacy string items)
    std::wstring GetSelectedFullPath() {
        auto sel = m_fileGrid.SelectedItem();
        if (!sel) return {};
        auto fe = sel.try_as<FrameworkElement>();
        if (fe) {
            try {
                auto tag = fe.Tag();
                if (tag) return std::wstring(unbox_value<hstring>(tag).c_str());
            }
            catch (...) {}
        }
        // legacy fallback
        try {
            hstring name = unbox_value<hstring>(sel);
            return std::wstring(name.c_str());
        } catch (...) {}
        return {};
    }

    // Find FileItem by full path
    std::vector<FileItem>::iterator FindItemByPath(std::wstring const& path) {
        return std::find_if(currentItems.begin(), currentItems.end(), [&path](FileItem const& fi) { return fi.fullPath == path; });
    }

    // Create the Border card for GridView (used by PopulateFiles and SortAndRefresh)
    Border CreateItemCard(FileItem const& fi) {
        Border itemCard;
        itemCard.CornerRadius(Microsoft::UI::Xaml::CornerRadius{ 8 });
        itemCard.Padding(ThicknessHelper::FromUniformLength(8));
        itemCard.Margin(ThicknessHelper::FromLengths(6, 6, 6, 6));
        itemCard.Background(SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 48, 48, 48)));

        StackPanel cardStack;
        cardStack.Orientation(Orientation::Vertical);

        TextBlock iconTb;
        iconTb.FontSize(28);
        iconTb.Margin(ThicknessHelper::FromLengths(0, 0, 0, 4));
        iconTb.Text(winrt::hstring(fi.isFolder ? L"📁" : L"📄"));
        iconTb.Foreground(SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 230, 230, 230)));

        TextBlock nameTb;
        nameTb.Text(winrt::hstring(fi.name));
        nameTb.TextWrapping(TextWrapping::NoWrap);
        nameTb.MaxWidth(220);
        nameTb.FontSize(14);
        nameTb.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        nameTb.Foreground(SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 235, 235, 235)));

        cardStack.Children().Append(iconTb);
        cardStack.Children().Append(nameTb);

        if (m_showDetails) {
            std::wstringstream ss;
            ss << L"Size: " << fi.size << L" bytes\n";
            SYSTEMTIME stUTC, stLocal;
            FileTimeToSystemTime(&fi.modifiedTime, &stUTC);
            SystemTimeToTzSpecificLocalTime(NULL, &stUTC, &stLocal);
            ss << L"Modified: " << stLocal.wDay << L"." << stLocal.wMonth << L"." << stLocal.wYear;
            TextBlock detailsTb;
            detailsTb.Text(winrt::hstring(ss.str()));
            detailsTb.FontSize(11);
            detailsTb.Opacity(0.9);
            detailsTb.TextWrapping(TextWrapping::Wrap);
            detailsTb.Margin(ThicknessHelper::FromLengths(0, 4, 0, 0));
            detailsTb.Foreground(SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 190, 190, 190)));
            cardStack.Children().Append(detailsTb);
        }

        itemCard.Child(cardStack);
        itemCard.Tag(winrt::box_value(winrt::hstring(fi.fullPath)));
        return itemCard;
    }

    // Generate several rename suggestions (AI heuristics)
    std::vector<std::wstring> GenerateRenameSuggestions(FileItem const& fi) {
        std::vector<std::wstring> out;
        std::filesystem::path p(fi.fullPath);
        std::wstring stem = p.stem().wstring();
        std::wstring ext = p.extension().wstring();
        // 1) Cleaned original (normalize spaces, lowercase first char)
        auto clean = stem;
        for (auto& c : clean) if (c == L'/' || c == L'\\' || c == L':' || c == L'*' || c == L'?' || c == L'\"' || c == L'<' || c == L'>' || c == L'|') c = L'_';
        out.push_back(clean + ext);
        // 2) Add date suffix if available
        SYSTEMTIME stUTC, stLocal;
        FileTimeToSystemTime(&fi.modifiedTime, &stUTC);
        SystemTimeToTzSpecificLocalTime(NULL, &stUTC, &stLocal);
        {
            std::wstringstream ss;
            ss << stem << L"_" << stLocal.wYear << std::setw(2) << std::setfill(L'0') << stLocal.wMonth << std::setw(2) << stLocal.wDay << ext;
            out.push_back(ss.str());
        }
        // 3) Use tags for text files
        std::wstring lowerExt = ext; std::transform(lowerExt.begin(), lowerExt.end(), lowerExt.begin(), ::towlower);
        if (lowerExt == L".txt" || lowerExt == L".md" || lowerExt == L".csv") {
            auto tags = ExtractTagsFromTextFile(fi.fullPath, 3);
            if (!tags.empty()) {
                std::wstring t = L"";
                for (size_t i = 0; i < tags.size(); ++i) {
                    if (i) t += L"_";
                    t += tags[i];
                }
                out.push_back(stem + L"_" + t + ext);
            }
        }
        // 4) Add copy variant
        out.push_back(stem + L"_copy" + ext);
        // Deduplicate keep order
        std::vector<std::wstring> uniq;
        for (auto const& s : out) if (std::find(uniq.begin(), uniq.end(), s) == uniq.end()) uniq.push_back(s);
        return uniq;
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
            m_fileGrid.Items().Append(CreateItemCard(fi));
        }
    }

    void CopyItem(IInspectable const&, RoutedEventArgs const&) {
        std::wstring selPath = GetSelectedFullPath();
        if (selPath.empty()) return;
        auto it = FindItemByPath(selPath);
        if (it == currentItems.end()) return;
        std::wstring path = it->fullPath;
        // Build CF_HDROP data (use double null termination)
        size_t len = sizeof(DROPFILES) + (path.length() + 2) * sizeof(wchar_t);
        HGLOBAL hg = GlobalAlloc(GHND, len);
        if (!hg) return;
        DROPFILES* df = (DROPFILES*)GlobalLock(hg);
        if (!df) { GlobalFree(hg); return; }
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        wchar_t* dst = (wchar_t*)(df + 1);
        wcscpy_s(dst, path.length() + 1, path.c_str());
        dst[path.length() + 1] = L'\0';
        GlobalUnlock(hg);
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            SetClipboardData(CF_HDROP, hg);
            CloseClipboard();
        } else {
            GlobalFree(hg);
        }
        m_fileGrid.SelectedItem(nullptr);
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
        std::wstring selPath = GetSelectedFullPath();
        if (selPath.empty()) return;
        auto it = FindItemByPath(selPath);
        if (it == currentItems.end()) return;
        std::wstring path = it->fullPath;
        std::vector<wchar_t> doubleNullPath(path.length() + 2, 0);
        wcscpy_s(doubleNullPath.data(), path.length() + 1, path.c_str());

        SHFILEOPSTRUCTW fileOp = { 0 };
        fileOp.wFunc = FO_DELETE;
        fileOp.pFrom = doubleNullPath.data();
        fileOp.fFlags = FOF_ALLOWUNDO;

        int result = SHFileOperationW(&fileOp);
        if (result == 0 && !fileOp.fAnyOperationsAborted) PopulateFiles(m_addressBar.Text());
        m_fileGrid.SelectedItem(nullptr); // Auswahl zurücksetzen
    }

    // RenameItem: show dialog with AI suggestions combo + textbox; apply chosen name
    fire_and_forget RenameItem(IInspectable const&, RoutedEventArgs const&) {
        std::wstring selPath = GetSelectedFullPath();
        if (selPath.empty()) co_return;
        auto it = FindItemByPath(selPath);
        if (it == currentItems.end()) co_return;

        FileItem fi = *it;
        // Build suggestions
        auto suggestions = GenerateRenameSuggestions(fi);

        TextBox input;
        input.Text(winrt::hstring(fi.name));

        ComboBox suggestionsCombo;
        for (auto const& s : suggestions) suggestionsCombo.Items().Append(box_value(winrt::hstring(s)));
        // capture suggestionsCombo and input by reference so selection updates textbox
        suggestionsCombo.SelectionChanged([&input, &suggestionsCombo](auto const&, auto const&) {
            auto sel = suggestionsCombo.SelectedItem();
            if (sel) input.Text(unbox_value<hstring>(sel));
        });

        StackPanel content;
        content.Orientation(Orientation::Vertical);
        content.Children().Append(input);
        if (!suggestions.empty()) {
            TextBlock hint; hint.Text(winrt::hstring(L"AI Vorschläge:")); hint.Foreground(SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255,190,190,190)));
            content.Children().Append(hint);
            content.Children().Append(suggestionsCombo);
        }

        ContentDialog dialog;
        dialog.Title(box_value(winrt::hstring(L"Umbenennen (AI)")));
        dialog.Content(content);
        dialog.PrimaryButtonText(L"OK");
        dialog.SecondaryButtonText(L"Abbrechen");
        dialog.XamlRoot(m_fileGrid.XamlRoot());

        auto result = co_await dialog.ShowAsync();
        if (result == ContentDialogResult::Primary) {
            std::wstring newName = input.Text().c_str();
            if (!newName.empty() && newName != it->name) {
                std::filesystem::path pold(it->fullPath);
                std::wstring newPath = pold.parent_path().wstring() + L"\\" + newName;
                // push undo entry
                m_undoStack.emplace_back(std::wstring(L"rename"), it->fullPath, newPath);
                MoveFile(pold.wstring().c_str(), newPath.c_str());
                PopulateFiles(m_addressBar.Text());
            }
        }
        m_fileGrid.SelectedItem(nullptr);
    }

    // SuggestRename: show AI suggestions only (separate command), use same suggestion generator but show multiple choices
    fire_and_forget SuggestRename(IInspectable const&, RoutedEventArgs const&) {
        std::wstring selPath = GetSelectedFullPath();
        if (selPath.empty()) co_return;
        auto it = FindItemByPath(selPath);
        if (it == currentItems.end()) co_return;
        FileItem fi = *it;
        auto suggestions = GenerateRenameSuggestions(fi);
        if (suggestions.empty()) {
            ContentDialog dlg; dlg.Title(box_value(winrt::hstring(L"AI Vorschläge"))); TextBlock tb; tb.Text(winrt::hstring(L"No suggestions")); dlg.Content(tb); dlg.PrimaryButtonText(L"OK"); dlg.XamlRoot(m_fileGrid.XamlRoot()); co_await dlg.ShowAsync(); co_return;
        }
        StackPanel sp; sp.Orientation(Orientation::Vertical);
        ComboBox cb; cb.PlaceholderText(L"Choose suggestion");
        for (auto const& s : suggestions) cb.Items().Append(box_value(winrt::hstring(s)));
        sp.Children().Append(cb);
        TextBlock note; note.Text(winrt::hstring(L"Select a suggestion and click Apply.")); note.Foreground(SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255,190,190,190)));
        sp.Children().Append(note);
        ContentDialog dlg; dlg.Title(box_value(winrt::hstring(L"AI Rename Suggestions"))); dlg.Content(sp); dlg.PrimaryButtonText(L"Apply"); dlg.SecondaryButtonText(L"Cancel"); dlg.XamlRoot(m_fileGrid.XamlRoot());
        auto res = co_await dlg.ShowAsync();
        if (res == ContentDialogResult::Primary) {
            auto sel = cb.SelectedItem();
            if (sel) {
                std::wstring chosen = unbox_value<hstring>(sel).c_str();
                std::filesystem::path oldp(fi.fullPath);
                std::filesystem::path newp = oldp.parent_path() / chosen;
                try { std::filesystem::rename(oldp, newp); PopulateFiles(m_addressBar.Text()); } catch (...) {}
            }
        }
    }

    // BatchRename: use Tag-based selected items and optionally use AI suggestions pattern
    fire_and_forget BatchRename(IInspectable const&, RoutedEventArgs const&) {
        // Get selection as full paths
        std::vector<std::wstring> selPaths;
        for (uint32_t i = 0; i < m_fileGrid.SelectedItems().Size(); ++i) {
            auto item = m_fileGrid.SelectedItems().GetAt(i);
            auto fe = item.try_as<FrameworkElement>();
            if (!fe) continue;
            try { selPaths.push_back(unbox_value<hstring>(fe.Tag()).c_str()); } catch (...) {}
        }
        if (selPaths.empty()) co_return;

        // Ask user for pattern or AI mode
        StackPanel content; content.Orientation(Orientation::Vertical);
        TextBox patternBox; patternBox.PlaceholderText(L"Pattern (use {n} for index, leave empty to use AI)");
        content.Children().Append(patternBox);
        ContentDialog dlg; dlg.Title(box_value(winrt::hstring(L"Batch-Rename (Pattern or AI)"))); dlg.Content(content); dlg.PrimaryButtonText(L"Apply"); dlg.SecondaryButtonText(L"Cancel"); dlg.XamlRoot(m_fileGrid.XamlRoot());
        auto res = co_await dlg.ShowAsync();
        if (res != ContentDialogResult::Primary) co_return;
        std::wstring pattern = patternBox.Text().c_str();

        int idx = 1;
        for (auto const& path : selPaths) {
            auto it = FindItemByPath(path);
            if (it == currentItems.end()) continue;
            std::wstring newName;
            if (!pattern.empty()) {
                newName = pattern;
                size_t pos = newName.find(L"{n}");
                if (pos != std::wstring::npos) {
                    newName.replace(pos, 3, std::to_wstring(idx++));
                }
            } else {
                // AI suggestion for this item
                auto sugg = GenerateRenameSuggestions(*it);
                newName = sugg.empty() ? it->name : sugg.front();
            }
            std::filesystem::path oldp(it->fullPath);
            std::filesystem::path newp = oldp.parent_path() / newName;
            // record undo (src, dst)
            m_undoStack.emplace_back(std::wstring(L"rename"), oldp.wstring(), newp.wstring());
            try { std::filesystem::rename(oldp, newp); } catch (...) {}
        }
        PopulateFiles(m_addressBar.Text());
    }

    // Settings load/save
    void LoadSettings() {
        try {
            if (m_settingsPath.empty()) return;
            std::ifstream f(m_settingsPath, std::ios::binary);
            if (!f.is_open()) return;
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            Json::CharReaderBuilder rb;
            std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
            Json::Value root; std::string errs;
            if (reader->parse(content.c_str(), content.c_str()+content.size(), &root, &errs)) {
                m_settings = root;
                if (root.isMember("thumbnailSize")) m_thumbnailSize = root["thumbnailSize"].asUInt();
                if (root.isMember("fuzzyThreshold")) m_fuzzyThreshold = root["fuzzyThreshold"].asInt();
                if (root.isMember("showHidden")) m_showHidden = root["showHidden"].asBool();
                if (root.isMember("showDetails")) m_showDetails = root["showDetails"].asBool();
            }
        } catch (...) {}
    }

    void SaveSettings() {
        try {
            if (m_settingsPath.empty()) return;
            Json::Value root;
            root["thumbnailSize"] = m_thumbnailSize;
            root["fuzzyThreshold"] = m_fuzzyThreshold;
            root["showHidden"] = m_showHidden;
            root["showDetails"] = m_showDetails;
            Json::StreamWriterBuilder w; w["indentation"] = "  ";
            auto out = Json::writeString(w, root);
            std::ofstream f(m_settingsPath, std::ios::binary);
            if (f.is_open()) f << out;
        } catch (...) {}
    }

    // Clear thumbnail cache helper
    void ClearThumbnailCache() {
        m_thumbnailCache.clear();
    }

    // Indexer controls
    void StartIndexing() {
        if (m_indexRunning) return;
        m_indexRunning = true;
        m_progressBar.Value(0);
        std::wstring root = m_addressBar.Text().c_str();
        m_indexThread = std::thread([this, root]() {
            this->BuildIndex(root);
            // update UI after done (post to UI thread)
            DispatcherQueue::GetForCurrentThread().TryEnqueue([this]() {
                m_indexRunning = false;
                m_progressBar.Value(100);
                SaveIndex();
                PopulateFiles(m_addressBar.Text());
            });
        });
        m_indexThread.detach();
    }

    void StopIndexing() {
        if (!m_indexRunning) return;
        m_indexRunning = false; // BuildIndex should check this flag
        m_indexCv.notify_all();
    }

    void BuildIndex(std::wstring root) {
        try {
            std::lock_guard<std::mutex> lg(m_indexMutex);
            m_index.clear();
            std::vector<std::wstring> files;
            for (auto const& entry : std::filesystem::recursive_directory_iterator(root)) {
                if (entry.is_regular_file()) files.push_back(entry.path().wstring());
                if (!m_indexRunning) break;
            }
            size_t total = files.size();
            for (size_t i = 0; i < files.size() && m_indexRunning; ++i) {
                auto const& path = files[i];
                Json::Value meta;
                try {
                    meta["path"] = Utf8FromWString(path);
                    meta["sha1"] = ComputeFileSHA1(path);
                    meta["size"] = (Json::UInt64)std::filesystem::file_size(path);
                    auto tags = ExtractTagsFromTextFile(path, 5);
                    for (auto const& t : tags) meta["tags"].append(Utf8FromWString(t));
                } catch (...) {}
                m_index[path] = meta;
                // update progress on UI thread
                int percent = total ? (int)((i * 100) / total) : 0;
                DispatcherQueue::GetForCurrentThread().TryEnqueue([this, percent]() {
                    m_progressBar.Value(percent);
                });
                // small pause to be responsive
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        } catch (...) {}
    }

    void SaveIndex() {
        try {
            Json::Value root;
            for (auto const& kv : m_index) {
                Json::Value m = kv.second;
                root[Utf8FromWString(kv.first)] = m;
            }
            Json::StreamWriterBuilder w;
            w["indentation"] = "  ";
            auto out = Json::writeString(w, root);
            auto p = std::filesystem::path(GetFavoritesPath()).parent_path() / L"index.json";
            std::ofstream f(p.wstring(), std::ios::binary);
            if (f.is_open()) f << out;
        } catch (...) {}
    }

    void LoadIndex() {
        try {
            auto p = std::filesystem::path(GetFavoritesPath()).parent_path() / L"index.json";
            if (!std::filesystem::exists(p)) return;
            std::ifstream f(p.wstring(), std::ios::binary);
            if (!f.is_open()) return;
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            Json::CharReaderBuilder rb;
            std::unique_ptr<Json::CharReader> r(rb.newCharReader());
            Json::Value root; std::string errs;
            if (r->parse(content.c_str(), content.c_str() + content.size(), &root, &errs)) {
                std::lock_guard<std::mutex> lg(m_indexMutex);
                m_index.clear();
                for (auto it = root.begin(); it != root.end(); ++it) {
                    std::wstring key = WStringFromUtf8(it.memberName());
                    m_index[key] = *it;
                }
            }
        } catch (...) {}
    }

    // Semantic search: dialog to input query and show matching index entries
    void SemanticSearchDialog() {
        ContentDialog dlg;
        dlg.Title(box_value(winrt::hstring(L"Semantic Search")));
        TextBox q; q.PlaceholderText(L"Enter search query...");
        StackPanel sp; sp.Orientation(Orientation::Vertical);
        sp.Children().Append(q);
        dlg.Content(sp);
        dlg.PrimaryButtonText(L"Search");
        dlg.SecondaryButtonText(L"Cancel");
        dlg.XamlRoot(m_fileGrid.XamlRoot());

        auto res = dlg.ShowAsync().get();
        if (res != ContentDialogResult::Primary) return;
        std::wstring query = q.Text().c_str();
        if (query.empty()) return;
        // simple semantic: compare tag overlap + name fuzzy
        std::vector<std::pair<int,std::wstring>> scored;
        std::vector<std::wstring> qtags = ExtractTagsFromTextFile(query, 8); // treat query as pseudo-file
        for (auto const& kv : m_index) {
            int score = 0;
            auto const& meta = kv.second;
            if (meta.isMember("tags")) {
                for (auto const& qt : qtags) {
                    std::string qt8 = Utf8FromWString(qt);
                    for (auto const& mt : meta["tags"]) {
                        if (mt.asString() == qt8) score += 3;
                    }
                }
            }
            // fuzzy name match
            std::wstring name = WStringFromUtf8(meta.get("path", "").asString());
            if (FuzzyMatch(name, query, 3)) score += 2;
            if (score > 0) scored.emplace_back(score, kv.first);
        }
        std::sort(scored.begin(), scored.end(), [](auto const& a, auto const& b){ return a.first > b.first; });
        // show results as simple dialog selecting top 10
        StackPanel results; results.Orientation(Orientation::Vertical);
        int count = 0;
        for (auto const& p : scored) {
            if (++count > 20) break;
            TextBlock tb; tb.Text(winrt::hstring(p.second)); tb.Foreground(SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255,230,230,230)));
            results.Children().Append(tb);
        }
        if (scored.empty()) {
            results.Children().Append(TextBlock{ winrt::box_value(L"No semantic results") });
        }
        ContentDialog rdlg; rdlg.Title(box_value(winrt::hstring(L"Semantic Results"))); rdlg.Content(results); rdlg.PrimaryButtonText(L"OK"); rdlg.XamlRoot(m_fileGrid.XamlRoot()); rdlg.ShowAsync();
    }

    // Auto-Categorize: buckets files by top tag
    void AutoCategorize() {
        std::unordered_map<std::wstring, std::vector<std::wstring>> cats;
        for (auto const& kv : m_index) {
            auto const& meta = kv.second;
            if (meta.isMember("tags") && meta["tags"].size() > 0) {
                std::string t = meta["tags"][0].asString();
                std::wstring wt = WStringFromUtf8(t);
                cats[wt].push_back(kv.first);
            } else {
                cats[L"uncategorized"].push_back(kv.first);
            }
        }
        // present categories in a dialog
        StackPanel sp; sp.Orientation(Orientation::Vertical);
        for (auto const& c : cats) {
            TextBlock tb; tb.Text(winrt::hstring(c.first + L" (" + std::to_wstring(c.second.size()) + L")")); tb.Foreground(SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255,230,230,230)));
            sp.Children().Append(tb);
        }
        ContentDialog dlg; dlg.Title(box_value(winrt::hstring(L"Auto-Categories"))); dlg.Content(sp); dlg.PrimaryButtonText(L"OK"); dlg.XamlRoot(m_fileGrid.XamlRoot()); dlg.ShowAsync();
    }

    // AutoRenameAll button handler already wired to BatchRename for pattern/AI; provide Undo support
	fire_and_forget AutoRenameAll(IInspectable const&, RoutedEventArgs const&) {
		// reuse BatchRename UI but record actions into undo stack
		// (this wrapper calls BatchRename which already applies renames)
		co_await BatchRename(nullptr, {});
		// Note: BatchRename currently performs renames without undo records;
		// for full undo support you'd push rename pairs to m_undoStack during rename operations.
	}

	// Undo: pop last rename and revert
	void UndoLast() {
		if (m_undoStack.empty()) {
			ContentDialog dlg; dlg.Title(box_value(winrt::hstring(L"Undo"))); dlg.Content(box_value(winrt::hstring(L"No actions to undo"))); dlg.PrimaryButtonText(L"OK"); dlg.XamlRoot(m_fileGrid.XamlRoot()); dlg.ShowAsync();
			return;
		}
		auto op = m_undoStack.back(); m_undoStack.pop_back();
		auto type = std::get<0>(op);
		auto src = std::get<1>(op);
		auto dst = std::get<2>(op);
		if (type == L"rename") {
			try { std::filesystem::rename(dst, src); PopulateFiles(m_addressBar.Text()); } catch (...) {}
		} else if (type == L"create") {
			try {
				if (!dst.empty() && std::filesystem::exists(dst)) {
					// remove the created file
					std::filesystem::remove(dst);
					PopulateFiles(m_addressBar.Text());
				}
			} catch (...) {}
		}
        // other types could be implemented similarly
	}
};

int main()
{
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    Application::Start([](auto&&) { make<ExplorerFinal>(); });
}
