#define _CRT_SECURE_NO_WARNINGS // 允许在 MSVC 下使用 strcpy、wcscpy 等函数
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdbool.h>
#include <locale.h>  // 用于 _wsetlocale 设置区域
#include <io.h>      // 用于 _setmode 设置输出模式
#include <fcntl.h>   // 用于 _O_U8TEXT 设置 UTF-8 输出
#include <shlwapi.h> // 用于 StrFormatByteSizeW (可选，这里自定义实现) PathMatchSpecW (可选，这里自定义实现)

#pragma comment(lib, "Shlwapi.lib") // 如果使用 Shlwapi 函数

#define MAX_PATH_LEN FILENAME_MAX // 使用 FILENAME_MAX 作为路径缓冲区大小
#define ATTR_STR_LEN 12           // 用于存放属性字符串的最大长度 (例如 "[RHSDA L]")
#define SIZE_STR_LEN 20           // 用于存放文件大小字符串的最大长度

// --- 配置结构体与选项 ---

typedef struct
{
    wchar_t path_to_tree[MAX_PATH_LEN];    // 要遍历的起始路径
    int max_level;                         // -L：最大递归层数，-1 表示不限制
    bool list_files;                       // /F：是否显示文件
    bool use_ascii;                        // /A：是否使用 ASCII 字符画树状线
    bool show_hidden;                      // -a：是否显示隐藏文件和目录 (包括系统文件)
    bool no_report;                        // --noreport：是否抑制最后的统计报告
    bool show_size;                        // -s, --show-size：显示文件/目录大小
    bool use_si_units;                     // --si：文件大小使用 SI 单位 (1000基数) 而非 IEC (1024基数)
    bool show_attributes;                  // -p, --show-perms：显示文件/目录属性
    wchar_t include_pattern[MAX_PATH_LEN]; // --include：包含过滤器模式
    wchar_t exclude_pattern[MAX_PATH_LEN]; // --exclude：排除过滤器模式
    bool ignore_case_filter;               // --ignore-case：过滤器忽略大小写
    wchar_t output_filename[MAX_PATH_LEN]; // -o, --output：输出到指定文件
} TreeOptions;

typedef struct
{
    long long dir_count;  // 目录计数
    long long file_count; // 文件计数
} Counts;

// 目录项结构体，保存遍历到的文件或目录信息
typedef struct
{
    wchar_t name[MAX_PATH_LEN]; // 文件或目录名
    bool is_directory;          // 是否为目录
    DWORD attributes;           // 文件属性
    unsigned long long size;    // 文件大小 (字节) 或目录总大小 (字节)
} DirEntry;

// --- 树状线条字符定义 ---
const wchar_t *L_VERT = L"│";
const wchar_t *L_VERT_RIGHT = L"├";
const wchar_t *L_UP_RIGHT = L"└";
const wchar_t *L_HORZ = L"─";

const wchar_t *L_VERT_ASCII = L"|";
const wchar_t *L_VERT_RIGHT_ASCII = L"+";
const wchar_t *L_UP_RIGHT_ASCII = L"+";
const wchar_t *L_HORZ_ASCII = L"-";

// --- 帮助文本 ---
const wchar_t *help_text_zh_CN[] = {
    L"用法: ctree [路径] [选项]",
    L"图形化显示驱动器或路径的文件夹结构。",
    L"",
    L"选项:",
    L"  [路径]            指定要显示树状结构的目录。默认为当前目录。",
    L"  -L <层数>         限制目录树的显示层数。例如: -L 2",
    L"  /F, -f            显示每个文件夹中文件的名称。",
    L"  /A, --ascii       使用 ASCII 字符代替扩展字符绘制树状线条。",
    L"  -a                显示隐藏文件和目录 (包括系统文件)。",
    L"  -s, --show-size   显示文件和目录的大小。目录大小为其所含所有文件和子目录的总大小。默认使用二进制单位 (KiB, MiB)。",
    L"      --si          与 -s 一起使用时，以十进制单位 (KB, MB) 显示大小。",
    L"  -p, --show-perms  显示文件/目录的属性 (R=只读,H=隐藏,S=系统,A=存档,D=目录,L=链接,C=压缩,E=加密)。",
    L"  -o <文件>, --output <文件>  将树状目录输出到指定文件 (TXT格式, UTF-8编码)。",
    L"  --include <模式>  只显示名称匹配模式的文件/目录。支持 '*' 和 '?' 通配符。",
    L"                    例如: --include \"*.txt\"",
    L"  --exclude <模式>  排除名称匹配模式的文件/目录。优先于 --include。",
    L"                    例如: --exclude \"node_modules\"",
    L"  --ignore-case     使 --include 和 --exclude 的模式匹配忽略大小写。",
    L"  --noreport        禁止在末尾显示目录和文件的统计报告。",
    L"  -h, --help        显示此帮助信息。",
    L"  --lang=en         将帮助信息切换为英文。",
    L"",
    L"示例:",
    L"  ctree C:\\Users\\YourName\\Documents -L 3 /F -s",
    L"  ctree . -a --ascii --exclude \"*.tmp\" --include \"src/*\"",
    L"  ctree /F -p -s -o output.txt",
    NULL};

const wchar_t *help_text_en_US[] = {
    L"Usage: ctree [PATH] [OPTIONS]",
    L"Graphically displays the folder structure of a drive or path.",
    L"",
    L"Options:",
    L"  [PATH]            Specify the directory for which to display the tree.",
    L"                    Defaults to the current directory.",
    L"  -L <level>        Descend only <level> directories deep. Example: -L 2",
    L"  /F, -f            Display the names of the files in each folder.",
    L"  /A, --ascii       Use ASCII characters instead of extended characters for tree lines.",
    L"  -a                Show hidden files and directories (including system files).",
    L"  -s, --show-size   Display file and directory sizes. Directory size is the sum of all files and subdirectories it contains. Uses binary units (KiB, MiB) by default.",
    L"      --si          When used with -s, display sizes in SI units (KB, MB).",
    L"  -p, --show-perms  Show attributes of files/directories (R=Read-only,H=Hidden,S=System,A=Archive,D=Directory,L=Link,C=Compressed,E=Encrypted).",
    L"  -o <file>, --output <file>  Output the directory tree to the specified file (TXT format, UTF-8 encoded).",
    L"  --include <pattern> Include only files/directories matching the pattern. Supports '*' and '?' wildcards.",
    L"                      Example: --include \"*.txt\"",
    L"  --exclude <pattern> Exclude files/directories matching the pattern. Overrides --include.",
    L"                      Example: --exclude \"node_modules\"",
    L"  --ignore-case     Makes pattern matching for --include and --exclude case-insensitive.",
    L"  --noreport        Do not print the summary report of directories and files.",
    L"  -h, --help        Display this help message.",
    L"  --lang=en         Switch help message to English.",
    L"",
    L"Examples:",
    L"  ctree C:\\Users\\YourName\\Documents -L 3 /F -s",
    L"  ctree . -a --ascii --exclude \"*.tmp\" --include \"src/*\"",
    L"  ctree /F -p -s -o output.txt",
    NULL};

// --- 辅助函数 ---

// 简单的通配符匹配辅助函数 (char by char for '*' and '?')
bool pattern_matches_char(wchar_t text_char, wchar_t pattern_char, bool ignore_case)
{
    if (pattern_char == L'?')
        return true;
    if (ignore_case)
    {
        return towlower(text_char) == towlower(pattern_char);
    }
    return text_char == pattern_char;
}

// 简单的通配符匹配 (支持 * 和 ?)
bool wildcard_match(const wchar_t *text, const wchar_t *pattern, bool ignore_case)
{
    if (!pattern || pattern[0] == L'\0')
        return (!text || text[0] == L'\0');

    const wchar_t *tp = text;
    const wchar_t *pp = pattern;
    const wchar_t *star_p_text = NULL;
    const wchar_t *star_p_pattern = NULL;

    while (*tp)
    {
        if (*pp == L'*')
        {
            star_p_pattern = pp++;
            star_p_text = tp;
            if (!*pp)
                return true;
        }
        else if (pattern_matches_char(*tp, *pp, ignore_case))
        {
            tp++;
            pp++;
        }
        else
        {
            if (star_p_pattern)
            {
                pp = star_p_pattern + 1;
                tp = ++star_p_text;
            }
            else
            {
                return false;
            }
        }
    }

    while (*pp == L'*')
        pp++;
    return !*pp;
}

// 格式化文件大小 (二进制 IEC: KiB, MiB, GiB)
void format_file_size_binary(unsigned long long size_bytes, wchar_t *buffer, size_t buffer_len)
{
    const wchar_t *units[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    int i = 0;
    double size_display = (double)size_bytes;
    while (size_display >= 1024 && i < (sizeof(units) / sizeof(units[0]) - 1))
    {
        size_display /= 1024;
        i++;
    }
    swprintf(buffer, buffer_len, L"%.*f %s", (i == 0) ? 0 : 1, size_display, units[i]);
}

// 格式化文件大小 (十进制 SI: KB, MB, GB)
void format_file_size_si(unsigned long long size_bytes, wchar_t *buffer, size_t buffer_len)
{
    const wchar_t *units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    int i = 0;
    double size_display = (double)size_bytes;
    while (size_display >= 1000 && i < (sizeof(units) / sizeof(units[0]) - 1))
    {
        size_display /= 1000;
        i++;
    }
    swprintf(buffer, buffer_len, L"%.*f %s", (i == 0) ? 0 : 1, size_display, units[i]);
}

// 获取文件/目录属性字符串
void get_attribute_string(DWORD dwFileAttributes, bool is_dir, wchar_t *attr_str, size_t attr_str_len)
{
    wchar_t temp_str[ATTR_STR_LEN] = L"";
    size_t current_len = 0;

    if (attr_str_len < 2)
    {
        if (attr_str_len > 0)
            attr_str[0] = L'\0';
        return;
    }

    wcscpy(temp_str, L"[");
    current_len = 1;

    if (is_dir && (dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        if (current_len < attr_str_len - 1)
            temp_str[current_len++] = L'D';
    }
    if (dwFileAttributes & FILE_ATTRIBUTE_READONLY)
    {
        if (current_len < attr_str_len - 1)
            temp_str[current_len++] = L'R';
    }
    if (dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)
    {
        if (current_len < attr_str_len - 1)
            temp_str[current_len++] = L'H';
    }
    if (dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)
    {
        if (current_len < attr_str_len - 1)
            temp_str[current_len++] = L'S';
    }
    if (dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE)
    {
        if (current_len < attr_str_len - 1)
            temp_str[current_len++] = L'A';
    }
    if (dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
    {
        if (current_len < attr_str_len - 1)
            temp_str[current_len++] = L'L';
    }
    if (dwFileAttributes & FILE_ATTRIBUTE_COMPRESSED)
    {
        if (current_len < attr_str_len - 1)
            temp_str[current_len++] = L'C';
    }
    if (dwFileAttributes & FILE_ATTRIBUTE_ENCRYPTED)
    {
        if (current_len < attr_str_len - 1)
            temp_str[current_len++] = L'E';
    }
    // ... 可以添加更多属性 ...

    if (current_len > 1)
    {
        if (current_len < attr_str_len - 1)
        {
            temp_str[current_len++] = L']';
            temp_str[current_len] = L'\0';
            wcscpy(attr_str, temp_str);
        }
        else
        {
            temp_str[attr_str_len - 2] = L']';
            temp_str[attr_str_len - 1] = L'\0';
            wcscpy(attr_str, temp_str);
        }
    }
    else
    {
        attr_str[0] = L'\0';
    }
}

void display_help(bool prefer_chinese)
{
    const wchar_t **help_text = prefer_chinese ? help_text_zh_CN : help_text_en_US;
#if defined(_WIN32) || defined(_WIN64)
    // 如果输出不是控制台 (例如重定向到文件了)，GetConsoleOutputCP 可能行为不确定或失败。
    // 但这里主要目的是为了在控制台正确显示帮助信息。
    UINT old_cp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    // _setmode 对于 stdout，如果 stdout 已被重定向到文件，则此设置可能不影响文件编码，
    // 文件编码由 _wfreopen 的 ccs 标志决定。
    // 但如果帮助信息强制输出到控制台的 stderr，则 _setmode(_fileno(stderr), _O_U8TEXT) 会更好。
    // 为简单起见，假设帮助信息总是通过当前的 stdout (可能是控制台或文件)。
    _setmode(_fileno(stdout), _O_U8TEXT); // 确保帮助文本在控制台或UTF-8文件中正确显示
#endif

    for (int i = 0; help_text[i] != NULL; ++i)
    {
        wprintf(L"%s\n", help_text[i]);
    }
#if defined(_WIN32) || defined(_WIN64)
    SetConsoleOutputCP(old_cp); // 恢复原始控制台代码页
#endif
}

// --- 核心树遍历与打印逻辑 ---

int compare_dir_entries(const void *a, const void *b)
{
    const DirEntry *entry_a = (const DirEntry *)a;
    const DirEntry *entry_b = (const DirEntry *)b;

    if (entry_a->is_directory && !entry_b->is_directory)
        return -1;
    if (!entry_a->is_directory && entry_b->is_directory)
        return 1;
    return _wcsicmp(entry_a->name, entry_b->name);
}

// 修改: 函数返回当前处理目录的总大小
unsigned long long print_tree_recursive(const wchar_t *current_path, int level, TreeOptions *opts, Counts *counts, wchar_t *prefix_str)
{
    if (opts->max_level != -1 && level > opts->max_level)
    {
        return 0; // 超过最大层数，此目录大小贡献为0，也不打印或处理
    }

    WIN32_FIND_DATAW find_data;
    HANDLE h_find = INVALID_HANDLE_VALUE;
    wchar_t search_path[MAX_PATH_LEN];
    unsigned long long current_directory_total_size = 0; // 用于累加当前目录的总大小

    wcscpy(search_path, current_path);
    if (search_path[wcslen(search_path) - 1] != L'\\' && search_path[wcslen(search_path) - 1] != L'/')
    {
        wcscat(search_path, L"\\");
    }
    wcscat(search_path, L"*");

    h_find = FindFirstFileW(search_path, &find_data);

    if (h_find == INVALID_HANDLE_VALUE)
    {
        if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
        {
            // 错误信息输出到 stderr，以避免干扰重定向到文件的 stdout
            fwprintf(stderr, L"警告: 无法访问路径 '%s' (错误代码: %lu)。跳过。\n", current_path, GetLastError());
        }
        return 0; // 无法访问或目录为空，大小为0
    }

    DirEntry *entries = NULL;
    size_t entry_count = 0;
    size_t capacity = 0;

    // 第一遍：收集所有符合条件的条目
    do
    {
        if (wcscmp(find_data.cFileName, L".") == 0 || wcscmp(find_data.cFileName, L"..") == 0)
        {
            continue;
        }

        bool is_dir = (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

        if (!opts->show_hidden && (find_data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN))
        {
            continue;
        }
        if (!opts->show_hidden && (find_data.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM))
        {
            continue;
        }

        if (opts->exclude_pattern[0] != L'\0')
        {
            if (wildcard_match(find_data.cFileName, opts->exclude_pattern, opts->ignore_case_filter))
            {
                continue;
            }
        }
        if (opts->include_pattern[0] != L'\0')
        {
            if (!wildcard_match(find_data.cFileName, opts->include_pattern, opts->ignore_case_filter))
            {
                continue;
            }
        }

        if (entry_count >= capacity)
        {
            capacity = (capacity == 0) ? 16 : capacity * 2;
            DirEntry *new_entries = (DirEntry *)realloc(entries, capacity * sizeof(DirEntry));
            if (!new_entries)
            {
                fwprintf(stderr, L"内存分配失败。\n"); // 输出到 stderr
                if (entries)
                    free(entries); // 释放已分配的内存
                FindClose(h_find);
                return current_directory_total_size;
            }
            entries = new_entries;
        }

        wcscpy(entries[entry_count].name, find_data.cFileName);
        entries[entry_count].is_directory = is_dir;
        entries[entry_count].attributes = find_data.dwFileAttributes;
        if (is_dir)
        {
            entries[entry_count].size = 0;
        }
        else
        {
            entries[entry_count].size = ((unsigned long long)find_data.nFileSizeHigh << 32) + find_data.nFileSizeLow;
        }
        entry_count++;

    } while (FindNextFileW(h_find, &find_data) != 0);

    DWORD dwError = GetLastError();
    FindClose(h_find); // 确保句柄总是被关闭

    if (dwError != ERROR_NO_MORE_FILES)
    {
        // fwprintf(stderr, L"读取目录 '%s' 时发生错误 (代码: %lu)。\n", current_path, dwError);
    }

    if (entry_count > 0)
    {
        qsort(entries, entry_count, sizeof(DirEntry), compare_dir_entries);
    }

    // 第二遍：处理条目（递归计算目录大小、累加大小、打印）
    for (size_t i = 0; i < entry_count; ++i)
    {
        bool is_last_item_in_level = (i == entry_count - 1);

        if (entries[i].is_directory)
        {
            counts->dir_count++;

            wchar_t child_path[MAX_PATH_LEN];
            wcscpy(child_path, current_path);
            if (child_path[wcslen(child_path) - 1] != L'\\' && child_path[wcslen(child_path) - 1] != L'/')
            {
                wcscat(child_path, L"\\");
            }
            wcscat(child_path, entries[i].name);

            wchar_t next_prefix_str[MAX_PATH_LEN * 2];
            wcscpy(next_prefix_str, prefix_str);
            const wchar_t *v_line_segment = opts->use_ascii ? L_VERT_ASCII : L_VERT;
            wcscat(next_prefix_str, is_last_item_in_level ? L"    " : v_line_segment);
            wcscat(next_prefix_str, L"   ");

            unsigned long long subdir_total_size = 0;
            subdir_total_size = print_tree_recursive(child_path, level + 1, opts, counts, next_prefix_str);

            entries[i].size = subdir_total_size;
            current_directory_total_size += subdir_total_size;
        }
        else
        {
            counts->file_count++;
            current_directory_total_size += entries[i].size;
        }

        bool should_print_this_entry = true;
        if (!entries[i].is_directory && !opts->list_files)
        {
            should_print_this_entry = false;
        }

        if (should_print_this_entry)
        {
            // 所有打印操作都通过 wprintf 进行，它会根据 stdout 的状态（控制台或文件）进行输出
            wprintf(L"%s", prefix_str);

            const wchar_t *connector = is_last_item_in_level ? (opts->use_ascii ? L_UP_RIGHT_ASCII : L_UP_RIGHT)
                                                             : (opts->use_ascii ? L_VERT_RIGHT_ASCII : L_VERT_RIGHT);
            const wchar_t *h_line = opts->use_ascii ? L_HORZ_ASCII : L_HORZ;
            wprintf(L"%s%s%s ", connector, h_line, h_line);

            if (opts->show_attributes)
            {
                wchar_t attr_str[ATTR_STR_LEN];
                get_attribute_string(entries[i].attributes, entries[i].is_directory, attr_str, ATTR_STR_LEN);
                if (attr_str[0] != L'\0')
                {
                    wprintf(L"%-*s ", ATTR_STR_LEN - 1, attr_str);
                }
                else
                {
                    wprintf(L"%-*s ", ATTR_STR_LEN - 1, L"");
                }
            }

            if (opts->show_size)
            {
                wchar_t size_str[SIZE_STR_LEN];
                if (opts->use_si_units)
                {
                    format_file_size_si(entries[i].size, size_str, SIZE_STR_LEN);
                }
                else
                {
                    format_file_size_binary(entries[i].size, size_str, SIZE_STR_LEN);
                }
                wprintf(L"[%*s] ", SIZE_STR_LEN - 3, size_str);
            }

            wprintf(L"%s\n", entries[i].name);
        }
    }

    // 内存管理: 释放为当前目录条目分配的内存。
    // 对于由 Ctrl+C 等信号导致的程序终止，操作系统会负责回收所有进程内存。
    // 此处的 free 确保在函数正常返回时内存得到释放。
    if (entries)
    {
        free(entries);
    }
    return current_directory_total_size;
}

// --- 主函数与参数解析 ---
int wmain(int argc, wchar_t *argv[])
{
    // 默认情况下，为 stderr 设置UTF-8输出模式，以保证错误信息在控制台正确显示。
    // stdout 的模式将在确定其是否为文件或控制台后设置。
#if defined(_WIN32) || defined(_WIN64)
    _setmode(_fileno(stderr), _O_U8TEXT);
#endif

    TreeOptions options;
    wcscpy(options.path_to_tree, L".");
    options.max_level = -1;
    options.list_files = false;
    options.use_ascii = false;
    options.show_hidden = false;
    options.no_report = false;
    options.show_size = false;
    options.use_si_units = false;
    options.show_attributes = false;
    options.include_pattern[0] = L'\0';
    options.exclude_pattern[0] = L'\0';
    options.ignore_case_filter = false;
    options.output_filename[0] = L'\0'; // 初始化输出文件名为空

    bool help_requested = false;
    bool chinese_lang_preferred = true;

    for (int i = 1; i < argc; ++i)
    {
        if (wcscmp(argv[i], L"-L") == 0)
        {
            if (i + 1 < argc)
            {
                options.max_level = _wtoi(argv[++i]);
                if (options.max_level < 0)
                    options.max_level = 0;
            }
            else
            {
                fwprintf(stderr, L"错误: -L 参数需要一个层数值。\nError: -L option requires a level value.\n");
                return 1;
            }
        }
        else if (wcscmp(argv[i], L"/F") == 0 || wcscmp(argv[i], L"-f") == 0)
        {
            options.list_files = true;
        }
        else if (wcscmp(argv[i], L"/A") == 0 || wcscmp(argv[i], L"--ascii") == 0)
        {
            options.use_ascii = true;
        }
        else if (wcscmp(argv[i], L"-a") == 0)
        {
            options.show_hidden = true;
        }
        else if (wcscmp(argv[i], L"--noreport") == 0)
        {
            options.no_report = true;
        }
        else if (wcscmp(argv[i], L"-s") == 0 || wcscmp(argv[i], L"--show-size") == 0)
        {
            options.show_size = true;
        }
        else if (wcscmp(argv[i], L"--si") == 0)
        {
            options.use_si_units = true;
        }
        else if (wcscmp(argv[i], L"-p") == 0 || wcscmp(argv[i], L"--show-perms") == 0)
        {
            options.show_attributes = true;
        }
        else if (wcscmp(argv[i], L"-o") == 0 || wcscmp(argv[i], L"--output") == 0) // 新增输出文件选项
        {
            if (i + 1 < argc)
            {
                wcsncpy(options.output_filename, argv[++i], MAX_PATH_LEN - 1);
                options.output_filename[MAX_PATH_LEN - 1] = L'\0';
            }
            else
            {
                fwprintf(stderr, L"错误: %s 参数需要一个文件名。\nError: %s option requires a filename.\n", argv[i], argv[i]);
                return 1;
            }
        }
        else if (wcscmp(argv[i], L"--include") == 0)
        {
            if (i + 1 < argc)
            {
                wcsncpy(options.include_pattern, argv[++i], MAX_PATH_LEN - 1);
                options.include_pattern[MAX_PATH_LEN - 1] = L'\0';
            }
            else
            {
                fwprintf(stderr, L"错误: --include 参数需要一个模式字符串。\nError: --include option requires a pattern string.\n");
                return 1;
            }
        }
        else if (wcscmp(argv[i], L"--exclude") == 0)
        {
            if (i + 1 < argc)
            {
                wcsncpy(options.exclude_pattern, argv[++i], MAX_PATH_LEN - 1);
                options.exclude_pattern[MAX_PATH_LEN - 1] = L'\0';
            }
            else
            {
                fwprintf(stderr, L"错误: --exclude 参数需要一个模式字符串。\nError: --exclude option requires a pattern string.\n");
                return 1;
            }
        }
        else if (wcscmp(argv[i], L"--ignore-case") == 0)
        {
            options.ignore_case_filter = true;
        }
        else if (wcscmp(argv[i], L"-h") == 0 || wcscmp(argv[i], L"--help") == 0)
        {
            help_requested = true;
        }
        else if (wcscmp(argv[i], L"--lang=en") == 0)
        {
            chinese_lang_preferred = false;
        }
        else if (argv[i][0] == L'-' || argv[i][0] == L'/')
        {
            fwprintf(stderr, L"错误: 未知选项 '%s'\nError: Unknown option '%s'\n", argv[i], argv[i]);
            // display_help 在这里调用前，stdout 可能还未被重定向或设置模式
            // 但由于错误信息已打印到 stderr，这里可以考虑不显示完整帮助，或确保 display_help 使用 stderr
            // 为简单起见，如果发生未知选项错误，我们仍然尝试显示帮助（它内部会尝试设置stdout模式）
            display_help(chinese_lang_preferred);
            return 1;
        }
        else
        {
            wcsncpy(options.path_to_tree, argv[i], MAX_PATH_LEN - 1);
            options.path_to_tree[MAX_PATH_LEN - 1] = L'\0';
        }
    }

    // 处理输出重定向
    FILE *output_file_stream = NULL;
    if (options.output_filename[0] != L'\0')
    {
        // 使用 "w, ccs=UTF-8" 来确保文件以 UTF-8 编码写入，这对于宽字符输出很重要。
        // _wfopen_s 是更安全的选择，但 _wfreopen 更直接用于重定向 stdout。
        output_file_stream = _wfreopen(options.output_filename, L"w, ccs=UTF-8", stdout);
        if (output_file_stream == NULL)
        {
            fwprintf(stderr, L"错误: 无法打开或创建输出文件 '%s'。\nError: Could not open or create output file '%s'.\n", options.output_filename, options.output_filename);
            return 1;
        }
        // stdout 现在指向文件。不需要显式 fclose(output_file_stream)，因为 stdout 会在程序结束时自动关闭。
    }
    else
    {
        // 如果不输出到文件，确保控制台输出为 UTF-8
#if defined(_WIN32) || defined(_WIN64)
        _setmode(_fileno(stdout), _O_U8TEXT);
#endif
    }

    if (help_requested)
    {
        // display_help 现在会使用（可能已重定向的）stdout
        display_help(chinese_lang_preferred);
        // 如果 stdout 被重定向到文件，帮助信息也会写入文件。
        // 如果希望帮助信息总是显示在控制台，display_help 应明确使用 stderr，
        // 或者在调用 display_help 前临时恢复 stdout (复杂)。
        // 当前行为：帮助信息遵循 -o 参数。
        return 0;
    }

    if (options.use_si_units && !options.show_size)
    {
        options.show_size = true;
    }

    wchar_t full_root_path[MAX_PATH_LEN];
    if (_wfullpath(full_root_path, options.path_to_tree, MAX_PATH_LEN) == NULL)
    {
        fwprintf(stderr, L"错误: 无法解析路径 '%s'。\nError: Could not resolve path '%s'.\n", options.path_to_tree, options.path_to_tree);
        return 1;
    }
    wcscpy(options.path_to_tree, full_root_path);

    DWORD dwAttrib = GetFileAttributesW(options.path_to_tree);
    if (dwAttrib == INVALID_FILE_ATTRIBUTES || !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY))
    {
        fwprintf(stderr, L"错误: 路径 '%s' 无效或不是一个目录。\nError: Path '%s' is invalid or not a directory.\n", options.path_to_tree, options.path_to_tree);
        return 1;
    }

    Counts counts = {0, 0};
    wprintf(L"%s\n", options.path_to_tree); // 打印根目录名

    wchar_t initial_prefix[MAX_PATH_LEN * 2] = L"";
    print_tree_recursive(options.path_to_tree, 1, &options, &counts, initial_prefix);

    if (!options.no_report)
    {
        wprintf(L"\n%lld 个目录", counts.dir_count);
        wprintf(L", %lld 个文件", counts.file_count);
        wprintf(L"\n");
    }

    // 如果 stdout 被重定向到文件 (output_file_stream != NULL),
    // 它将在程序退出时自动关闭和刷新。
    // 如果之前使用了 _wfopen_s 并自行管理 FILE* 指针，则需要 fclose。
    // 但由于使用了 _wfreopen，stdout 的管理由 CRT 负责。

    return 0;
}
