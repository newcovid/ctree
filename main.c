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
#define ESCAPE_BUF_MULT 2         // JSON/XML 名称转义缓冲区大小的乘数

// --- 输出格式枚举 ---
typedef enum
{
    OUTPUT_TEXT,
    OUTPUT_JSON,
    OUTPUT_XML
} OutputFormat;

// --- 配置结构体与选项 ---
typedef struct
{
    wchar_t path_to_tree[MAX_PATH_LEN];    // 要遍历的起始路径
    int max_level;                         // -L：最大递归层数，-1 表示不限制
    bool list_files;                       // /F：是否显示文件 (主要用于文本模式)
    bool use_ascii;                        // /A：是否使用 ASCII 字符画树状线 (文本模式)
    bool show_hidden;                      // -a：是否显示隐藏文件和目录 (包括系统文件)
    bool no_report;                        // --noreport：是否抑制最后的统计报告
    bool show_size;                        // -s, --show-size：显示文件/目录大小
    bool use_si_units;                     // --si：文件大小使用 SI 单位 (1000基数) 而非 IEC (1024基数)
    bool show_attributes;                  // -p, --show-perms：显示文件/目录属性
    wchar_t include_pattern[MAX_PATH_LEN]; // --include：包含过滤器模式
    wchar_t exclude_pattern[MAX_PATH_LEN]; // --exclude：排除过滤器模式
    bool ignore_case_filter;               // --ignore-case：过滤器忽略大小写
    wchar_t output_filename[MAX_PATH_LEN]; // -o, --output：输出到指定文件

    // 新选项
    bool use_color;             // -c: 为输出着色 (文本模式，仅控制台)
    OutputFormat output_format; // 输出格式

    // 用于颜色处理
    WORD saved_console_attributes; // 保存的控制台属性
    bool output_is_console;        // 输出是否为控制台
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

// --- 树状线条字符定义 (文本模式) ---
// FIX: 使用 Unicode 代码点以避免源文件编码问题
const wchar_t *L_VERT = L"\x2502";        // │
const wchar_t *L_VERT_RIGHT = L"\x251C";  // ├
const wchar_t *L_UP_RIGHT = L"\x2514";    // └
const wchar_t *L_HORZ = L"\x2500";        // ─

const wchar_t *L_VERT_ASCII = L"|";
const wchar_t *L_VERT_RIGHT_ASCII = L"+";
const wchar_t *L_UP_RIGHT_ASCII = L"+";
const wchar_t *L_HORZ_ASCII = L"-";

// --- 控制台颜色定义 (Windows 特定) ---
#define COLOR_DEFAULT_FG (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)  // 默认前景色
#define COLOR_DIR (FOREGROUND_BLUE | FOREGROUND_INTENSITY)                      // 亮蓝色 (目录)
#define COLOR_FILE COLOR_DEFAULT_FG                                             // 默认颜色 (通常为白色，文件)
#define COLOR_HIDDEN (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY) // 亮黄色 (隐藏)
#define COLOR_SYSTEM (FOREGROUND_RED | FOREGROUND_INTENSITY)                    // 亮红色 (系统)
#define COLOR_LINK (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)  // 亮青色 (链接)
#define COLOR_EXECUTABLE (FOREGROUND_GREEN | FOREGROUND_INTENSITY)              // 亮绿色 (可执行文件)
#define COLOR_ARCHIVE (FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY) // 亮品红色 (存档文件)
#define COLOR_IMAGE (FOREGROUND_RED | FOREGROUND_GREEN)                         // 暗黄色 (图片)

// 开始：添加新的颜色定义和 FOREGROUND_BLACK
#define FOREGROUND_BLACK 0                                   // 定义 FOREGROUND_BLACK 为 0
#define COLOR_DOCUMENT (FOREGROUND_GREEN | FOREGROUND_BLUE)  // 青色 (暗青色，文档)
#define COLOR_AUDIO (FOREGROUND_GREEN)                       // 暗绿色 (音频)
#define COLOR_VIDEO (FOREGROUND_BLUE)                        // 暗蓝色 (视频)
#define COLOR_SOURCECODE (FOREGROUND_RED)                    // 暗红色 (源代码)
#define COLOR_TEMP (FOREGROUND_BLACK | FOREGROUND_INTENSITY) // 灰色 (通过黑色 + 强度实现，临时文件)
// 结束：添加新的颜色定义和 FOREGROUND_BLACK

// --- 帮助文本 ---
const wchar_t *help_text_zh_CN[] = {
    L"\x7528\x6CD5: ctree [\x8DEF\x5F84] [\x9009\x9879]",
    L"\x56FE\x5F62\x5316\x663E\x793A\x9A71\x52A8\x5668\x6216\x8DEF\x5F84\x7684\x6587\x4EF6\x5939\x7ED3\x6784\x3002",
    L"",
    L"\x9009\x9879:",
    L"  [\x8DEF\x5F84]            \x6307\x5B9A\x8981\x663E\x793A\x6811\x72B6\x7ED3\x6784\x7684\x76EE\x5F55\x3002\x9ED8\x8BA4\x4E3A\x5F53\x524D\x76EE\x5F55\x3002",
    L"  -L <\x5C42\x6570>         \x9650\x5236\x76EE\x5F55\x6811\x7684\x663E\x793A\x5C42\x6570\x3002\x4F8B\x5982: -L 2",
    L"  /F, -f            \x663E\x793A\x6BCF\x4E2A\x6587\x4EF6\x5939\x4E2D\x6587\x4EF6\x7684\x540D\x79F0 (\x9002\x7528\x4E8E\x6240\x6709\x8F93\x51FA\x6A21\x5F0F)\x3002",
    L"  /A, --ascii       (\x6587\x672C\x6A21\x5F0F) \x4F7F\x7528 ASCII \x5B57\x7B26\x4EE3\x66FF\x6269\x5C55\x5B57\x7B26\x7ED8\x5236\x6811\x72B6\x7EBF\x6761\x3002",
    L"  -c                (\x6587\x672C\x6A21\x5F0F) \x4E3A\x8F93\x51FA\x7740\x8272\x4EE5\x533A\x5206\x4E0D\x540C\x7C7B\x578B\x7684\x6587\x4EF6/\x76EE\x5F55 (\x4EC5\x63A7\x5236\x53F0)\x3002",
    L"  -a                \x663E\x793A\x9690\x85CF\x6587\x4EF6\x548C\x76EE\x5F55 (\x5305\x62EC\x7CFB\x7EDF\x6587\x4EF6)\x3002",
    L"  -s, --show-size   \x663E\x793A\x6587\x4EF6\x548C\x76EE\x5F55\x7684\x5927\x5C0F\x3002\x76EE\x5F55\x5927\x5C0F\x4E3A\x5176\x6240\x542B\x6240\x6709\x6587\x4EF6\x548C\x5B50\x76EE\x5F55\x7684\x603B\x5927\x5C0F\x3002",
    L"                    \x9ED8\x8BA4\x4F7F\x7528\x4E8C\x8FDB\x5236\x5355\x4F4D (KiB, MiB)\x3002",
    L"      --si          \x4E0E -s \x4E00\x8D77\x4F7F\x7528\x65F6\x_FF0C\x4EE5\x5341\x8FDB\x5236\x5355\x4F4D (KB, MB) \x663E\x793A\x5927\x5C0F\x3002",
    L"  -p, --show-perms  \x663E\x793A\x6587\x4EF6/\x76EE\x5F55\x7684\x5C5E\x6027 (R=\x53EA\x8BFB,H=\x9690\x85CF,S=\x7CFB\x7EDF,A=\x5B58\x6863,D=\x76EE\x5F55,L=\x94FE\x63A5,C=\x538B\x7F29,E=\x52A0\x5BC6)\x3002",
    L"  -o <\x6587\x4EF6>, --output <\x6587\x4EF6>  \x5C06\x6811\x72B6\x76EE\x5F55\x8F93\x51FA\x5230\x6307\x5B9A\x6587\x4EF6 (UTF-8\x7F16\x7801)\x3002\x6B64\x9009\x9879\x9002\x7528\x4E8E\x6240\x6709\x8F93\x51FA\x683C\x5F0F(\x6587\x672C, JSON, XML)\x3002",
    L"  -J                \x4EE5 JSON \x683C\x5F0F\x8F93\x51FA\x3002\x5982\x679C\x672A\x6307\x5B9A -o\x_FF0C\x5219\x8F93\x51FA\x5230\x63A7\x5236\x53F0\x3002",
    L"  -X                \x4EE5 XML \x683C\x5F0F\x8F93\x51FA\x3002\x5982\x679C\x672A\x6307\x5B9A -o\x_FF0C\x5219\x8F93\x51FA\x5230\x63A7\x5236\x53F0\x3002",
    L"  --include <\x6A21\x5F0F>  \x53EA\x663E\x793A\x540D\x79F0\x5339\x914D\x6A21\x5F0F\x7684\x6587\x4EF6/\x76EE\x5F55\x3002\x652F\x6301 '*' \x548C '?' \x901A\x914D\x7B26\x3002",
    L"                    \x4F8B\x5982: --include \"*.txt\"",
    L"  --exclude <\x6A21\x5F0F>  \x6392\x9664\x540D\x79F0\x5339\x914D\x6A21\x5F0F\x7684\x6587\x4EF6/\x76EE\x5F55\x3002\x4F18\x5148\x4E8E --include\x3002",
    L"                    \x4F8B\x5982: --exclude \"node_modules\"",
    L"  --ignore-case     \x4F7F --include \x548C --exclude \x7684\x6A21\x5F0F\x5339\x914D\x5FFD\x7565\x5927\x5C0F\x5199\x3002",
    L"  --noreport        \x7981\x6B62\x5728\x672B\x5C3E\x663E\x793A\x76EE\x5F55\x548C\x6587\x4EF6\x7684\x7EDF\x8BA1\x62A5\x544A\x3002",
    L"  -h, --help        \x663E\x793A\x6B64\x5E2E\x52A9\x4FE1\x606F\x3002",
    L"  --lang=en         \x5C06\x5E2E\x52A9\x4FE1\x606F\x5207\x6362\x4E3A\x82F1\x6587\x3002",
    L"",
    L"\x793A\x4F8B:",
    L"  ctree C:\\Users\\YourName\\Documents -L 3 /F -s -c",
    L"  ctree . -a --ascii --exclude \"*.tmp\" --include \"src/*\"",
    L"  ctree /F -p -s -o output.txt",
    L"  ctree . -J -o tree.json",
    L"  ctree C:\\Windows -X -L 2 -s --output tree.xml",
    NULL};

const wchar_t *help_text_en_US[] = { // 英文帮助文本
    L"Usage: ctree [PATH] [OPTIONS]",
    L"Graphically displays the folder structure of a drive or path.",
    L"",
    L"Options:",
    L"  [PATH]            Specify the directory for which to display the tree.",
    L"                    Defaults to the current directory.",
    L"  -L <level>        Descend only <level> directories deep. Example: -L 2",
    L"  /F, -f            Display the names of the files in each folder (applies to all output modes).", // Modified description
    L"  /A, --ascii       (Text mode) Use ASCII characters instead of extended characters for tree lines.",
    L"  -c                (Text mode) Colorize the output for different file/directory types (console only).",
    L"  -a                Show hidden files and directories (including system files).",
    L"  -s, --show-size   Display file and directory sizes. Directory size is the sum of all files and subdirectories it contains. Uses binary units (KiB, MiB) by default.",
    L"      --si          When used with -s, display sizes in SI units (KB, MB).",
    L"  -p, --show-perms  Show attributes of files/directories (R=Read-only,H=Hidden,S=System,A=Archive,D=Directory,L=Link,C=Compressed,E=Encrypted).",
    L"  -o <file>, --output <file>  Output the directory tree to the specified file (UTF-8 encoded). This option applies to all output formats (Text, JSON, XML).",
    L"  -J                Output in JSON format. If -o is not specified, output is to console.",
    L"  -X                Output in XML format. If -o is not specified, output is to console.",
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
    L"  ctree C:\\Users\\YourName\\Documents -L 3 /F -s -c",
    L"  ctree . -a --ascii --exclude \"*.tmp\" --include \"src/*\"",
    L"  ctree /F -p -s -o output.txt",
    L"  ctree . -J -o tree.json",
    L"  ctree C:\\Windows -X -L 2 -s --output tree.xml",
    NULL};

// --- 辅助函数 ---

// 简单的通配符匹配辅助函数 (逐字符匹配 '*' 和 '?')
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
            if (!*pp) // 如果 '*' 是模式的最后一个字符
                return true;
        }
        else if (pattern_matches_char(*tp, *pp, ignore_case))
        {
            tp++;
            pp++;
        }
        else
        {
            if (star_p_pattern) // 如果之前遇到过 '*'
            {
                pp = star_p_pattern + 1; // 模式指针回到 '*' 之后
                tp = ++star_p_text;      // 文本指针向前移动一个字符
            }
            else
            {
                return false; // 不匹配且没有 '*' 可以回溯
            }
        }
    }

    while (*pp == L'*') // 跳过模式末尾的 '*'
        pp++;
    return !*pp; // 如果模式也到达末尾，则匹配成功
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

    if (attr_str_len < 2) // 至少需要空间存放 "[]" 或空字符串
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
    if (dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) // 符号链接、连接点
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

    if (current_len > 1) // 如果添加了除 '[' 之外的任何属性
    {
        if (current_len < attr_str_len - 1) // 确保有空间放 ']'
        {
            temp_str[current_len++] = L']';
            temp_str[current_len] = L'\0';
            wcscpy(attr_str, temp_str);
        }
        else // 空间不足以放 ']'，覆盖最后一个字符
        {
            temp_str[attr_str_len - 2] = L']';
            temp_str[attr_str_len - 1] = L'\0';
            wcscpy(attr_str, temp_str);
        }
    }
    else // 没有属性，仅 "[]"
    {
        attr_str[0] = L'\0'; // 或者如果为了间距更喜欢 "[ ]"
    }
}

// 获取文件扩展名
const wchar_t *get_file_extension(const wchar_t *filename)
{
    const wchar_t *dot = wcsrchr(filename, L'.');
    if (!dot || dot == filename) // 没有扩展名或以点开头
        return L"";
    return dot + 1;
}

// 开始：更新的 get_color_for_entry 函数
WORD get_color_for_entry(const DirEntry *entry, const TreeOptions *opts)
{
    if (!opts->use_color || !opts->output_is_console)
        return opts->saved_console_attributes; // 如果不使用颜色，则返回默认值

    if (entry->is_directory)
        return COLOR_DIR;

    // 基于属性的颜色优先
    if (entry->attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return COLOR_LINK;
    if (entry->attributes & FILE_ATTRIBUTE_SYSTEM)
        return COLOR_SYSTEM;
    if (entry->attributes & FILE_ATTRIBUTE_HIDDEN)
        return COLOR_HIDDEN;

    const wchar_t *ext = get_file_extension(entry->name);
    if (ext[0] != L'\0') // 仅当存在扩展名时才检查扩展名
    {
        // 可执行文件
        if (_wcsicmp(ext, L"exe") == 0 || _wcsicmp(ext, L"com") == 0 || _wcsicmp(ext, L"bat") == 0 || _wcsicmp(ext, L"cmd") == 0 || _wcsicmp(ext, L"msi") == 0 || _wcsicmp(ext, L"ps1") == 0 || _wcsicmp(ext, L"vbs") == 0)
        {
            return COLOR_EXECUTABLE;
        }
        // 存档文件
        if (_wcsicmp(ext, L"zip") == 0 || _wcsicmp(ext, L"rar") == 0 || _wcsicmp(ext, L"7z") == 0 || _wcsicmp(ext, L"tar") == 0 || _wcsicmp(ext, L"gz") == 0 || _wcsicmp(ext, L"bz2") == 0 || _wcsicmp(ext, L"xz") == 0 || _wcsicmp(ext, L"iso") == 0 || _wcsicmp(ext, L"img") == 0)
        {
            return COLOR_ARCHIVE;
        }
        // 图片
        if (_wcsicmp(ext, L"jpg") == 0 || _wcsicmp(ext, L"jpeg") == 0 || _wcsicmp(ext, L"png") == 0 || _wcsicmp(ext, L"gif") == 0 || _wcsicmp(ext, L"bmp") == 0 || _wcsicmp(ext, L"svg") == 0 || _wcsicmp(ext, L"webp") == 0 || _wcsicmp(ext, L"ico") == 0 || _wcsicmp(ext, L"tif") == 0 || _wcsicmp(ext, L"tiff") == 0)
        {
            return COLOR_IMAGE;
        }
        // 音频
        if (_wcsicmp(ext, L"mp3") == 0 || _wcsicmp(ext, L"wav") == 0 || _wcsicmp(ext, L"ogg") == 0 || _wcsicmp(ext, L"flac") == 0 || _wcsicmp(ext, L"aac") == 0 || _wcsicmp(ext, L"m4a") == 0 || _wcsicmp(ext, L"wma") == 0)
        {
            return COLOR_AUDIO;
        }
        // 视频
        if (_wcsicmp(ext, L"mp4") == 0 || _wcsicmp(ext, L"mkv") == 0 || _wcsicmp(ext, L"avi") == 0 || _wcsicmp(ext, L"mov") == 0 || _wcsicmp(ext, L"wmv") == 0 || _wcsicmp(ext, L"flv") == 0 || _wcsicmp(ext, L"webm") == 0)
        {
            return COLOR_VIDEO;
        }
        // 文档
        if (_wcsicmp(ext, L"pdf") == 0 || _wcsicmp(ext, L"doc") == 0 || _wcsicmp(ext, L"docx") == 0 || _wcsicmp(ext, L"odt") == 0 ||
            _wcsicmp(ext, L"xls") == 0 || _wcsicmp(ext, L"xlsx") == 0 || _wcsicmp(ext, L"ods") == 0 ||
            _wcsicmp(ext, L"ppt") == 0 || _wcsicmp(ext, L"pptx") == 0 || _wcsicmp(ext, L"odp") == 0 ||
            _wcsicmp(ext, L"txt") == 0 || _wcsicmp(ext, L"rtf") == 0 || _wcsicmp(ext, L"md") == 0 || _wcsicmp(ext, L"csv") == 0 || _wcsicmp(ext, L"tex") == 0)
        {
            return COLOR_DOCUMENT;
        }
        // 源代码
        if (_wcsicmp(ext, L"c") == 0 || _wcsicmp(ext, L"h") == 0 || _wcsicmp(ext, L"cpp") == 0 || _wcsicmp(ext, L"hpp") == 0 || _wcsicmp(ext, L"cs") == 0 ||
            _wcsicmp(ext, L"java") == 0 || _wcsicmp(ext, L"class") == 0 || _wcsicmp(ext, L"py") == 0 || _wcsicmp(ext, L"pyc") == 0 || _wcsicmp(ext, L"rb") == 0 ||
            _wcsicmp(ext, L"js") == 0 || _wcsicmp(ext, L"ts") == 0 || _wcsicmp(ext, L"jsx") == 0 || _wcsicmp(ext, L"tsx") == 0 || _wcsicmp(ext, L"html") == 0 || _wcsicmp(ext, L"css") == 0 ||
            _wcsicmp(ext, L"php") == 0 || _wcsicmp(ext, L"swift") == 0 || _wcsicmp(ext, L"go") == 0 || _wcsicmp(ext, L"rs") == 0 || _wcsicmp(ext, L"lua") == 0 ||
            _wcsicmp(ext, L"pl") == 0 || _wcsicmp(ext, L"sh") == 0 || _wcsicmp(ext, L"json") == 0 || _wcsicmp(ext, L"xml") == 0 || _wcsicmp(ext, L"yml") == 0 || _wcsicmp(ext, L"yaml") == 0 ||
            _wcsicmp(ext, L"sln") == 0 || _wcsicmp(ext, L"csproj") == 0 || _wcsicmp(ext, L"vcxproj") == 0 || _wcsicmp(ext, L"mk") == 0 || _wcsicmp(ext, L"makefile") == 0)
        {
            return COLOR_SOURCECODE;
        }
        // 临时/备份文件
        if (_wcsicmp(ext, L"tmp") == 0 || _wcsicmp(ext, L"temp") == 0 || _wcsicmp(ext, L"bak") == 0 || _wcsicmp(ext, L"swo") == 0 || _wcsicmp(ext, L"swp") == 0 || _wcsicmp(ext, L"log") == 0)
        {
            return COLOR_TEMP;
        }
    }
    return COLOR_FILE; // 如果没有特定规则匹配，则为默认文件颜色
}
// 结束：更新的 get_color_for_entry 函数

// 显示帮助信息
void display_help(bool prefer_chinese)
{
    const wchar_t **help_text = prefer_chinese ? help_text_zh_CN : help_text_en_US;
    // 不需要在此处设置代码页，因为 wmain 已经处理了
    for (int i = 0; help_text[i] != NULL; ++i)
    {
        wprintf(L"%s\n", help_text[i]);
    }
}

// --- JSON 和 XML 字符串转义 ---
// 将输入字符串转义为 JSON 格式并存入缓冲区
void escape_json_string_to_buffer(const wchar_t *input, wchar_t *buffer, size_t buffer_size)
{
    if (!input || !buffer || buffer_size == 0)
        return;
    size_t j = 0;      // 缓冲区写入位置索引
    buffer[0] = L'\0'; // 确保空或太小时以 null 结尾
    for (size_t i = 0; input[i] != L'\0'; ++i)
    {
        wchar_t to_escape = input[i];
        const wchar_t *replacement = NULL;
        size_t replacement_len = 0;
        wchar_t unicode_buf[7]; // 用于 \uXXXX

        switch (to_escape)
        {
        case L'"':
            replacement = L"\\\"";
            replacement_len = 2;
            break;
        case L'\\':
            replacement = L"\\\\";
            replacement_len = 2;
            break;
        case L'\b':
            replacement = L"\\b";
            replacement_len = 2;
            break;
        case L'\f':
            replacement = L"\\f";
            replacement_len = 2;
            break;
        case L'\n':
            replacement = L"\\n";
            replacement_len = 2;
            break;
        case L'\r':
            replacement = L"\\r";
            replacement_len = 2;
            break;
        case L'\t':
            replacement = L"\\t";
            replacement_len = 2;
            break;
        default:
            if (to_escape < 32 || to_escape == 0x7F) // 控制字符
            {
                swprintf(unicode_buf, sizeof(unicode_buf) / sizeof(wchar_t), L"\\u%04x", (unsigned int)to_escape);
                replacement = unicode_buf;
                replacement_len = 6;
            }
            break;
        }

        if (replacement)
        {
            if (j + replacement_len >= buffer_size)
            { /* 空间不足 */
                break;
            }
            wcscpy(&buffer[j], replacement);
            j += replacement_len;
        }
        else
        {
            if (j + 1 >= buffer_size)
            { /* 空间不足 */
                break;
            }
            buffer[j++] = to_escape;
        }
    }
    if (j < buffer_size)
    {
        buffer[j] = L'\0';
    }
    else if (buffer_size > 0) // 如果溢出，确保以 null 结尾
    {
        buffer[buffer_size - 1] = L'\0';
    }
}

// 将输入字符串转义为 XML 格式并存入缓冲区
void escape_xml_string_to_buffer(const wchar_t *input, wchar_t *buffer, size_t buffer_size)
{
    if (!input || !buffer || buffer_size == 0)
        return;
    size_t j = 0; // 缓冲区写入位置索引
    buffer[0] = L'\0';
    for (size_t i = 0; input[i] != L'\0'; ++i)
    {
        const wchar_t *replacement = NULL;
        size_t replacement_len = 0;

        switch (input[i])
        {
        case L'&':
            replacement = L"&amp;";
            replacement_len = 5;
            break;
        case L'<':
            replacement = L"&lt;";
            replacement_len = 4;
            break;
        case L'>':
            replacement = L"&gt;";
            replacement_len = 4;
            break;
        case L'"':
            replacement = L"&quot;";
            replacement_len = 6;
            break;
        case L'\'':
            replacement = L"&apos;";
            replacement_len = 6;
            break;
        default:
            // 允许有效的 XML 字符。为简单起见，这不会过滤所有无效的 XML 字符。
            // 基本的 ASCII 控制字符（除了制表符、换行符、回车符）在 XML 1.0 中是有问题的。
            if ((input[i] < 0x20 && input[i] != L'\t' && input[i] != L'\n' && input[i] != L'\r') ||
                (input[i] >= 0x7F && input[i] <= 0x9F && input[i] != 0x85 /*NEL*/))
            {
                // 跳过或替换无效的 XML 字符（例如，用 '?'）
                // 目前，只是跳过它们以避免格式错误的 XML。
                continue;
            }
            break;
        }

        if (replacement)
        {
            if (j + replacement_len >= buffer_size)
            {
                break;
            }
            wcscpy(&buffer[j], replacement);
            j += replacement_len;
        }
        else
        {
            if (j + 1 >= buffer_size)
            {
                break;
            }
            buffer[j++] = input[i];
        }
    }
    if (j < buffer_size)
    {
        buffer[j] = L'\0';
    }
    else if (buffer_size > 0) // 如果溢出，确保以 null 结尾
    {
        buffer[buffer_size - 1] = L'\0';
    }
}

// --- 核心树遍历与打印逻辑 (文本模式) ---

// 比较目录项，用于 qsort
int compare_dir_entries(const void *a, const void *b)
{
    const DirEntry *entry_a = (const DirEntry *)a;
    const DirEntry *entry_b = (const DirEntry *)b;

    // 目录优先于文件
    if (entry_a->is_directory && !entry_b->is_directory)
        return -1;
    if (!entry_a->is_directory && entry_b->is_directory)
        return 1;
    // 按名称排序 (忽略大小写)
    return _wcsicmp(entry_a->name, entry_b->name);
}


// 递归打印树状结构 (文本模式)
// 返回当前目录的总大小
unsigned long long print_tree_recursive(const wchar_t *current_path, int level, TreeOptions *opts, Counts *counts, const wchar_t *prefix_str)
{
    if (opts->max_level != -1 && level > opts->max_level)
    {
        return 0;
    }

    WIN32_FIND_DATAW find_data;
    HANDLE h_find = INVALID_HANDLE_VALUE;
    wchar_t search_path[MAX_PATH_LEN];
    unsigned long long current_directory_total_size = 0;

    wcscpy(search_path, current_path);
    if (search_path[wcslen(search_path) - 1] != L'\\')
    {
        wcscat(search_path, L"\\");
    }
    wcscat(search_path, L"*");

    h_find = FindFirstFileW(search_path, &find_data);

    if (h_find == INVALID_HANDLE_VALUE)
    {
        if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
        {
            fwprintf(stderr, L"\x8B66\x544A: \x65E0\x6CD5\x8BBF\x95EE\x8DEF\x5F84 '%s' (\x9519\x8BEF\x4EE3\x7801: %lu)\x3002\x8DF3\x8FC7\x3002\n", current_path, GetLastError());
        }
        return 0;
    }

    DirEntry *entries = NULL;
    size_t entry_count = 0;
    size_t capacity = 0;

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
        if (!opts->show_hidden && (find_data.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) && !is_dir)
        {
            continue;
        }
        if (opts->exclude_pattern[0] != L'\0' && wildcard_match(find_data.cFileName, opts->exclude_pattern, opts->ignore_case_filter))
        {
            continue;
        }
        if (opts->include_pattern[0] != L'\0' && !wildcard_match(find_data.cFileName, opts->include_pattern, opts->ignore_case_filter))
        {
            continue;
        }

        if (entry_count >= capacity)
        {
            capacity = (capacity == 0) ? 16 : capacity * 2;
            DirEntry *new_entries = (DirEntry *)realloc(entries, capacity * sizeof(DirEntry));
            if (!new_entries)
            {
                fwprintf(stderr, L"\x5185\x5B58\x5206\x914D\x5931\x8D25\x3002\n");
                if (entries) free(entries);
                FindClose(h_find);
                return 0;
            }
            entries = new_entries;
        }

        wcscpy(entries[entry_count].name, find_data.cFileName);
        entries[entry_count].is_directory = is_dir;
        entries[entry_count].attributes = find_data.dwFileAttributes;
        if (is_dir)
        {
            entries[entry_count].size = 0; // 稍后计算
        }
        else
        {
            entries[entry_count].size = ((unsigned long long)find_data.nFileSizeHigh << 32) + find_data.nFileSizeLow;
            current_directory_total_size += entries[entry_count].size;
        }
        entry_count++;

    } while (FindNextFileW(h_find, &find_data) != 0);

    DWORD dwError = GetLastError();
    FindClose(h_find);

    if (dwError != ERROR_NO_MORE_FILES)
    {
        // fwprintf(stderr, L"读取目录 '%s' 时发生错误 (代码: %lu)。\n", current_path, dwError);
    }

    if (entry_count > 0)
    {
        qsort(entries, entry_count, sizeof(DirEntry), compare_dir_entries);
    }
    
    // 先计算所有子目录的大小
    for (size_t i = 0; i < entry_count; ++i) {
        if (entries[i].is_directory) {
            wchar_t child_path[MAX_PATH_LEN];
            swprintf(child_path, MAX_PATH_LEN, L"%s\\%s", current_path, entries[i].name);
            
            // 仅为计算大小而递归，不传递前缀
            unsigned long long subdir_size = print_tree_recursive(child_path, level + 1, opts, counts, L""); 
            entries[i].size = subdir_size;
            current_directory_total_size += subdir_size;
        }
    }

    // 如果 prefix_str 不为空，则表示这是一个递归调用，需要打印内容
    if (wcslen(prefix_str) > 0 || level == 1) {
        HANDLE hConsole = NULL;
        if (opts->use_color && opts->output_is_console)
        {
            hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        }

        for (size_t i = 0; i < entry_count; ++i)
        {
            bool is_last_item_in_level = (i == entry_count - 1);

            if (!entries[i].is_directory && !opts->list_files)
            {
                continue;
            }

            wprintf(L"%s", prefix_str);

            const wchar_t *connector = is_last_item_in_level ? (opts->use_ascii ? L_UP_RIGHT_ASCII : L_UP_RIGHT)
                                                             : (opts->use_ascii ? L_VERT_RIGHT_ASCII : L_VERT_RIGHT);
            const wchar_t *h_line = opts->use_ascii ? L_HORZ_ASCII : L_HORZ;
            wprintf(L"%s%s%s ", connector, h_line, h_line);

            if (opts->show_attributes)
            {
                wchar_t attr_str[ATTR_STR_LEN];
                get_attribute_string(entries[i].attributes, entries[i].is_directory, attr_str, ATTR_STR_LEN);
                wprintf(L"%-*s ", ATTR_STR_LEN - 1, attr_str[0] != L'\0' ? attr_str : L"");
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

            if (hConsole)
            {
                SetConsoleTextAttribute(hConsole, get_color_for_entry(&entries[i], opts));
            }
            wprintf(L"%s", entries[i].name);
            if (hConsole)
            {
                SetConsoleTextAttribute(hConsole, opts->saved_console_attributes);
            }
            wprintf(L"\n");

            if (entries[i].is_directory)
            {
                counts->dir_count++;
                wchar_t next_prefix_str[MAX_PATH_LEN * 2];
                wcscpy(next_prefix_str, prefix_str);
                const wchar_t *v_line_segment = opts->use_ascii ? L_VERT_ASCII : L_VERT;
                wcscat(next_prefix_str, is_last_item_in_level ? L"    " : v_line_segment);
                wcscat(next_prefix_str, L"   ");

                wchar_t child_path[MAX_PATH_LEN];
                swprintf(child_path, MAX_PATH_LEN, L"%s\\%s", current_path, entries[i].name);
                // 再次调用以打印子目录内容
                print_tree_recursive(child_path, level + 1, opts, counts, next_prefix_str);
            }
            else
            {
                if(opts->list_files) counts->file_count++;
            }
        }
    }


    if (entries)
    {
        free(entries);
    }
    return current_directory_total_size;
}

// --- JSON 输出函数 ---
// 递归构建 JSON 树
unsigned long long recursive_json_builder(const wchar_t *current_path, int level, TreeOptions *opts, Counts *counts, FILE *out, bool is_first_in_parent_contents, wchar_t *indent_str);

// 生成 JSON 格式的树
void generate_json_tree(const wchar_t *root_path, TreeOptions *opts, Counts *global_counts, FILE *out)
{
    fwprintf(out, L"[\n"); // JSON 数组开始

    wchar_t indent_str[MAX_PATH_LEN] = L"  "; // 根对象的初始缩进
    recursive_json_builder(root_path, 1, opts, global_counts, out, true, indent_str);

    fwprintf(out, L"\n]\n"); // JSON 数组结束
}

// 递归构建 JSON 对象的函数
// 返回当前目录（及其子内容）的总大小
unsigned long long recursive_json_builder(
    const wchar_t *current_path_abs, // 当前处理的目录的绝对路径
    int level,                       // 当前递归层级
    TreeOptions *opts,               // 程序选项
    Counts *global_counts,           // 全局文件和目录计数器 (用于最终报告)
    FILE *out,                       // 输出文件流
    bool is_first_sibling,           // 当前项是否为其父目录 "contents" 数组中的第一个元素
    wchar_t *indent_str)             // 当前 JSON 对象的缩进字符串
{
    if (opts->max_level != -1 && level > opts->max_level && level > 1)
    {
        return 0;
    }

    WIN32_FIND_DATAW current_find_data;
    HANDLE hCurrentFind = FindFirstFileW(current_path_abs, &current_find_data);

    if (hCurrentFind == INVALID_HANDLE_VALUE)
    {
        fwprintf(stderr, L"JSON \x8B66\x544A: \x65E0\x6CD5\x8BBF\x95EE\x8DEF\x5F84 '%s' (\x9519\x8BEF: %lu)\n", current_path_abs, GetLastError());
        return 0;
    }

    if (!is_first_sibling)
    {
        fwprintf(out, L",\n");
    }

    fwprintf(out, L"%ls{", indent_str);
    wchar_t temp_name_buf[MAX_PATH_LEN * ESCAPE_BUF_MULT];

    const wchar_t *name_ptr = wcsrchr(current_path_abs, L'\\');
    if (!name_ptr)
        name_ptr = wcsrchr(current_path_abs, L'/');
    name_ptr = (name_ptr && *(name_ptr + 1) != L'\0') ? name_ptr + 1 : current_path_abs;

    wchar_t adjusted_name[MAX_PATH_LEN];
    wcscpy(adjusted_name, name_ptr);
    if (wcslen(adjusted_name) == 0 && wcslen(current_path_abs) > 0)
    {
        wcsncpy(adjusted_name, current_path_abs, MAX_PATH_LEN - 1);
        adjusted_name[MAX_PATH_LEN - 1] = L'\0';
        size_t len = wcslen(adjusted_name);
        if (len > 1 && (adjusted_name[len - 1] == L'\\' || adjusted_name[len - 1] == L'/'))
        {
            if (!(len == 3 && adjusted_name[1] == L':'))
            {
                adjusted_name[len - 1] = L'\0';
            }
        }
    }

    escape_json_string_to_buffer(adjusted_name, temp_name_buf, sizeof(temp_name_buf) / sizeof(wchar_t));
    fwprintf(out, L"\n%ls  \"type\": \"directory\",", indent_str);
    fwprintf(out, L"\n%ls  \"name\": \"%ls\"", indent_str, temp_name_buf);

    if (opts->show_attributes)
    {
        wchar_t attr_str[ATTR_STR_LEN];
        get_attribute_string(current_find_data.dwFileAttributes, true, attr_str, ATTR_STR_LEN);
        escape_json_string_to_buffer(attr_str, temp_name_buf, sizeof(temp_name_buf) / sizeof(wchar_t));
        fwprintf(out, L",\n%ls  \"attributes\": \"%ls\"", indent_str, temp_name_buf);
    }

    FindClose(hCurrentFind);

    Counts content_counts = {0, 0}; // 用于当前目录 "report" 字段的计数
    // global_counts->dir_count 在这里由调用者 (上一级或 generate_json_tree) 增加，或在此处 level==1 时增加
    // 为了与 XML 和 TEXT 模式的计数方式统一，我们让递归函数自身在遇到目录时增加 global_counts->dir_count
    // 对于根目录 (level 1)，它本身就是一个目录
    if (level == 1)
    { // 如果是正在处理的根目录对象
      // global_counts->dir_count++; // 统计根目录本身 - 移到循环外，确保只加一次
    }

    WIN32_FIND_DATAW find_data_content;
    HANDLE h_find_content = INVALID_HANDLE_VALUE;
    wchar_t search_path[MAX_PATH_LEN];
    unsigned long long current_directory_content_size = 0;

    wcscpy(search_path, current_path_abs);
    if (search_path[wcslen(search_path) - 1] != L'\\' && search_path[wcslen(search_path) - 1] != L'/')
    {
        wcscat(search_path, L"\\");
    }
    wcscat(search_path, L"*");

    h_find_content = FindFirstFileW(search_path, &find_data_content);

    DirEntry *entries = NULL;
    size_t entry_count = 0;
    size_t capacity = 0;

    if (h_find_content != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (wcscmp(find_data_content.cFileName, L".") == 0 || wcscmp(find_data_content.cFileName, L"..") == 0)
                continue;

            bool is_dir = (find_data_content.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!opts->show_hidden && (find_data_content.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN))
                continue;
            if (!opts->show_hidden && (find_data_content.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) && !is_dir)
                continue;

            if (opts->exclude_pattern[0] != L'\0' && wildcard_match(find_data_content.cFileName, opts->exclude_pattern, opts->ignore_case_filter))
                continue;
            if (opts->include_pattern[0] != L'\0' && !wildcard_match(find_data_content.cFileName, opts->include_pattern, opts->ignore_case_filter))
                continue;

            // 如果是文件且不要求显示文件 (受 -f 参数控制)
            if (!is_dir && !opts->list_files)
            {
                continue;
            }

            if (entry_count >= capacity)
            {
                capacity = (capacity == 0) ? 16 : capacity * 2;
                DirEntry *new_entries = (DirEntry *)realloc(entries, capacity * sizeof(DirEntry));
                if (!new_entries)
                {
                    fwprintf(stderr, L"JSON: \x76EE\x5F55\x6761\x76EE\x5185\x5B58\x5206\x914D\x5931\x8D25\x3002\n");
                    if (entries)
                        free(entries);
                    entries = NULL;
                    break;
                }
                entries = new_entries;
            }
            wcscpy(entries[entry_count].name, find_data_content.cFileName);
            entries[entry_count].is_directory = is_dir;
            entries[entry_count].attributes = find_data_content.dwFileAttributes;
            entries[entry_count].size = is_dir ? 0 : ((unsigned long long)find_data_content.nFileSizeHigh << 32) + find_data_content.nFileSizeLow;
            entry_count++;
        } while (FindNextFileW(h_find_content, &find_data_content) != 0);
        FindClose(h_find_content);
    }

    if (entry_count > 0)
    {
        qsort(entries, entry_count, sizeof(DirEntry), compare_dir_entries);
    }

    bool can_have_contents = (opts->max_level == -1 || level < opts->max_level);

    if (entry_count > 0 || (can_have_contents && level == 1 && opts->list_files) || (can_have_contents && level == 1 && !opts->list_files && entry_count == 0))
    {
        // 修正逻辑: 总是为目录输出 "contents" 数组，即使它是空的 (除非达到 max_level 且该目录不应再展开其子项)
        // 如果 list_files 为 false，空的 "contents" 数组是合适的。
        // 如果 entry_count > 0 (意味着有子目录或文件被列出)
        // 或者 (是根目录 level 1 且可以有内容) -> 这样空目录也会有 "contents": []
        fwprintf(out, L",\n%ls  \"contents\": [", indent_str);
        wchar_t child_indent_str[MAX_PATH_LEN];
        swprintf(child_indent_str, MAX_PATH_LEN, L"%ls  ", indent_str);
        bool first_child = true;

        for (size_t i = 0; i < entry_count; ++i)
        {
            if (entries[i].is_directory)
            {
                global_counts->dir_count++; // 统计子目录
                content_counts.dir_count++; // 统计当前目录的子目录数
                if (opts->max_level != -1 && level >= opts->max_level)
                {
                    if (!first_child)
                        fwprintf(out, L",\n");
                    fwprintf(out, L"\n%ls  {", child_indent_str);
                    escape_json_string_to_buffer(entries[i].name, temp_name_buf, sizeof(temp_name_buf) / sizeof(wchar_t));
                    fwprintf(out, L"\n%ls    \"type\": \"directory\",", child_indent_str);
                    fwprintf(out, L"\n%ls    \"name\": \"%ls\"", child_indent_str, temp_name_buf);
                    if (opts->show_attributes)
                    {
                        wchar_t attr_str_child[ATTR_STR_LEN];
                        get_attribute_string(entries[i].attributes, true, attr_str_child, ATTR_STR_LEN);
                        escape_json_string_to_buffer(attr_str_child, temp_name_buf, sizeof(temp_name_buf) / sizeof(wchar_t));
                        fwprintf(out, L",\n%ls    \"attributes\": \"%ls\"", child_indent_str, temp_name_buf);
                    }
                    if (opts->show_size)
                    {
                        fwprintf(out, L",\n%ls    \"size\": 0", child_indent_str);
                    }
                    fwprintf(out, L"\n%ls  }", child_indent_str);
                    first_child = false;
                }
                else
                {
                    wchar_t child_path[MAX_PATH_LEN];
                    wcscpy(child_path, current_path_abs);
                    if (child_path[wcslen(child_path) - 1] != L'\\' && child_path[wcslen(child_path) - 1] != L'/')
                    {
                        wcscat(child_path, L"\\");
                    }
                    wcscat(child_path, entries[i].name);
                    unsigned long long subdir_size = recursive_json_builder(child_path, level + 1, opts, global_counts, out, first_child, child_indent_str);
                    current_directory_content_size += subdir_size;
                    entries[i].size = subdir_size;
                    first_child = false;
                }
            }
            else // 如果是文件 (此时 opts->list_files 必然为 true，因为前面已经 continue 掉了)
            {
                if (!first_child)
                {
                    fwprintf(out, L",\n");
                }
                fwprintf(out, L"\n%ls  {", child_indent_str);
                escape_json_string_to_buffer(entries[i].name, temp_name_buf, sizeof(temp_name_buf) / sizeof(wchar_t));
                fwprintf(out, L"\n%ls    \"type\": \"file\",", child_indent_str);
                fwprintf(out, L"\n%ls    \"name\": \"%ls\"", child_indent_str, temp_name_buf);
                current_directory_content_size += entries[i].size;
                global_counts->file_count++; // 统计文件
                content_counts.file_count++; // 统计当前目录的文件数

                if (opts->show_attributes)
                {
                    wchar_t attr_str_file[ATTR_STR_LEN];
                    get_attribute_string(entries[i].attributes, false, attr_str_file, ATTR_STR_LEN);
                    escape_json_string_to_buffer(attr_str_file, temp_name_buf, sizeof(temp_name_buf) / sizeof(wchar_t));
                    fwprintf(out, L",\n%ls    \"attributes\": \"%ls\"", child_indent_str, temp_name_buf);
                }
                if (opts->show_size)
                {
                    fwprintf(out, L",\n%ls    \"size\": %llu", child_indent_str, entries[i].size);
                }
                fwprintf(out, L"\n%ls  }", child_indent_str);
                first_child = false;
            }
        }
        if (entry_count > 0)
            fwprintf(out, L"\n%ls  ", indent_str);
        fwprintf(out, L"]");
    }
    else if (entry_count == 0 && can_have_contents)
    { // 空目录且未达层级限制，输出空的 contents
        fwprintf(out, L",\n%ls  \"contents\": []", indent_str);
    }

    if (entries)
        free(entries);

    if (level == 1 && !opts->no_report)
    {
        fwprintf(out, L",\n%ls  \"report\": {", indent_str);
        fwprintf(out, L"\n%ls    \"directories\": %lld,", indent_str, content_counts.dir_count); // 使用 content_counts
        fwprintf(out, L"\n%ls    \"files\": %lld", indent_str, content_counts.file_count);       // 使用 content_counts
        fwprintf(out, L"\n%ls  }", indent_str);
    }

    if (opts->show_size)
    {
        fwprintf(out, L",\n%ls  \"size\": %llu", indent_str, current_directory_content_size);
    }

    fwprintf(out, L"\n%ls}", indent_str);

    return current_directory_content_size;
}

// --- XML 输出函数 ---
// 递归构建 XML 树
unsigned long long recursive_xml_builder(const wchar_t *current_path, int level, TreeOptions *opts, Counts *counts, FILE *out, wchar_t *indent_str);

// 生成 XML 格式的树
void generate_xml_tree(const wchar_t *root_path, TreeOptions *opts, Counts *global_counts, FILE *out)
{
    fwprintf(out, L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fwprintf(out, L"<tree>\n");

    wchar_t indent_str[MAX_PATH_LEN] = L"  ";
    // 根目录本身也算一个目录，在 recursive_xml_builder 中 level 1 时处理并计数
    global_counts->dir_count = 0; // 重置，由递归函数填充
    global_counts->file_count = 0;
    recursive_xml_builder(root_path, 1, opts, global_counts, out, indent_str);

    if (!opts->no_report)
    {
        fwprintf(out, L"%ls<report>\n", indent_str);
        fwprintf(out, L"%ls  <directories>%lld</directories>\n", indent_str, global_counts->dir_count);
        fwprintf(out, L"%ls  <files>%lld</files>\n", indent_str, global_counts->file_count);
        fwprintf(out, L"%ls</report>\n", indent_str);
    }
    fwprintf(out, L"</tree>\n");
}

// 递归构建 XML 元素的函数
// 返回当前目录（及其子内容）的总大小
unsigned long long recursive_xml_builder(
    const wchar_t *current_path_abs, // 当前处理的目录的绝对路径
    int level,                       // 当前递归层级
    TreeOptions *opts,               // 程序选项
    Counts *global_counts,           // 全局文件和目录计数器
    FILE *out,                       // 输出文件流
    wchar_t *indent_str)             // 当前 XML 元素的缩进字符串
{
    if (opts->max_level != -1 && level > opts->max_level && level > 1)
    {
        return 0;
    }

    WIN32_FIND_DATAW current_find_data;
    HANDLE hCurrentFind = FindFirstFileW(current_path_abs, &current_find_data);

    if (hCurrentFind == INVALID_HANDLE_VALUE)
    {
        fwprintf(stderr, L"XML \x8B66\x544A: \x65E0\x6CD5\x8BBF\x95EE\x8DEF\x5F84 '%s' (\x9519\x8BEF: %lu)\n", current_path_abs, GetLastError());
        return 0;
    }

    // 当前目录自身是一个目录，进行计数
    if (level == 1)
    { // 根目录
        global_counts->dir_count++;
    }
    // 子目录的计数在递归调用后，由其自身调用时在 level > 1 的情况下完成 (由其父级在循环中递增)
    // 或者更一致地，在每次进入此函数处理目录时都计数，然后在循环中只计数子目录。
    // 当前实现：根目录在此计数，子目录在父级循环中计数。

    fwprintf(out, L"%ls<directory", indent_str);
    wchar_t temp_attr_buf[MAX_PATH_LEN * ESCAPE_BUF_MULT];

    const wchar_t *name_ptr = wcsrchr(current_path_abs, L'\\');
    if (!name_ptr)
        name_ptr = wcsrchr(current_path_abs, L'/');
    name_ptr = (name_ptr && *(name_ptr + 1) != L'\0') ? name_ptr + 1 : current_path_abs;

    wchar_t adjusted_name[MAX_PATH_LEN];
    wcscpy(adjusted_name, name_ptr);
    if (wcslen(adjusted_name) == 0 && wcslen(current_path_abs) > 0)
    {
        wcsncpy(adjusted_name, current_path_abs, MAX_PATH_LEN - 1);
        adjusted_name[MAX_PATH_LEN - 1] = L'\0';
        size_t len = wcslen(adjusted_name);
        if (len > 1 && (adjusted_name[len - 1] == L'\\' || adjusted_name[len - 1] == L'/'))
        {
            if (!(len == 3 && adjusted_name[1] == L':'))
            {
                adjusted_name[len - 1] = L'\0';
            }
        }
    }

    escape_xml_string_to_buffer(adjusted_name, temp_attr_buf, sizeof(temp_attr_buf) / sizeof(wchar_t));
    fwprintf(out, L" name=\"%ls\"", temp_attr_buf);

    if (opts->show_attributes)
    {
        wchar_t attr_str[ATTR_STR_LEN];
        get_attribute_string(current_find_data.dwFileAttributes, true, attr_str, ATTR_STR_LEN);
        escape_xml_string_to_buffer(attr_str, temp_attr_buf, sizeof(temp_attr_buf) / sizeof(wchar_t));
        fwprintf(out, L" attributes=\"%ls\"", temp_attr_buf);
    }
    FindClose(hCurrentFind);

    WIN32_FIND_DATAW find_data_content;
    HANDLE h_find_content = INVALID_HANDLE_VALUE;
    wchar_t search_path[MAX_PATH_LEN];
    unsigned long long current_directory_content_size = 0;

    wcscpy(search_path, current_path_abs);
    if (search_path[wcslen(search_path) - 1] != L'\\' && search_path[wcslen(search_path) - 1] != L'/')
    {
        wcscat(search_path, L"\\");
    }
    wcscat(search_path, L"*");
    h_find_content = FindFirstFileW(search_path, &find_data_content);

    DirEntry *entries = NULL;
    size_t entry_count = 0;
    size_t capacity = 0;

    if (h_find_content != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (wcscmp(find_data_content.cFileName, L".") == 0 || wcscmp(find_data_content.cFileName, L"..") == 0)
                continue;
            bool is_dir = (find_data_content.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!opts->show_hidden && (find_data_content.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN))
                continue;
            if (!opts->show_hidden && (find_data_content.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) && !is_dir)
                continue;
            if (opts->exclude_pattern[0] != L'\0' && wildcard_match(find_data_content.cFileName, opts->exclude_pattern, opts->ignore_case_filter))
                continue;
            if (opts->include_pattern[0] != L'\0' && !wildcard_match(find_data_content.cFileName, opts->include_pattern, opts->ignore_case_filter))
                continue;

            // 如果是文件且不要求显示文件 (受 -f 参数控制)
            if (!is_dir && !opts->list_files)
            {
                continue;
            }

            if (entry_count >= capacity)
            {
                capacity = (capacity == 0) ? 16 : capacity * 2;
                DirEntry *new_entries = (DirEntry *)realloc(entries, capacity * sizeof(DirEntry));
                if (!new_entries)
                {
                    fwprintf(stderr, L"XML: \x76EE\x5F55\x6761\x76EE\x5185\x5B58\x5206\x914D\x5931\x8D25\n");
                    if (entries)
                        free(entries);
                    entries = NULL;
                    break;
                }
                entries = new_entries;
            }
            wcscpy(entries[entry_count].name, find_data_content.cFileName);
            entries[entry_count].is_directory = is_dir;
            entries[entry_count].attributes = find_data_content.dwFileAttributes;
            entries[entry_count].size = is_dir ? 0 : ((unsigned long long)find_data_content.nFileSizeHigh << 32) + find_data_content.nFileSizeLow;
            entry_count++;
        } while (FindNextFileW(h_find_content, &find_data_content) != 0);
        FindClose(h_find_content);
    }

    if (entry_count > 0)
    {
        qsort(entries, entry_count, sizeof(DirEntry), compare_dir_entries);
    }

    bool can_have_children_shown = (opts->max_level == -1 || level < opts->max_level);

    if (entry_count == 0 || !can_have_children_shown)
    {
        if (opts->show_size)
        {
            fwprintf(out, L" size=\"%llu\"", current_directory_content_size);
        }
        fwprintf(out, L"/>\n");
    }
    else
    {
        fwprintf(out, L">\n");

        wchar_t child_indent_str[MAX_PATH_LEN];
        swprintf(child_indent_str, MAX_PATH_LEN, L"%ls  ", indent_str);

        for (size_t i = 0; i < entry_count; ++i)
        {
            if (entries[i].is_directory)
            {
                global_counts->dir_count++; // 统计子目录
                if (opts->max_level != -1 && level >= opts->max_level)
                {
                    fwprintf(out, L"%ls<directory", child_indent_str);
                    escape_xml_string_to_buffer(entries[i].name, temp_attr_buf, sizeof(temp_attr_buf) / sizeof(wchar_t));
                    fwprintf(out, L" name=\"%ls\"", temp_attr_buf);
                    if (opts->show_attributes)
                    {
                        wchar_t attr_str_child[ATTR_STR_LEN];
                        get_attribute_string(entries[i].attributes, true, attr_str_child, ATTR_STR_LEN);
                        escape_xml_string_to_buffer(attr_str_child, temp_attr_buf, sizeof(temp_attr_buf) / sizeof(wchar_t));
                        fwprintf(out, L" attributes=\"%ls\"", temp_attr_buf);
                    }
                    if (opts->show_size)
                        fwprintf(out, L" size=\"0\"");
                    fwprintf(out, L"/>\n");
                }
                else
                {
                    wchar_t child_path[MAX_PATH_LEN];
                    wcscpy(child_path, current_path_abs);
                    if (child_path[wcslen(child_path) - 1] != L'\\' && child_path[wcslen(child_path) - 1] != L'/')
                    {
                        wcscat(child_path, L"\\");
                    }
                    wcscat(child_path, entries[i].name);
                    unsigned long long subdir_size = recursive_xml_builder(child_path, level + 1, opts, global_counts, out, child_indent_str);
                    current_directory_content_size += subdir_size;
                }
            }
            else // 如果是文件 (此时 opts->list_files 必然为 true)
            {
                fwprintf(out, L"%ls<file", child_indent_str);
                escape_xml_string_to_buffer(entries[i].name, temp_attr_buf, sizeof(temp_attr_buf) / sizeof(wchar_t));
                fwprintf(out, L" name=\"%ls\"", temp_attr_buf);
                current_directory_content_size += entries[i].size;
                global_counts->file_count++; // 统计文件

                if (opts->show_attributes)
                {
                    wchar_t attr_str_file[ATTR_STR_LEN];
                    get_attribute_string(entries[i].attributes, false, attr_str_file, ATTR_STR_LEN);
                    escape_xml_string_to_buffer(attr_str_file, temp_attr_buf, sizeof(temp_attr_buf) / sizeof(wchar_t));
                    fwprintf(out, L" attributes=\"%ls\"", temp_attr_buf);
                }
                if (opts->show_size)
                {
                    fwprintf(out, L" size=\"%llu\"", entries[i].size);
                }
                fwprintf(out, L"/>\n");
            }
        }

        if (opts->show_size)
        {
            // 对于 XML，size 通常作为父 <directory> 的属性或子元素
            // 为了与 JSON 的 "size" 同级，这里可以作为子元素添加
            fwprintf(out, L"%ls  <size>%llu</size>\n", child_indent_str, current_directory_content_size);
        }

        fwprintf(out, L"%ls</directory>\n", indent_str);
    }
    if (entries)
        free(entries);
    return current_directory_content_size;
}

// --- 主函数与参数解析 ---
int wmain(int argc, wchar_t *argv[])
{
    // FIX START: 设置控制台输出编码为 UTF-8
    // 保存原始控制台代码页，以便在程序退出时恢复
    UINT original_console_cp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    // 将 C 运行时的 stdout 和 stderr 文件流的转换模式设置为 UTF-8
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
    // FIX END

    TreeOptions options;
    wcscpy(options.path_to_tree, L".");
    options.max_level = -1;
    options.list_files = false; // 默认不显示文件，由 -f 控制
    options.use_ascii = false;
    options.show_hidden = false;
    options.no_report = false;
    options.show_size = false;
    options.use_si_units = false;
    options.show_attributes = false;
    options.include_pattern[0] = L'\0';
    options.exclude_pattern[0] = L'\0';
    options.ignore_case_filter = false;
    options.output_filename[0] = L'\0';

    options.use_color = false;
    options.output_format = OUTPUT_TEXT;
    options.output_is_console = (_isatty(_fileno(stdout)) != 0);

    if (options.output_is_console)
    {
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO consoleInfo;
        if (GetConsoleScreenBufferInfo(hConsole, &consoleInfo))
        {
            options.saved_console_attributes = consoleInfo.wAttributes;
        }
        else
        {
            options.saved_console_attributes = COLOR_DEFAULT_FG;
        }
    }
    else
    {
        options.saved_console_attributes = COLOR_DEFAULT_FG;
    }

    bool help_requested = false;
    bool chinese_lang_preferred = true;

    for (int i = 1; i < argc; ++i)
    {
        if (wcscmp(argv[i], L"-L") == 0)
        {
            if (i + 1 < argc)
                options.max_level = _wtoi(argv[++i]);
            else
            {
                fwprintf(stderr, L"\x9519\x8BEF: -L \x9009\x9879\x9700\x8981\x4E00\x4E2A\x5C42\x6570\x53C2\x6570\x3002\n");
                SetConsoleOutputCP(original_console_cp);
                return 1;
            }
        }
        else if (wcscmp(argv[i], L"/F") == 0 || wcscmp(argv[i], L"-f") == 0)
        {
            options.list_files = true; // -f 参数设置 list_files
        }
        else if (wcscmp(argv[i], L"/A") == 0 || wcscmp(argv[i], L"--ascii") == 0)
        {
            options.use_ascii = true;
        }
        else if (wcscmp(argv[i], L"-c") == 0)
        {
            options.use_color = true;
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
        else if (wcscmp(argv[i], L"-o") == 0 || wcscmp(argv[i], L"--output") == 0)
        {
            if (i + 1 < argc)
            {
                wcsncpy(options.output_filename, argv[++i], MAX_PATH_LEN - 1);
                options.output_filename[MAX_PATH_LEN - 1] = L'\0';
            }
            else
            {
                fwprintf(stderr, L"\x9519\x8BEF: -o \x9009\x9879\x9700\x8981\x4E00\x4E2A\x6587\x4EF6\x540D\x53C2\x6570\x3002\n");
                SetConsoleOutputCP(original_console_cp);
                return 1;
            }
        }
        else if (wcscmp(argv[i], L"-J") == 0)
        {
            options.output_format = OUTPUT_JSON;
        }
        else if (wcscmp(argv[i], L"-X") == 0)
        {
            options.output_format = OUTPUT_XML;
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
                fwprintf(stderr, L"\x9519\x8BEF: --include \x9009\x9879\x9700\x8981\x4E00\x4E2A\x6A21\x5F0F\x53C2\x6570\x3002\n");
                SetConsoleOutputCP(original_console_cp);
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
                fwprintf(stderr, L"\x9519\x8BEF: --exclude \x9009\x9879\x9700\x8981\x4E00\x4E2A\x6A21\x5F0F\x53C2\x6570\x3002\n");
                SetConsoleOutputCP(original_console_cp);
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
            fwprintf(stderr, L"\x9519\x8BEF: \x672A\x77E5\x9009\x9879 '%s'\nError: Unknown option '%s'\n", argv[i], argv[i]);
            display_help(chinese_lang_preferred);
            SetConsoleOutputCP(original_console_cp);
            return 1;
        }
        else
        {
            wcsncpy(options.path_to_tree, argv[i], MAX_PATH_LEN - 1);
            options.path_to_tree[MAX_PATH_LEN - 1] = L'\0';
        }
    }

    if (options.output_filename[0] != L'\0' || options.output_format != OUTPUT_TEXT)
    {
        options.use_color = false;
    }
    if (!options.output_is_console)
    {
        options.use_color = false;
    }

    FILE *output_stream_main = stdout;
    if (options.output_filename[0] != L'\0')
    {
        output_stream_main = _wfreopen(options.output_filename, L"w, ccs=UTF-8", stdout);
        if (output_stream_main == NULL)
        {
            fwprintf(stderr, L"\x9519\x8BEF: \x65E0\x6CD5\x6253\x5F00\x8F93\x51FA\x6587\x4EF6 '%s'\x3002\n", options.output_filename);
            SetConsoleOutputCP(original_console_cp);
            return 1;
        }
        options.output_is_console = false;
        options.use_color = false;
    }
    
    if (help_requested)
    {
        display_help(chinese_lang_preferred);
        SetConsoleOutputCP(original_console_cp);
        return 0;
    }

    if (options.use_si_units && !options.show_size)
    {
        options.show_size = true;
    }

    wchar_t full_root_path[MAX_PATH_LEN];
    if (_wfullpath(full_root_path, options.path_to_tree, MAX_PATH_LEN) == NULL)
    {
        fwprintf(stderr, L"\x9519\x8BEF: \x65E0\x6CD5\x89E3\x6790\x8DEF\x5F84 '%s'\x3002\n", options.path_to_tree);
        SetConsoleOutputCP(original_console_cp);
        return 1;
    }

    DWORD dwAttrib = GetFileAttributesW(full_root_path);
    if (dwAttrib == INVALID_FILE_ATTRIBUTES || !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY))
    {
        fwprintf(stderr, L"\x9519\x8BEF: \x8DEF\x5F84 '%s' \x65E0\x6548\x6216\x4E0D\x662F\x4E00\x4E2A\x76EE\x5F55\x3002\n", full_root_path);
        SetConsoleOutputCP(original_console_cp);
        return 1;
    }

    Counts counts = {0, 0}; // 初始化计数器

    switch (options.output_format)
    {
    case OUTPUT_JSON:
        counts.dir_count = 1; 
        generate_json_tree(full_root_path, &options, &counts, output_stream_main);
        break;
    case OUTPUT_XML:
        generate_xml_tree(full_root_path, &options, &counts, output_stream_main);
        break;
    case OUTPUT_TEXT:
    default:
        if (options.use_color && options.output_is_console)
        {
            HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
            SetConsoleTextAttribute(hConsole, COLOR_DIR);
            wprintf(L"%s", full_root_path);
            SetConsoleTextAttribute(hConsole, options.saved_console_attributes);
            wprintf(L"\n");
        }
        else
        {
            wprintf(L"%s\n", full_root_path);
        }
        wchar_t initial_prefix[MAX_PATH_LEN * 2] = L"";
        counts.dir_count = 0; 
        counts.file_count = 0;
        print_tree_recursive(full_root_path, 1, &options, &counts, initial_prefix);
        if (!options.no_report)
        {
            // FIX: 使用 Unicode 代码点来表示中文字符串
            wprintf(L"\n%lld \x4E2A\x76EE\x5F55", counts.dir_count); // 个目录
            if (options.list_files || options.output_format != OUTPUT_TEXT)
            {
                wprintf(L", %lld \x4E2A\x6587\x4EF6", counts.file_count); // 个文件
            }
            wprintf(L"\n");
        }
        break;
    }

    if (output_stream_main != stdout && output_stream_main != NULL)
    {
        fclose(output_stream_main);
    }

    // FIX: 在程序退出前恢复原始控制台代码页
    SetConsoleOutputCP(original_console_cp);
    return 0;
}
