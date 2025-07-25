#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <fstream>
#include <zlib.h>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <sstream>

extern "C" {
#include "LzmaDec.h"
#include "Alloc.h"
}

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib") // Для прогресс бара
#pragma comment(lib, "zlib.lib")
#pragma comment(lib, "shlwapi.lib")

HINSTANCE hInst;
HWND hMainWnd, hButtonOpen, hButtonExtract, hLogEdit, hProgress;

std::wstring selectedFile;
std::wstring outputFolder;

std::mutex logMutex;
std::wstring logBuffer;

std::mutex queueMutex;
std::condition_variable cv;
std::queue<size_t> fileQueue;

std::atomic<size_t> processedFiles = 0;
size_t totalFiles = 0;

bool extractionStarted = false;

// Вспомогательные функции для UI

void AppendLog(const std::wstring& msg) {
    std::lock_guard<std::mutex> lock(logMutex);
    logBuffer += msg;
}

// Создаем каталоги по пути к файлу
void CreateDirectoriesForFile(const std::wstring& filePath) {
    size_t pos = 0;
    while ((pos = filePath.find(L'\\', pos)) != std::wstring::npos) {
        std::wstring folder = filePath.substr(0, pos);
        CreateDirectoryW(folder.c_str(), NULL);
        pos++;
    }
}

void WriteFileToDisk(const std::wstring& outDir, const std::string& name, const std::vector<uint8_t>& data) {
    std::wstring wname(name.begin(), name.end());
    for (auto& ch : wname) if (ch == L'/') ch = L'\\';
    std::wstring fullPath = outDir + L"\\" + wname;
    CreateDirectoriesForFile(fullPath);

    std::ofstream out(fullPath, std::ios::binary);
    if (out.is_open()) {
        out.write(reinterpret_cast<const char*>(data.data()), data.size());
        out.close();
        AppendLog(L"File saved: " + fullPath + L"\r\n");
    }
    else {
        AppendLog(L"Failed to save: " + fullPath + L"\r\n");
    }
}

bool DecompressLZMA(const std::vector<uint8_t>& src, std::vector<uint8_t>& dst, size_t outSize) {
    ELzmaStatus status;
    SizeT destLen = outSize;
    SizeT srcLen = src.size() - LZMA_PROPS_SIZE;
    dst.resize(outSize);

    int res = LzmaDecode(
        dst.data(), &destLen,
        src.data() + LZMA_PROPS_SIZE, &srcLen,
        src.data(), LZMA_PROPS_SIZE,
        LZMA_FINISH_END,
        &status,
        &g_Alloc);

    return (res == SZ_OK && destLen == outSize);
}

bool DecompressZlib(const std::vector<uint8_t>& src, std::vector<uint8_t>& dst, size_t outSize) {
    dst.resize(outSize);
    uLongf destLen = static_cast<uLongf>(outSize);
    return uncompress(dst.data(), &destLen, src.data(), static_cast<uLong>(src.size())) == Z_OK
        && destLen == outSize;
}

// Здесь хранится вся информация о файлах из архива
struct FileEntry {
    std::string name;
    uint32_t offset;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
};

std::vector<uint8_t> archiveData;
std::vector<FileEntry> fileEntries;
uint8_t flags = 0;
uint8_t key[4] = { 0 };
uint32_t baseOff = 0;

enum { COMP_ZLIB = 0x00, COMP_LZMA = 0x10, COMP_COPY = 0x20 };

// Функция потоковой распаковки одного файла
void WorkerThread() {
    while (true) {
        size_t idx = 0;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            if (fileQueue.empty()) break;
            idx = fileQueue.front();
            fileQueue.pop();
        }

        // Извлекаем файл по индексу
        const FileEntry& fe = fileEntries[idx];
        if (fe.offset + fe.compressedSize > archiveData.size()) {
            AppendLog(L"Invalid file offset/size: " + std::wstring(fe.name.begin(), fe.name.end()) + L"\r\n");
            processedFiles++;
            continue;
        }

        std::vector<uint8_t> raw(archiveData.begin() + fe.offset, archiveData.begin() + fe.offset + fe.compressedSize);
        for (size_t j = 0; j < raw.size(); j++)
            raw[j] ^= key[j % 4];

        std::vector<uint8_t> finalData;
        bool success = false;

        if (fe.compressedSize == fe.uncompressedSize || flags == COMP_COPY) {
            finalData = raw;
            success = true;
        }
        else if (flags == COMP_ZLIB) {
            success = DecompressZlib(raw, finalData, fe.uncompressedSize);
        }
        else if (flags == COMP_LZMA) {
            success = DecompressLZMA(raw, finalData, fe.uncompressedSize);
        }

        if (success) {
            WriteFileToDisk(outputFolder, fe.name, finalData);
        }
        else {
            AppendLog(L"Failed to decompress: " + std::wstring(fe.name.begin(), fe.name.end()) + L"\r\n");
        }

        processedFiles++;
        // Прогресс будем обновлять в главном потоке
    }
}

void StartExtraction() {
    if (extractionStarted) return;
    extractionStarted = true;

    // Очистка лога и прогресса
    SendMessageW(hLogEdit, WM_SETTEXT, 0, (LPARAM)L"");
    SendMessage(hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, (int)totalFiles));
    SendMessage(hProgress, PBM_SETPOS, 0, 0);

    // Заполнение очереди заданий
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        while (!fileQueue.empty()) fileQueue.pop();
        for (size_t i = 0; i < fileEntries.size(); i++) {
            fileQueue.push(i);
        }
    }
    processedFiles = 0;

    unsigned int threadCount = std::thread::hardware_concurrency();
    if (threadCount == 0) threadCount = 2; // запасной вариант

    // Запускаем воркеры
    std::vector<std::thread> workers;
    for (unsigned int i = 0; i < threadCount; i++) {
        workers.emplace_back(WorkerThread);
    }

    // Отдельный поток ждет завершения воркеров и ставит extractionStarted=false
    std::thread([workers = std::move(workers)]() mutable {
        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }
        extractionStarted = false;
        }).detach();
}

// Функция обновления UI логов и прогресса — вызывается таймером
void UpdateUI() {
    static std::wstring lastLog;
    std::wstring toWrite;
    {
        std::lock_guard<std::mutex> lock(logMutex);
        toWrite.swap(logBuffer);
    }
    if (!toWrite.empty()) {
        int len = GetWindowTextLengthW(hLogEdit);
        SendMessageW(hLogEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        SendMessageW(hLogEdit, EM_REPLACESEL, FALSE, (LPARAM)toWrite.c_str());
        SendMessageW(hLogEdit, EM_SCROLLCARET, 0, 0);
    }

    if (totalFiles > 0) {
        SendMessage(hProgress, PBM_SETPOS, (WPARAM)processedFiles.load(), 0);
    }
}

// Диалог выбора папки
std::wstring BrowseFolderDialog(HWND hwnd, const std::wstring& title) {
    wchar_t path[MAX_PATH] = { 0 };
    BROWSEINFOW bi = { 0 };
    bi.hwndOwner = hwnd;
    bi.lpszTitle = title.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl && SHGetPathFromIDListW(pidl, path)) {
        CoTaskMemFree(pidl);
        return std::wstring(path);
    }
    if (pidl) CoTaskMemFree(pidl);
    return L"";
}

void ExtractSMF(const std::wstring& filename) {
    std::ifstream in(filename, std::ios::binary);
    if (!in) {
        AppendLog(L"Failed to open file.\r\n");
        return;
    }

    archiveData.assign(std::istreambuf_iterator<char>(in), {});
    in.close();

    if (archiveData.size() < 20) {
        AppendLog(L"File too small or corrupted.\r\n");
        return;
    }

    size_t pos = 0;
    flags = archiveData[pos++] & 0x30;

    if (pos + 4 > archiveData.size()) return;
    uint32_t nameSz = *reinterpret_cast<const uint32_t*>(&archiveData[pos]);
    pos += 4;

    if (pos + nameSz + 4 + 4 + 4 > archiveData.size()) return;
    pos += nameSz;

    memcpy(key, &archiveData[pos], 4);
    pos += 4;

    if (pos + 4 + 4 > archiveData.size()) return;
    totalFiles = *reinterpret_cast<const uint32_t*>(&archiveData[pos]);
    pos += 4;

    baseOff = *reinterpret_cast<const uint32_t*>(&archiveData[pos]);
    pos += 4;

    if (baseOff > archiveData.size()) {
        AppendLog(L"Error: baseOff is larger than file size.\r\n");
        return;
    }

    if (pos > baseOff) {
        AppendLog(L"Error: header position is beyond baseOff.\r\n");
        return;
    }

    std::vector<uint8_t> header(archiveData.begin() + pos, archiveData.begin() + baseOff);
    for (size_t i = 0; i < header.size(); i++)
        header[i] ^= key[i % 4];

    size_t hpos = 0;
    fileEntries.clear();

    AppendLog(L"File: " + filename + L"\r\n");
    AppendLog(L"Flags = " + std::to_wstring(flags) + L"\r\n");
    AppendLog(L"Files count = " + std::to_wstring(totalFiles) + L"\r\n");
    AppendLog(L"Base offset = " + std::to_wstring(baseOff) + L"\r\n");
    AppendLog(L"Header size = " + std::to_wstring(header.size()) + L"\r\n\r\n");

    for (uint32_t i = 0; i < totalFiles; i++) {
        if (hpos + 4 > header.size()) {
            AppendLog(L"Not enough data to read file entry at index " + std::to_wstring(i) + L"\r\n");
            break;
        }
        uint32_t namesz = *reinterpret_cast<const uint32_t*>(&header[hpos]);
        hpos += 4;

        if (hpos + namesz + 4 * 4 + 2 > header.size()) {
            AppendLog(L"Not enough data to read full file entry at index " + std::to_wstring(i) + L"\r\n");
            break;
        }

        std::string fname((char*)&header[hpos], namesz);
        hpos += namesz;

        uint32_t offset = *reinterpret_cast<const uint32_t*>(&header[hpos]);
        hpos += 4;
        uint32_t zsize = *reinterpret_cast<const uint32_t*>(&header[hpos]);
        hpos += 4;
        uint32_t usize = *reinterpret_cast<const uint32_t*>(&header[hpos]);
        hpos += 4;
        uint32_t crc_dummy = *reinterpret_cast<const uint32_t*>(&header[hpos]);
        hpos += 4;
        hpos += 2;

        offset += baseOff;

        fileEntries.push_back({ fname, offset, zsize, usize });
    }

    // Запрашиваем папку для распаковки
    outputFolder = BrowseFolderDialog(hMainWnd, L"Select folder to extract files to");
    if (outputFolder.empty()) {
        AppendLog(L"Extraction cancelled by user.\r\n");
        return;
    }

    CreateDirectoryW(outputFolder.c_str(), NULL);

    // Запускаем поток на распаковку
    StartExtraction();
}

// Таймер для обновления UI лога и прогресса
VOID CALLBACK TimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime) {
    UpdateUI();

    if (!extractionStarted && processedFiles == totalFiles && totalFiles != 0) {
        KillTimer(hwnd, idEvent);
        AppendLog(L"Extraction completed.\r\n");
        MessageBoxW(hwnd, L"Extraction finished successfully.", L"Done", MB_OK | MB_ICONINFORMATION);
    }
}

std::wstring OpenFileDialog(HWND hwnd) {
    wchar_t szFile[260] = { 0 };
    OPENFILENAMEW ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    ofn.lpstrFilter = L"SMF Files\0*.smf\0All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    return GetOpenFileNameW(&ofn) ? szFile : L"";
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        // Создаем кнопки, поле лога и прогресс бар
        hButtonOpen = CreateWindowW(L"BUTTON", L"Open .SMF File", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
            20, 20, 140, 40, hwnd, (HMENU)1, hInst, NULL);
        hButtonExtract = CreateWindowW(L"BUTTON", L"Extract to...", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON | WS_DISABLED,
            180, 20, 140, 40, hwnd, (HMENU)2, hInst, NULL);
        hLogEdit = CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            20, 70, 540, 300, hwnd, NULL, hInst, NULL);
        hProgress = CreateWindowW(PROGRESS_CLASS, NULL, WS_CHILD | WS_VISIBLE,
            20, 380, 540, 20, hwnd, NULL, hInst, NULL);
        SendMessage(hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessage(hProgress, PBM_SETPOS, 0, 0);
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == 1) {
            std::wstring file = OpenFileDialog(hwnd);
            if (!file.empty()) {
                selectedFile = file;
                SetWindowTextW(hLogEdit, L"");
                AppendLog(L"Selected file: " + selectedFile + L"\r\n");
                EnableWindow(hButtonExtract, TRUE);
            }
        }
        else if (LOWORD(wParam) == 2) {
            if (!selectedFile.empty()) {
                ExtractSMF(selectedFile);
                // Запускаем таймер для UI обновления каждые 100 мс
                SetTimer(hwnd, 1, 100, TimerProc);
            }
        }
        break;
    case WM_SIZE:
        if (hLogEdit && hProgress) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            MoveWindow(hLogEdit, 20, 70, rc.right - 40, rc.bottom - 110, TRUE);
            MoveWindow(hProgress, 20, rc.bottom - 30, rc.right - 40, 20, TRUE);
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}



int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    hInst = hInstance;

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icex);

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"SMFExtractorClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    hMainWnd = CreateWindowExW(0, L"SMFExtractorClass", L"ShiVa SMF Extractor",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 600, 460,
        NULL, NULL, hInstance, NULL);

    ShowWindow(hMainWnd, nCmdShow);
    UpdateWindow(hMainWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
